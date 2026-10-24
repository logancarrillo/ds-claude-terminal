#!/usr/bin/env python3
"""Generate dsclient/source/ui_font.h: the UTF-8 UI glyph atlas.

The terminal's PTY cell grid stays 1-byte fast (ASCII 0x20..0x7E + extended
box/sparkline glyphs 0x80..0xBF).  The handheld's own UI chrome (Control tiles,
headers, hints, Settings, Composer, and imported .lang files) draws UTF-8 strings
through this sorted codepoint table:

  - 6x8 narrow glyphs for Latin-1 punctuation (the HAND_DRAWN set below, and
    any non-blank glyph tools/spleen-5x8.bdf has for a character lang.c uses),
    padded to UI_FONT_H with two blank rows. Accented Latin-1 LETTERS are not
    in the table: the card composes those at draw time (lang_latin_glyph).
  - 12x10 (2-cell wide, 10px tall) pixel glyphs for Chinese Hanzi and Japanese
    Kana/Kanji used in dsclient/source/lang.c, taken directly from
    tools/fusion-pixel-10px-subset.bdf (Fusion Pixel Font 10px Monospaced, SIL OFL).

The atlas holds only the characters the built-in strings in lang.c use (plus
the hand-drawn punctuation). An imported .lang file can use any accented
Latin-1 letter, but any other character it has that lang.c does not -- a
Hanzi the built-in Chinese never uses, Cyrillic, Greek -- draws as the
renderer's placeholder box.
"""

from __future__ import annotations

import sys
from pathlib import Path

# genfont.py sits beside this script; its own directory on the import path
# makes `python3 tools/genuifont.py` work from any working directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from genfont import parse_bdf  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
BDF_PATH = REPO / "tools" / "spleen-5x8.bdf"
CJK_BDF_PATH = REPO / "tools" / "fusion-pixel-10px-subset.bdf"
LANG_C = REPO / "dsclient" / "source" / "lang.c"
OUT_H = REPO / "dsclient" / "source" / "ui_font.h"

UI_FONT_H = 10
WIDE_W = 12


