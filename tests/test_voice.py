"""Tests for Push-to-Talk DS microphone codec, wire framing, and server STT."""

from __future__ import annotations

import io
import json
import os
import socket
import sys
import threading
import time
import wave

import pytest

from dsterm import auth as dsauth
from dsterm import camera as dscamera
from dsterm import voice as dsvoice
from dsterm.protocol import (
    FEATURE_MIC,
    FEATURE_SEAL3,
    HEADER_SIZE,
    VOICE_BYTES_MAX,
    VOICE_CHUNK_MAX,
    VOICE_TEXT_MAX,
    FrameReader,
    MsgType,
    ProtocolError,
    Seal,
    VOICE_FORMAT_IMA_ADPCM_8K,
    VOICE_STATUS_BUSY,
    VOICE_STATUS_EMPTY,
    VOICE_STATUS_INTEGRITY,
    VOICE_STATUS_NO_STT,
    VOICE_STATUS_OK,
    decode_mic_ready,
    decode_voice_ack,
    decode_voice_chunk,
    decode_voice_end,
    decode_voice_start,
    decode_voice_text,
    encode,
    encode_hello,
    encode_mic_ready,
    encode_voice_ack,
    encode_voice_chunk,
    encode_voice_end,
    encode_voice_start,
    encode_voice_text,
    seal_key,
)
from dsterm.server import Server


class TestAdpcmAndWav:
    def test_adpcm_round_trip_tracks_waveform(self):
        samples = [int(8000 * ((i % 20) - 10) / 10) for i in range(200)]
        encoded = dsvoice.adpcm_encode(samples)
        assert len(encoded) == 100
        decoded = dsvoice.adpcm_decode(encoded)
        assert len(decoded) == 200
        # ADPCM is lossy 4:1; after the first step adaptation, error stays bounded.
        avg_err = sum(abs(a - b) for a, b in zip(samples[20:], decoded[20:])) / 180
        assert avg_err < 1800

    def test_adpcm_to_wav_produces_valid_8khz_mono_wav(self):
        encoded = dsvoice.adpcm_encode([1000, -1000] * 80)
        wav_bytes = dsvoice.adpcm_to_wav(encoded)
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            assert wf.getnchannels() == 1
            assert wf.getsampwidth() == 2
            assert wf.getframerate() == 8000
            assert wf.getnframes() == 160


class TestVoiceWireCodecs:
    def test_voice_start_round_trip_and_bounds(self):
        raw = encode_voice_start(VOICE_FORMAT_IMA_ADPCM_8K, "es")[HEADER_SIZE:]
        assert decode_voice_start(raw) == (VOICE_FORMAT_IMA_ADPCM_8K, "es")
        with pytest.raises(ProtocolError):
            encode_voice_start(99, "en")
        with pytest.raises(ProtocolError):
            encode_voice_start(1, "bad/lang")
        with pytest.raises(ProtocolError):
            encode_voice_start(1, "en\n")

    def test_voice_chunk_round_trip_and_bounds(self):
        raw = encode_voice_chunk(480, b"\x12\x34\x56")[HEADER_SIZE:]
        assert decode_voice_chunk(raw) == (480, b"\x12\x34\x56")
        with pytest.raises(ProtocolError):
            encode_voice_chunk(0, b"")
        with pytest.raises(ProtocolError):
            encode_voice_chunk(0, b"x" * 481)

    def test_voice_end_round_trip(self):
        raw = encode_voice_end(960, 0xCAFEBABE)[HEADER_SIZE:]
        assert decode_voice_end(raw) == (960, 0xCAFEBABE)

    def test_mic_ready_and_ack_and_text_round_trip(self):
        assert decode_mic_ready(encode_mic_ready(True)[HEADER_SIZE:]) is True
        assert decode_mic_ready(encode_mic_ready(False)[HEADER_SIZE:]) is False
        assert decode_voice_ack(encode_voice_ack(4096)[HEADER_SIZE:]) == 4096

        ok_raw = encode_voice_text(VOICE_STATUS_OK, "fix the bug in main.c")[HEADER_SIZE:]
        assert decode_voice_text(ok_raw) == (VOICE_STATUS_OK, "fix the bug in main.c")

        for status in (VOICE_STATUS_BUSY, VOICE_STATUS_INTEGRITY,
                       VOICE_STATUS_NO_STT, VOICE_STATUS_EMPTY):
            raw = encode_voice_text(status)[HEADER_SIZE:]
            assert decode_voice_text(raw) == (status, "")


