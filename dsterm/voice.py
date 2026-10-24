"""Push-to-Talk microphone: 4-bit IMA ADPCM codec, assembler, and transcription.

The DS sends 8,000 Hz mono audio compressed 4:1 with 4-bit IMA ADPCM (4 KB/s)
and a short language code hint ("en", "es", ...).  This module reassembles the
chunks, verifies the CRC-32, expands the ADPCM to 16-bit PCM WAV, and hands it
to ONE speech-to-text backend, chosen when the server starts:

  1. a local one, if any is installed: ``DSTERM_STT_CMD``, then the Python
     libraries ``mlx-whisper``, ``faster-whisper`` and ``openai-whisper`` (run
     in a worker process, ``stt_worker.py``), then a ``whisper-cli`` /
     ``whisper-cpp`` binary on PATH;
  2. otherwise a cloud API -- OpenAI, Groq, or ``DSTERM_STT_URL`` -- but ONLY
     when the person running the server opted in (``dsterm serve --stt-cloud``
     or ``DSTERM_STT_CLOUD=1`` in the server's own environment).

Three properties are load-bearing:

* **Voice stays on this computer unless asked.**  An ``OPENAI_API_KEY`` in the
  environment is there for other programs; its presence alone sends nothing
  anywhere.  Without the opt-in no cloud backend is even considered, and the
  card is told "no transcription available" rather than having its audio
  quietly uploaded.
* **No silent fallback.**  The backend is one, picked once and named at
  startup.  When it fails, that failure is logged and the card told; the
  audio is not then tried somewhere else.
* **Bounded.**  Every backend runs under a hard deadline: a process is killed
  at it, a cloud read gives up at it.  A wedged transcription can cost one
  utterance, never the microphone.

What crosses into a backend: the WAV (a temporary 0600 file, removed in all
cases), and a language code that must match ``^[a-z]{2,3}$`` (``voice_lang``)
or is dropped for auto-detection.  Both go as argv elements or form fields,
never through a shell.  The text that comes back is folded to printable ASCII
(``clip.convert``) and sent in ``MSG_VOICE_TEXT``, where it lands in the
handheld's composer and goes nowhere until the person presses A.
"""

from __future__ import annotations

import importlib.metadata
import importlib.util
import io
import json
import logging
import os
import re
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import wave
from dataclasses import dataclass, field
from pathlib import Path

from . import camera as dscamera
from . import clip as dsclip
from .protocol import (
    ProtocolError,
    VOICE_BYTES_MAX,
    VOICE_CHUNK_MAX,
    VOICE_FORMAT_IMA_ADPCM_8K,
    VOICE_SAMPLE_RATE,
    VOICE_STATUS_BUSY,
    VOICE_STATUS_EMPTY,
    VOICE_STATUS_NO_STT,
    VOICE_STATUS_OK,
    VOICE_TEXT_MAX,
)

# The server's own logger (connection.py's), named rather than imported:
# connection.py imports this module.
log = logging.getLogger("dsterm.server")


STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)


def _clamp_i16(v: int) -> int:
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def _clamp_idx(idx: int) -> int:
    return 0 if idx < 0 else (88 if idx > 88 else idx)


@dataclass(slots=True)
class AdpcmState:
    predictor: int = 0
    index: int = 0


def adpcm_encode(samples: list[int] | tuple[int, ...],
                 state: AdpcmState | None = None) -> bytes:
    """Encode an even sequence of 16-bit signed PCM samples into 4-bit IMA
    ADPCM bytes, matching dsclient/source/voice.c bit for bit."""
    st = state if state is not None else AdpcmState()
    pairs = len(samples) // 2
    out = bytearray(pairs)
    for i in range(pairs):
        lo = _encode_nibble(st, samples[2 * i])
        hi = _encode_nibble(st, samples[2 * i + 1])
        out[i] = lo | (hi << 4)
    return bytes(out)


