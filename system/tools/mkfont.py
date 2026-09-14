#!/usr/bin/env python3
"""Generate font8x8_rom.h: the 256-glyph ROM character set.

    python3 tools/mkfont.py > font8x8_rom.h      (run from system/)

Codes 0x20-0x7E are the public-domain font8x8_basic glyphs (read from
font8x8_basic.h). Everything else follows the CP437 layout, so the
codes match every classic box-drawing reference:

    0x01-0x1F  pictographs: card suits, music notes, arrows, pointers
    0xB0-0xDF  shades, single/double box drawing, block elements
    0xF0-0xFE  a few symbols (equivalence, plus-minus, degree, ...)

Undefined codes are blank. Programs can still override any code with
ScreenDefineTile. Stroke conventions match the font's 2 px stems:
single lines occupy rows/columns 3-4, double lines rows/columns 2 and 5.

Bit 0 of a row byte is the leftmost pixel (the font8x8 convention).
"""
import re
import sys

SRC = "font8x8_basic.h"

# --------------------------------------------------------------------
# Pixel-art glyphs: 8 strings of 8, '#' = set.
# --------------------------------------------------------------------

ART = {}


def art(code, name, *rows):
    assert len(rows) == 8 and all(len(r) == 8 for r in rows), name
    ART[code] = (name, rows)


art(0x01, "white smiling face",
    ".######.",
    "#......#",
    "#.#..#.#",
    "#......#",
    "#.#..#.#",
    "#..##..#",
    ".######.",
    "........")
art(0x02, "black smiling face",
    ".######.",
    "########",
    "##.##.##",
    "########",
    "##.##.##",
    "###..###",
    ".######.",
    "........")
art(0x03, "heart",
    "........",
    ".##..##.",
    "########",
    "########",
    ".######.",
    "..####..",
    "...##...",
    "........")
art(0x04, "diamond",
    "...#....",
    "..###...",
    ".#####..",
    "#######.",
    ".#####..",
    "..###...",
    "...#....",
    "........")
art(0x05, "club",
    "...#....",
    "..###...",
    "..###...",
    ".##.##..",
    "#######.",
    "##.#.##.",
    "...#....",
    "..###...")
art(0x06, "spade",
    "...#....",
    "..###...",
    ".#####..",
    "#######.",
    "#######.",
    "..#.#...",
    "..###...",
    "........")
art(0x07, "bullet",
    "........",
    "........",
    "...##...",
    "..####..",
    "..####..",
    "...##...",
    "........",
    "........")
art(0x09, "white circle",
    "........",
    "..####..",
    ".#....#.",
    ".#....#.",
    ".#....#.",
    ".#....#.",
    "..####..",
    "........")
art(0x0D, "eighth note",
    "...##...",
    "...###..",
    "...#.##.",
    "...#....",
    "...#....",
    ".###....",
    "####....",
    ".##.....")
art(0x0E, "beamed eighth notes",
    "..#####.",
    "..#####.",
    "..#...#.",
    "..#...#.",
    "..#...#.",
    "###.###.",
    "###.###.",
    "........")
art(0x10, "right-pointing pointer",
    "#.......",
    "###.....",
    "#####...",
    "#######.",
    "#####...",
    "###.....",
    "#.......",
    "........")
art(0x11, "left-pointing pointer",
    "......#.",
    "....###.",
    "..#####.",
    "#######.",
    "..#####.",
    "....###.",
    "......#.",
    "........")
art(0x12, "up down arrow",
    "...#....",
    "..###...",
    ".#####..",
    "...#....",
    "...#....",
    ".#####..",
    "..###...",
    "...#....")
art(0x18, "up arrow",
    "...##...",
    "..####..",
    ".######.",
    "...##...",
    "...##...",
    "...##...",
    "...##...",
    "........")
art(0x19, "down arrow",
    "...##...",
    "...##...",
    "...##...",
    "...##...",
    ".######.",
    "..####..",
    "...##...",
    "........")
art(0x1A, "right arrow",
    "........",
    "....#...",
    "....##..",
    "#######.",
    "#######.",
    "....##..",
    "....#...",
    "........")
art(0x1B, "left arrow",
    "........",
    "...#....",
    "..##....",
    ".#######",
    ".#######",
    "..##....",
    "...#....",
    "........")