class TestDecoderBounds:
    """Every decoder refuses what its encoder would never produce: a card (or
    anything pretending to be one) is held to the same bounds on the way in."""

    @pytest.mark.parametrize("payload", [
        b"",                               # no format, no length
        b"\x01",                           # no length
        b"\x02\x02en",                     # format 2 does not exist
        b"\x01\x03en",                     # declares 3, carries 2
        b"\x01\x08" + b"a" * 8,            # longer than VOICE_LANG_MAX
        b"\x01\x02e;",                     # not [A-Za-z0-9_-]
        b"\x01\x02\xc3\xa9",               # not ASCII
        b"\x01\x03en\n",                   # "$" would match before the newline
    ])
    def test_voice_start_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_voice_start(payload)

    def test_voice_start_empty_language_is_allowed_and_means_detect(self):
        assert decode_voice_start(b"\x01\x00") == (VOICE_FORMAT_IMA_ADPCM_8K, "")

    @pytest.mark.parametrize("payload", [
        b"\x00\x00\x00\x00",                                   # no data
        b"\x00\x00\x00\x00" + b"x" * (VOICE_CHUNK_MAX + 1),    # chunk too long
        (VOICE_BYTES_MAX - 1).to_bytes(4, "big") + b"xy",      # runs past the end
        (0xFFFFFFFF).to_bytes(4, "big") + b"x",                # offset wraps nothing
    ])
    def test_voice_chunk_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_voice_chunk(payload)

    @pytest.mark.parametrize("payload", [
        b"\x00" * 7,                                           # short
        b"\x00" * 9,                                           # long
        (0).to_bytes(4, "big") + b"\x00" * 4,                  # empty utterance
        (VOICE_BYTES_MAX + 1).to_bytes(4, "big") + b"\x00" * 4,
    ])
    def test_voice_end_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_voice_end(payload)

    @pytest.mark.parametrize("payload", [
        b"", b"\x01\x00", b"\x02",                             # length, reserved bits
    ])
    def test_mic_ready_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_mic_ready(payload)

    def test_voice_ack_refuses_more_than_an_utterance(self):
        with pytest.raises(ProtocolError):
            decode_voice_ack((VOICE_BYTES_MAX + 1).to_bytes(4, "big"))
        with pytest.raises(ProtocolError):
            encode_voice_ack(VOICE_BYTES_MAX + 1)

    @pytest.mark.parametrize("payload", [
        b"\x00\x00",                                   # short
        b"\x00\x00\x00",                               # OK with no text
        b"\x00\x00\x02a\x1b",                          # an escape byte
        b"\x00\x00\x02a\x80",                          # not ASCII
        b"\x00\x00\x05abc",                            # declares 5, carries 3
        b"\x03\x00\x01x",                              # a failure carrying text
        b"\x05\x00\x00",                               # no such status
        b"\x00" + (VOICE_TEXT_MAX + 1).to_bytes(2, "big") + b"a" * (VOICE_TEXT_MAX + 1),
    ])
    def test_voice_text_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_voice_text(payload)

    def test_voice_text_encoder_refuses_what_the_card_would(self):
        with pytest.raises(ProtocolError):
            encode_voice_text(VOICE_STATUS_OK, "")
        with pytest.raises(ProtocolError):
            encode_voice_text(VOICE_STATUS_OK, "café")
        with pytest.raises(ProtocolError):
            encode_voice_text(VOICE_STATUS_OK, "a" * (VOICE_TEXT_MAX + 1))
        with pytest.raises(ProtocolError):
            encode_voice_text(VOICE_STATUS_NO_STT, "why")


class TestVoiceAssembler:
    def test_contiguous_assembly_and_crc(self):
        asm = dsvoice.VoiceAssembler()
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "ja")
        data = b"\x55" * 600
        assert asm.feed(0, data[:480]) == 480
        assert asm.feed(480, data[480:]) == 600
        crc = dscamera.crc32(data)
        assert asm.finish(600, crc) == data

    def test_bad_crc_returns_none(self):
        asm = dsvoice.VoiceAssembler()
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "en")
        asm.feed(0, b"\x11" * 100)
        assert asm.finish(100, 0x12345678) is None

    def test_gap_in_offset_raises(self):
        asm = dsvoice.VoiceAssembler()
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "en")
        asm.feed(0, b"\x11" * 100)
        with pytest.raises(ProtocolError):
            asm.feed(200, b"\x22" * 50)

    def test_offset_zero_restarts_and_the_utterance_is_bounded(self):
        asm = dsvoice.VoiceAssembler()
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "en")
        asm.feed(0, b"\x11" * 100)
        assert asm.feed(0, b"\x22" * 10) == 10      # restart, not append
        off = 10
        while off + VOICE_CHUNK_MAX <= VOICE_BYTES_MAX:
            off = asm.feed(off, b"\x00" * VOICE_CHUNK_MAX)
        with pytest.raises(ProtocolError):
            asm.feed(off, b"\x00" * VOICE_CHUNK_MAX)

    def test_chunk_or_end_without_start_and_a_short_total_raise(self):
        asm = dsvoice.VoiceAssembler()
        with pytest.raises(ProtocolError):
            asm.feed(0, b"x")
        with pytest.raises(ProtocolError):
            asm.finish(1, 0)
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "en")
        asm.feed(0, b"x" * 10)
        with pytest.raises(ProtocolError):
            asm.finish(11, dscamera.crc32(b"x" * 10))
        assert not asm.active

    def test_the_language_kept_is_the_validated_one(self):
        asm = dsvoice.VoiceAssembler()
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "pt-BR")
        assert asm.lang == "pt"
        asm.start(VOICE_FORMAT_IMA_ADPCM_8K, "-rf")
        assert asm.lang is None


