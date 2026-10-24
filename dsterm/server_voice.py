"""Push-to-Talk microphone on the server: reassembling ADPCM chunks and running
speech-to-text on a background thread.

Part of the server, split out by feature: the methods below are mixed into
Server (server.py) and share its state through `self`.

What the card can make happen here is narrow, and each step is bounded:

* **Assembly.** Chunks at contiguous offsets into one buffer per connection,
  at most VOICE_BYTES_MAX; a gap, an overrun or a chunk before VOICE_START
  drops the connection (voice.VoiceAssembler).
* **One transcription per card at a time.** VOICE_END while this card's last
  one is still running answers BUSY. The backend itself takes one utterance
  at a time across cards (voice.Transcriber) and answers BUSY to the second.
* **A deadline on every one.** Each backend has its own (a process killed, a
  read given up); on top of that the watchdog, _expire_voice_jobs (run from
  _voice_tick), abandons a job that outlives `_voice_deadline_s`, answers the card NO_STT and clears its busy state. A
  late result from an abandoned job is recognised by its job number and
  dropped, so it can never land in the composer after the card was told no.
* **Nothing from the card names anything.** The process that runs, its
  arguments and where the audio goes were all fixed when the server started
  (voice.choose_backend); the card contributes samples and a language code
  that must be two or three letters (voice.voice_lang) or is dropped.
"""

from __future__ import annotations

import os
import queue
import threading
import time
from typing import Callable

from . import voice as dsvoice
from .connection import ClientState, log
from .protocol import (
    Message,
    MsgType,
    ProtocolError,
    VOICE_STATUS_BUSY,
    VOICE_STATUS_EMPTY,
    VOICE_STATUS_INTEGRITY,
    VOICE_STATUS_NO_STT,
    VOICE_STATUS_OK,
    decode_voice_chunk,
    decode_voice_end,
    decode_voice_start,
    encode_voice_ack,
    encode_voice_text,
)

_STATUS_WORDS = {
    VOICE_STATUS_BUSY: "busy",
    VOICE_STATUS_INTEGRITY: "integrity",
    VOICE_STATUS_NO_STT: "no speech-to-text",
    VOICE_STATUS_EMPTY: "empty",
}


