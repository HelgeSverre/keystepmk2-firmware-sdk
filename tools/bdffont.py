#!/usr/bin/env python3
"""Make the SDK's two display fonts from the X11 Adobe bitmaps in sdk/third_party/x11-adobe.

    uv run tools/bdffont.py header -o sdk/include/keystep_fonts.h   # what `just fonts` runs
    uv run tools/bdffont.py show small "Tempo: 120.0"                # preview in the terminal

Each font is placed in a cell laid out like the stock firmware's font of the same role, so a
screen drawn for one lines up with the other: in the small font the capitals fill rows 0 to 7 of a
10-row cell, in the large font rows 3 to 11 of a 15-row cell. Below the capitals there are two and
three rows for descenders. Characters 0x20 to 0xFF; in the small font the accents above
capitals (and some above lowercase) fall outside the cell and are left off.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "sdk/third_party/x11-adobe"
FIRST, LAST = 0x20, 0xFF


@dataclass(frozen=True)
class Role:
    name: str
    bdf: str
    height: int  # rows in the cell
    cap_top: int  # the row the capitals start on


ROLES = (Role("small", "helvR10.bdf", 10, 0), Role("large", "helvB12.bdf", 15, 3))


def parse_bdf(path: Path) -> dict[int, tuple[int, set[tuple[int, int]]]]:
    """Per character code: the advance and its inked pixels as (x, y), y 0 = the row on the
    baseline, negative upwards."""
    glyphs, code, adv, bbx, rows = {}, None, 0, None, None
    for line in path.read_text(encoding="latin-1").splitlines():
        key, _, value = line.partition(" ")
        if key == "ENCODING":
            code = int(value.split()[0])
        elif key == "DWIDTH":
            adv = int(value.split()[0])
        elif key == "BBX":
            bbx = list(map(int, value.split()))
        elif key == "BITMAP":
            rows = []
        elif key == "ENDCHAR":
            w, h, xo, yo = bbx
            ink = set()
            for j, text in enumerate(rows):
                bits = int(text, 16)
                for i in range(w):
                    if bits >> (len(text) * 4 - 1 - i) & 1:
                        ink.add((xo + i, -(yo + h - 1 - j)))
            if code is not None and code >= 0:
                glyphs[code] = (adv, ink)
            code, rows = None, None
        elif rows is not None:
            rows.append(line.strip())
    return glyphs


@dataclass
class Font:
    role: Role
    ascent: int  # rows above the baseline: the capitals' bottom row is ascent - 1
    glyphs: list[tuple[int, bytes]]  # per character from FIRST: width, column bytes as ks_font holds them
    lost: list[str]  # characters with pixels outside the cell or their advance
    first: int = FIRST

    @property
    def height(self) -> int:
        return self.role.height

    def rows(self, code: int) -> list[str]:
        width, data = self.glyphs[code - FIRST]
        h = self.role.height
        return [
            "".join("1" if data[(x >> 3) * h + y] & (0x80 >> (x & 7)) else "0" for x in range(width)) for y in range(h)
        ]

    def render(self, text: str) -> str:
        chars = [self.rows(ord(c) if FIRST <= ord(c) <= LAST else ord("?")) for c in text]
        return "\n".join(
            "".join(r[y] for r in chars).replace("0", " ").replace("1", "█") for y in range(self.role.height)
        )


def build(role: Role) -> Font:
    source = parse_bdf(SOURCE / role.bdf)
    cap = max(y for (_x, y) in source[ord("H")][1]) - min(y for (_x, y) in source[ord("H")][1]) + 1
    ascent = role.cap_top + cap
    glyphs, lost = [], []
    for code in range(FIRST, LAST + 1):
        adv, ink = source.get(code, source[ord("?")])
        left = min(
            [x for (x, _y) in ink] + [0]
        )  # a hook that reaches left of the origin (j): the glyph gets that much wider
        ink = {(x - left, y) for (x, y) in ink}
        width = max(adv - left, 1)
        data = bytearray(((width + 7) // 8) * role.height)
        for x, y in ink:
            row = ascent - 1 + y
            if not (0 <= x < width and 0 <= row < role.height):
                lost.append(chr(code))
                continue
            data[(x >> 3) * role.height + row] |= 0x80 >> (x & 7)
        glyphs.append((width, bytes(data)))
    return Font(role, ascent, glyphs, sorted(set(lost)))


NOTICE = (SOURCE / "COPYING").read_text(encoding="latin-1").strip()


def header(fonts: list[Font]) -> str:
    notice = "\n".join((" * " + line).rstrip() for line in NOTICE.splitlines())
    out = [
        "/* The SDK's two display fonts, made by tools/bdffont.py from the X11 Adobe bitmaps in",
        " * sdk/third_party/x11-adobe (Helvetica Medium 10 px and Helvetica Bold 12 px, 75 dpi).",
        " * Regenerate with `just fonts`; do not edit. Their notice:",
        " *",
        notice,
        " */",
        "#ifndef KEYSTEP_FONTS_H",
        "#define KEYSTEP_FONTS_H",
        '#include "keystep.h"',
        "",
    ]
    for font in fonts:
        bits, offsets, widths = bytearray(), [], []
        for width, data in font.glyphs:
            offsets.append(len(bits))
            widths.append(width)
            bits += data

        def table(values, per_line):
            lines = [", ".join(str(v) for v in values[i : i + per_line]) for i in range(0, len(values), per_line)]
            return "    " + ",\n    ".join(lines) + ",\n"

        n = font.role.name
        out += [
            f"/* {font.role.height} rows; capitals on rows {font.role.cap_top} to {font.ascent - 1}, characters {FIRST:#x} to {LAST:#x} */",
            f"static const uint8_t ks_font_{n}_bits[] = {{\n{table(list(bits), 24)}}};",
            f"static const uint16_t ks_font_{n}_offsets[] = {{\n{table(offsets, 16)}}};",
            f"static const uint8_t ks_font_{n}_widths[] = {{\n{table(widths, 32)}}};",
            (
                f"static const ks_font ks_font_{n} = {{ {font.role.height}, {font.ascent}, {FIRST}, {LAST}, "
                f"ks_font_{n}_offsets, ks_font_{n}_widths, ks_font_{n}_bits }};"
            ),
            "",
        ]
    out += ["#endif", ""]
    return "\n".join(out)


def fonts() -> list[Font]:
    return [build(role) for role in ROLES]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    h = sub.add_parser("header", help="write the C header")
    h.add_argument("-o", "--output", type=Path, required=True)
    s = sub.add_parser("show", help="draw a text in the terminal")
    s.add_argument("font", choices=[r.name for r in ROLES])
    s.add_argument("text")
    args = parser.parse_args(argv)
    built = fonts()
    for f in built:
        ascii_lost = [c for c in f.lost if ord(c) < 0x7F]
        if ascii_lost:
            print(f"{f.role.name}: pixels outside the cell in {''.join(ascii_lost)!r}", file=sys.stderr)
    if args.command == "header":
        args.output.write_text(header(built))
        print(f"wrote {args.output}")
    else:
        print(next(f for f in built if f.role.name == args.font).render(args.text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