def _encode_nibble(st: AdpcmState, sample: int) -> int:
    step = STEP_TABLE[st.index]
    diff = int(sample) - st.predictor
    nibble = 0
    if diff < 0:
        nibble = 8
        diff = -diff
    vpdiff = step >> 3
    if diff >= step:
        nibble |= 4
        diff -= step
        vpdiff += step
    if diff >= (step >> 1):
        nibble |= 2
        diff -= step >> 1
        vpdiff += step >> 1
    if diff >= (step >> 2):
        nibble |= 1
        vpdiff += step >> 2
    if nibble & 8:
        st.predictor -= vpdiff
    else:
        st.predictor += vpdiff
    st.predictor = _clamp_i16(st.predictor)
    st.index = _clamp_idx(st.index + INDEX_TABLE[nibble & 7])
    return nibble & 0x0F


def adpcm_decode(adpcm: bytes, state: AdpcmState | None = None) -> list[int]:
    """Decode 4-bit IMA ADPCM bytes into 16-bit signed PCM samples."""
    st = state if state is not None else AdpcmState()
    out: list[int] = []
    for b in adpcm:
        out.append(_decode_nibble(st, b & 0x0F))
        out.append(_decode_nibble(st, (b >> 4) & 0x0F))
    return out


def _decode_nibble(st: AdpcmState, nibble: int) -> int:
    step = STEP_TABLE[st.index]
    vpdiff = step >> 3
    if nibble & 4:
        vpdiff += step
    if nibble & 2:
        vpdiff += step >> 1
    if nibble & 1:
        vpdiff += step >> 2
    if nibble & 8:
        st.predictor -= vpdiff
    else:
        st.predictor += vpdiff
    st.predictor = _clamp_i16(st.predictor)
    st.index = _clamp_idx(st.index + INDEX_TABLE[nibble & 7])
    return st.predictor


def wav_16k(wav_bytes: bytes) -> bytes:
    """An 8 kHz mono WAV at 16 kHz, each new sample the mean of its two
    neighbours. whisper.cpp reads only 16 kHz; the Python libraries
    resample by themselves."""
    with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
        rate = wf.getframerate()
        frames = wf.readframes(wf.getnframes())
    if rate != VOICE_SAMPLE_RATE or not frames:
        return wav_bytes
    src = struct.unpack(f"<{len(frames) // 2}h", frames)
    out: list[int] = []
    for i, v in enumerate(src):
        nxt = src[i + 1] if i + 1 < len(src) else v
        out.append(v)
        out.append((v + nxt) // 2)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(2 * VOICE_SAMPLE_RATE)
        wf.writeframes(struct.pack(f"<{len(out)}h", *out))
    return buf.getvalue()


def adpcm_to_wav(adpcm: bytes, sample_rate: int = VOICE_SAMPLE_RATE) -> bytes:
    """Decode ADPCM to an in-memory 16-bit mono WAV."""
    samples = adpcm_decode(adpcm)
    pcm_bytes = struct.pack(f"<{len(samples)}h", *samples)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


@dataclass(slots=True)
class VoiceAssembler:
    """Reassembles contiguous VOICE_CHUNK frames for one utterance."""

    fmt: int = VOICE_FORMAT_IMA_ADPCM_8K
    # The hint as voice_lang() left it: a 2-3 letter code, or None.
    lang: str | None = None
    active: bool = False
    _buf: bytearray = field(default_factory=bytearray)

    def start(self, fmt: int, lang: str) -> None:
        if fmt != VOICE_FORMAT_IMA_ADPCM_8K:
            raise ProtocolError(f"unsupported voice format {fmt}")
        self.fmt = fmt
        self.lang = voice_lang(lang)
        self.active = True
        self._buf.clear()

    def reset(self) -> None:
        self.active = False
        self._buf.clear()

    def feed(self, offset: int, data: bytes) -> int:
        if not self.active:
            raise ProtocolError("voice chunk without voice start")
        if not 1 <= len(data) <= VOICE_CHUNK_MAX:
            raise ProtocolError(f"voice chunk length {len(data)} out of range")
        if offset == 0:
            self._buf.clear()
        elif offset != len(self._buf):
            raise ProtocolError(
                f"voice chunk offset {offset} does not match {len(self._buf)}")
        if len(self._buf) + len(data) > VOICE_BYTES_MAX:
            raise ProtocolError("voice chunks exceed VOICE_BYTES_MAX")
        self._buf.extend(data)
        return len(self._buf)

    def finish(self, total: int, expected_crc: int) -> bytes | None:
        if not self.active:
            raise ProtocolError("voice end without voice start")
        raw = bytes(self._buf)
        self.reset()
        if len(raw) != total:
            raise ProtocolError(f"voice end total {total} != received {len(raw)}")
        if dscamera.crc32(raw) != (expected_crc & 0xFFFFFFFF):
            return None
        return raw


# -- what reaches a backend ---------------------------------------------------

# fullmatch, not match with "$": "$" also matches before a final newline.
_LANG_RE = re.compile(r"[a-z]{2,3}")


def voice_lang(raw: str | None) -> str | None:
    """The card's language hint as a two- or three-letter ISO 639 code, or
    None, which every backend reads as "detect it".

    The wire already holds it to seven of ``[A-Za-z0-9_-]`` (protocol.py).
    This is the narrower rule for what may reach an argv or a form field: a
    region suffix ("en-US", "pt_BR") is cut off, and anything else that is not
    two or three letters is dropped whole rather than repaired -- a hint that
    does not look like a language is not one, and auto-detection is a safe
    answer to it."""
    if not raw:
        return None
    base = re.split(r"[-_]", raw, maxsplit=1)[0].lower()
    return base if _LANG_RE.fullmatch(base) else None


# A model name goes to a library (a Hugging Face repo id at most) and to
# nothing else; held to a shape that cannot be an option or a path outside.
_MODEL_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,95}")