art(0x1C, "right angle",
    "........",
    ".#......",
    ".#......",
    ".#......",
    ".#......",
    ".#......",
    ".######.",
    "........")
art(0x1D, "left right arrow",
    "........",
    "..#...#.",
    ".#.....#",
    "########",
    ".#.....#",
    "..#...#.",
    "........",
    "........")
art(0x1E, "up-pointing triangle",
    "...#....",
    "...#....",
    "..###...",
    "..###...",
    ".#####..",
    ".#####..",
    "#######.",
    "........")
art(0x1F, "down-pointing triangle",
    "#######.",
    ".#####..",
    ".#####..",
    "..###...",
    "..###...",
    "...#....",
    "...#....",
    "........")

art(0xAE, "left guillemet",
    "........",
    "...#..#.",
    "..#..#..",
    ".#..#...",
    "..#..#..",
    "...#..#.",
    "........",
    "........")
art(0xAF, "right guillemet",
    "........",
    ".#..#...",
    "..#..#..",
    "...#..#.",
    "..#..#..",
    ".#..#...",
    "........",
    "........")

art(0xB0, "light shade",
    "#...#...",
    "..#...#.",
    "#...#...",
    "..#...#.",
    "#...#...",
    "..#...#.",
    "#...#...",
    "..#...#.")
art(0xB1, "medium shade",
    "#.#.#.#.",
    ".#.#.#.#",
    "#.#.#.#.",
    ".#.#.#.#",
    "#.#.#.#.",
    ".#.#.#.#",
    "#.#.#.#.",
    ".#.#.#.#")
art(0xB2, "dark shade",
    ".###.###",
    "##.###.#",
    ".###.###",
    "##.###.#",
    ".###.###",
    "##.###.#",
    ".###.###",
    "##.###.#")

art(0xF0, "identical to",
    "........",
    "######..",
    "........",
    "######..",
    "........",
    "######..",
    "........",
    "........")
art(0xF1, "plus-minus",
    "...#....",
    "...#....",
    ".#####..",
    "...#....",
    "...#....",
    "........",
    ".#####..",
    "........")
art(0xF2, "greater-than or equal",
    "........",
    "##......",
    "..##....",
    "....##..",
    "..##....",
    "##......",
    "######..",
    "........")
art(0xF3, "less-than or equal",
    "........",
    "....##..",
    "..##....",
    "##......",
    "..##....",
    "....##..",
    "######..",
    "........")
art(0xF6, "division",
    "........",
    "...#....",
    "........",
    ".#####..",
    "........",
    "...#....",
    "........",
    "........")
art(0xF7, "almost equal",
    "........",
    ".##...#.",
    "#..###..",
    "........",
    ".##...#.",
    "#..###..",
    "........",
    "........")
art(0xF8, "degree",
    "..##....",
    ".#..#...",
    ".#..#...",
    "..##....",
    "........",
    "........",
    "........",
    "........")
art(0xF9, "bullet operator",
    "........",
    "........",
    "........",
    "...##...",
    "...##...",
    "........",
    "........",
    "........")
art(0xFA, "middle dot",
    "........",
    "........",
    "........",
    "...#....",
    "........",
    "........",
    "........",
    "........")
art(0xFD, "superscript two",
    ".##.....",
    "#..#....",
    "...#....",
    "..#.....",
    ".#......",
    "####....",
    "........",
    "........")
art(0xFE, "black square",
    "........",
    "........",
    "..####..",
    "..####..",
    "..####..",
    "..####..",
    "........",
    "........")

# --------------------------------------------------------------------
# Box drawing (0xB3-0xDA): built from arm descriptions.
# S = single (2 px, rows/cols 3-4), D = double (1 px at 2 and 5).
# --------------------------------------------------------------------

N, S, D = None, "S", "D"


class Grid:
    def __init__(self):
        self.px = [[False] * 8 for _ in range(8)]

    def row(self, r, c0, c1):
        for c in range(c0, c1 + 1):
            self.px[r][c] = True

    def col(self, c, r0, r1):
        for r in range(r0, r1 + 1):
            self.px[r][c] = True

    def rows(self):
        return ["".join("#" if p else "." for p in r) for r in self.px]