class TestLanguageAndText:
    @pytest.mark.parametrize("raw, want", [
        ("en", "en"), ("ES", "es"), ("yue", "yue"), ("en-US", "en"),
        ("pt_BR", "pt"), ("", None), (None, None), ("auto", None),
        ("e", None), ("-l", None), ("e1", None), ("--help", None), ("en\n", None),
    ])
    def test_voice_lang_lets_only_two_or_three_letters_through(self, raw, want):
        assert dsvoice.voice_lang(raw) == want

    def test_fold_transcript_is_printable_ascii_on_one_line(self):
        assert dsvoice.fold_transcript(
            "  café — “done”…\nnext line\r\n ") == \
            'cafe - "done"... next line'
        assert dsvoice.fold_transcript("你好") == ""
        assert dsvoice.fold_transcript("") == ""
        long = dsvoice.fold_transcript("word " * 200)
        assert len(long) == VOICE_TEXT_MAX
        assert all(0x20 <= ord(ch) <= 0x7E for ch in long)


# -- choosing a backend: local first, cloud only on the opt-in ---------------

@pytest.fixture
def nothing_local(monkeypatch):
    monkeypatch.setattr(dsvoice, "_has_library", lambda module, dist: False)
    monkeypatch.setattr(dsvoice.shutil, "which", lambda name: None)


class TestBackendChoice:
    def test_an_api_key_alone_sends_nothing_anywhere(self, nothing_local):
        env = {"OPENAI_API_KEY": "sk-test", "GROQ_API_KEY": "gsk-test",
               "DSTERM_STT_URL": "https://stt.example/v1"}
        backend, where = dsvoice.choose_backend(env, cloud=False)
        assert backend is None
        assert "opt-in" in where

    def test_cloud_with_the_opt_in_names_its_host(self, nothing_local):
        backend, where = dsvoice.choose_backend({"OPENAI_API_KEY": "sk-test"},
                                                cloud=True)
        assert backend is not None and backend.cloud
        assert backend.name == "api.openai.com"
        assert backend.key_var == "OPENAI_API_KEY"
        assert "api.openai.com" in where and "sk-test" not in repr(backend)
        backend, _ = dsvoice.choose_backend({"GROQ_API_KEY": "g"}, cloud=True)
        assert backend.name == "api.groq.com"

    def test_opt_in_without_a_cloud_service_is_off_and_says_why(self, nothing_local):
        backend, where = dsvoice.choose_backend({}, cloud=True)
        assert backend is None and "--stt-cloud" in where

    def test_local_wins_even_with_the_opt_in(self, monkeypatch):
        monkeypatch.setattr(dsvoice, "_has_library",
                            lambda module, dist: module == "faster_whisper")
        backend, where = dsvoice.choose_backend({"OPENAI_API_KEY": "sk"}, cloud=True)
        assert backend.kind == "library" and backend.name == "faster-whisper"
        assert "this computer" in where

    def test_a_library_that_needs_ffmpeg_is_skipped_without_it(self, monkeypatch):
        monkeypatch.setattr(dsvoice, "_has_library", lambda module, dist: True)
        monkeypatch.setattr(dsvoice.shutil, "which", lambda name: None)
        # mlx-whisper and openai-whisper run ffmpeg; faster-whisper does not.
        assert dsvoice.find_local_backend({}).target == "faster_whisper"
        monkeypatch.setattr(dsvoice, "_has_library",
                            lambda module, dist: module != "faster_whisper")
        assert dsvoice.find_local_backend({}) is None

    def test_order_is_cmd_then_libraries_then_binaries(self, monkeypatch):
        monkeypatch.setattr(dsvoice, "_has_library", lambda module, dist: True)
        monkeypatch.setattr(dsvoice.shutil, "which", lambda name: f"/bin/{name}")
        assert dsvoice.find_local_backend({"DSTERM_STT_CMD": "/opt/stt"}).kind == "cmd"
        assert dsvoice.find_local_backend({}).target == "mlx_whisper"
        monkeypatch.setattr(dsvoice, "_has_library",
                            lambda module, dist: module == "whisper")
        assert dsvoice.find_local_backend({}).name == "openai-whisper"
        monkeypatch.setattr(dsvoice, "_has_library", lambda module, dist: False)
        assert dsvoice.find_local_backend({}).target == "/bin/whisper-cli"

    def test_a_whisper_module_that_is_not_openai_whisper_is_not_a_backend(
            self, monkeypatch):
        import importlib.metadata
        import importlib.util

        monkeypatch.setattr(importlib.util, "find_spec",
                            lambda name: object() if name == "whisper" else None)

        def no_dist(name):
            raise importlib.metadata.PackageNotFoundError(name)

        monkeypatch.setattr(importlib.metadata, "distribution", no_dist)
        monkeypatch.setattr(dsvoice.shutil, "which", lambda name: None)
        assert dsvoice.find_local_backend({}) is None

    def test_a_custom_url_must_be_https_and_never_gets_the_openai_key(
            self, nothing_local):
        backend, where = dsvoice.choose_backend(
            {"DSTERM_STT_URL": "http://stt.example/v1", "OPENAI_API_KEY": "sk"},
            cloud=True)
        assert backend is None and "https" in where
        backend, _ = dsvoice.choose_backend(
            {"DSTERM_STT_URL": "https://stt.example/v1", "OPENAI_API_KEY": "sk"},
            cloud=True)
        assert backend.name == "stt.example"
        assert backend.key_var == "DSTERM_STT_KEY"

    def test_the_opt_in_is_the_flag_or_the_servers_own_environment(self):
        assert dsvoice.cloud_opted_in(True, {}) is True
        assert dsvoice.cloud_opted_in(False, {"DSTERM_STT_CLOUD": "1"}) is True
        assert dsvoice.cloud_opted_in(False, {"DSTERM_STT_CLOUD": "0"}) is False
        assert dsvoice.cloud_opted_in(False, {"OPENAI_API_KEY": "sk"}) is False

    def test_the_timeout_is_bounded(self):
        assert dsvoice.stt_timeout({}) == dsvoice.DEFAULT_TIMEOUT_S
        assert dsvoice.stt_timeout({"DSTERM_STT_TIMEOUT": "1"}) == 5.0
        # Never past the card's own 20 s wait: the watchdog must answer first.
        assert dsvoice.stt_timeout({"DSTERM_STT_TIMEOUT": "9999"}) == dsvoice.MAX_TIMEOUT_S
        assert dsvoice.MAX_TIMEOUT_S + dsvoice.WATCHDOG_MARGIN_S < 20
        assert dsvoice.stt_timeout({"DSTERM_STT_TIMEOUT": "nan"}) == dsvoice.DEFAULT_TIMEOUT_S
        assert dsvoice.stt_timeout({"DSTERM_STT_TIMEOUT": "soon"}) == dsvoice.DEFAULT_TIMEOUT_S