# Under the card's own wait: the DS gives up on a transcript 20 s after its
# VOICE_END (MIC_WAIT_TIMEOUT_FRAMES in dsclient/source/mic.c), drops an answer
# that arrives later, and refuses new recordings until it does. So the
# watchdog (the timeout + WATCHDOG_MARGIN_S) must answer first: 15 s by
# default, at most 17 s, and the 2 s of margin mean the card is told rather
# than left to time out -- a longer timeout would only leave it saying "still
# finishing".
DEFAULT_TIMEOUT_S = 15.0
MAX_TIMEOUT_S = 17.0
WATCHDOG_MARGIN_S = 2.0
# The first utterance a library worker sees also loads its model, and on a
# first run ever that can mean downloading the weights (tens of MB to over a
# GB). Long enough for that on an ordinary connection; still a deadline. The
# card is not kept waiting for it: the server's watchdog answers it at the
# usual deadline, the load carries on, and utterances meanwhile are BUSY.
LOAD_TIMEOUT_S = 300.0
# The most a backend may answer with. A transcript is at most a few hundred
# characters; anything near this is not one.
_REPLY_MAX = 64 * 1024


def stt_timeout(env: dict[str, str] | None = None) -> float:
    """``DSTERM_STT_TIMEOUT`` in seconds, held to 5..MAX_TIMEOUT_S; 15 otherwise."""
    e = env if env is not None else os.environ
    try:
        value = float(e.get("DSTERM_STT_TIMEOUT", DEFAULT_TIMEOUT_S))
    except ValueError:
        return DEFAULT_TIMEOUT_S
    if value != value:          # NaN compares false to everything
        return DEFAULT_TIMEOUT_S
    return min(MAX_TIMEOUT_S, max(5.0, value))


def cloud_opted_in(flag: bool, environ: dict[str, str] | None = None) -> bool:
    """``--stt-cloud``, or ``DSTERM_STT_CLOUD=1`` in the SERVER's environment.

    Not the env file (``~/.config/dsterm/env``): that is the child's
    environment, and a switch that decides whether audio leaves this computer
    is read from what the person started the server with, nowhere else."""
    e = environ if environ is not None else os.environ
    return bool(flag) or e.get("DSTERM_STT_CLOUD", "").strip() == "1"


