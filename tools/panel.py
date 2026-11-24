#!/usr/bin/env python3
"""A KeyStep mk2 on the screen: run a firmware image in a window that looks like the device.

The window sits on tools/emu.py, so what runs is the real ARM image: the stock firmware today,
SDK firmware later. Click the buttons and keys, turn the encoder, drag the touch strips. The
strip under the device shows the display enlarged and the MIDI sent on the DIN output.

    uv run --with unicorn --with pygame-ce tools/panel.py firmware/keystep_mk2__1_0_2.ks2fw
    uv run --with unicorn --with pygame-ce tools/panel.py image.ks2fw --ms 4000 \\
        --script "2500 tap=seq; 3000 tap=play" --shot out.png

Mouse: click a button or key; scroll over the encoder to turn it, click it to push it; drag on
a touch strip. Shift + click or right-click leaves a control held until you click it again.
Keyboard: 1-8 Seq Arp Mutate Chord Tempo Record Stop Play, space Play, Backspace Back,
Enter encoder push, Left/Right turn the encoder, Tab Shift, Q Hold, - and = octave,
Z X C V B N M , . / white keys, S D F H J L ; black keys, 0 lets go of everything,
P screenshot, Esc quit.

Positions follow Arturia's front-panel drawing of the KeyStep mk2. The display is as small as
on the device, so the strip below repeats it at twice the size. An LED above a key is drawn
only when the firmware lights it; that it sits above the key with the same number is assumed.
"""

from __future__ import annotations

import argparse
import math
import os
import sys
import time
from pathlib import Path

import emu

ASSETS = Path(__file__).resolve().parent / "assets"
REF_W, REF_H = 2000, 621  # the front-panel drawing all positions below are measured on
SS = 2  # the device is drawn this many times larger and scaled down, for smooth edges
FAST_RATE = 16  # instructions per simulated microsecond; the chip manages about 40 (--exact)
STRIP_H = 96  # window pixels under the device for the enlarged display and the MIDI log

BACKDROP, BODY, BODY_EDGE = (198, 201, 206), (236, 236, 235), (208, 208, 206)
RECESS, RECESS_EDGE = (241, 241, 240), (218, 218, 216)
FACE, FACE_EDGE, FACE_SHADE = (249, 249, 248), (158, 158, 156), (120, 120, 118)
INK, BLUE, SHIFT_BLUE = (44, 46, 50), (12, 118, 186), (20, 134, 198)
GLASS, BEZEL, PIXEL = (14, 14, 16), (26, 27, 31), (214, 236, 255)
WELL, WHITE_KEY, WHITE_FRONT = (26, 26, 28), (253, 253, 252), (226, 226, 224)
BLACK_KEY, BLACK_TOP = (20, 20, 22), (46, 46, 50)
STRIP, STRIP_EDGE = (236, 236, 235), (222, 222, 220)
BAR, BAR_INK = (30, 32, 37), (188, 192, 200)