class _Opened:
    """What urllib's opener returned: a body, read in pieces."""

    def __init__(self, body: bytes) -> None:
        self._body = io.BytesIO(body)

    def read(self, n: int = -1) -> bytes:
        return self._body.read(n)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class TestCloudRequest:
    def test_a_custom_url_is_sent_its_own_key_and_a_valid_language(self, monkeypatch):
        seen = []

        def fake_open(req, timeout):
            seen.append(req)
            return _Opened(json.dumps({"text": "  hola  "}).encode())

        monkeypatch.setattr(dsvoice._OPENER, "open", fake_open)
        backend = dsvoice.Backend("cloud", "stt.example", "https://stt.example/v1",
                                  "DSTERM_STT_KEY", "whisper-1")
        env = {"DSTERM_STT_KEY": "own-key", "OPENAI_API_KEY": "sk-not-for-you"}
        t = dsvoice.Transcriber(backend, env, timeout=5)
        assert t.transcribe(b"RIFFwav", "es-ES") == (VOICE_STATUS_OK, "hola")
        req = seen[0]
        assert req.full_url == "https://stt.example/v1"
        assert req.get_header("Authorization") == "Bearer own-key"
        body = req.data
        assert b"sk-not-for-you" not in body
        assert b'name="language"\r\n\r\nes\r\n' in body

    def test_an_invalid_language_is_left_out_not_sent(self, monkeypatch):
        seen = []
        monkeypatch.setattr(dsvoice._OPENER, "open",
                            lambda req, timeout: seen.append(req) or _Opened(b'{"text":"x"}'))
        backend = dsvoice.Backend("cloud", "api.openai.com", dsvoice.OPENAI_URL,
                                  "OPENAI_API_KEY", "whisper-1")
        dsvoice.Transcriber(backend, {"OPENAI_API_KEY": "k"}, timeout=5).transcribe(
            b"RIFF", "x1")
        assert b'name="language"' not in seen[0].data

    def test_a_redirect_is_refused_not_followed(self):
        handler = dsvoice._NoRedirect()
        assert handler.redirect_request(None, None, 302, "Found", {},
                                        "http://elsewhere.example/") is None

    def test_a_failed_upload_is_no_stt_and_logged(self, monkeypatch, caplog):
        import urllib.error

        def refuse(req, timeout):
            raise urllib.error.URLError("down")

        monkeypatch.setattr(dsvoice._OPENER, "open", refuse)
        backend = dsvoice.Backend("cloud", "api.openai.com", dsvoice.OPENAI_URL,
                                  "OPENAI_API_KEY", "whisper-1")
        with caplog.at_level("WARNING", logger="dsterm.server"):
            got = dsvoice.Transcriber(backend, {"OPENAI_API_KEY": "secret-k"},
                                      timeout=5).transcribe(b"RIFF", "en")
        assert got == (VOICE_STATUS_NO_STT, "")
        assert "api.openai.com" in caplog.text and "secret-k" not in caplog.text


# -- local backends: argv, deadlines, the worker -----------------------------

def _script(tmp_path, name: str, body: str) -> str:
    path = tmp_path / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)
    return str(path)


class TestDoctorSeesWhatServeSees:
    """`dsterm doctor` decides the voice line from the environment `dsterm
    serve` would: this one with the env file merged over it."""

    def test_the_env_file_is_merged_over_the_environment(self, tmp_path, monkeypatch):
        from dsterm import doctor

        monkeypatch.setenv("DSTERM_STT_MODEL", "inherited")
        env_file = tmp_path / "env"
        env_file.write_text("DSTERM_STT_CMD=/opt/stt/run\nHF_HOME=/models\n")
        env_file.chmod(0o600)
        env, problem = doctor.serve_env(env_file)
        assert problem is None
        assert env["DSTERM_STT_CMD"] == "/opt/stt/run"
        assert env["DSTERM_STT_MODEL"] == "inherited"
        env_file.chmod(0o644)
        env, problem = doctor.serve_env(env_file)
        assert "DSTERM_STT_CMD" not in env and "chmod 600" in problem
        assert doctor.serve_env(tmp_path / "absent")[1] is None

    def test_a_command_in_the_env_file_is_the_backend(self, nothing_local):
        from dsterm import doctor

        level, detail = doctor.voice_check({"DSTERM_STT_CMD": "/opt/stt/run"})
        assert level == doctor.OK
        assert "transcribed on this computer" in detail and "run" in detail

    def test_no_backend_says_why_and_how(self, nothing_local, monkeypatch):
        from dsterm import doctor

        monkeypatch.setenv("DSTERM_STT_CLOUD", "1")
        level, detail = doctor.voice_check({})
        assert level == doctor.OK
        assert "--stt-cloud is on, but" in detail
        assert "pipx inject ds-claude-terminal faster-whisper" in detail
        monkeypatch.delenv("DSTERM_STT_CLOUD")
        assert "opt-in" in doctor.voice_check({})[1]

    def test_cloud_where_says_opted_in(self, nothing_local):
        backend, where = dsvoice.choose_backend({"GROQ_API_KEY": "k"}, cloud=True)
        assert where == "sent to api.groq.com (cloud, opted in)"