# -- backends ------------------------------------------------------------------

# (module, the distribution that must provide it). The distribution check is
# what makes "whisper" mean openai-whisper: an unrelated package that installs
# a module of that name (Graphite's time-series "whisper" does) is not a
# speech-to-text backend, and would otherwise be picked and fail on every
# utterance.
_LIBRARIES = (
    ("mlx_whisper", "mlx-whisper"),
    ("faster_whisper", "faster-whisper"),
    ("whisper", "openai-whisper"),
)
_BINARIES = ("whisper-cli", "whisper-cpp")
# mlx-whisper and openai-whisper read audio by running ffmpeg; without it every
# utterance fails. faster-whisper decodes with PyAV and needs no ffmpeg.
_NEEDS_FFMPEG = frozenset({"mlx_whisper", "whisper"})

OPENAI_URL = "https://api.openai.com/v1/audio/transcriptions"
GROQ_URL = "https://api.groq.com/openai/v1/audio/transcriptions"


@dataclass(frozen=True, slots=True)
class Backend:
    """The one speech-to-text backend this server uses. Holds no secret: a
    cloud backend names the variable its key is read from at request time."""

    kind: str                   # "cmd", "library", "binary" or "cloud"
    name: str                   # what the startup line and the doctor say
    target: str                 # the command, the module, the binary, the URL
    key_var: str | None = None  # cloud: the variable holding the key
    model: str | None = None

    @property
    def cloud(self) -> bool:
        return self.kind == "cloud"

    def where(self) -> str:
        """Where an utterance goes, in words for the startup line."""
        if self.cloud:
            return f"sent to {self.name} (cloud, opted in)"
        return f"transcribed on this computer ({self.name})"


def _has_library(module: str, dist: str) -> bool:
    if importlib.util.find_spec(module) is None:
        return False
    try:
        importlib.metadata.distribution(dist)
    except importlib.metadata.PackageNotFoundError:
        return False
    return True


def _model(env: dict[str, str]) -> str | None:
    name = env.get("DSTERM_STT_MODEL", "").strip()
    return name if _MODEL_RE.fullmatch(name) else None


def find_local_backend(env: dict[str, str] | None = None) -> Backend | None:
    """The first local backend installed, or None. Nothing here touches the
    network."""
    e = env if env is not None else os.environ
    custom = e.get("DSTERM_STT_CMD", "").strip()
    if custom:
        return Backend("cmd", f"DSTERM_STT_CMD {Path(custom).name}", custom)
    for module, dist in _LIBRARIES:
        if module in _NEEDS_FFMPEG and not shutil.which("ffmpeg"):
            continue
        if _has_library(module, dist):
            return Backend("library", dist, module, model=_model(e))
    for cli_bin in _BINARIES:
        found = shutil.which(cli_bin)
        if found:
            # whisper.cpp needs its model file named (-m); DSTERM_STT_MODEL
            # may be that file's path. Without one it uses its own default.
            path = os.path.expanduser(e.get("DSTERM_STT_MODEL", "").strip())
            return Backend("binary", cli_bin, found,
                           model=path if path and os.path.isfile(path) else None)
    return None


def find_cloud_backend(env: dict[str, str] | None = None
                       ) -> tuple[Backend | None, str]:
    """The cloud backend the environment describes, or (None, why not).

    Only ever consulted after the opt-in (``choose_backend``). Two rules hold
    whatever is set: the URL is https, so the audio and the key are never sent
    in the clear; and a key goes only to its own service -- a custom
    ``DSTERM_STT_URL`` is sent ``DSTERM_STT_KEY`` and never ``OPENAI_API_KEY``,
    which a typo'd or hostile URL would otherwise collect."""
    e = env if env is not None else os.environ
    model = _model(e)
    url = e.get("DSTERM_STT_URL", "").strip()
    if url:
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != "https" or not parts.hostname:
            return None, "DSTERM_STT_URL is not an https:// URL"
        return Backend("cloud", parts.hostname, url, "DSTERM_STT_KEY",
                       model or "whisper-1"), ""
    if e.get("OPENAI_API_KEY"):
        return Backend("cloud", "api.openai.com", OPENAI_URL, "OPENAI_API_KEY",
                       model or "whisper-1"), ""
    if e.get("GROQ_API_KEY"):
        return Backend("cloud", "api.groq.com", GROQ_URL, "GROQ_API_KEY",
                       model or "whisper-large-v3-turbo"), ""
    return None, "none of OPENAI_API_KEY, GROQ_API_KEY or DSTERM_STT_URL is set"