# name: (x, y, label, blue legend under it). Every button is 76 x 43.
BUTTON_W, BUTTON_H = 76, 43
BUTTONS = {
    "seq": (115, 85, "Seq", "Save"),
    "arp": (208, 85, "Arp", "To Seq"),
    "mutate": (301, 85, "Mutate", "Reset"),
    "chord": (394, 85, "Chord", "Scale"),
    "tempo": (509, 85, "", "Length"),
    "record": (602, 85, "", "Overdub"),
    "stop": (695, 85, "", "MIDI Ch"),
    "play": (788, 85, "", "Chain"),
    "shift": (115, 173, "Shift", ""),
    "hold": (208, 173, "Hold", ""),
    "oct-": (115, 240, "Oct −", ""),
    "oct+": (208, 240, "Oct +", ""),
}
TOP_PANEL = (97, 58, 1153, 96)  # the framed area holding the top buttons, display and encoder
DISPLAY = (903, 76, 119, 57)  # the glass
DISPLAY_ACTIVE = (86, 21.5)  # lit area, centred in the glass
BACK = (1068, 106, 23)  # x, y, radius
ENCODER = (1182, 106, 29)
STRIPS = {1: (117, 320, 73, 225, "Bend", "Gate"), 0: (210, 320, 73, 225, "Mod", "Spice")}  # touch channel: rect
KEYBED = (330, 207, 1546, 371)
BLACK_W, BLACK_H = 46, 221
BLACK_SHIFT = {1: -11, 3: 2, 5: 15, 8: -7, 10: 9}  # semitone above F: offset from the white-key gap
WHITE_STEPS = (0, 2, 4, 6, 7, 9, 11)  # semitones above F of the white keys F G A B C D E
GROUPS = ((337, "Seq Bank / Arp Octave"), (520, "Seq Pattern / Arp Mode"), (1274, "Rate"), (1418, "Seq Edit"))
PATTERN_X = (530, 577, 618, 670, 717, 765, 818, 858, 911, 955, 1000, 1046, 1092, 1139, 1185, 1236)
LEGENDS = (
    [(x, str(i + 1)) for i, x in enumerate((357, 402, 447, 494))]
    + [(x, str(i + 1)) for i, x in enumerate(PATTERN_X)]
    + [(1293, "‹"), (1322, "Time Div"), (1349, "›"), (1393, "3"), (1437, "‹"), (1466, "Nudge")]
    + [(1495, "›"), (1530, "‹"), (1558, "Octave"), (1586, "›"), (1628, "Double"), (1669, "Quant")]
    + [(1721, "Reload"), (1765, "Clear"), (1830, "Undo")]
)
KEY_NAMES = {
    "z": 0, "s": 1, "x": 2, "d": 3, "c": 4, "f": 5, "v": 6, "b": 7, "h": 8, "n": 9, "j": 10, "m": 11,
    ",": 12, "l": 13, ".": 14, ";": 15, "/": 16,
}  # fmt: skip
HOTKEYS = {
    "1": "seq", "2": "arp", "3": "mutate", "4": "chord", "5": "tempo", "6": "record", "7": "stop", "8": "play",
    " ": "play", "\b": "back", "\r": "encoder", "\t": "shift", "q": "hold", "-": "oct-", "=": "oct+",
}  # fmt: skip
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")


def key_layout() -> list[tuple[int, bool, tuple[float, float, float, float]]]:
    """(key number, is black, rectangle in drawing units) for the 32 keys, white keys first."""
    x0, y0, w, h = KEYBED
    whites = [k for k in range(emu.KEYS) if k % 12 in WHITE_STEPS]
    step = w / len(whites)
    out, left = [], {}
    for i, k in enumerate(whites):
        left[k] = x0 + i * step
        out.append((k, False, (x0 + i * step, y0, step, h)))
    for k in range(emu.KEYS):
        if k % 12 not in WHITE_STEPS:
            centre = left[k + 1] + BLACK_SHIFT[k % 12]
            out.append((k, True, (centre - BLACK_W / 2, y0, BLACK_W, BLACK_H)))
    return out


def mix(a, b, t: float):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b, strict=True))


def led_colour(rgb: tuple[int, int, int], off: tuple[int, int, int]) -> tuple[int, int, int]:
    """An LED's drive values (the firmware stays below 0x8D) as a screen colour over `off`."""
    level = max(rgb)
    if not level:
        return off
    gain = 255 / max(level, 0x8D)
    lit = tuple(min(255, round(c * gain)) for c in rgb)
    return mix(off, lit, min(1.0, 0.35 + level / 0x8D * 0.65))


def describe_midi(data: bytes) -> list[str]:
    """Channel messages in a run of MIDI bytes, as short text."""
    out, i = [], 0
    while i < len(data):
        status = data[i]
        kind, size = status & 0xF0, 3
        if kind in (0xC0, 0xD0):
            size = 2
        elif status >= 0xF0 or status < 0x80:
            i += 1
            continue
        if i + size > len(data):
            break
        a, b = data[i + 1], data[i + 2] if size == 3 else 0
        note = f"{NOTE_NAMES[a % 12]}{a // 12 - 1}"
        if kind == 0x90 and b:
            out.append(f"on {note} v{b}")
        elif kind in (0x80, 0x90):
            out.append(f"off {note}")
        elif kind == 0xB0:
            out.append(f"cc{a}={b}")
        elif kind == 0xE0:
            out.append(f"bend {(b << 7 | a) - 8192:+d}")
        else:
            out.append(data[i : i + size].hex(" "))
        i += size
    return out