class TestLocalBackends:
    def test_a_failing_local_backend_never_falls_back_to_the_cloud(
            self, tmp_path, monkeypatch, caplog):
        def no_cloud(req, timeout):
            raise AssertionError("audio was uploaded")

        monkeypatch.setattr(dsvoice._OPENER, "open", no_cloud)
        cmd = _script(tmp_path, "stt", "exit 3\n")
        env = {"DSTERM_STT_CMD": cmd, "OPENAI_API_KEY": "sk"}
        backend, _ = dsvoice.choose_backend(env, cloud=True)
        assert backend.kind == "cmd"
        with caplog.at_level("WARNING", logger="dsterm.server"):
            assert dsvoice.Transcriber(backend, env, timeout=5).transcribe(
                b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
        assert "exited with 3" in caplog.text

    def test_the_command_gets_argv_and_the_temporary_file_is_removed(self, tmp_path):
        log = tmp_path / "argv"
        cmd = _script(tmp_path, "stt",
                      f'printf "%s\\n" "$@" > "{log}"\n'
                      f'stat -f %Lp "$1" >> "{log}" 2>/dev/null || stat -c %a "$1" >> "{log}"\n'
                      'echo "  heard you  "\n')
        backend = dsvoice.Backend("cmd", "stt", cmd)
        t = dsvoice.Transcriber(backend, {}, timeout=10)
        assert t.transcribe(b"RIFFdata", "es") == (VOICE_STATUS_OK, "heard you")
        wav, lang, mode = log.read_text().split()
        assert lang == "es" and mode == "600"
        assert not os.path.exists(wav)
        t.transcribe(b"RIFFdata", "x;y")
        assert log.read_text().split()[1] == "auto"

    def test_a_hung_command_is_stopped_at_the_deadline_and_busy_clears(self, tmp_path):
        cmd = _script(tmp_path, "stt", "sleep 30\n")
        t = dsvoice.Transcriber(dsvoice.Backend("cmd", "stt", cmd), {}, timeout=0.5)
        start = time.monotonic()
        assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
        assert time.monotonic() - start < 5
        # Not BUSY: the lock went with the failure.
        assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")

    def test_a_second_utterance_while_one_runs_is_busy(self, tmp_path):
        cmd = _script(tmp_path, "stt", "sleep 1; echo done\n")
        t = dsvoice.Transcriber(dsvoice.Backend("cmd", "stt", cmd), {}, timeout=10)
        results = []
        th = threading.Thread(target=lambda: results.append(t.transcribe(b"R", "en")))
        th.start()
        time.sleep(0.3)
        assert t.transcribe(b"R", "en") == (VOICE_STATUS_BUSY, "")
        th.join(10)
        assert results == [(VOICE_STATUS_OK, "done")]

    def test_warm_up_loads_the_model_and_holds_utterances_off_meanwhile(self, tmp_path):
        # A worker that takes a moment to answer, as a model load does.
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, sys, time\n"
            "for line in sys.stdin:\n"
            "    time.sleep(0.5)\n"
            "    print(json.dumps({'ok': True, 'text': 'x'}), flush=True)\n")
        backend = dsvoice.Backend("library", "fake", "faster_whisper")
        t = dsvoice.Transcriber(backend, {}, timeout=10,
                                worker_argv=[sys.executable, str(fake)])
        try:
            t.warm_up()
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_BUSY, "")
            deadline = time.monotonic() + 10
            while t._lock.locked() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_OK, "x")
        finally:
            t.close()

    def test_the_library_worker_keeps_its_process_between_utterances(self, tmp_path):
        # A stand-in for stt_worker.py: same protocol, reports its own pid,
        # so "the model stays loaded" is "the process is the same one".
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, os, sys\n"
            "for line in sys.stdin:\n"
            "    req = json.loads(line)\n"
            "    print(json.dumps({'ok': True, 'text': f\"{os.getpid()} {req['lang']}\"}),"
            " flush=True)\n")
        backend = dsvoice.Backend("library", "fake", "faster_whisper")
        t = dsvoice.Transcriber(backend, {}, timeout=10,
                                worker_argv=[sys.executable, str(fake)])
        try:
            s1, first = t.transcribe(b"RIFF", "fr")
            s2, second = t.transcribe(b"RIFF", "zz-top")
            assert s1 == s2 == VOICE_STATUS_OK
            assert first.split()[1] == "fr"
            assert first.split()[0] == second.split()[0]
            assert second.split()[1] == "zz"
        finally:
            t.close()

    def test_a_hung_library_worker_is_killed_and_the_next_one_starts_fresh(
            self, tmp_path, monkeypatch):
        monkeypatch.setattr(dsvoice, "LOAD_TIMEOUT_S", 0.5)
        flag = tmp_path / "hang-once"
        flag.write_text("")
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, os, sys, time\n"
            f"flag = {str(flag)!r}\n"
            "for line in sys.stdin:\n"
            "    if os.path.exists(flag):\n"
            "        os.unlink(flag)\n"
            "        time.sleep(60)\n"
            "    print(json.dumps({'ok': True, 'text': 'back'}), flush=True)\n")
        t = dsvoice.Transcriber(dsvoice.Backend("library", "fake", "whisper"), {},
                                timeout=0.5, worker_argv=[sys.executable, str(fake)])
        try:
            start = time.monotonic()
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
            assert time.monotonic() - start < 5
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_OK, "back")
        finally:
            t.close()

    def test_a_library_error_is_reported_not_retried_elsewhere(self, tmp_path):
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, sys\n"
            "for line in sys.stdin:\n"
            "    print(json.dumps({'ok': False, 'error': 'no ffmpeg'}), flush=True)\n")
        t = dsvoice.Transcriber(dsvoice.Backend("library", "fake", "whisper"), {},
                                timeout=5, worker_argv=[sys.executable, str(fake)])
        try:
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
        finally:
            t.close()

    def test_an_error_answer_still_ends_the_model_load_deadline(
            self, tmp_path, monkeypatch):
        # One ok:false (no speech, say) is still an answer from a loaded
        # model. The next utterance must be held to the per-utterance
        # deadline, not LOAD_TIMEOUT_S: here the second request hangs, and
        # must be given up on in ~0.5 s, not the 60 s load deadline.
        monkeypatch.setattr(dsvoice, "LOAD_TIMEOUT_S", 60.0)
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, sys, time\n"
            "n = 0\n"
            "for line in sys.stdin:\n"
            "    n += 1\n"
            "    if n > 1:\n"
            "        time.sleep(60)\n"
            "    print(json.dumps({'ok': False, 'error': 'no speech'}), flush=True)\n")
        t = dsvoice.Transcriber(dsvoice.Backend("library", "fake", "whisper"), {},
                                timeout=0.5, worker_argv=[sys.executable, str(fake)])
        try:
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
            assert t._worker._warm
            start = time.monotonic()
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_NO_STT, "")
            assert time.monotonic() - start < 5
        finally:
            t.close()

    def test_closing_the_worker_mid_read_is_an_stt_error(self, tmp_path):
        # Ctrl-C while the warm-up thread waits on a model load: close()
        # pulls the pipe out from under the read. That is "the worker
        # exited", not an OSError traceback from the thread.
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import sys, time\n"
            "for line in sys.stdin:\n"
            "    time.sleep(60)\n")
        worker = dsvoice.LibraryWorker("whisper", None,
                                       [sys.executable, str(fake)], env={})
        got: list[BaseException] = []

        def run():
            try:
                worker.transcribe("/nonexistent.wav", None, 30)
            except BaseException as exc:    # noqa: BLE001 - recorded, asserted
                got.append(exc)

        th = threading.Thread(target=run)
        th.start()
        time.sleep(0.5)
        worker.close()
        th.join(10)
        assert not th.is_alive()
        assert len(got) == 1 and isinstance(got[0], dsvoice.SttError), got
        assert "exited" in str(got[0])

    def test_the_worker_gets_the_transcribers_environment(self, tmp_path):
        # HF_HOME / HF_TOKEN in the env file must reach the library.
        fake = tmp_path / "worker.py"
        fake.write_text(
            "import json, os, sys\n"
            "for line in sys.stdin:\n"
            "    print(json.dumps({'ok': True,"
            " 'text': os.environ.get('HF_HOME', 'unset')}), flush=True)\n")
        t = dsvoice.Transcriber(dsvoice.Backend("library", "fake", "whisper"),
                                {"HF_HOME": "/models-here"}, timeout=5,
                                worker_argv=[sys.executable, str(fake)])
        try:
            assert t.transcribe(b"RIFF", "en") == (VOICE_STATUS_OK, "/models-here")
        finally:
            t.close()

    def test_a_transcript_with_no_ascii_is_empty_and_says_why(
            self, tmp_path, caplog):
        cmd = _script(tmp_path, "stt", "echo '你好世界です'\n")
        t = dsvoice.Transcriber(dsvoice.Backend("cmd", "stt", cmd), {}, timeout=10)
        with caplog.at_level("INFO", logger="dsterm.server"):
            assert t.transcribe(b"RIFF", "zh") == (VOICE_STATUS_EMPTY, "")
        assert "no ASCII text (6 characters dropped)" in caplog.text
        assert "你好" not in caplog.text
        caplog.clear()
        silent = dsvoice.Transcriber(
            dsvoice.Backend("cmd", "stt", _script(tmp_path, "quiet", "echo\n")),
            {}, timeout=10)
        with caplog.at_level("INFO", logger="dsterm.server"):
            assert silent.transcribe(b"RIFF", "en") == (VOICE_STATUS_EMPTY, "")
        assert "no ASCII" not in caplog.text

    def test_the_real_worker_refuses_a_bad_request_and_an_unknown_library(self):
        from dsterm import stt_worker

        assert stt_worker.main(["graphite", ""]) == 2
        with pytest.raises(ValueError):
            stt_worker._request(json.dumps({"wav": "/tmp/x.wav", "lang": "-rf"}))
        with pytest.raises(ValueError):
            stt_worker._request(json.dumps({"lang": "en"}))
        assert stt_worker._request(json.dumps({"wav": "/w", "lang": None})) == ("/w", None)