class VoiceMixin:
    """Owns the speech-to-text backend and result queue (_init_voice); per
    connection, ClientState.voice."""

    def _on_voice(self, c: ClientState, msg: Message) -> None:
        # Announced AND offered: a card that never said "mic", or any card
        # under --no-mic, was never sent MIC_READY, so a voice frame from it
        # is a protocol error, exactly as a camera frame is under --no-camera.
        if not c.caps.mic or not self.voice_enabled:
            self._drop(c, f"unexpected message {msg.type.name} "
                          "(mic was not negotiated)")
            return
        self._handle_voice(c, msg)

    def _handle_voice(self, c: ClientState, msg: Message) -> None:
        if msg.type is MsgType.VOICE_START:
            try:
                fmt, lang = decode_voice_start(msg.payload)
            except ProtocolError as exc:
                self._drop(c, f"bad voice start: {exc}")
                return
            if c.voice.assembler is None:
                c.voice.assembler = dsvoice.VoiceAssembler()
            try:
                c.voice.assembler.start(fmt, lang)
            except ProtocolError as exc:
                self._drop(c, f"voice start: {exc}")
                return
            self._emit(f"voice recording started "
                       f"({c.voice.assembler.lang or 'language auto'})")
            return

        if msg.type is MsgType.VOICE_CHUNK:
            try:
                offset, data = decode_voice_chunk(msg.payload)
            except ProtocolError as exc:
                self._drop(c, f"bad voice chunk: {exc}")
                return
            if c.voice.assembler is None:
                self._drop(c, "voice chunk without voice start")
                return
            try:
                received = c.voice.assembler.feed(offset, data)
            except ProtocolError as exc:
                self._drop(c, f"voice chunk: {exc}")
                return
            self._send_to(c, encode_voice_ack(received))
            return

        if msg.type is MsgType.VOICE_END:
            try:
                total, crc = decode_voice_end(msg.payload)
            except ProtocolError as exc:
                self._drop(c, f"bad voice end: {exc}")
                return
            if c.voice.assembler is None:
                self._drop(c, "voice end without voice start")
                return
            lang = c.voice.assembler.lang
            try:
                adpcm = c.voice.assembler.finish(total, crc)
            except ProtocolError as exc:
                self._drop(c, f"voice end: {exc}")
                return
            if c.voice.transcribing:
                self._send_to(c, encode_voice_text(VOICE_STATUS_BUSY))
                return
            if adpcm is None:
                self._send_to(c, encode_voice_text(VOICE_STATUS_INTEGRITY))
                return
            if not self._voice_can_transcribe():
                # MIC_READY already said so; a card that records anyway is
                # told again rather than having its audio go anywhere.
                self._send_to(c, encode_voice_text(VOICE_STATUS_NO_STT))
                return
            self._start_voice_transcribe(c, adpcm, lang)
            return

    def _voice_can_transcribe(self) -> bool:
        return self._voice_transcriber is not None or self._voice_engine is not None

    def _start_voice_transcribe(self, c: ClientState, adpcm: bytes,
                                lang: str | None) -> None:
        self._voice_jobs += 1
        job = self._voice_jobs
        c.voice.transcribing = True
        c.voice.job = job
        c.voice.deadline = time.monotonic() + self._voice_deadline_s
        threading.Thread(
            target=self._run_voice_transcribe,
            args=(c, job, adpcm, lang),
            name="dsterm-voice",
            daemon=True,
        ).start()

    def _run_voice_transcribe(self, c: ClientState, job: int, adpcm: bytes,
                              lang: str | None) -> None:
        """On the worker thread. Touches nothing of the connection's: the
        result goes on the queue, and the loop decides whether it still
        applies."""
        try:
            wav_bytes = dsvoice.adpcm_to_wav(adpcm)
            if self._voice_transcriber is not None:
                status, text = self._voice_transcriber(wav_bytes, lang)
                text = dsvoice.fold_transcript(text) if status == VOICE_STATUS_OK else ""
                if status == VOICE_STATUS_OK and not text:
                    status = VOICE_STATUS_EMPTY
            else:
                status, text = self._voice_engine.transcribe(wav_bytes, lang)
        except Exception as exc:  # noqa: BLE001 - a bug here must not wedge busy
            log.warning("voice transcription failed: %r", exc)
            status, text = VOICE_STATUS_NO_STT, ""
        self._voice_results.put((c, job, status, text))

    def _collect_voice_results(self) -> None:
        while True:
            try:
                c, job, status, text = self._voice_results.get_nowait()
            except queue.Empty:
                return
            # Not this card's current job: abandoned at its deadline (the card
            # was already answered) or the card left. Dropped, never sent.
            if c.voice.job != job:
                continue
            c.voice.transcribing = False
            c.voice.job = 0
            if c not in self._clients or not c.authed:
                continue
            if status == VOICE_STATUS_OK:
                self._emit(f"voice transcribed ({len(text)} chars)")
                self._send_to(c, encode_voice_text(VOICE_STATUS_OK, text))
            else:
                self._emit(f"voice not transcribed: {_STATUS_WORDS.get(status, status)}")
                self._send_to(c, encode_voice_text(status))

    def _expire_voice_jobs(self, now: float) -> None:
        """The watchdog: a transcription past its deadline is abandoned, the
        card told NO_STT, and its busy state cleared -- whatever the backend
        is doing, the microphone works again."""
        for c in tuple(self._clients):
            if not c.voice.transcribing or now < c.voice.deadline:
                continue
            c.voice.transcribing = False
            c.voice.job = 0
            self._emit(f"voice: no transcript in {self._voice_deadline_s:.0f} s; "
                       "abandoned")
            if c.authed:
                self._send_to(c, encode_voice_text(VOICE_STATUS_NO_STT))

    # -- what the core calls (server.py) -------------------------------------

    def _init_voice(self, voice_enabled: bool = True,
                    transcriber: Callable[[bytes, str | None], tuple[int, str]] | None = None,
                    cloud: bool = False) -> None:
        """The microphone's state on the server. Only this module touches it
        (tests/test_layout.py)."""
        # Off under --no-mic: the HELLO token is then never honoured, MIC_READY
        # never sent, and any voice frame drops the client.
        self.voice_enabled = voice_enabled
        # A test's stand-in for the backend. Never set by `dsterm serve`.
        self._voice_transcriber = transcriber
        # The one backend, chosen now and named at startup (voice_where): local
        # if installed, cloud only with the opt-in, otherwise none -- and then
        # MIC_READY says so and the card shows it.
        self._voice_engine: dsvoice.Transcriber | None = None
        if not voice_enabled:
            self.voice_where = "off (--no-mic)"
        elif transcriber is not None:
            self.voice_where = "handed in by the caller"
        else:
            env = self.env if self.env is not None else dict(os.environ)
            backend, self.voice_where = dsvoice.choose_backend(env, cloud)
            if backend is not None:
                self._voice_engine = dsvoice.Transcriber(backend, env)
        # (client, job, status, text) from the worker threads.
        self._voice_results: queue.Queue = queue.Queue()
        # Numbers each transcription, so a result is matched to the job the
        # card is still waiting for and nothing else.
        self._voice_jobs = 0
        # The watchdog's limit: the backend's timeout plus a margin
        # (voice.WATCHDOG_MARGIN_S), inside the 20 s the card itself waits
        # (MIC_WAIT_TIMEOUT_FRAMES in mic.c; voice.MAX_TIMEOUT_S keeps it so).
        self._voice_deadline_s = (self._voice_engine.watchdog_s
                                  if self._voice_engine is not None
                                  else dsvoice.DEFAULT_TIMEOUT_S
                                  + dsvoice.WATCHDOG_MARGIN_S)

    def _voice_offered(self) -> bool:
        return self.voice_enabled

    def _voice_tick(self) -> None:
        self._collect_voice_results()
        self._expire_voice_jobs(time.monotonic())

    def _voice_card_gone(self, c: ClientState) -> None:
        """A card left: forget its half-recorded utterance, and its job, so a
        transcript that finishes later is dropped instead of delivered to
        whoever connects next."""
        if c.voice.assembler is not None:
            c.voice.assembler.reset()
        c.voice.transcribing = False
        c.voice.job = 0

    def warm_voice(self) -> None:
        """Load the speech model now, so the first recording is not the one
        that waits for it. Only `dsterm serve` calls this, once, as it starts
        serving -- never the constructor: the test suite and the tools build
        Server objects by the hundred, and a model load each (a worker process
        holding hundreds of MB) is how one test run took a whole Mac down."""
        if self._voice_engine is not None:
            self._voice_engine.warm_up()

    def _stop_voice(self) -> None:
        if self._voice_engine is not None:
            self._voice_engine.close()