class Panel:
    def __init__(self, board: emu.Board, scale: float):
        import pygame

        self.pg, self.board, self.scale = pygame, board, scale
        self.size = (round(REF_W * scale), round(REF_H * scale) + STRIP_H)
        self.keys = key_layout()
        self.held: set[str] = set()  # controls down now
        self.since: dict[str, float] = {}  # device time each was pressed
        self.release_at: dict[str, float] = {}  # device time each is to be let go
        self.latched: set[str] = set()  # held until clicked again
        self.mouse: str | None = None  # control under the pressed mouse button
        self.strip: int | None = None
        self.strip_at: dict[int, float | None] = {0: None, 1: None}
        self.midi_seen = 0
        self.midi_log: list[str] = []
        self.speed = 0.0  # device time per real time, measured while a window is open
        self.cache = None  # (state, picture of the device)
        pygame.font.init()
        k = scale * SS

        def font(weight: str, units: float):
            return pygame.font.Font(str(ASSETS / f"BarlowSemiCondensed-{weight}.ttf"), round(units * k))

        self.f_button, self.f_legend, self.f_small = font("Medium", 15.5), font("Medium", 14), font("Medium", 12)
        self.f_key, self.f_step, self.f_brand = font("Light", 44), font("SemiBold", 44), font("SemiBold", 22)
        self.f_bar = pygame.font.Font(str(ASSETS / "BarlowSemiCondensed-Medium.ttf"), 15)

    # ---- controls ----

    # The stock firmware debounces its buttons and needs a press of about 33 ms; a click on a
    # trackpad can be over within one frame. So a press here always lasts HOLD_MS of device time.
    HOLD_MS = 70

    def down(self, name: str) -> None:
        self.release_at.pop(name, None)
        if name not in self.held:
            self.held.add(name)
            self.since[name] = self.board.us
            self.board.press(name)

    def up(self, name: str) -> None:
        if name in self.held and name not in self.latched:
            self.release_at[name] = self.since.get(name, 0) + self.HOLD_MS * 1000

    def tick(self) -> None:
        """Let go of the controls whose release is due."""
        for name, when in list(self.release_at.items()):
            if self.board.us >= when:
                del self.release_at[name]
                self.held.discard(name)
                self.board.release(name)

    def release_all(self) -> None:
        self.latched.clear()
        for name in list(self.held):
            self.up(name)
        for ch in (0, 1):
            self.touch(ch, None)

    def touch(self, ch: int, position: float | None) -> None:
        self.strip_at[ch] = position
        self.board.set_touch(ch, position)

    def at(self, pos) -> str | None:
        """The control under a window position: a button name, k<n>, back, encoder, or strip<n>."""
        x, y = pos[0] / self.scale, pos[1] / self.scale
        for name, (bx, by, *_rest) in BUTTONS.items():
            if bx <= x < bx + BUTTON_W and by <= y < by + BUTTON_H:
                return name
        for name, (cx, cy, r) in (("back", BACK), ("encoder", ENCODER)):
            if (x - cx) ** 2 + (y - cy) ** 2 <= (r + 3) ** 2:
                return name
        for ch, (sx, sy, sw, sh, *_rest) in STRIPS.items():
            if sx <= x < sx + sw and sy <= y < sy + sh:
                return f"strip{ch}"
        for k, _black, (kx, ky, kw, kh) in reversed(self.keys):  # black keys lie on top
            if kx <= x < kx + kw and ky <= y < ky + kh:
                return f"k{k}"
        return None

    def strip_position(self, ch: int, y: int) -> float:
        _, sy, _, sh, *_rest = STRIPS[ch]
        return min(1.0, max(0.0, 1 - (y / self.scale - sy) / sh))

    def mouse_down(self, pos, latch: bool) -> None:
        name = self.at(pos)
        if not name:
            return
        if name.startswith("strip"):
            self.strip = int(name[5:])
            self.touch(self.strip, self.strip_position(self.strip, pos[1]))
        elif latch:
            if name in self.latched:
                self.latched.discard(name)
                self.up(name)
            else:
                self.latched.add(name)
                self.down(name)
        else:
            self.mouse = name
            self.down(name)

    def mouse_up(self) -> None:
        if self.mouse:
            self.up(self.mouse)
            self.mouse = None
        if self.strip is not None:
            self.touch(self.strip, None)
            self.strip = None

    # ---- drawing the device (in drawing units, on a surface SS times the window scale) ----

    def px(self, *v: float):
        k = self.scale * SS
        return tuple(round(x * k) for x in v) if len(v) > 1 else round(v[0] * k)

    def rect(self, s, colour, x, y, w, h, radius=0.0, width=0.0) -> None:
        r = self.pg.Rect(*self.px(x, y), self.px(w), self.px(h))
        self.pg.draw.rect(s, colour, r, width=self.px(width) if width else 0, border_radius=self.px(radius))

    def label(self, s, font, string: str, x: float, y: float, colour=INK, anchor: str = "c", spacing: float = 0):
        """Text centred on, or starting ("l") or ending ("r") at, x; returns its width in drawing units."""
        k = self.scale * SS
        if spacing:
            glyphs = [font.render(ch, True, colour) for ch in string]
            gap = self.px(spacing)
            width = sum(g.get_width() for g in glyphs) + gap * (len(glyphs) - 1)
            left = self.px(x) - width // 2
            for g in glyphs:
                s.blit(g, (left, self.px(y) - g.get_height() // 2))
                left += g.get_width() + gap
            return width / k
        image = font.render(string, True, colour)
        left = self.px(x) - {"c": image.get_width() // 2, "l": 0, "r": image.get_width()}[anchor]
        s.blit(image, (left, self.px(y) - image.get_height() // 2))
        return image.get_width() / k

    def draw_button(self, s, name: str) -> None:
        pg = self.pg
        x, y, text, under = BUTTONS[name]
        lit = self.board.leds[1][emu.BUTTON_LEDS[name]] if name in emu.BUTTON_LEDS else (0, 0, 0)
        base = SHIFT_BLUE if name == "shift" else FACE
        face = led_colour(lit, base)
        if name == "shift" and any(lit):  # a blue cap over a white LED: it gets brighter, not white
            face = mix(SHIFT_BLUE, (255, 255, 255), 0.3)
        down = name in self.held
        if any(lit):  # light spills past the edge of a lit button
            around = RECESS if y < 150 else BODY
            for grow, weight in ((7, 0.25), (4, 0.5)):
                glow = mix(around, led_colour(lit, base), weight)
                self.rect(s, glow, x - grow, y - grow, BUTTON_W + 2 * grow, BUTTON_H + 2 * grow, 6 + grow)
        if not down:  # the button stands a little proud: a darker lower edge
            self.rect(s, FACE_SHADE, x, y + 2, BUTTON_W, BUTTON_H, 5)
        top = y + (2 if down else 0)
        self.rect(s, face, x, top, BUTTON_W, BUTTON_H, 5)
        edge = BLUE if name in self.latched else mix(FACE_EDGE, face, 0.3) if any(lit) else FACE_EDGE
        self.rect(s, edge, x, top, BUTTON_W, BUTTON_H, 5, 1.6)
        ink = (255, 255, 255) if sum(face) < 400 else INK
        cx, cy = x + BUTTON_W / 2, top + BUTTON_H / 2
        if text:
            self.label(s, self.f_button, text, cx, cy - 1, ink)
        elif name == "record":
            pg.draw.circle(s, ink, self.px(cx, cy), self.px(6.5))
        elif name == "stop":
            self.rect(s, ink, cx - 6, cy - 6, 12, 12, 1.5)
        elif name == "play":
            for dx in (-17, -11):
                self.rect(s, ink, cx + dx, cy - 7, 3.2, 14)
            pg.draw.line(s, ink, self.px(cx - 6.5, cy + 9), self.px(cx - 0.5, cy - 9), self.px(1.6))
            pg.draw.polygon(s, ink, [self.px(cx + 3, cy - 7), self.px(cx + 3, cy + 7), self.px(cx + 16, cy)])
        elif name == "tempo":  # a metronome
            body = [(cx - 4, cy - 10), (cx + 4, cy - 10), (cx + 9, cy + 9), (cx - 9, cy + 9)]
            pg.draw.polygon(s, ink, [self.px(*p) for p in body], self.px(1.6))
            pg.draw.line(s, ink, self.px(cx - 4, cy + 5), self.px(cx + 7, cy - 12), self.px(1.6))
            pg.draw.line(s, ink, self.px(cx - 8, cy + 4), self.px(cx + 8, cy + 4), self.px(1.4))
        if under:
            self.label(s, self.f_legend, under, cx, y + BUTTON_H + 14, BLUE)

    def draw_knob(self, s) -> None:
        """Spun aluminium: brightness follows the angle, with two bright and two dark bands."""
        pg = self.pg
        cx, cy, r = ENCODER
        scale = 0.97 if "encoder" in self.held else 1.0
        ring = self.board.leds[1][emu.BUTTON_LEDS["encoder"]]
        pg.draw.circle(s, led_colour(ring, (204, 204, 202)), self.px(cx, cy + 3), self.px(r + 4))
        for radius, shift, lo, hi in ((r * scale, 0.0, 168, 252), (r * 0.8 * scale, 0.5, 190, 255)):
            previous = None
            for i in range(121):
                a = i * math.tau / 120
                point = self.px(cx + math.cos(a) * radius, cy + math.sin(a) * radius)
                if previous:
                    level = round(lo + (hi - lo) * (0.5 + 0.5 * math.cos(2 * (a - 0.9 - shift))))
                    pg.draw.polygon(s, (level, level, min(255, level + 2)), [self.px(cx, cy), previous, point])
                previous = point
            pg.draw.circle(s, (150, 150, 152), self.px(cx, cy), self.px(radius), self.px(0.8))
        a = self.board.encoder.position * math.tau / 96 - math.pi / 2  # 24 detents a turn
        dot = self.px(cx + math.cos(a) * r * 0.55, cy + math.sin(a) * r * 0.55)
        pg.draw.circle(s, (120, 122, 126), dot, self.px(2.2))

    def draw_device(self, s) -> None:
        pg, board = self.pg, self.board
        s.fill(BACKDROP)
        self.rect(s, mix(BACKDROP, (0, 0, 0), 0.18), 70, 37, 1864, 558, 18)  # shadow
        self.rect(s, BODY, 68, 32, 1864, 558, 16)
        self.rect(s, BODY_EDGE, 68, 32, 1864, 558, 16, 1.5)

        px, py, pw, ph = TOP_PANEL
        self.rect(s, RECESS, px, py, pw, ph, 5)
        self.rect(s, RECESS_EDGE, px, py, pw, ph, 5, 1.5)
        bottom = py + ph
        tab = [self.px(*p) for p in ((998, bottom + 1), (1012, bottom - 13), (1126, bottom - 13), (1140, bottom + 1))]
        pg.draw.polygon(s, BODY, tab)  # the cut-out the Arturia name sits in
        pg.draw.lines(s, RECESS_EDGE, False, tab, self.px(1.5))
        pg.draw.line(s, RECESS_EDGE, self.px(886, py), self.px(886, bottom), self.px(1.5))
        self.label(s, self.f_brand, "ARTURIA", 1069, 160, INK, spacing=2.4)

        for name in BUTTONS:
            self.draw_button(s, name)

        gx, gy, gw, gh = DISPLAY
        self.rect(s, GLASS, gx, gy, gw, gh, 3)
        self.rect(s, BEZEL, gx + 9, gy + 9, gw - 18, gh - 18, 2)

        bx, by, br = BACK
        dy = 2 if "back" in self.held else 0
        if not dy:
            pg.draw.circle(s, FACE_SHADE, self.px(bx, by + 2), self.px(br))
        pg.draw.circle(s, FACE, self.px(bx, by + dy), self.px(br))
        edge = BLUE if "back" in self.latched else FACE_EDGE
        pg.draw.circle(s, edge, self.px(bx, by + dy), self.px(br), self.px(1.6))
        self.label(s, self.f_legend, "Back", bx, by + dy - 1)
        self.draw_knob(s)

        width = self.label(s, self.f_step, "STEP", 1876, 106, INK, "r")
        self.label(s, self.f_key, "KEY", 1875 - width, 106, INK, "r")

        for ch, (x, y, w, h, name, under) in STRIPS.items():
            self.rect(s, STRIP, x, y, w, h, 5)
            self.rect(s, STRIP_EDGE, x, y, w, h, 5, 1.2)
            if ch == 1:
                middle = y + h / 2
                pg.draw.line(s, (150, 150, 150), self.px(x + 10, middle), self.px(x + w - 10, middle), self.px(1))
            at = self.strip_at[ch]
            if at is not None:
                self.rect(s, BLUE, x + 4, y + h - at * h - 3, w - 8, 6, 3)
            left = x + w / 2 - 34
            first = self.label(s, self.f_legend, name + " / ", left, y + h + 13, INK, "l")
            self.label(s, self.f_legend, under, left + first, y + h + 13, BLUE, "l")

        for x, text in GROUPS:
            pg.draw.line(s, BLUE, self.px(x, 169), self.px(x, 200), self.px(1.2))
            self.label(s, self.f_legend, text, x + 5, 176, BLUE, "l")
        for x, text in LEGENDS:
            self.label(s, self.f_small, text, x, 195, INK)

        kx, ky, kw, kh = KEYBED
        self.rect(s, WELL, kx - 4, ky - 4, kw + 8, kh + 12, 3)
        for k, black, (x, y, w, h) in self.keys:
            down = bool(board.key_contacts[k]) or f"k{k}" in self.held
            if black:
                self.rect(s, BLACK_KEY, x, y, w, h + (0 if down else 3), 3)
                top = mix(BLACK_TOP, (150, 190, 230), 0.45 if down else 0)
                self.rect(s, top, x + 4, y, w - 8, h - (10 if down else 22), 2)
            else:
                tint = 0.55 if down else 0
                self.rect(s, mix(WHITE_KEY, (176, 208, 238), tint), x + 0.6, y, w - 1.2, h, 3)
                front = mix(WHITE_FRONT, (150, 186, 220), tint)
                self.rect(s, front, x + 0.6, y + h - (5 if down else 9), w - 1.2, 9, 3)

    def state(self) -> tuple:
        board = self.board
        return (
            frozenset(self.held), frozenset(self.latched), tuple(board.leds[1]),
            tuple(board.key_contacts[: emu.KEYS]), self.strip_at[0], self.strip_at[1], board.encoder.position,
        )  # fmt: skip

    def draw(self, surface) -> None:
        pg, board = self.pg, self.board
        w, h = self.size[0], self.size[1] - STRIP_H
        state = self.state()
        if not self.cache or self.cache[0] != state:
            big = pg.Surface((w * SS, h * SS))
            self.draw_device(big)
            self.cache = (state, pg.transform.smoothscale(big, (w, h)))
        surface.blit(self.cache[1], (0, 0))

        oled = pg.Surface((128, 32))
        oled.fill(BEZEL)
        if board.oled.on:
            lit = mix(BEZEL, PIXEL, 0.6 + 0.4 * board.oled.contrast / 255)
            ram = board.oled.ram
            for page in range(4):
                for x in range(128):
                    column = ram[page * 128 + x]
                    if column:
                        for bit in range(8):
                            if column >> bit & 1:
                                oled.set_at((x, page * 8 + bit), lit)
        gx, gy, gw, gh = DISPLAY
        aw, ah = round(DISPLAY_ACTIVE[0] * self.scale), round(DISPLAY_ACTIVE[1] * self.scale)
        left, top = (gx + gw / 2) * self.scale - aw / 2, (gy + gh / 2) * self.scale - ah / 2
        surface.blit(pg.transform.smoothscale(oled, (aw, ah)), (round(left), round(top)))

        surface.fill(BAR, (0, h, w, STRIP_H))
        pg.draw.rect(surface, GLASS, (16, h + 12, 272, 72), border_radius=6)
        surface.blit(pg.transform.scale(oled, (256, 64)), (24, h + 16))
        speed = f"   speed {self.speed * 100:.0f}%" if self.speed else ""
        held = "   held: " + " ".join(sorted(self.latched)) if self.latched else ""
        lines = (f"{board.us / 1e6:.1f} s{speed}{held}", "MIDI out:  " + "   ".join(self.midi_log[-10:]))
        for i, line in enumerate(lines):
            surface.blit(self.f_bar.render(line, True, BAR_INK), (312, h + 22 + 28 * i))

    def poll_midi(self) -> None:
        data = self.board.midi_out
        if len(data) > self.midi_seen:
            self.midi_log += describe_midi(bytes(data[self.midi_seen :]))
            self.midi_log = self.midi_log[-40:]
            self.midi_seen = len(data)


def interactive(panel: Panel, shot: Path | None) -> None:
    pg, board = panel.pg, panel.board
    screen = pg.display.set_mode(panel.size)
    pg.display.set_caption("KeyStep mk2")
    clock = pg.time.Clock()
    shots = 0
    while not board.stopped and not board.interrupted:
        for event in pg.event.get():
            if event.type == pg.QUIT:
                return
            if event.type == pg.KEYDOWN:
                if event.key == pg.K_ESCAPE:
                    return
                char = event.unicode.lower() if event.unicode else ""
                if event.key == pg.K_LEFT:
                    board.turn(-1)
                elif event.key == pg.K_RIGHT:
                    board.turn(1)
                elif char == "0":
                    panel.release_all()
                elif char == "p":
                    path = shot or Path(f"keystep-{shots}.png")
                    pg.image.save(screen, str(path))
                    shots += 1
                    print(f"saved {path}")
                elif char in HOTKEYS:
                    panel.down(HOTKEYS[char])
                elif char in KEY_NAMES:
                    panel.down(f"k{KEY_NAMES[char]}")
            elif event.type == pg.KEYUP:
                char = pg.key.name(event.key)
                char = {"space": " ", "backspace": "\b", "return": "\r", "tab": "\t"}.get(char, char)
                if char in HOTKEYS:
                    panel.up(HOTKEYS[char])
                elif char in KEY_NAMES:
                    panel.up(f"k{KEY_NAMES[char]}")
            elif event.type == pg.MOUSEBUTTONDOWN and event.button in (1, 3):
                panel.mouse_down(event.pos, event.button == 3 or bool(pg.key.get_mods() & pg.KMOD_SHIFT))
            elif event.type == pg.MOUSEBUTTONUP and event.button == 1:
                panel.mouse_up()
            elif event.type == pg.MOUSEMOTION and panel.strip is not None:
                panel.touch(panel.strip, panel.strip_position(panel.strip, event.pos[1]))
            elif event.type == pg.MOUSEWHEEL and panel.at(pg.mouse.get_pos()) == "encoder":
                board.turn(1 if event.y > 0 else -1)
        started = time.perf_counter()
        before = board.us
        panel.tick()
        board.run(33)  # one frame of device time; the frame takes as long as that needs
        panel.poll_midi()
        panel.draw(screen)
        pg.display.flip()
        clock.tick(30)
        rate = (board.us - before) / 1e6 / max(time.perf_counter() - started, 1e-6)
        panel.speed = rate if not panel.speed else 0.9 * panel.speed + 0.1 * rate


def headless(panel: Panel, ms: float, script: str, shot: Path) -> None:
    pg = panel.pg
    surface = pg.Surface(panel.size)
    emu.run_script(panel.board, emu.parse_script(script), ms)
    panel.poll_midi()
    panel.draw(surface)
    pg.image.save(surface, str(shot))
    print(f"saved {shot}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=Path, help="an image, a .ks2fw package, or a raw body with --raw")
    parser.add_argument("--raw", action="store_true", help="the file is a body without the 64-byte header")
    parser.add_argument("--flash", type=Path, help="external flash contents: loaded if present, saved on exit")
    parser.add_argument("--exact", action="store_true", help="full instruction rate: slower, closer to the chip")
    parser.add_argument("--scale", type=float, default=0.8, help="window pixels per drawing unit (default 0.8)")
    parser.add_argument("--ms", type=float, help="run this long without a window, then save --shot")
    parser.add_argument("--script", default="", help='with --ms: timed actions, e.g. "2500 tap=seq; 3000 tap=play"')
    parser.add_argument("--shot", type=Path, help="where to save the picture")
    args = parser.parse_args()

    if args.ms is not None:
        if not args.shot:
            parser.error("--ms needs --shot")
        os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    try:
        import pygame
    except ModuleNotFoundError:
        sys.exit("error: needs pygame. Run: uv run --with unicorn --with pygame-ce tools/panel.py ...")

    flash = args.flash.read_bytes() if args.flash and args.flash.exists() else None
    board = emu.Board(emu.load(args.image, args.raw), flash, insns_per_us=emu.INSNS_PER_US if args.exact else FAST_RATE)
    pygame.init()
    emu.stop_on_signals(board)  # after pygame.init, which may install its own SIGTERM handler
    panel = Panel(board, args.scale)
    if args.ms is not None:
        headless(panel, args.ms, args.script, args.shot)
    else:
        interactive(panel, args.shot)
    if args.flash and board.flash.dirty:
        args.flash.write_bytes(board.flash.mem)
    if board.stopped:
        print(f"the firmware stopped: {board.stopped}")
        for line in board.log[-5:]:
            print(line)
    pygame.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