# -- the server, end to end --------------------------------------------------

KEY = b"\x42" * 16


class Card:
    """A logged-in card on a real socket, reading every frame through SEAL."""

    def __init__(self, srv: Server, features: tuple[str, ...]) -> None:
        self.sock = socket.create_connection(("127.0.0.1", srv.bound_port), timeout=3.0)
        self.reader = FrameReader()
        self.seal = None
        self.frames: list = []
        hello = encode_hello((FEATURE_SEAL3, *features, "cn=" + "11" * 16))
        self.sock.sendall(hello)
        nonce = None
        while nonce is None:
            for m in self.reader.feed(self.sock.recv(4096)):
                if m.type is MsgType.NONCE:
                    nonce = m.payload
        self.sock.sendall(encode(MsgType.AUTH, dsauth.compute_mac(KEY, nonce)))
        deadline = time.monotonic() + 3.0
        while self.seal is None and time.monotonic() < deadline:
            for m in self.reader.feed(self.sock.recv(4096)):
                if m.type is MsgType.AUTH_OK:
                    self.seal = Seal.for_card(seal_key(KEY, nonce, hello[HEADER_SIZE:]))
                    self.reader.start_sealing(self.seal)
                else:
                    self.frames.append(m)
        assert self.seal is not None

    def send(self, frame: bytes) -> None:
        self.sock.sendall(self.seal.seal(frame))

    def read_for(self, seconds: float) -> list:
        """Every frame that arrives within `seconds`; [] plus closed=True on EOF."""
        out = []
        end = time.monotonic() + seconds
        self.closed = False
        while time.monotonic() < end:
            self.sock.settimeout(max(0.01, end - time.monotonic()))
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                break
            except OSError:
                self.closed = True
                break
            if not data:
                self.closed = True
                break
            out.extend(self.reader.feed(data))
        self.frames.extend(out)
        return out

    def wait_for(self, mtype: MsgType, seconds: float = 3.0):
        end = time.monotonic() + seconds
        for m in self.frames:
            if m.type is mtype:
                self.frames.remove(m)
                return m
        while time.monotonic() < end:
            for m in self.read_for(0.1):
                if m.type is mtype:
                    self.frames.remove(m)
                    return m
            if getattr(self, "closed", False):
                return None
        return None

    def utterance(self, lang: str = "es") -> None:
        adpcm = dsvoice.adpcm_encode([500, -500] * 100)
        self.send(encode_voice_start(VOICE_FORMAT_IMA_ADPCM_8K, lang))
        self.send(encode_voice_chunk(0, adpcm))
        self.send(encode_voice_end(len(adpcm), dscamera.crc32(adpcm)))

    def close(self) -> None:
        self.sock.close()