def box(up, down, left, right):
    """Mixed and all-single junctions by rule: a single line runs to the
    far edge of the double pair it meets, a double line's pair runs to the
    far edge of the single line it meets, and lines of the same style
    simply merge. Pure double junctions are listed explicitly below."""
    g = Grid()
    v_d = D in (up, down)
    h_d = D in (left, right)
    v_s = S in (up, down)
    h_s = S in (left, right)
    assert not (v_d and h_d), "pure double junctions are hand-listed"
    if up == S:
        g.col(3, 0, 5 if h_d else 4)
        g.col(4, 0, 5 if h_d else 4)
    if down == S:
        g.col(3, 2 if h_d else 3, 7)
        g.col(4, 2 if h_d else 3, 7)
    if left == S:
        g.row(3, 0, 5 if v_d else 4)
        g.row(4, 0, 5 if v_d else 4)
    if right == S:
        g.row(3, 2 if v_d else 3, 7)
        g.row(4, 2 if v_d else 3, 7)
    if up == D:
        end = 4 if h_s else 7
        g.col(2, 0, end)
        g.col(5, 0, end)
    if down == D:
        start = 3 if h_s else 0
        g.col(2, start, 7)
        g.col(5, start, 7)
    if left == D:
        end = 4 if v_s else 7
        g.row(2, 0, end)
        g.row(5, 0, end)
    if right == D:
        start = 3 if v_s else 0
        g.row(2, start, 7)
        g.row(5, start, 7)
    return g.rows()


def dbl(*segments):
    """Pure double junction from explicit (kind, index, from, to) runs."""
    g = Grid()
    for kind, i, a, b in segments:
        (g.row if kind == "r" else g.col)(i, a, b)
    return g.rows()


BOX = {
    0xB3: ("box light vertical", box(S, S, N, N)),
    0xB4: ("box light vertical and left", box(S, S, S, N)),
    0xB5: ("box vertical single and left double", box(S, S, D, N)),
    0xB6: ("box vertical double and left single", box(D, D, S, N)),
    0xB7: ("box down double and left single", box(N, D, S, N)),
    0xB8: ("box down single and left double", box(N, S, D, N)),
    0xB9: ("box double vertical and left", dbl(
        ("c", 5, 0, 7), ("c", 2, 0, 2), ("c", 2, 5, 7),
        ("r", 2, 0, 2), ("r", 5, 0, 2))),
    0xBA: ("box double vertical", dbl(("c", 2, 0, 7), ("c", 5, 0, 7))),
    0xBB: ("box double down and left", dbl(
        ("r", 2, 0, 5), ("c", 5, 2, 7), ("r", 5, 0, 2), ("c", 2, 5, 7))),
    0xBC: ("box double up and left", dbl(
        ("c", 5, 0, 5), ("r", 5, 0, 5), ("c", 2, 0, 2), ("r", 2, 0, 2))),
    0xBD: ("box up double and left single", box(D, N, S, N)),
    0xBE: ("box up single and left double", box(S, N, D, N)),
    0xBF: ("box light down and left", box(N, S, S, N)),
    0xC0: ("box light up and right", box(S, N, N, S)),
    0xC1: ("box light up and horizontal", box(S, N, S, S)),
    0xC2: ("box light down and horizontal", box(N, S, S, S)),
    0xC3: ("box light vertical and right", box(S, S, N, S)),
    0xC4: ("box light horizontal", box(N, N, S, S)),
    0xC5: ("box light vertical and horizontal", box(S, S, S, S)),
    0xC6: ("box vertical single and right double", box(S, S, N, D)),
    0xC7: ("box vertical double and right single", box(D, D, N, S)),
    0xC8: ("box double up and right", dbl(
        ("c", 2, 0, 5), ("r", 5, 2, 7), ("c", 5, 0, 2), ("r", 2, 5, 7))),
    0xC9: ("box double down and right", dbl(
        ("r", 2, 2, 7), ("c", 2, 2, 7), ("r", 5, 5, 7), ("c", 5, 5, 7))),
    0xCA: ("box double up and horizontal", dbl(
        ("r", 5, 0, 7), ("r", 2, 0, 2), ("r", 2, 5, 7),
        ("c", 2, 0, 2), ("c", 5, 0, 2))),
    0xCB: ("box double down and horizontal", dbl(
        ("r", 2, 0, 7), ("r", 5, 0, 2), ("r", 5, 5, 7),
        ("c", 2, 5, 7), ("c", 5, 5, 7))),
    0xCC: ("box double vertical and right", dbl(
        ("c", 2, 0, 7), ("c", 5, 0, 2), ("c", 5, 5, 7),
        ("r", 2, 5, 7), ("r", 5, 5, 7))),
    0xCD: ("box double horizontal", dbl(("r", 2, 0, 7), ("r", 5, 0, 7))),
    0xCE: ("box double vertical and horizontal", dbl(
        ("r", 2, 0, 2), ("r", 2, 5, 7), ("r", 5, 0, 2), ("r", 5, 5, 7),
        ("c", 2, 0, 2), ("c", 2, 5, 7), ("c", 5, 0, 2), ("c", 5, 5, 7))),
    0xCF: ("box up single and horizontal double", box(S, N, D, D)),
    0xD0: ("box up double and horizontal single", box(D, N, S, S)),
    0xD1: ("box down single and horizontal double", box(N, S, D, D)),
    0xD2: ("box down double and horizontal single", box(N, D, S, S)),
    0xD3: ("box up double and right single", box(D, N, N, S)),
    0xD4: ("box up single and right double", box(S, N, N, D)),
    0xD5: ("box down single and right double", box(N, S, N, D)),
    0xD6: ("box down double and right single", box(N, D, N, S)),
    0xD7: ("box vertical double and horizontal single", box(D, D, S, S)),
    0xD8: ("box vertical single and horizontal double", box(S, S, D, D)),
    0xD9: ("box light up and left", box(S, N, S, N)),
    0xDA: ("box light down and right", box(N, S, N, S)),
}
for code, (name, rows) in BOX.items():
    art(code, name, *rows)