def parse_cjk_bdf(path: Path) -> dict[int, list[int]]:
    """Parse a 10px BDF font into 12-bit row masks (bit 11 leftmost, 10 rows)."""
    glyphs: dict[int, list[int]] = {}
    ascent = 9
    descent = 1
    cp: int | None = None
    bbx: list[int] | None = None
    in_bmp = False
    raw_rows: list[str] = []

    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("FONT_ASCENT "):
            ascent = int(s.split()[1])
        elif s.startswith("FONT_DESCENT "):
            descent = int(s.split()[1])
        elif s.startswith("ENCODING "):
            cp = int(s.split()[1])
        elif cp is not None and s.startswith("BBX "):
            bbx = [int(x) for x in s.split()[1:]]
        elif cp is not None and s == "BITMAP":
            in_bmp = True
            raw_rows = []
        elif in_bmp and s == "ENDCHAR":
            in_bmp = False
            assert bbx is not None
            w, h, xoff, yoff = bbx
            cell_h = ascent + descent
            rows = [0] * UI_FONT_H
            # Center the 10px glyph horizontally inside the 12px cell (+1px left margin).
            x_pad = 1
            for r_idx, hex_str in enumerate(raw_rows):
                val = int(hex_str, 16)
                pad = ((w + 7) // 8) * 8 - w
                val >>= pad
                y = ascent - yoff - h + r_idx
                if 0 <= y < min(cell_h, UI_FONT_H):
                    bits = 0
                    for bit in range(w):
                        if (val >> (w - 1 - bit)) & 1:
                            px = xoff + bit + x_pad
                            if 0 <= px < WIDE_W:
                                bits |= 1 << (WIDE_W - 1 - px)
                    rows[y] = bits
            glyphs[cp] = rows
            cp = None
            bbx = None
        elif in_bmp:
            raw_rows.append(s)
    return glyphs


# Latin-1 punctuation a European .lang pack reaches for, drawn here in the
# terminal font's 6x8 style: tools/spleen-5x8.bdf is a trimmed copy whose
# Latin-1 glyphs are all blank, and a blank glyph is worse than none -- the
# renderer draws a visible placeholder for a missing one. Accented LETTERS are
# not here: the card builds those from the ASCII letter plus a mark at draw
# time (lang_latin_glyph, dsclient/source/lang.c), for built-in strings and
# custom packs alike.
HAND_DRAWN = {
    0x00A1: [0x00, 0x08, 0x00, 0x08, 0x08, 0x08, 0x08, 0x00],  # ¡
    0x00BF: [0x00, 0x08, 0x00, 0x08, 0x10, 0x22, 0x1C, 0x00],  # ¿
    0x00AB: [0x00, 0x00, 0x12, 0x24, 0x12, 0x00, 0x00, 0x00],  # «
    0x00BB: [0x00, 0x00, 0x24, 0x12, 0x24, 0x00, 0x00, 0x00],  # »
    0x00B0: [0x0C, 0x12, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00],  # °
    0x00B7: [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x00],  # ·
}


def collect_lang_codepoints() -> set[int]:
    """Every character the built-in strings use, plus the hand-drawn set."""
    text = LANG_C.read_text(encoding="utf-8")
    return {ord(c) for c in text if ord(c) >= 0x80} | set(HAND_DRAWN)


def latin_built_at_runtime(cp: int) -> bool:
    """A Latin-1 letter the card composes itself (lang_latin_glyph)."""
    return 0xC0 <= cp <= 0xFF and chr(cp).isalpha()


def build_atlas() -> list[tuple[int, int, int, list[int]]]:
    bdf_glyphs, _ = parse_bdf(str(BDF_PATH))
    cjk_glyphs = parse_cjk_bdf(CJK_BDF_PATH)
    cps = sorted(collect_lang_codepoints())
    atlas: list[tuple[int, int, int, list[int]]] = []

    for cp in cps:
        if cp in HAND_DRAWN:
            atlas.append((cp, 6, 8, HAND_DRAWN[cp] + [0] * (UI_FONT_H - 8)))
            continue
        if latin_built_at_runtime(cp):
            continue
        if cp in bdf_glyphs and any(bdf_glyphs[cp]):
            rows8 = [r & 0x3F for r in bdf_glyphs[cp][:8]]
            while len(rows8) < UI_FONT_H:
                rows8.append(0)
            atlas.append((cp, 6, 8, rows8))
            continue
        if cp in cjk_glyphs:
            atlas.append((cp, WIDE_W, UI_FONT_H, cjk_glyphs[cp]))
            continue
        raise RuntimeError(
            f"U+{cp:04X} ({chr(cp)!r}) in lang.c is missing from {CJK_BDF_PATH.name}"
        )
    return atlas


def emit_header(atlas: list[tuple[int, int, int, list[int]]]) -> str:
    lines = [
        "// Generated by tools/genuifont.py -- do not edit by hand.",
        "//",
        "// Sorted Unicode glyph atlas for the DS UI chrome (Latin-1 accents,",
        "// Chinese Hanzi, and Japanese Kana/Kanji).  Narrow glyphs are 6x8",
        "// (matching font6x8); CJK/Kana glyphs are 12x10 (from Fusion Pixel Font",
        "// 10px Monospaced, SIL OFL 1.1; see THIRD_PARTY_NOTICES.md).",
        "",
        "#ifndef DSTERM_UI_FONT_H",
        "#define DSTERM_UI_FONT_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define UI_FONT_H {UI_FONT_H}",
        "",
        "typedef struct {",
        "    uint16_t cp;",
        "    uint8_t width;",
        "    uint8_t height;",
        f"    uint16_t rows[{UI_FONT_H}];",
        "} ui_glyph_t;",
        "",
        f"#define UI_FONT_COUNT {len(atlas)}u",
        "",
        "static const ui_glyph_t ui_font_table[UI_FONT_COUNT] = {",
    ]
    for cp, width, height, rows in atlas:
        row_hex = ", ".join(f"0x{r:03X}" for r in rows)
        lines.append(
            f"    {{ 0x{cp:04X}, {width:2d}, {height:2d}, {{ {row_hex} }} }},"
        )
    lines += [
        "};",
        "",
        "static inline const ui_glyph_t *ui_font_lookup(uint32_t cp)",
        "{",
        "    if (cp < 0x80 || cp > 0xFFFF) return 0;",
        "    int lo = 0, hi = (int)UI_FONT_COUNT - 1;",
        "    while (lo <= hi) {",
        "        const int mid = lo + (hi - lo) / 2;",
        "        const uint16_t have = ui_font_table[mid].cp;",
        "        if (have == (uint16_t)cp) return &ui_font_table[mid];",
        "        if (have < (uint16_t)cp) lo = mid + 1;",
        "        else hi = mid - 1;",
        "    }",
        "    return 0;",
        "}",
        "",
        "#endif  // DSTERM_UI_FONT_H",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    atlas = build_atlas()
    OUT_H.write_text(emit_header(atlas), encoding="utf-8")
    print(f"wrote {OUT_H.relative_to(REPO)} ({len(atlas)} glyphs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