def _serve(**kwargs):
    srv = Server(["sh", "-c", "sleep 5"], KEY, host="127.0.0.1", port=0, **kwargs)
    srv.__enter__()
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class TestServerVoiceEndToEnd:
    def test_voice_utterance_transcribes_and_returns_voice_text(self):
        seen_langs: list = []

        def fake_transcriber(wav_bytes: bytes, lang):
            assert wav_bytes.startswith(b"RIFF")
            seen_langs.append(lang)
            return VOICE_STATUS_OK, "hola mundo desde el microfono"

        srv = _serve(voice_enabled=True, voice_transcriber=fake_transcriber)
        card = Card(srv, (FEATURE_MIC,))
        try:
            ready = card.wait_for(MsgType.MIC_READY)
            assert ready is not None and decode_mic_ready(ready.payload) is True
            card.utterance("es")
            ack = card.wait_for(MsgType.VOICE_ACK)
            text = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_ack(ack.payload) == 100   # 200 samples, two a byte
            assert decode_voice_text(text.payload) == (
                VOICE_STATUS_OK, "hola mundo desde el microfono")
            assert seen_langs == ["es"]
        finally:
            card.close()
            srv.stop()

    def test_a_card_without_mic_gets_no_mic_ready_and_voice_from_it_drops_it(self):
        srv = _serve(voice_transcriber=lambda w, l: (VOICE_STATUS_OK, "x"))
        card = Card(srv, ())
        try:
            card.read_for(0.5)
            assert not [m for m in card.frames if m.type is MsgType.MIC_READY]
            card.send(encode_voice_start(VOICE_FORMAT_IMA_ADPCM_8K, "en"))
            card.read_for(1.0)
            assert card.closed
        finally:
            card.close()
            srv.stop()

    def test_under_no_mic_a_mic_card_is_never_offered_and_is_dropped(self):
        called = []
        srv = _serve(voice_enabled=False,
                     voice_transcriber=lambda w, l: called.append(1) or (VOICE_STATUS_OK, "x"))
        assert srv.voice_where == "off (--no-mic)"
        card = Card(srv, (FEATURE_MIC,))
        try:
            card.read_for(0.5)
            assert not [m for m in card.frames if m.type is MsgType.MIC_READY]
            # The first voice frame is the end of the connection: the rest
            # of an utterance would meet a closed socket.
            card.send(encode_voice_start(VOICE_FORMAT_IMA_ADPCM_8K, "en"))
            card.read_for(1.0)
            assert card.closed and not called
        finally:
            card.close()
            srv.stop()

    def test_no_backend_and_no_opt_in_says_so_and_uploads_nothing(
            self, monkeypatch, nothing_local):
        def no_cloud(req, timeout):
            raise AssertionError("audio was uploaded")

        monkeypatch.setattr(dsvoice._OPENER, "open", no_cloud)
        env = dict(os.environ, OPENAI_API_KEY="sk-test")
        env.pop("DSTERM_STT_CMD", None)
        srv = _serve(env=env)
        assert "opt-in" in srv.voice_where
        card = Card(srv, (FEATURE_MIC,))
        try:
            ready = card.wait_for(MsgType.MIC_READY)
            assert decode_mic_ready(ready.payload) is False
            card.utterance()
            text = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_text(text.payload) == (VOICE_STATUS_NO_STT, "")
        finally:
            card.close()
            srv.stop()

    def test_a_second_utterance_while_this_cards_first_runs_is_busy(self):
        release = threading.Event()

        def slow(wav, lang):
            release.wait(5)
            return VOICE_STATUS_OK, "first"

        srv = _serve(voice_transcriber=slow)
        card = Card(srv, (FEATURE_MIC,))
        try:
            card.wait_for(MsgType.MIC_READY)
            card.utterance()
            card.utterance()
            busy = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_text(busy.payload) == (VOICE_STATUS_BUSY, "")
            release.set()
            done = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_text(done.payload) == (VOICE_STATUS_OK, "first")
        finally:
            release.set()
            card.close()
            srv.stop()

    def test_a_hung_transcription_is_abandoned_and_its_late_result_dropped(self):
        release = threading.Event()
        calls = []

        def hangs_once(wav, lang):
            calls.append(1)
            if len(calls) == 1:
                release.wait(10)
                return VOICE_STATUS_OK, "too late"
            return VOICE_STATUS_OK, "second"

        srv = _serve(voice_transcriber=hangs_once)
        srv._voice_deadline_s = 0.5
        card = Card(srv, (FEATURE_MIC,))
        try:
            card.wait_for(MsgType.MIC_READY)
            card.utterance()
            gave_up = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_text(gave_up.payload) == (VOICE_STATUS_NO_STT, "")
            # Busy is clear: the next utterance is transcribed, not BUSY.
            card.utterance()
            nxt = card.wait_for(MsgType.VOICE_TEXT)
            assert decode_voice_text(nxt.payload) == (VOICE_STATUS_OK, "second")
            # And the abandoned one, finishing now, never reaches the composer.
            release.set()
            late = [m for m in card.read_for(0.6) if m.type is MsgType.VOICE_TEXT]
            assert late == []
        finally:
            release.set()
            card.close()
            srv.stop()

    def test_a_result_for_a_card_that_left_is_not_delivered_to_the_next(self):
        release = threading.Event()
        started = threading.Event()

        def slow(wav, lang):
            started.set()
            release.wait(5)
            return VOICE_STATUS_OK, "for the first card"

        srv = _serve(voice_transcriber=slow)
        first = Card(srv, (FEATURE_MIC,))
        try:
            first.wait_for(MsgType.MIC_READY)
            first.utterance()
            assert started.wait(3)
            first.close()
            deadline = time.monotonic() + 3
            while srv._clients and time.monotonic() < deadline:
                time.sleep(0.02)
            second = Card(srv, (FEATURE_MIC,))
            try:
                second.wait_for(MsgType.MIC_READY)
                release.set()
                texts = [m for m in second.read_for(0.8) if m.type is MsgType.VOICE_TEXT]
                assert texts == []
            finally:
                second.close()
        finally:
            release.set()
            srv.stop()


