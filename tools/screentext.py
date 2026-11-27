#!/usr/bin/env python3
"""Read the text on the emulated display by matching a pair of fonts' glyphs.

    from screentext import Reader
    reader = Reader()                 # the stock firmware's fonts, from stock 1.0.2 (tools/fontextract.py)
    reader = Reader.sdk()             # the SDK's fonts (tools/bdffont.py)
    print(reader.read(board))         # ["Tempo: 120.0   1/16", "Bank: 1 | Preset: 1"]

Only text drawn in the reader's fonts is found: the stock reader reads the stock firmware's
screens, the SDK reader screens drawn with ks_font_small and ks_font_large. Lines come back top to bottom; text in the large font is
marked by the caller if needed through `read_lines`, which also gives positions.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fontextract

WIDTH, HEIGHT = 128, 32


class Reader:
    def __init__(self, image: Path = fontextract.STOCK, fonts: list | None = None):
        self.fonts = []
        self.alike = set()  # heights of the fonts that draw 0 and O alike
        for font in fonts if fonts is not None else fontextract.find(image):
            if font.rows(ord("0")) == font.rows(ord("O")):
                self.alike.add(font.height)
            cells = {}
            for code, (width, _data) in enumerate(font.glyphs, font.first):
                if not width or code > 0x7E or chr(code) in "^`~_'\"":  # marks a frame's corners read as
                    continue
                cells[chr(code)] = (width, [int(r, 2) for r in font.rows(code)])
            inked = [r for _w, rows in cells.values() for r in range(font.height) if rows[r]]
            top, bottom = min(inked), max(inked)
            base = max(r for r in range(font.height) if cells["E"][1][r])  # the baseline
            # A glyph is compared down to the baseline, and below it only as far as it has ink:
            # the rows of the descenders may hold the top of whatever is drawn underneath.
            glyphs = []
            for char, (width, rows) in cells.items():
                last = max([r for r in range(font.height) if rows[r]] + [base])
                glyphs.append((char, width, rows, sum(r.bit_count() for r in rows), range(top, last + 1)))
            glyphs.sort(key=lambda g: -g[3])  # most ink first, so "B" is tried before "I"
            # A blank column, read as nothing: pieces of text drawn apart need not be a whole
            # number of spaces apart to be read as one line.
            glyphs.append(("", 1, [0] * font.height, 0, range(top, base + 1)))
            self.fonts.append((font.height, glyphs, range(top, bottom + 1)))

    @classmethod
    def sdk(cls) -> Reader:
        import bdffont

        return cls(fonts=bdffont.fonts())

    def width(self, height: int, text: str) -> int:
        widths = next({c: w for c, w, *_rest in g if c} for h, g, _u in self.fonts if h == height)
        return sum(widths[c] for c in text)

    def settle(self, height: int, text: str) -> str:
        """The stock small font draws 0 and O alike. Word by word: a zero in a word with other
        digits, or with no letters, stays a zero; in a word of letters it is an O ("OK", "Oct",
        "Overdub"). Fonts that tell them apart are left as read."""
        if height not in self.alike:
            return text
        words = []
        for word in text.split(" "):
            rest = word.replace("0", "").replace("O", "")
            letters = any(c.isalpha() for c in rest)
            digits = any(c.isdigit() for c in rest)
            words.append(word.replace("0", "O") if letters and not digits else word.replace("O", "0"))
        return " ".join(words)

    def read_lines(self, board) -> list[tuple[int, int, int, str]]:
        """(y, x, font height, text) for each run of text, top to bottom."""
        rows = [sum(board.oled.pixel(x, y) << (WIDTH - 1 - x) for x in range(WIDTH)) for y in range(HEIGHT)]
        found = []
        for height, glyphs, _used in self.fonts:  # rows no glyph inks may hold a neighbouring line
            for y in range(HEIGHT - height + 1):
                # best[x]: the reading from column x with the most ink, as (ink, glyph, next column)
                best: list[tuple[int, str, int] | None] = [None] * (WIDTH + 1)
                for x in range(WIDTH - 1, -1, -1):
                    for char, width, grows, gink, used in glyphs:
                        if x + width > WIDTH:
                            continue
                        shift, mask = WIDTH - x - width, (1 << width) - 1
                        if any((rows[y + r] >> shift) & mask != grows[r] for r in used):
                            continue
                        after = best[x + width]
                        ink = gink + (after[0] if after else 0)
                        if ink and (best[x] is None or ink > best[x][0]):
                            best[x] = (ink, char, x + width)
                for x in range(WIDTH):
                    before = best[x - 1] if x else None  # a character that ends here: x is not where a reading starts
                    if (
                        best[x] is None
                        or best[x][1] in (" ", "")
                        or (before and before[2] == x and before[1] not in (" ", ""))
                    ):
                        continue  # not the start of a reading
                    run, at = "", x
                    while best[at]:
                        run += best[at][1]
                        at = best[at][2]
                    if sum(c != " " for c in run) >= 2 and best[x][0] >= 12:
                        found.append((best[x][0], y, x, height, run.strip()))
        found = [(ink, y, x, height, self.settle(height, text)) for ink, y, x, height, text in found]
        taken: list[tuple[int, int, int, int]] = []
        lines = []
        for _ink, y, x, height, text in sorted(found, reverse=True):  # the reading with most ink wins an area
            used = next(u for h, _g, u in self.fonts if h == height)
            box = (y + used.start, y + used.stop, x, x + self.width(height, text))
            if any(box[0] < b and t < box[1] and x < r and le < box[3] for t, b, le, r in taken):
                continue
            taken.append(box)
            lines.append((y, x, height, text))
        return sorted(lines)

    def read(self, board) -> list[str]:
        out: list[tuple[int, str]] = []
        for y, _x, _height, text in self.read_lines(board):
            if out and abs(out[-1][0] - y) <= 2:
                out[-1] = (out[-1][0], out[-1][1] + "   " + text)
            else:
                out.append((y, text))
        return [text for _y, text in out]