def choose_backend(env: dict[str, str] | None = None, cloud: bool = False
                   ) -> tuple[Backend | None, str]:
    """(the backend, where voice goes in words). Local first, always; cloud
    only with ``cloud`` (the opt-in) and only when nothing local is installed.
    Never both: the backend chosen here is the only one an utterance meets."""
    local = find_local_backend(env)
    if local is not None:
        return local, local.where()
    if not cloud:
        return None, ("off: no local speech-to-text installed (cloud is "
                      "opt-in: dsterm serve --stt-cloud)")
    remote, why = find_cloud_backend(env)
    if remote is None:
        return None, f"off: --stt-cloud is on, but {why}"
    return remote, remote.where()


def fold_transcript(raw: str) -> str:
    """Fold UTF-8 transcript text into printable ASCII that fits the composer.

    ASCII because the card's composer is: an accent loses its mark ("cafe"),
    and a script with no ASCII form at all (Chinese, Japanese) folds to
    nothing, which ``Transcriber.transcribe`` answers EMPTY and logs."""
    snap = dsclip.convert(raw.strip())
    text = " ".join(snap.text.splitlines()).strip()
    return text[:VOICE_TEXT_MAX]


class SttError(Exception):
    """A backend did not produce a transcript. The message is for the log:
    it never carries the audio, the transcript or a key."""


def _stop_process(proc: subprocess.Popen) -> None:
    """Kill a backend process and everything it started (it leads its own
    session), then reap it."""
    if proc.poll() is None:
        try:
            if hasattr(os, "killpg"):
                os.killpg(proc.pid, 9)
            else:               # pragma: no cover - not POSIX
                proc.kill()
        except OSError:
            pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:   # pragma: no cover - kill -9 ignored
        pass


def _run_argv(argv: list[str], timeout: float) -> str:
    """One run of a local binary: argv, never a shell; no stdin; killed with
    its children at the deadline."""
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL,
                                start_new_session=True)
    except OSError as exc:
        raise SttError(f"cannot run {Path(argv[0]).name}: "
                       f"{exc.strerror or exc}") from None
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        _stop_process(proc)
        raise SttError(f"{Path(argv[0]).name} gave no answer in "
                       f"{timeout:.0f} s and was stopped") from None
    if proc.returncode != 0:
        raise SttError(f"{Path(argv[0]).name} exited with {proc.returncode}")
    return out[:_REPLY_MAX].decode("utf-8", errors="replace")