class TestWhisperCpp:
    def test_the_binary_gets_16k_audio_its_model_and_no_shell(self, tmp_path, monkeypatch):
        model = tmp_path / "ggml-base.bin"
        model.write_bytes(b"model")
        seen = tmp_path / "seen"
        cli = _script(tmp_path, "whisper-cli",
                      f'printf "%s\\n" "$@" > "{seen}"\n'
                      'while [ "$1" != "-f" ]; do shift; done\n'
                      f'cp "$2" "{tmp_path}/copy.wav"\n'
                      'echo " it works"\n')
        monkeypatch.setattr(dsvoice, "_has_library", lambda module, dist: False)
        monkeypatch.setattr(dsvoice.shutil, "which",
                            lambda name: cli if name == "whisper-cli" else None)
        backend = dsvoice.find_local_backend({"DSTERM_STT_MODEL": str(model)})
        assert backend.kind == "binary" and backend.model == str(model)
        wav = dsvoice.adpcm_to_wav(dsvoice.adpcm_encode([0, 1000] * 40))
        t = dsvoice.Transcriber(backend, {}, timeout=10)
        assert t.transcribe(wav, "de") == (VOICE_STATUS_OK, "it works")
        argv = seen.read_text().split("\n")
        assert argv[:2] == ["-m", str(model)] and argv[4:7] == ["-l", "de", "-nt"]
        with wave.open(str(tmp_path / "copy.wav"), "rb") as wf:
            assert wf.getframerate() == 16000 and wf.getnframes() == 160
