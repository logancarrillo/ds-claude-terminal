#!/usr/bin/env python3
"""The extended glyph set: everything above ASCII that the DS can render.

Claude Code frames its whole UI in box-drawing characters. The wire format
originally folded everything outside ASCII to '?', which turned a screen of
boxes into a screen of question marks -- the grid preserved them and the encoder
destroyed them, an inconsistency between two layers.

This table is the single source of truth for the fix. genfont.py generates both
the C font table and the Python encoder mapping from it, so the two ends cannot
disagree about what byte 0x83 means.

Wire codes start at 0x80. ASCII 0x20..0x7E is unchanged, so nothing that worked
before changes meaning.

Each entry is (codepoint, source), where source is one of:
    ("bdf",  cp)    take the glyph for `cp` from the BDF
    ("copy", cp)    substitute a different character's glyph -- used where the
                    font lacks the exact character but a near-identical one
                    exists (rounded corners rendered as square ones)
    ("bits", [..])  eight row bitmasks, 6 bits each, bit 5 leftmost -- used for
                    shapes that are trivially describable and absent from the
                    font (block elements, bullets)
"""

from __future__ import annotations

# Block elements are pure geometry, so drawing them beats hunting for a font
# that has them.
_FULL = [0x3F] * 8
_UPPER_HALF = [0x3F] * 4 + [0x00] * 4
_LOWER_HALF = [0x00] * 4 + [0x3F] * 4
_LOWER_1_8 = [0x00] * 7 + [0x3F] * 1
_LOWER_2_8 = [0x00] * 6 + [0x3F] * 2
_LOWER_3_8 = [0x00] * 5 + [0x3F] * 3
_LOWER_5_8 = [0x00] * 3 + [0x3F] * 5
_LOWER_6_8 = [0x00] * 2 + [0x3F] * 6
_LOWER_7_8 = [0x00] * 1 + [0x3F] * 7
_LEFT_HALF = [0x38] * 8
_RIGHT_HALF = [0x07] * 8
_LIGHT_SHADE = [0x15, 0x00, 0x2A, 0x00] * 2
_MEDIUM_SHADE = [0x15, 0x2A] * 4
_DARK_SHADE = [0x3F, 0x2A] * 4

# Quadrant blocks. In a 6x8 cell the left half is columns 0-2 (0x38) and the
# right half is columns 3-5 (0x07); top is rows 0-3. Claude Code's startup
# mascot is built entirely from these.
def _quad(top: int, bottom: int) -> list[int]:
    return [top] * 4 + [bottom] * 4


_Q_UL = _quad(0x38, 0x00)
_Q_UR = _quad(0x07, 0x00)
_Q_LL = _quad(0x00, 0x38)
_Q_LR = _quad(0x00, 0x07)
_Q_UL_LR = _quad(0x38, 0x07)
_Q_UR_LL = _quad(0x07, 0x38)
_Q_UL_UR_LL = _quad(0x3F, 0x38)
_Q_UL_UR_LR = _quad(0x3F, 0x07)
_Q_UL_LL_LR = _quad(0x38, 0x3F)
_Q_UR_LL_LR = _quad(0x07, 0x3F)

_BLANK = [0x00] * 8

_BULLET_FILLED = [0x00, 0x00, 0x0C, 0x1E, 0x1E, 0x0C, 0x00, 0x00]
_BULLET_HOLLOW = [0x00, 0x00, 0x0C, 0x12, 0x12, 0x0C, 0x00, 0x00]
_BULLET_SMALL = [0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x00]
_ELLIPSIS = [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x00]
_CHECK = [0x00, 0x01, 0x02, 0x04, 0x24, 0x18, 0x00, 0x00]
_CROSS = [0x00, 0x21, 0x12, 0x0C, 0x12, 0x21, 0x00, 0x00]
_ARROW_L = [0x00, 0x04, 0x08, 0x3F, 0x08, 0x04, 0x00, 0x00]
_ARROW_R = [0x00, 0x08, 0x04, 0x3F, 0x04, 0x08, 0x00, 0x00]
_ARROW_U = [0x00, 0x0C, 0x1E, 0x2D, 0x0C, 0x0C, 0x0C, 0x00]
_ARROW_D = [0x00, 0x0C, 0x0C, 0x0C, 0x2D, 0x1E, 0x0C, 0x00]
_PAUSE = [0x00, 0x00, 0x1B, 0x1B, 0x1B, 0x1B, 0x00, 0x00]
_SPARK = [0x00, 0x08, 0x2A, 0x1C, 0x2A, 0x08, 0x00, 0x00]

