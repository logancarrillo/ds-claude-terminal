"""The speech-to-text worker: one Python library, its model loaded once.

Started by ``voice.LibraryWorker`` as ``python -P -m dsterm.stt_worker MODULE
MODEL``, never by anything the card says. MODULE is one of three names fixed
below; MODEL is ``DSTERM_STT_MODEL`` as voice.py validated it, or empty for
the library's default.

The protocol is one JSON object a line. In, on stdin: ``{"wav": path,
"lang": code or null}`` -- a temporary file the server wrote and will remove,
and a language code already held to ``^[a-z]{2,3}$``. Out, on the real
stdout: ``{"ok": true, "text": ...}`` or ``{"ok": false, "error": ...}``.
Anything a library prints itself (progress bars, a first run's download)
goes to stderr, which the server discards, so it can never be mistaken for an
answer.

The worker never decides anything: it does not time itself out, retry, or
try another library. The server kills it at the deadline and starts a fresh
one for the next utterance.
"""

from __future__ import annotations

import json
import os
import re
import sys

_LIBRARIES = ("mlx_whisper", "faster_whisper", "whisper")
# fullmatch, not match with "$": "$" also matches before a final newline.
# A deliberate copy of voice._LANG_RE, not an import of it: this worker runs
# in its own process and imports nothing from the server, and a request that
# reaches it is checked again here rather than trusted.
_LANG_RE = re.compile(r"[a-z]{2,3}")


class _Engine:
    """The library, imported on the first utterance and its model kept."""

    def __init__(self, module: str, model: str | None) -> None:
        self.module = module
        self.model_name = model
        self._model = None

    def transcribe(self, wav: str, lang: str | None) -> str:
        if self.module == "mlx_whisper":
            import mlx_whisper  # type: ignore[import-not-found]
            # mlx-whisper keeps the loaded model itself (its ModelHolder), so
            # there is nothing to hold here.
            kw: dict = {"language": lang}
            if self.model_name:
                kw["path_or_hf_repo"] = self.model_name
            return str(mlx_whisper.transcribe(wav, **kw).get("text", "")).strip()

        if self.module == "faster_whisper":
            if self._model is None:
                from faster_whisper import WhisperModel  # type: ignore[import-not-found]
                self._model = WhisperModel(self.model_name or "base",
                                           device="cpu", compute_type="int8")
            segments, _ = self._model.transcribe(wav, language=lang)
            return " ".join(s.text.strip() for s in segments).strip()

        if self._model is None:
            import whisper  # type: ignore[import-not-found]
            # voice.py chose this only with the openai-whisper distribution
            # installed; this is the same check at the point of use, in case
            # another package's "whisper" module shadows it.
            if not callable(getattr(whisper, "load_model", None)):
                raise RuntimeError("the 'whisper' module here is not openai-whisper")
            model = whisper.load_model(self.model_name or "base")
            if not callable(getattr(model, "transcribe", None)):
                raise RuntimeError("the 'whisper' model has no transcribe()")
            self._model = model
        res = self._model.transcribe(wav, language=lang, fp16=False)
        return str(res.get("text", "")).strip()


def _request(line: str) -> tuple[str, str | None]:
    req = json.loads(line)
    if not isinstance(req, dict):
        raise ValueError("request is not an object")
    wav, lang = req.get("wav"), req.get("lang")
    if not isinstance(wav, str) or not wav:
        raise ValueError("request has no wav path")
    if lang is not None and not (isinstance(lang, str) and _LANG_RE.fullmatch(lang)):
        raise ValueError("request language is not a 2-3 letter code")
    return wav, lang


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 2 or args[0] not in _LIBRARIES:
        print(f"usage: stt_worker {{{'|'.join(_LIBRARIES)}}} MODEL", file=sys.stderr)
        return 2
    # The answers' channel is the real stdout, kept on a duplicate; fd 1 and
    # sys.stdout then point at stderr, so a library's own printing cannot
    # interleave with an answer.
    out = os.fdopen(os.dup(1), "w", encoding="utf-8")
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    engine = _Engine(args[0], args[1] or None)
    for line in sys.stdin:
        try:
            wav, lang = _request(line)
            reply = {"ok": True, "text": engine.transcribe(wav, lang)}
        except Exception as exc:  # noqa: BLE001 - reported, and the server decides
            reply = {"ok": False, "error": f"{type(exc).__name__}: {exc}"[:200]}
        out.write(json.dumps(reply) + "\n")
        out.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