class LibraryWorker:
    """A Python speech-to-text library in a process of its own (stt_worker.py).

    Its own process, for two reasons. The model stays loaded between
    utterances -- loading one costs seconds, and it was being paid on every
    utterance -- and a call that hangs can actually be stopped: a thread
    cannot be killed, a process can. At the deadline the worker is killed with
    everything it started, and the next utterance starts a fresh one.

    ``-P``: the worker does not put the current directory on its import path,
    so a ``json.py`` in the project being worked on is never imported by it.
    The package's own directory is given through PYTHONPATH instead.
    """

    def __init__(self, module: str, model: str | None,
                 argv: list[str] | None = None,
                 env: dict[str, str] | None = None) -> None:
        # The Transcriber's environment (os.environ with the env file over
        # it), so HF_HOME or HF_TOKEN set there reach the library.
        self._env = dict(env if env is not None else os.environ)
        self._argv = argv or [sys.executable, "-P", "-m", "dsterm.stt_worker",
                              module, model or ""]
        self._proc: subprocess.Popen | None = None
        self._buf = b""
        # Whether this worker has answered once, i.e. has its model loaded:
        # until then the deadline is LOAD_TIMEOUT_S, not the per-utterance one.
        self._warm = False
        # The process a thread is reading an answer from, if any. close() on
        # another thread leaves that process's stdout for the reader to close:
        # a descriptor closed mid-read can be reused by the next file opened,
        # and the read would then take bytes that are not the worker's.
        self._io = threading.Lock()
        self._reading: subprocess.Popen | None = None

    def _start(self) -> None:
        env = dict(self._env)
        root = str(Path(__file__).resolve().parent.parent)
        env["PYTHONPATH"] = (root + os.pathsep + env["PYTHONPATH"]
                             if env.get("PYTHONPATH") else root)
        try:
            self._proc = subprocess.Popen(
                self._argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        except OSError as exc:
            raise SttError(f"cannot start the worker: {exc.strerror or exc}") from None
        self._buf = b""
        self._warm = False

    def transcribe(self, wav_path: str, lang: str | None, timeout: float) -> str:
        if self._proc is None or self._proc.poll() is not None:
            self.close()
            self._start()
        proc = self._proc
        assert proc is not None and proc.stdin is not None
        limit = timeout if self._warm else max(timeout, LOAD_TIMEOUT_S)
        request = json.dumps({"wav": wav_path, "lang": lang}) + "\n"
        try:
            proc.stdin.write(request.encode("utf-8"))
            proc.stdin.flush()
        except (OSError, ValueError):
            # ValueError: the pipe was closed under us (close() on another
            # thread), which is the same news as a broken one.
            self.close()
            raise SttError("the worker exited") from None
        line = self._read_line(proc, limit)
        try:
            reply = json.loads(line)
        except ValueError:
            self.close()
            raise SttError("the worker's answer was not JSON") from None
        if not isinstance(reply, dict):
            self.close()
            raise SttError("the worker's answer was not an object")
        # Any well-formed answer means the model load is behind it, an error
        # included ("no speech", a bad file): the next utterance is held to
        # the per-utterance deadline, not LOAD_TIMEOUT_S, which would leave
        # the card answered BUSY for minutes behind one that hung.
        self._warm = True
        if reply.get("ok") is not True:
            raise SttError(str(reply.get("error", "failed"))[:200])
        return str(reply.get("text", ""))

    def _read_line(self, proc: subprocess.Popen, limit: float) -> bytes:
        with self._io:
            if self._proc is not proc:
                raise SttError("the worker exited")
            self._reading = proc
        try:
            return self._read_line_from(proc, limit)
        except (OSError, ValueError):
            # close() on another thread (the server shutting down, Ctrl-C
            # during a model load) kills the worker under this read, which
            # then sees its end of file -- or, on a pipe already gone, an
            # OSError or a ValueError. The worker is stopped either way; say
            # that, rather than a traceback from the warm-up thread.
            raise SttError("the worker exited") from None
        finally:
            with self._io:
                self._reading = None
                orphaned = self._proc is not proc
            if orphaned and proc.stdout is not None:
                try:
                    proc.stdout.close()
                except OSError:
                    pass

    def _read_line_from(self, proc: subprocess.Popen, limit: float) -> bytes:
        assert proc.stdout is not None
        fd = proc.stdout.fileno()
        deadline = time.monotonic() + limit
        with selectors.DefaultSelector() as sel:
            sel.register(fd, selectors.EVENT_READ)
            while b"\n" not in self._buf:
                left = deadline - time.monotonic()
                if left <= 0:
                    self.close()
                    raise SttError(f"no answer in {limit:.0f} s; the worker "
                                   "was stopped")
                if not sel.select(left):
                    continue
                chunk = os.read(fd, 65536)
                if not chunk:
                    self.close()
                    raise SttError("the worker exited")
                self._buf += chunk
                if len(self._buf) > _REPLY_MAX:
                    self.close()
                    raise SttError("the worker's answer was too long")
        line, _, self._buf = self._buf.partition(b"\n")
        return line

    def close(self) -> None:
        with self._io:
            proc, self._proc = self._proc, None
            being_read = proc is not None and self._reading is proc
        self._buf = b""
        self._warm = False
        if proc is None:
            return
        # Killing it is what wakes a reader on another thread: its pipe
        # reaches end of file, and the reader closes that pipe itself.
        _stop_process(proc)
        pipes = (proc.stdin,) if being_read else (proc.stdin, proc.stdout)
        for pipe in pipes:
            if pipe is not None:
                try:
                    pipe.close()
                except OSError:
                    pass


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """A redirect is refused, not followed: urllib would resend the
    Authorization header to wherever it pointed, http:// included."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def _cloud_transcribe(backend: Backend, wav_bytes: bytes, lang: str | None,
                      env: dict[str, str], timeout: float) -> str:
    """One upload, under the opt-in only. The audio and the language code go
    as form fields of a fixed multipart body; the key as a bearer header, to
    the https URL ``find_cloud_backend`` accepted and nowhere else."""
    if urllib.parse.urlsplit(backend.target).scheme != "https":
        raise SttError("refusing a cloud URL that is not https")
    boundary = "----dsterm-" + os.urandom(12).hex()
    parts: list[bytes] = []

    def add_field(name: str, value: str) -> None:
        parts.append(f"--{boundary}\r\n".encode())
        parts.append(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
                     f"{value}\r\n".encode())

    add_field("model", backend.model or "whisper-1")
    if lang:
        add_field("language", lang)
    parts.append(f"--{boundary}\r\n".encode())
    parts.append(b'Content-Disposition: form-data; name="file"; '
                 b'filename="speech.wav"\r\nContent-Type: audio/wav\r\n\r\n')
    parts.append(wav_bytes)
    parts.append(f"\r\n--{boundary}--\r\n".encode())

    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    key = env.get(backend.key_var, "") if backend.key_var else ""
    if key:
        headers["Authorization"] = f"Bearer {key}"
    req = urllib.request.Request(backend.target, data=b"".join(parts),
                                 headers=headers, method="POST")
    deadline = time.monotonic() + timeout
    body = bytearray()
    try:
        # The socket timeout bounds each read; the loop bounds them all, so a
        # server that drips one byte at a time still meets the deadline.
        with _OPENER.open(req, timeout=timeout) as resp:
            while True:
                if time.monotonic() > deadline:
                    raise SttError(f"no answer in {timeout:.0f} s")
                chunk = resp.read(4096)
                if not chunk:
                    break
                body += chunk
                if len(body) > _REPLY_MAX:
                    raise SttError("the answer was too long")
    except urllib.error.HTTPError as exc:
        raise SttError(f"{backend.name} answered HTTP {exc.code}") from None
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise SttError(f"{backend.name}: {type(exc).__name__}") from None
    try:
        data = json.loads(body.decode("utf-8", errors="replace"))
    except ValueError:
        raise SttError(f"{backend.name}'s answer was not JSON") from None
    if not isinstance(data, dict):
        raise SttError(f"{backend.name}'s answer was not an object")
    return str(data.get("text", ""))


def _silence_wav(seconds: float = 0.5) -> bytes:
    """Half a second of 16 kHz silence: enough to make a model load."""
    rate = 2 * VOICE_SAMPLE_RATE
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\x00\x00" * int(rate * seconds))
    return buf.getvalue()


class Transcriber:
    """One backend, one utterance at a time.

    A second ``transcribe`` while one runs answers BUSY at once instead of
    queueing behind it. Every path out releases that, and every backend has
    a deadline (``timeout``, or LOAD_TIMEOUT_S while a library loads its
    model), so busy cannot outlive a hung transcription by more than that;
    the server's own watchdog (server_voice.py) answers the card sooner."""

    def __init__(self, backend: Backend, env: dict[str, str] | None = None,
                 timeout: float | None = None,
                 worker_argv: list[str] | None = None) -> None:
        self.backend = backend
        self._env = dict(env if env is not None else os.environ)
        self.timeout = timeout if timeout is not None else stt_timeout(self._env)
        self._lock = threading.Lock()
        self._worker = (LibraryWorker(backend.target, backend.model, worker_argv,
                                      env=self._env)
                        if backend.kind == "library" else None)

    @property
    def watchdog_s(self) -> float:
        """How long the server waits for one transcript before answering the
        card NO_STT. The backend itself may run on to its own deadline (a
        model loading, LOAD_TIMEOUT_S); this lock stays held meanwhile, so
        the card is answered BUSY, not queued, until it ends."""
        return self.timeout + WATCHDOG_MARGIN_S

    def transcribe(self, wav_bytes: bytes, lang: str | None) -> tuple[int, str]:
        """(VOICE_STATUS_*, folded text). A failure is logged by backend
        name and reason -- never the transcript -- and answered NO_STT."""
        if not self._lock.acquire(blocking=False):
            return VOICE_STATUS_BUSY, ""
        try:
            raw = self._run(wav_bytes, voice_lang(lang))
        except SttError as exc:
            log.warning("voice: %s did not transcribe: %s", self.backend.name, exc)
            return VOICE_STATUS_NO_STT, ""
        finally:
            self._lock.release()
        folded = fold_transcript(raw)
        if not folded:
            dropped = len(raw.strip())
            if dropped:
                # Speech was heard, in a script the composer cannot hold. A
                # count, never the text: the transcript is not logged.
                log.info("voice: transcript had no ASCII text "
                         "(%d characters dropped)", dropped)
            return VOICE_STATUS_EMPTY, ""
        return VOICE_STATUS_OK, folded

    def _run(self, wav_bytes: bytes, lang: str | None) -> str:
        b = self.backend
        if b.cloud:
            return _cloud_transcribe(b, wav_bytes, lang, self._env, self.timeout)
        # mkstemp makes the file 0600 and opens it in one step; written inside
        # the try, so a failed write still removes it.
        if b.kind == "binary":
            wav_bytes = wav_16k(wav_bytes)
        fd, path = tempfile.mkstemp(prefix="dsterm-voice-", suffix=".wav")
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(wav_bytes)
            if self._worker is not None:
                return self._worker.transcribe(path, lang, self.timeout)
            if b.kind == "cmd":
                return _run_argv([b.target, path, lang or "auto"], self.timeout)
            model = ["-m", b.model] if b.model else []
            return _run_argv([b.target, *model, "-f", path, "-l", lang or "auto",
                              "-nt"], self.timeout)
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass

    def warm_up(self) -> None:
        """Load a library backend's model now, on a thread, so the first
        utterance is not the one that waits for it (and, on a first run ever,
        for its download). Holds the lock meanwhile, so an utterance that
        arrives during the load is answered BUSY rather than timing out as if
        speech-to-text were missing. A no-op for the other backends."""
        if self._worker is None or not self._lock.acquire(blocking=False):
            return

        def load() -> None:
            fd, path = tempfile.mkstemp(prefix="dsterm-voice-", suffix=".wav")
            try:
                with os.fdopen(fd, "wb") as f:
                    f.write(_silence_wav())
                started = time.monotonic()
                try:
                    self._worker.transcribe(path, None, LOAD_TIMEOUT_S)
                except SttError as exc:
                    # Half a second of silence may well be "no speech"; a
                    # worker that answered at all has its model loaded.
                    if not self._worker._warm:
                        raise
                log.info("voice: %s loaded its model in %.0f s",
                         self.backend.name, time.monotonic() - started)
            except SttError as exc:
                log.warning("voice: %s did not load: %s", self.backend.name, exc)
            finally:
                try:
                    os.unlink(path)
                except OSError:
                    pass
                self._lock.release()

        threading.Thread(target=load, name="dsterm-stt-warm", daemon=True).start()

    def close(self) -> None:
        """Stop the library worker, if there is one."""
        if self._worker is not None:
            self._worker.close()