EXTENDED: list[tuple[int, tuple]] = [
    # --- light box drawing: present in Spleen, taken directly ---
    (0x2500, ("bdf", 0x2500)),   # horizontal
    (0x2502, ("bdf", 0x2502)),   # vertical
    (0x250C, ("bdf", 0x250C)),   # top-left
    (0x2510, ("bdf", 0x2510)),   # top-right
    (0x2514, ("bdf", 0x2514)),   # bottom-left
    (0x2518, ("bdf", 0x2518)),   # bottom-right
    (0x251C, ("bdf", 0x251C)),   # tee right
    (0x2524, ("bdf", 0x2524)),   # tee left
    (0x252C, ("bdf", 0x252C)),   # tee down
    (0x2534, ("bdf", 0x2534)),   # tee up
    (0x253C, ("bdf", 0x253C)),   # cross

    # --- rounded corners: Claude Code uses these constantly. Spleen has no
    # rounded variants, and at 6x8 the square ones are visually equivalent.
    (0x256D, ("copy", 0x250C)),
    (0x256E, ("copy", 0x2510)),
    (0x256F, ("copy", 0x2518)),
    (0x2570, ("copy", 0x2514)),

    # --- heavy and double borders fall back to light rather than to '?' ---
    (0x2501, ("copy", 0x2500)),
    (0x2503, ("copy", 0x2502)),
    (0x2550, ("copy", 0x2500)),
    (0x2551, ("copy", 0x2502)),

    # --- block elements: the startup mascot, and progress indicators ---
    (0x2588, ("bits", _FULL)),
    (0x2580, ("bits", _UPPER_HALF)),
    (0x2584, ("bits", _LOWER_HALF)),
    (0x258C, ("bits", _LEFT_HALF)),
    (0x2590, ("bits", _RIGHT_HALF)),
    (0x2591, ("bits", _LIGHT_SHADE)),
    (0x2592, ("bits", _MEDIUM_SHADE)),
    (0x2593, ("bits", _DARK_SHADE)),

    # Quadrant blocks. tools/unknown.py found U+2598/259B/259C/259D on screen
    # hundreds of times in a real session -- the startup mascot is made of them.
    # The whole family is included so a future frame cannot land on a gap.
    (0x2596, ("bits", _Q_LL)),
    (0x2597, ("bits", _Q_LR)),
    (0x2598, ("bits", _Q_UL)),
    (0x2599, ("bits", _Q_UL_LL_LR)),
    (0x259A, ("bits", _Q_UL_LR)),
    (0x259B, ("bits", _Q_UL_UR_LL)),
    (0x259C, ("bits", _Q_UL_UR_LR)),
    (0x259D, ("bits", _Q_UR)),
    (0x259E, ("bits", _Q_UR_LL)),
    (0x259F, ("bits", _Q_UR_LL_LR)),

    # Non-breaking space. Claude Code uses it for layout; rendering it as '?'
    # put a question mark immediately after the prompt chevron.
    (0x00A0, ("bits", _BLANK)),

    # --- bullets and status marks seen in the real 42x24 capture ---
    (0x25CF, ("bits", _BULLET_FILLED)),
    (0x25CB, ("bits", _BULLET_HOLLOW)),
    (0x2022, ("bits", _BULLET_FILLED)),
    (0x25AA, ("bits", _BULLET_SMALL)),
    # Spleen has U+00B7 but its bounding box places it outside our 6x8 cell and
    # it parses to an empty glyph. A middle dot is one pixel pair -- draw it.
    (0x00B7, ("bits", _BULLET_SMALL)),
    (0x2026, ("bits", _ELLIPSIS)),
    (0x2713, ("bits", _CHECK)),
    (0x2717, ("bits", _CROSS)),
    (0x2718, ("bits", _CROSS)),
    (0x23F8, ("bits", _PAUSE)),      # the manual-mode indicator
    (0x273B, ("bits", _SPARK)),      # the "thinking" spinner
    (0x2728, ("bits", _SPARK)),

    # --- arrows and the prompt chevron ---
    (0x2190, ("bits", _ARROW_L)),
    (0x2192, ("bits", _ARROW_R)),
    (0x2191, ("bits", _ARROW_U)),
    (0x2193, ("bits", _ARROW_D)),
    (0x276F, ("copy", ord(">"))),    # the input prompt
    (0x276E, ("copy", ord("<"))),
    (0x203A, ("copy", ord(">"))),
    (0x2039, ("copy", ord("<"))),

    # --- vertical sparkline bars (1/8 .. 7/8; 4/8 and 8/8 are above) ---
    (0x2581, ("bits", _LOWER_1_8)),
    (0x2582, ("bits", _LOWER_2_8)),
    (0x2583, ("bits", _LOWER_3_8)),
    (0x2585, ("bits", _LOWER_5_8)),
    (0x2586, ("bits", _LOWER_6_8)),
    (0x2587, ("bits", _LOWER_7_8)),
]

WIRE_BASE = 0x80

# codepoint -> wire byte
TO_WIRE: dict[int, int] = {cp: WIRE_BASE + i for i, (cp, _) in enumerate(EXTENDED)}


def check() -> None:
    if len(EXTENDED) > 0x80:
        raise SystemExit(f"extended set has {len(EXTENDED)} entries, max 128")
    seen = set()
    for cp, _ in EXTENDED:
        if cp in seen:
            raise SystemExit(f"duplicate codepoint U+{cp:04X}")
        seen.add(cp)


check()

if __name__ == "__main__":
    print(f"{len(EXTENDED)} extended glyphs, wire 0x{WIRE_BASE:02X}..0x{WIRE_BASE + len(EXTENDED) - 1:02X}")
    for cp, src in EXTENDED:
        print(f"  U+{cp:04X} -> 0x{TO_WIRE[cp]:02X}  {src[0]}")