# Block elements.
art(0xDB, "full block", *(["########"] * 8))
art(0xDC, "lower half block", *(["........"] * 4 + ["########"] * 4))
art(0xDD, "left half block", *(["####...."] * 8))
art(0xDE, "right half block", *(["....####"] * 8))
art(0xDF, "upper half block", *(["########"] * 4 + ["........"] * 4))

# --------------------------------------------------------------------
# Emit
# --------------------------------------------------------------------


def read_basic():
    text = open(SRC, encoding="utf-8").read()
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2}\s*,?\s*){8})\}", text)
    assert len(rows) == 128, "font8x8_basic.h should hold 128 glyphs"
    out = []
    for r in rows:
        out.append([int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", r)])
    return out


def bits(rows):
    out = []
    for r in rows:
        b = 0
        for i, ch in enumerate(r):
            if ch == "#":
                b |= 1 << i  # bit 0 = leftmost pixel
        out.append(b)
    return out


def main():
    glyphs = [[0] * 8 for _ in range(256)]
    names = ["" for _ in range(256)]
    basic = read_basic()
    for code in range(0x20, 0x7F):
        glyphs[code] = basic[code]
        names[code] = "'%s'" % chr(code) if code != 0x27 else "'\\''"
    for code, (name, rows) in ART.items():
        glyphs[code] = bits(rows)
        names[code] = name

    w = sys.stdout.write
    w("/* font8x8_rom.h - the 256-glyph ROM character set.\n"
      " *\n"
      " * GENERATED by tools/mkfont.py; edit the pixel art there, not this\n"
      " * file. Codes 0x20-0x7E come from font8x8_basic.h (public domain);\n"
      " * the rest follow the CP437 layout: pictographs and arrows in\n"
      " * 0x01-0x1F, shades, box drawing and block elements in 0xB0-0xDF,\n"
      " * a few symbols in 0xF0-0xFE. Undefined codes are blank. Bit 0 of\n"
      " * a row byte is the leftmost pixel. Static: each renderer that\n"
      " * includes this keeps its own copy.\n"
      " */\n"
      "#pragma once\n\n"
      "#include <stdint.h>\n\n"
      "static const uint8_t font8x8_rom[256][8] = {\n")
    for code in range(256):
        g = glyphs[code]
        w("    {%s}, /* 0x%02X %s */\n" % (
            ", ".join("0x%02X" % v for v in g), code, names[code] or "-"))
    w("};\n")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--show":
        # Print every defined glyph as pixel art, for review.
        for code in sorted(ART):
            name, rows = ART[code]
            print("0x%02X %s" % (code, name))
            for r in rows:
                print("   " + r.replace(".", "·"))
        sys.exit(0)
    main()
