#!/usr/bin/env python3
"""Checks that run the stock firmware and the SDK examples in the emulator.

    uv run --with unicorn tools/emutest.py                 # everything
    uv run --with unicorn tools/emutest.py stock cv        # only the checks whose name contains a word
    uv run --with unicorn tools/emutest.py --patterns export.keystep2    # also: patterns from a file

The SDK checks need the examples built (`just test` does that). Each check prints one line and
the exit status is the number of failures. These compare the emulator with itself and with the
few facts known from a real unit (docs/SYSEX.md); they say nothing about hardware.
"""

from __future__ import annotations

import argparse
import itertools
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu

ROOT = Path(__file__).resolve().parent.parent
STOCK = ROOT / "firmware/keystep_mk2__1_0_2.ks2fw"
ARTURIA = bytes([0xF0, 0x00, 0x20, 0x6B])
OP_VERSION, OP_READ_BLOCK = b"\xea\xf9", b"\x08\xdf"


def pack7(data: bytes) -> bytes:
    bits = "".join(f"{b:08b}"[::-1] for b in data)
    bits += "0" * (-len(bits) % 7)
    return bytes(int(bits[i : i + 7][::-1], 2) for i in range(0, len(bits), 7))


def unpack7(groups: bytes) -> bytes:
    bits = "".join(f"{g:07b}"[::-1] for g in groups)
    return bytes(int(bits[i : i + 8][::-1], 2) for i in range(0, len(bits) - 7, 8))


def boot(image: Path, ms: float, **kwargs) -> emu.Board:
    board = emu.Board(emu.load(image, False), **kwargs)
    board.run(ms)
    return board


def stock(**kwargs) -> emu.Board:
    return boot(STOCK, 3000, **kwargs)


def sdk(app: str, ms: float = 400, **kwargs) -> emu.Board:
    image = ROOT / f"sdk/build/{app}/{app}.img"
    if not image.exists():
        raise FileNotFoundError(f"{image.relative_to(ROOT)} is not built: run `just build {app}`")
    return boot(image, ms, **kwargs)


def request(board: emu.Board, seq: int, op: bytes, args: bytes = b"", ms: float = 60) -> bytes | None:
    """One Arturia request over USB MIDI; the reply's CBOR, or None."""
    host = board.usb.host
    host.midi_send(ARTURIA + pack7(bytes([seq]) + op + args) + b"\xf7")
    board.run(ms)
    reply = host.midi_receive(40)
    if not reply.startswith(ARTURIA):
        return None
    body = unpack7(reply[4:-1])
    return body[1:] if body[0] == seq | 0x80 else None


def tap(board: emu.Board, control: str, hold: float = 80, after: float = 40) -> None:
    board.press(control)
    board.run(hold)
    board.release(control)
    board.run(after)


def clock_interval(board: emu.Board, ms: int) -> float:
    """Mean time between MIDI clock bytes on the DIN output over the next `ms`."""
    times, seen = [], len(board.midi_out)
    for _ in range(ms):
        board.run(1)
        times += [board.us / 1000] * board.midi_out[seen:].count(0xF8)
        seen = len(board.midi_out)
    return (times[-1] - times[0]) / (len(times) - 1) if len(times) > 1 else 0.0


def pitch(note: int) -> int:
    """What the stock firmware puts on the pitch outputs with a blank calibration."""
    return (note - 36) * 65536 // 120


def near(value: float, target: float, tolerance: float) -> bool:
    return abs(value - target) <= tolerance


# ---- the stock firmware ----


def check_stock_usb() -> str:
    board = stock()
    info = board.usb.host.enumerate()
    assert (info["vendor"], info["product"], info["product_name"]) == (0x1C75, 0x0228, "KeyStep mk2"), info
    reply = request(board, 5, OP_VERSION)
    assert reply == b"\x691.0.2.782", reply  # the reply a real unit on 1.0.2 gave
    return "enumerates as 1C75:0228, version reply 1.0.2.782"


def check_stock_keys_cv() -> str:
    board = stock()
    board.usb.host.enumerate()
    seen = []
    for key in (0, 12, 31):
        board.midi_out.clear()
        board.press(f"k{key}")
        usb = board.usb.host.midi_receive(40)  # runs the board while the host polls
        out = board.outputs()
        assert bytes(board.midi_out[:2]) == bytes([0x90, 53 + key]), bytes(board.midi_out).hex(" ")
        assert usb[:2] == bytes([0x90, 53 + key]), usb.hex(" ")
        assert out["gate_pin"] == 1 and near(out["pitch"], pitch(53 + key), 2), out
        board.release(f"k{key}")
        board.usb.host.midi_receive(40)
        assert board.outputs()["gate_pin"] == 0, "gate still up after release"
        seen.append(out["pitch"])
    return f"keys send notes on DIN and USB, gate follows, pitch {seen}"


def check_stock_clock() -> str:
    board = stock()
    tap(board, "play")
    internal = clock_interval(board, 600)
    assert near(internal, 20.83, 0.3), internal  # 24 per quarter note at 120 BPM
    pulses = edges = 0
    for _ in range(500):
        board.run(1)
        level = board.outputs()["sync_clock"]
        edges += level and not pulses
        pulses = level
    assert 3 <= edges <= 5 and board.outputs()["sync_run"] == 1, edges  # one pulse per sixteenth
    return f"MIDI clock every {internal:.2f} ms, {edges} sync pulses in 0.5 s, run line up"


def check_stock_sync_in() -> str:
    board = stock()
    board.sync(0, False)
    board.run(100)
    tap(board, "play")
    board.midi_out.clear()
    for _ in range(40):  # a pulse every 50 ms: sixteenths at 300 BPM
        board.sync(0, True)
        board.run(10)
        board.sync(0, False)
        board.run(40)
    clocks = bytes(board.midi_out).count(0xF8)
    assert 200 <= clocks <= 250, clocks  # 6 per pulse; 96 if it had stayed on its own 120 BPM
    return f"follows the sync clock input: {clocks} MIDI clocks for 40 pulses"


def check_stock_din_in() -> str:
    board = stock()
    board.midi_in += [0x90, 60, 100]
    board.run(60)
    out = board.outputs()
    assert out["gate_pin"] == 1 and near(out["pitch"], pitch(60), 2), out
    board.midi_in += [0x80, 60, 0]
    board.run(60)
    assert board.outputs()["gate_pin"] == 0
    board.midi_out.clear()
    board.midi_in += [0xB0, 1, 90]
    board.run(60)
    assert bytes(board.midi_out) == bytes([0xB0, 1, 90]) and board.outputs()["mod2"] > 0
    return "a note on the DIN input drives pitch and gate; CC 1 passes through and moves Mod 2"


def check_stock_37() -> str:
    board = stock(keys37=True)
    assert not board.stopped
    notes = []
    for key in (0, 36):
        board.midi_out.clear()
        tap(board, f"k{key}", 40)
        notes.append(board.midi_out[1] if len(board.midi_out) > 1 else None)
    assert notes[0] is not None and notes[1] == notes[0] + 36, notes
    return f"strapped as the 37-key model: keys 0 and 36 send notes {notes}"


def check_patterns(path: Path) -> str:
    raw = path.read_bytes()
    blobs = {}
    for m in re.finditer(rb'"BINARY_FILE_(\d)_(\d+)_49152": "', raw):
        blobs[(int(m[1]) - 1) * 16 + int(m[2]) - 1] = raw[m.end() : m.end() + 4096]
    used = [n for n, blob in blobs.items() if blob != b"\xff" * 4096]
    assert used, "no patterns in the file"
    flash = bytearray(b"\xff" * 0x80000)
    for n, blob in blobs.items():
        flash[n * 0x1000 : (n + 1) * 0x1000] = blob
    board = stock(flash=bytes(flash))
    board.usb.host.enumerate()
    for index in (used[-1], used[0]):
        got = b""
        for seq in range(1, 140):
            reply = request(board, seq & 0x7F, OP_READ_BLOCK, bytes([index]) if index < 24 else bytes([0x18, index]))
            assert reply is not None, f"no answer for pattern {index} after {len(got)} bytes"
            if reply[0] != 0x58:
                break
            got += reply[2 : 2 + reply[1]]
        assert got == blobs[index], f"pattern {index} differs from flash block {index}"
    return f"patterns {used[-1]} and {used[0]} read over SysEx equal flash blocks of the same number"


# ---- the SDK examples ----


def check_sdk_usb() -> str:
    reference = stock().usb.host.enumerate()
    board = sdk("hello")
    host = board.usb.host
    info = host.enumerate()
    assert info["configuration"] == reference["configuration"]
    assert request(board, 5, OP_VERSION) == b"\x670.0.1.0"
    assert request(board, 6, b"\x2a\x89", b"\x19\x20\x00") is None  # other operations: no reply
    board.midi_out.clear()
    host.midi_send(bytes([0x90, 60, 100]))
    board.run(20)
    assert bytes(board.midi_out) == bytes([0x90, 60, 100])
    board.midi_in += [0x91, 64, 90]
    board.run(30)
    assert host.midi_receive(30) == bytes([0x91, 64, 90])
    return "stock descriptors, version reply 0.0.1.0, MIDI passes USB to DIN and back"


def check_sdk_cv() -> str:
    board = sdk("cv")
    board.press("k7")
    board.run(40)
    out = board.outputs()
    assert out["gate_pin"] == 1 and out["pitch"] == pitch(60), out
    board.release("k7")
    board.set_touch(0, 1.0)
    board.adc[:] = [1234, 3210]
    board.run(60)
    out = board.outputs()
    assert (out["gate_pin"], out["mod1"], out["mod2"]) == (0, 4095, 3210), out
    tap(board, "hold")
    assert board.outputs()["gate_format"] == 2
    return "pitch, gate, both mod outputs, gate format and the analog inputs"


def check_sdk_seq4() -> str:
    board = sdk("seq4")
    tap(board, "play", 40, 0)
    interval = clock_interval(board, 700)
    assert near(interval, 20.83, 0.1), interval
    data = bytes(b for b in board.midi_out if b != 0xF8)
    assert data[0] == 0xFA and {0x90, 0x91, 0x92} <= set(data), data.hex(" ")
    assert board.outputs()["sync_run"] == 1
    # track 1 shortened to three steps comes round again after three
    board.press("shift")
    board.run(40)
    tap(board, "k6", 40)
    board.release("shift")
    tap(board, "stop")
    board.midi_out.clear()
    tap(board, "play", 40, 0)
    board.run(1000)
    first = [i for i, b in enumerate(board.midi_out) if b == 0x90]
    notes = [board.midi_out[i + 1] for i in first]
    assert notes[:3] == [41, 41, 41], notes  # steps 1, 4 (41) of 16 became 1, 1, 1 of 3
    tap(board, "stop")
    assert board.outputs()["gate_pin"] == 0 and board.outputs()["sync_run"] == 0
    return f"clock every {interval:.2f} ms, three channels, a three-step track repeats, CV idle after stop"


def check_sdk_seq4_live() -> str:
    board = sdk("seq4")
    board.press("seq")
    board.run(40)
    tap(board, "k3", 40)  # track 4, empty in the demo
    board.release("seq")
    tap(board, "record")
    tap(board, "play", 40, 0)
    board.run(260)  # into step 3 (steps are 125 ms)
    tap(board, "k20", 60)
    board.run(2000 - 60 - 40 - 300)  # the rest of the 16 steps
    board.midi_out.clear()
    times = []
    for _ in range(2000):
        board.run(1)
        if bytes([0x93, 73]) in bytes(board.midi_out):
            times.append(board.us / 1000)
            board.midi_out.clear()
    assert len(times) == 1, f"the recorded note played {len(times)} times in one pass"
    return "a key played while recording and running comes back on the next pass of track 4"


def check_sdk_store() -> str:
    board = sdk("seq4")
    board.press("shift")
    board.run(40)
    tap(board, "k8", 40)  # track 1: five steps
    tap(board, "seq", 80, 800)  # save
    board.release("shift")
    again = sdk("seq4", flash=bytes(board.flash.mem))
    tap(again, "play", 40, 0)
    again.run(1400)
    notes = [again.midi_out[i + 1] for i, b in enumerate(again.midi_out) if b == 0x90]
    assert notes[:4] == [41, 41, 41, 41], notes  # steps 1 and 4, then 1 and 4 again after five steps
    changed = {i for i in range(0x80) if board.flash.mem[i << 12 : (i + 1) << 12] != b"\xff" * 0x1000}
    assert changed == {0x41}, changed
    return "seq4 saves to the first sector of the SDK area only and loads it on the next boot"


def check_sdk_flash() -> str:
    board = sdk("flashtest", 1500)
    text = board.oled.render()
    assert not board.stopped and board.flash.dirty and text.strip(), "flashtest did not run"
    stock_area = bytes(board.flash.mem[:0x41000]) + bytes(board.flash.mem[0x60000:])
    assert stock_area == b"\xff" * len(stock_area), "a write landed outside the SDK area"
    return "flashtest wrote only inside 0x41000-0x5FFFF"


# ---- KeyStep MK2000 (docs/MK2000.md) ----


def hold(board: emu.Board, *controls: str) -> None:
    for control in controls:
        board.press(control)
        board.run(50)


def let_go(board: emu.Board, *controls: str) -> None:
    for control in controls:
        board.release(control)
        board.run(50)


def notes(board: emu.Board) -> list[tuple[int, int]]:
    """(channel, note) of every note on sent on DIN since the last call."""
    data = bytes(b for b in board.midi_out if b < 0xF8)
    board.midi_out.clear()
    return [(data[i] & 15, data[i + 1]) for i in range(0, len(data) - 2, 3) if data[i] & 0xF0 == 0x90]


def mk2000(**kwargs) -> emu.Board:
    board = sdk("mk2000", 1700, **kwargs)  # past the start-up screen
    board.midi_out.clear()
    return board


def mk_record_steps(board: emu.Board, keys: tuple[str, ...]) -> None:
    tap(board, "record")
    for key in keys:
        tap(board, key, 60, 60)
    tap(board, "record")
    board.midi_out.clear()


def check_mk_channels() -> str:
    board = mk2000()
    tap(board, "k12")
    assert notes(board) == [(0, 65)], "keys start on track 1's channel"
    hold(board, "shift", "stop")
    tap(board, "k12")  # the key numbered 9
    let_go(board, "stop", "shift")
    board.press("k12")
    board.run(60)
    hold(board, "shift")
    tap(board, "stop", 100)  # a tap: back to Track, while the key is still down
    let_go(board, "shift")
    board.release("k12")
    board.run(60)
    data = bytes(board.midi_out)
    assert data == bytes([0x98, 65, data[2], 0x88, 65, 0]), f"note off must follow its note on: {data.hex(' ')}"
    board.midi_out.clear()
    tap(board, "k12")
    assert notes(board) == [(0, 65)], "a tap of Shift + Stop returns the keys to the track"
    hold(board, "shift", "stop", "seq")
    tap(board, "k8")  # numbered 5: channel of the selected track
    let_go(board, "seq", "stop", "shift")
    tap(board, "k12")
    assert notes(board) == [(4, 65)]
    return "home channel, tap to flip, track channel, and a held note ends on the channel it started on"


def check_mk_sequence() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k7", "hold", "k5"))  # note, note, rest, note
    tap(board, "play", 40, 0)
    interval = clock_interval(board, 1000)
    played = [n for _c, n in notes(board)]
    assert near(interval, 20.83, 0.1), interval
    assert played[:6] == [53, 60, 58, 53, 60, 58], played  # four steps, the third a rest
    hold(board, "seq")
    tap(board, "k1")  # track 2
    let_go(board, "seq")
    tap(board, "record")
    board.run(130)
    tap(board, "k20", 60)
    tap(board, "record")
    board.midi_out.clear()
    board.run(1000)
    second = [n for c, n in notes(board) if c == 1]
    assert len(second) == 2 and set(second) == {73}, f"the live note comes round once per pass: {second}"
    hold(board, "seq")
    tap(board, "k4")  # mute track 1
    let_go(board, "seq")
    board.midi_out.clear()
    board.run(600)
    assert {c for c, _n in notes(board)} == {1}, "a muted track is silent"
    tap(board, "stop")
    assert bytes(board.midi_out)[-1] == 0xFC and board.outputs()["gate_pin"] == 0
    return f"step recording with a rest, clock every {interval:.2f} ms, live recording on track 2, mute"


def check_mk_patterns() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2"))
    hold(board, "shift")
    tap(board, "seq")
    let_go(board, "shift")
    tap(board, "encoder", 80, 900)  # confirm the Save Pattern page
    hold(board, "shift")
    tap(board, "k5")  # pattern 2: empty
    let_go(board, "shift")
    tap(board, "play", 40, 0)
    board.run(500)
    assert notes(board) == [], "an empty pattern plays nothing"
    hold(board, "shift")
    tap(board, "k4")  # back to pattern 1, at the wrap of track 1
    let_go(board, "shift")
    board.midi_out.clear()
    board.run(2600)
    assert [n for _c, n in notes(board)][:2] == [53, 55]
    tap(board, "stop")
    board.run(2500)  # settings are written a moment after stopping
    used = {i for i in range(0x80) if board.flash.mem[i << 12 : (i + 1) << 12] != b"\xff" * 0x1000}
    assert used and used <= set(range(0x41, 0x60)), f"sectors written: {sorted(map(hex, used))}"
    again = mk2000(flash=bytes(board.flash.mem))
    tap(again, "play", 40, 0)
    again.run(400)
    assert [n for _c, n in notes(again)][:2] == [53, 55], "the saved pattern is back after a restart"
    return "save, switch to an empty pattern and back while playing, and the pattern survives a restart"


def turn(board: emu.Board, detents: int) -> None:
    board.turn(detents)
    encoder = board.encoders[0]
    for _ in range(400):  # a long turn takes the model about 12 ms a quarter detent
        if encoder.position == encoder.target:
            break
        board.run(20)
    board.run(200)


def timed_notes(board: emu.Board, ms: int) -> list[tuple[float, int]]:
    """(milliseconds from now, note) of every note on sent on DIN in the next `ms`."""
    found, start, seen = [], board.us / 1000, 0
    board.midi_out.clear()
    for _ in range(ms):
        board.run(1)
        data = bytes(b for b in board.midi_out if b < 0xF8)
        for i in range(seen, len(data) - 2, 3):
            if data[i] & 0xF0 == 0x90:
                found.append((board.us / 1000 - start, data[i + 1]))
        seen = len(data) - len(data) % 3
    board.midi_out.clear()
    return found


_reader = None


def small_text(board: emu.Board) -> str:
    """The text on the display in the small font, which reads reliably; the large font's 1, l
    and I look alike."""
    global _reader
    if _reader is None:
        import screentext

        _reader = screentext.Reader()
    return "   ".join(text for _y, _x, height, text in _reader.read_lines(board) if height < 12)


def step_field(board: emu.Board, field: int, detents: int) -> None:
    """Inside a step, at Velocity: go to a field, change it, and go back to Velocity."""
    turn(board, field)
    tap(board, "encoder")
    turn(board, detents)
    tap(board, "encoder")
    turn(board, -field)


def check_mk_step_page() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2"))  # F3, G3
    hold(board, "seq")
    turn(board, 1)  # opens the step page of track 1 on step 1
    let_go(board, "seq")
    assert "T1 Step 1" in small_text(board), small_text(board)
    tap(board, "encoder")  # into step 1
    step_field(board, 3, 2)  # Ratchet 1 -> 3
    tap(board, "back")
    tap(board, "k5")  # the numbered key 2: step 2
    assert "T1 Step 2" in small_text(board), small_text(board)
    tap(board, "encoder")
    board.midi_out.clear()
    tap(board, "k7")  # replaces G3 with C4, heard on track 1's channel
    assert notes(board) == [(0, 60)]
    tap(board, "back")
    tap(board, "back")
    board.press("play")  # the first hit comes while Play is still down
    played = timed_notes(board, 260)
    board.release("play")
    tap(board, "stop")
    assert [n for _t, n in played][:4] == [53, 53, 53, 60], played
    gaps = [b - a for (a, _), (b, _) in itertools.pairwise(played[:4])]
    assert all(near(g, 125 / 3, 3) for g in gaps), gaps  # a 125 ms step in three
    hold(board, "seq")
    turn(board, 1)
    let_go(board, "seq")
    hold(board, "seq")
    tap(board, "k1")  # the Seq layer still works over the page: track 2
    let_go(board, "seq")
    assert "T2 Step 1" in small_text(board), small_text(board)
    hold(board, "seq")
    tap(board, "k0")
    let_go(board, "seq")
    tap(board, "play", 40, 0)  # edits are made while it plays
    turn(board, 1)
    tap(board, "encoder")  # into step 2 of track 1
    step_field(board, 2, -20)  # Probability 100 -> 0
    turn(board, 3)  # at Ratchet the line below shows velocity, gate and probability
    assert re.search(r"\bP(rob)? 0\b", small_text(board)), small_text(board)  # shorter names when it is full
    tap(board, "back")
    tap(board, "back")
    board.midi_out.clear()
    board.run(600)
    tap(board, "stop")
    assert {n for _c, n in notes(board)} == {53}, "a step with probability 0 is silent"
    return "ratchet 3 plays three hits 42 ms apart, keys replace a step's notes, probability 0 is silent"


def check_mk_track_settings() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    hold(board, "seq")
    tap(board, "encoder")
    let_go(board, "seq")
    assert "T1 Settings" in small_text(board), small_text(board)
    turn(board, 5)  # Direction
    tap(board, "encoder")
    turn(board, 1)
    tap(board, "encoder")
    assert "Reverse" in small_text(board), small_text(board)
    tap(board, "back")
    tap(board, "play", 40, 0)
    board.run(1000)
    tap(board, "stop")
    played = [n for _c, n in notes(board)]
    assert played[:8] == [58, 57, 55, 53] * 2, played
    return "track settings: Direction Reverse plays the steps backwards"


def check_mk_global_settings() -> str:
    board = mk2000()
    tap(board, "encoder")
    assert "Global Settings" in small_text(board), small_text(board)
    tap(board, "encoder")  # Brightness: 10 of 10 to 5 of 10, and the lights follow at once
    turn(board, -5)
    tap(board, "encoder")
    assert board.leds[1][51 - 39] == (70, 70, 70), board.leds[1][51 - 39]  # the encoder's light
    turn(board, 1)  # Pattern change
    tap(board, "encoder")
    turn(board, 1)
    tap(board, "encoder")
    assert "At once" in small_text(board), small_text(board)
    turn(board, 9)  # About, the last entry
    tap(board, "encoder")
    assert "of 31 sectors" in small_text(board), small_text(board)
    tap(board, "back")
    tap(board, "back")
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    tap(board, "play", 40, 0)
    board.run(150)
    board.midi_out.clear()
    hold(board, "shift")
    tap(board, "k5")  # pattern 2, empty: takes over at once instead of at the wrap
    let_go(board, "shift")
    board.run(400)
    assert notes(board) == [], "the old pattern went on playing"
    return "brightness applies to the lights at once, About shows the free memory, Pattern change At once"


def check_mk_chain() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2"))  # pattern 1: F3 G3
    hold(board, "shift")
    tap(board, "seq")
    let_go(board, "shift")
    tap(board, "encoder", 80, 900)
    hold(board, "shift")
    tap(board, "k5")
    let_go(board, "shift")
    mk_record_steps(board, ("k7",))  # pattern 2: C4
    hold(board, "shift")
    tap(board, "seq")
    let_go(board, "shift")
    tap(board, "encoder", 80, 900)
    hold(board, "shift")
    tap(board, "play")
    tap(board, "k8")  # Shift still down from opening the page: the key goes to the chain
    assert "1 of 32" in small_text(board), small_text(board)
    tap(board, "k30")  # Shift + Clear empties it
    let_go(board, "shift")
    assert "0 of 32" in small_text(board), small_text(board)
    for key in ("k4", "k4", "k5", "k4", "k6"):
        tap(board, key)
    tap(board, "hold")  # takes the 3 off again
    turn(board, 1)
    shown = small_text(board).replace("0n", "On")  # the small font's O and 0 look alike
    assert "1 1 2 1" in shown and "4 of 32" in shown and "Chain On" in shown, shown
    tap(board, "encoder", 80, 900)  # closes the page and saves the chain
    order = [53, 55, 53, 55, 60, 53, 55]  # 1 1 2 1, the second 1 played again rather than skipped
    tap(board, "play", 40, 0)
    board.run(1800)
    tap(board, "stop")
    played = [n for _c, n in notes(board)]
    assert played[:14] == order * 2, played
    again = mk2000(flash=bytes(board.flash.mem))
    tap(again, "play", 40, 0)
    again.run(950)
    played = [n for _c, n in notes(again)]
    assert played[:7] == order, f"the chain after a restart: {played}"
    hold(again, "shift")
    tap(again, "k4")  # picking a pattern by hand switches the chain off
    let_go(again, "shift")
    again.run(300)  # pattern 1 takes over at the next wrap
    again.midi_out.clear()
    again.run(1000)
    played = [n for _c, n in notes(again)]
    assert len(played) >= 7 and set(played) == {53, 55}, played
    return "chain 1 1 2 1 plays in order and wraps, survives a restart, and a pattern picked by hand ends it"


def check_mk_identity() -> str:
    board = mk2000()
    info = board.usb.host.enumerate()
    assert info["product_name"] == "KeyStep MK2000", info["product_name"]
    assert request(board, 5, OP_VERSION) == b"\x670.1.0.0"
    return "enumerates as KeyStep MK2000 and answers the version request with 0.1.0.0"


def balanced(data: bytes) -> bool:
    """Every note on in a DIN capture has its note off, on the same channel."""
    data = bytes(b for b in data if b < 0xF8)
    sounding: set[tuple[int, int]] = set()
    for i in range(0, len(data) - 2, 3):
        key = (data[i] & 15, data[i + 1])
        if data[i] & 0xF0 == 0x90 and data[i + 2]:
            if key in sounding:
                return False
            sounding.add(key)
        else:
            sounding.discard(key)
    return not sounding


def check_mk_arp() -> str:
    board = mk2000()
    tap(board, "arp")
    for key in ("k7", "k11", "k14"):  # C4, E4, G4
        board.press(key)
        board.run(20)
    board.run(800)
    up = [n for _c, n in notes(board)]
    assert up[:6] == [60, 64, 67, 60, 64, 67], up
    hold(board, "arp")
    tap(board, "k5")  # numbered 2: Down
    tap(board, "k1")  # bank key 2: two octaves
    let_go(board, "arp")
    board.midi_out.clear()
    board.run(900)
    down = [n for _c, n in notes(board)]
    assert set(down) == {60, 64, 67, 72, 76, 79} and any(a > b for a, b in itertools.pairwise(down)), down
    tap(board, "hold")  # latch, then let go of the keys
    for key in ("k7", "k11", "k14"):
        board.release(key)
    board.run(600)
    board.midi_out.clear()
    board.run(500)
    assert len(notes(board)) >= 3, "the latched arp keeps running with no key down"
    hold(board, "shift", "stop")
    tap(board, "k12")  # home channel 9 while it runs
    let_go(board, "stop", "shift")
    board.midi_out.clear()
    board.run(500)
    assert {c for c, _n in notes(board)} == {8}, "the arp follows the keys to the home channel"
    board.midi_out.clear()
    tap(board, "arp")  # off
    board.run(300)
    tail = bytes(board.midi_out)
    board.midi_out.clear()
    board.run(400)
    assert notes(board) == [] and balanced(tail), "silent once the arp is off, with nothing left sounding"
    tap(board, "hold")  # latch off
    board.press("k7")
    board.run(60)
    tap(board, "hold")  # latch the played note
    board.release("k7")
    board.run(100)
    held = bytes(board.midi_out)
    tap(board, "hold")
    assert held.count(0x98) == 1 and 0x88 not in held and bytes(board.midi_out).count(0x88) == 1, (
        "Hold latches a played note"
    )
    return "Up over three keys, Down over two octaves, latch, the arp follows a channel change, Hold latches notes"


def all_text(board: emu.Board) -> str:
    """Every line on the display, with the large font's look-alikes 1, l and I folded into l."""
    small_text(board)
    return re.sub("[1I]", "l", "   ".join(text for _y, _x, _h, text in _reader.read_lines(board)))


def shows(board: emu.Board, words: str) -> bool:
    return re.sub("[1I]", "l", words) in all_text(board)


def shift_key(board: emu.Board, key: str) -> None:
    hold(board, "shift")
    tap(board, key)
    let_go(board, "shift")


def play_from_top(board: emu.Board, ms: int) -> list[tuple[float, int, int, int]]:
    """Play for `ms` and stop: (milliseconds, channel, note, velocity) of each note message, 0 for an off."""
    found, seen = [], 0
    board.midi_out.clear()
    board.press("play")
    start = board.us / 1000
    for i in range(ms):
        board.run(1)
        if i == 40:
            board.release("play")
        data = bytes(b for b in board.midi_out if b < 0xF8)
        for at in range(seen, len(data) - 2, 3):
            on = data[at] & 0xF0 == 0x90
            found.append((board.us / 1000 - start, data[at] & 15, data[at + 1], data[at + 2] if on else 0))
        seen = len(data) - len(data) % 3
    tap(board, "stop")
    board.midi_out.clear()
    return found


def played(board: emu.Board, ms: int = 560) -> list[int]:
    return [note for _t, _c, note, velocity in play_from_top(board, ms) if velocity]


def menu_to(board: emu.Board, name: str) -> None:
    """In a menu, not editing: turn to the entry called `name`."""
    for direction in (1, -1):
        for _ in range(16):
            if shows(board, name):
                return
            turn(board, direction)
    raise AssertionError(f"no menu entry {name!r}: {all_text(board)}")


def menu_set(board: emu.Board, name: str, detents: int) -> None:
    menu_to(board, name)
    tap(board, "encoder")
    turn(board, detents)
    tap(board, "encoder")


def save_pattern(board: emu.Board) -> None:
    shift_key(board, "seq")
    tap(board, "encoder", 80, 900)


def check_mk_edit_keys() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    base = [53, 55, 57, 58]
    save_pattern(board)
    assert played(board)[:4] == base
    shift_key(board, "k24")
    assert played(board)[:4] == [58, 53, 55, 57], "Nudge + moves every step one later"
    shift_key(board, "k23")
    shift_key(board, "k23")
    assert played(board)[:4] == [55, 57, 58, 53], "Nudge -"
    shift_key(board, "k31")
    assert played(board)[:4] == base, "Undo takes the last nudge back"
    shift_key(board, "k26")
    assert played(board)[:4] == [n + 12 for n in base], "Octave +"
    shift_key(board, "k25")
    shift_key(board, "k25")
    assert played(board)[:4] == [n - 12 for n in base], "Octave -"
    shift_key(board, "k26")
    shift_key(board, "k27")  # Double: eight steps
    hold(board, "shift", "tempo")
    assert shows(board, "8 steps"), all_text(board)
    let_go(board, "tempo", "shift")
    assert played(board, 1100)[:8] == base * 2
    for key, gap in (("k21", 62.5), ("k20", 125), ("k20", 250), ("k21", 125), ("k22", 250 / 3), ("k22", 125)):
        shift_key(board, key)
        times = [t for t, _c, _n, v in play_from_top(board, 700) if v]
        gaps = [b - a for a, b in itertools.pairwise(times)]
        assert gaps and all(near(g, gap, 2.5) for g in gaps), f"{key}: steps {gap} ms apart, got {gaps}"
    shift_key(board, "k30")
    assert played(board) == [], "Clear empties the track"
    shift_key(board, "k31")
    assert played(board, 1100)[:8] == base * 2, "Undo brings it back"
    shift_key(board, "k29")
    assert shows(board, "Reloaded"), all_text(board)
    board.run(1300)
    hold(board, "shift", "tempo")
    assert shows(board, "4 steps"), f"Reload returns to the saved pattern: {all_text(board)}"
    let_go(board, "tempo", "shift")
    assert played(board)[:4] == base
    return "nudge, octave, double, time division slower, faster and triplet, clear, undo, reload"


def check_mk_solo_tempo_transpose() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2"))
    hold(board, "seq")
    tap(board, "k1")
    let_go(board, "seq")
    mk_record_steps(board, ("k7", "k9"))
    tap(board, "play", 40, 0)
    board.run(300)
    board.midi_out.clear()
    board.run(500)
    assert {c for c, _n in notes(board)} == {0, 1}
    for key, channel in (("k8", 0), ("k9", 1)):  # the numbered keys 5 and 6: solo tracks 1 and 2
        hold(board, "seq", key)
        board.run(150)
        board.midi_out.clear()
        board.run(500)
        assert {c for c, _n in notes(board)} == {channel}, f"solo of track {channel + 1}"
        let_go(board, key, "seq")
    board.run(150)
    board.midi_out.clear()
    board.run(500)
    assert {c for c, _n in notes(board)} == {0, 1}, "both tracks play again once the solo key is up"
    hold(board, "tempo")
    turn(board, 20)
    let_go(board, "tempo")
    interval = clock_interval(board, 600)
    assert near(interval, 60000 / 140 / 24, 0.1), f"Tempo + 20 detents is 140 BPM: clock every {interval}"
    for _ in range(4):
        tap(board, "tempo", 60, 340)
    interval = clock_interval(board, 600)
    assert near(interval, 60000 / 150 / 24, 0.1), f"four taps 400 ms apart are 150 BPM: clock every {interval}"
    tap(board, "stop")
    hold(board, "seq")
    tap(board, "k0")
    let_go(board, "seq")
    shift_key(board, "hold")  # Transpose Hold
    board.midi_out.clear()
    tap(board, "k2")  # G3 against the track's first note F3: up two
    assert notes(board) == [], "the keys do not sound in Transpose Hold"
    first = [(c, n) for _t, c, n, v in play_from_top(board, 400) if v]
    assert {n for c, n in first if c == 0} == {55, 57}, first
    assert {n for c, n in first if c == 1} == {60, 62}, f"track 2 does not follow unless set to: {first}"
    shift_key(board, "hold")
    tap(board, "k2")
    assert notes(board) == [(0, 55)], "the keys play again after Transpose Hold"
    return "solo while held, tempo with the encoder and by tapping, Transpose Hold moves the selected track"


def check_mk_step_fields() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2"))
    before = play_from_top(board, 300)
    velocity = next(v for _t, _c, n, v in before if n == 53 and v)
    length = next(t for t, _c, n, v in before if n == 53 and not v) - before[0][0]
    hold(board, "seq")
    turn(board, 1)
    let_go(board, "seq")
    tap(board, "encoder")  # into step 1, at Velocity
    tap(board, "encoder")
    turn(board, -30)
    tap(board, "encoder")
    step_field(board, 5, 1)  # Tie
    tap(board, "back")
    tap(board, "back")
    after = play_from_top(board, 300)
    assert next(v for _t, _c, n, v in after if n == 53 and v) == velocity - 30, after
    tied = next(t for t, _c, n, v in after if n == 53 and not v) - after[0][0]
    assert near(length, 62.5, 4) and near(tied, 125, 4), f"a tied step lasts until the next: {length} then {tied}"
    hold(board, "seq")
    turn(board, 1)
    let_go(board, "seq")
    tap(board, "k4")
    tap(board, "encoder")
    step_field(board, 5, -1)
    step_field(board, 4, 1)  # Condition: 1 of 2
    tap(board, "back")
    tap(board, "back")
    assert played(board, 1040)[:6] == [53, 55, 55, 53, 55, 55], "a 1 of 2 step plays every other pass"
    hold(board, "shift", "tempo")
    turn(board, 18)
    assert shows(board, "20 steps"), all_text(board)
    let_go(board, "tempo", "shift")
    hold(board, "seq")
    turn(board, 1)
    let_go(board, "seq")
    tap(board, "k1")  # the second page of 16
    assert "T1 Step 17" in small_text(board), small_text(board)
    turn(board, 2)
    tap(board, "encoder")
    tap(board, "k7")
    tap(board, "back")
    tap(board, "back")
    late = [(t, n) for t, _c, n, v in play_from_top(board, 2450) if v and n == 60]
    assert len(late) == 1 and near(late[0][0], 18 * 125, 8), f"step 19 of 20 plays at 2250 ms: {late}"
    return "a step's velocity, tie and condition, and a step on the second page of a 20-step track"


def check_mk_track_menu() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    hold(board, "seq")
    tap(board, "encoder")
    let_go(board, "seq")
    menu_set(board, "Channel", 4)
    menu_set(board, "Shift", 7)
    menu_set(board, "Length", -1)
    events = play_from_top(board, 800)
    assert {c for _t, c, _n, _v in events} == {4}, "Channel 5"
    assert [n for _t, _c, n, v in events if v][:6] == [60, 62, 64] * 2, "three steps, seven semitones up"
    menu_set(board, "Swing", 25)
    menu_set(board, "Gate", 8)
    menu_set(board, "Length", 1)
    events = play_from_top(board, 800)
    starts = [t for t, _c, _n, v in events if v]
    gaps = [b - a for a, b in itertools.pairwise(starts)][:4]
    assert all(near(g, want, 4) for g, want in zip(gaps, [187.5, 62.5] * 2, strict=True)), f"75 % swing: {gaps}"
    held = next(t for t, _c, n, v in events if n == 60 and not v) - starts[0]
    assert near(held, 0.9 * 187.5, 6) or near(held, 0.9 * 125, 6), f"90 % gate: the first note lasts {held}"
    menu_set(board, "Follows transpose", 1)
    tap(board, "back")
    hold(board, "seq")
    tap(board, "k1")
    let_go(board, "seq")
    mk_record_steps(board, ("k7",))
    shift_key(board, "hold")
    tap(board, "k9")  # up two, for track 2 and whoever follows
    first = [(c, n) for _t, c, n, v in play_from_top(board, 300) if v]
    assert first[:2] == [(4, 62), (1, 62)] or first[:2] == [(1, 62), (4, 62)], f"track 1 follows track 2: {first}"
    return "track settings: channel, shift, length, swing, gate, follows transpose"


def check_mk_global_menu() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4"))
    tap(board, "encoder")
    menu_set(board, "Overdub", 1)
    menu_set(board, "Home channel", 9)
    menu_set(board, "Keys", -1)
    hold(board, "shift", "stop")  # a held layer over the open page
    assert shows(board, "MIDI Channel") or shows(board, "Channel"), all_text(board)
    tap(board, "k14")  # the numbered key 11
    let_go(board, "stop", "shift")
    assert shows(board, "Global Settings"), f"the page is back when the layer is let go: {all_text(board)}"
    tap(board, "back")
    board.midi_out.clear()
    tap(board, "k12")
    assert notes(board) == [(10, 65)], "Keys: Home, on the channel picked over the page"
    mk_record_steps(board, ("k7",))  # overdub: only the first step is replaced
    assert played(board)[:3] == [60, 55, 57], "Overdub keeps the rest of the track"
    save_pattern(board)
    shift_key(board, "k5")  # pattern 2, at 140 BPM
    hold(board, "tempo")
    turn(board, 20)
    let_go(board, "tempo")
    mk_record_steps(board, ("k9",))
    save_pattern(board)
    shift_key(board, "k4")
    tap(board, "play", 40, 0)
    shift_key(board, "k5")
    board.run(500)
    interval = clock_interval(board, 400)
    tap(board, "stop")
    assert near(interval, 20.83, 0.1), f"Tempo on change, When stopped: a running tempo stays, got {interval}"
    tap(board, "encoder")
    menu_set(board, "Tempo on change", 1)
    tap(board, "back")
    shift_key(board, "k4")
    tap(board, "play", 40, 0)
    shift_key(board, "k5")
    board.run(500)
    interval = clock_interval(board, 400)
    tap(board, "stop")
    assert near(interval, 60000 / 140 / 24, 0.1), f"Tempo on change, Always: the new pattern's tempo, got {interval}"
    return "overdub, keys on Home, home channel, a held layer over the page, tempo on pattern change"


# ---- MK2000: scales, chords and the arp settings ----

KEYS_FROM_C = [f"k{k}" for k in range(7, 19)]  # C4 to B4


def struck(board: emu.Board, key: str, ms: float = 60) -> list[int]:
    """The notes a key switches on while it is held for `ms`; it is let go afterwards."""
    board.midi_out.clear()
    board.press(key)
    board.run(ms)
    found = [n for _c, n in notes(board)]
    board.release(key)
    board.run(40)
    board.midi_out.clear()
    return found


def held_page(board: emu.Board, button: str) -> None:
    hold(board, button)
    tap(board, "encoder")
    let_go(board, button)


def check_mk_scale() -> str:
    board = mk2000()
    shift_key(board, "chord")
    assert shows(board, "Scale") and shows(board, "Global Scale"), all_text(board)
    menu_set(board, "Global Scale", 2)
    tap(board, "back")
    minor = [struck(board, key) for key in KEYS_FROM_C]
    assert minor == [[60 + n] for n in (0, 0, 2, 3, 3, 5, 5, 7, 8, 8, 10, 10)], f"C minor, as stock plays it: {minor}"
    assert "C Min" in small_text(board), f"the scale is on the home screen: {small_text(board)}"
    shift_key(board, "chord")
    menu_set(board, "Global Root", 2)
    tap(board, "back")
    d_minor = [struck(board, key) for key in ("k8", "k10", "k11", "k15")]  # C#, D#, E, G#
    assert d_minor == [[60], [62], [64], [67]], f"D minor: {d_minor}"
    shift_key(board, "chord")
    menu_set(board, "Pattern Scale", 1)  # from Global to Major, root C
    tap(board, "back")
    assert [struck(board, key) for key in ("k8", "k10", "k13")] == [[60], [62], [65]], "the pattern's scale wins"
    assert "C Maj" in small_text(board), small_text(board)
    shift_key(board, "chord")
    menu_set(board, "Pattern Root", 7)
    menu_set(board, "Pattern Scale", 10)  # Pentatonic Minor on G: G A# C D F
    tap(board, "back")
    penta = [struck(board, key)[0] for key in KEYS_FROM_C]
    assert penta == [60, 60, 62, 62, 65, 65, 65, 67, 67, 70, 70, 70], f"G minor pentatonic: {penta}"
    mk_record_steps(board, ("k8", "k11"))  # what is recorded is what sounded
    assert played(board)[:2] == [60, 65], "recording takes the fitted notes"
    save_pattern(board)
    shift_key(board, "chord")
    menu_set(board, "Pattern Scale", -20)  # Off
    tap(board, "back")
    assert struck(board, "k8") == [61], "Pattern Scale Off: every key, whatever the global scale"
    shift_key(board, "k29")  # Reload
    board.run(200)
    assert struck(board, "k8") == [60], "the scale is saved with the pattern"
    tap(board, "arp")
    board.midi_out.clear()
    for key in ("k7", "k8", "k11"):  # C, C#, E: fitted to C, C, F
        board.press(key)
        board.run(20)
    board.run(700)
    arp = {n for _c, n in notes(board)}
    assert arp == {60, 65}, f"the arp plays the fitted notes: {arp}"
    return "global and pattern scale and root on the keys, recording and the arp; saved with the pattern"


def check_mk_chord() -> str:
    board = mk2000()
    tap(board, "chord")
    assert shows(board, "Chord On"), all_text(board)
    assert struck(board, "k7") == [60], "the user chord starts empty: the key alone, as stock"
    hold(board, "chord")
    assert shows(board, "User"), all_text(board)
    turn(board, 3)
    assert shows(board, "Maj"), all_text(board)
    let_go(board, "chord")
    board.midi_out.clear()
    board.press("k7")
    board.run(60)
    board.release("k7")
    board.run(60)
    data = bytes(board.midi_out)
    assert [n for _c, n in notes(board)] == [60, 64, 67] and balanced(data) and data.count(0x80) == 3, data.hex()
    # two keys sharing a pitch: C major and E minor both hold E and G
    held_page(board, "chord")
    assert shows(board, "Chord") and shows(board, "Preset"), all_text(board)
    menu_set(board, "Preset", 1)  # min
    tap(board, "back")
    board.press("k11")
    board.run(60)
    board.midi_out.clear()
    held_page(board, "chord")
    menu_set(board, "Preset", -1)
    tap(board, "back")
    board.press("k7")
    board.run(60)
    board.release("k7")
    board.run(60)
    data = bytes(b for b in board.midi_out if b < 0xF8)
    state: dict[int, bool] = {}
    for i in range(0, len(data) - 2, 3):
        state[data[i + 1]] = data[i] & 0xF0 == 0x90
    assert state == {60: False, 64: True, 67: True}, f"E and G stay on for the key still down: {state}"
    board.release("k11")
    board.run(60)
    data = bytes(b for b in board.midi_out if b < 0xF8)
    for i in range(0, len(data) - 2, 3):
        state[data[i + 1]] = data[i] & 0xF0 == 0x90
    assert not any(state.values()), f"everything ends with the last key: {state}"
    board.midi_out.clear()
    # strum
    held_page(board, "chord")
    menu_set(board, "Strum (ms)", 10)
    menu_set(board, "Strum Type", 2)
    assert shows(board, "Down"), all_text(board)
    tap(board, "back")
    board.press("k7")
    strummed = timed_notes(board, 100)
    board.release("k7")
    board.run(60)
    assert [n for _t, n in strummed] == [67, 64, 60], strummed
    gaps = [b[0] - a[0] for a, b in itertools.pairwise(strummed)]
    assert all(near(g, 20, 3) for g in gaps), f"20 ms from note to note, as stock: {gaps}"
    board.midi_out.clear()
    board.press("k7")
    board.run(30)
    board.release("k7")
    board.run(100)
    data = bytes(board.midi_out)
    assert [n for _c, n in notes(board)] == [67, 64] and balanced(data), "a key let go mid-strum: the rest never starts"
    held_page(board, "chord")
    menu_set(board, "Strum (ms)", -40)
    menu_set(board, "Strum Type", -2)
    menu_set(board, "Voicing", 1)
    tap(board, "back")
    assert struck(board, "k9") == [50, 62, 66, 69], "Bass -1: the root an octave down as well"
    held_page(board, "chord")
    menu_set(board, "Voicing", -1)
    tap(board, "back")
    # the user chord, the stock way: Chord held, keys played
    hold(board, "chord")
    for key in ("k14", "k7", "k10"):
        tap(board, key)
    assert shows(board, "User Chord") and shows(board, "3 notes"), all_text(board)
    let_go(board, "chord")
    assert shows(board, "User Chord Recorded"), all_text(board)
    assert struck(board, "k9") == [62, 65, 69], "the user chord moves with the key"
    # in a scale every note of the chord is fitted, as stock does
    shift_key(board, "chord")
    menu_set(board, "Global Scale", 1)
    tap(board, "back")
    assert struck(board, "k7") == [60, 62, 67], "C minor shape in C major: Eb comes down to D"
    shift_key(board, "chord")
    menu_set(board, "Global Scale", -1)
    tap(board, "back")
    # step recording takes the chord; the arp plays over it
    mk_record_steps(board, ("k7", "k9"))
    first = [n for _t, _c, n, v in play_from_top(board, 200) if v]
    assert sorted(first[:3]) == [60, 63, 67] and sorted(first[3:6]) == [62, 65, 69], f"chords in the steps: {first}"
    tap(board, "arp")
    board.midi_out.clear()
    board.press("k7")
    board.run(700)
    arp = [n for _c, n in notes(board)]
    board.release("k7")
    board.run(200)
    assert arp[:6] == [60, 63, 67] * 2, f"the arp runs over the chord of one key: {arp}"
    tap(board, "arp")
    tap(board, "chord")
    assert shows(board, "Chord Off") and struck(board, "k7") == [60], all_text(board)
    return "presets, shared pitches, strum, voicing, user chord, chords in a scale, in steps and in the arp"


def check_mk_arp_settings() -> str:
    board = mk2000()
    tap(board, "arp")

    def arp_notes(ms: float) -> list[int]:
        board.midi_out.clear()
        for key in ("k7", "k11", "k14"):
            board.press(key)
        board.run(ms)
        found = [n for _c, n in notes(board)]
        for key in ("k7", "k11", "k14"):
            board.release(key)
        board.run(200)
        return found

    held_page(board, "arp")
    assert shows(board, "Arp Settings") and shows(board, "Arp Length"), all_text(board)
    assert shows(board, "l6 steps"), f"16 steps to begin with, as stock: {all_text(board)}"
    menu_set(board, "Arp Length", -12)
    tap(board, "back")
    four = arp_notes(1400)
    assert four[:10] == [60, 64, 67, 60, 60, 64, 67, 60, 60, 64], f"Arp Length 4, as stock plays it: {four}"
    held_page(board, "arp")
    menu_set(board, "Arp Retrigger", -1)
    tap(board, "back")
    free = arp_notes(1400)
    assert free[:10] == [60, 64, 67] * 3 + [60], f"Arp Retrigger Off: {free}"
    held_page(board, "arp")
    menu_set(board, "Mode", 1)
    menu_set(board, "Octaves", 1)
    menu_set(board, "Time Division", 3)  # 1/16 to 1/32
    tap(board, "back")
    board.midi_out.clear()
    for key in ("k7", "k11", "k14"):
        board.press(key)
    fast = timed_notes(board, 500)
    for key in ("k7", "k11", "k14"):
        board.release(key)
    board.run(200)
    assert [n for _t, n in fast][:6] == [79, 76, 72, 67, 64, 60], f"Down over two octaves: {fast}"
    gaps = [b[0] - a[0] for a, b in itertools.pairwise(fast)]
    assert all(near(g, 62.5, 3) for g in gaps), f"1/32 at 120 BPM: {gaps}"
    return "Arp Length and Retrigger as stock, and mode, octaves and time division from the page"


# ---- MK2000: sync and routing ----


def clocked(board: emu.Board, interval: float, count: int, send=None) -> list[tuple[float, int]]:
    """Send `count` clocks `interval` ms apart (MIDI clock on DIN unless `send` is given) and
    return (ms from the first clock, note) of each note on that comes out on DIN meanwhile."""
    send = send or (lambda: board.midi_in.append(0xF8))
    found, seen, start = [], 0, board.us / 1000
    board.midi_out.clear()
    for k in range(count):
        send()
        while board.us / 1000 < start + (k + 1) * interval:
            board.run(min(1.0, start + (k + 1) * interval - board.us / 1000))
            data = bytes(b for b in board.midi_out if b < 0xF8)
            for i in range(seen, len(data) - 2, 3):
                if data[i] & 0xF0 == 0x90:
                    found.append((board.us / 1000 - start, data[i + 1]))
            seen = len(data) - len(data) % 3
    return found


def ext_tempo(board: emu.Board) -> float:
    """The tempo the home screen shows for the clock it follows, or 0."""
    m = re.search(r"Ext Tempo: (\d+\.\d)", small_text(board))
    return float(m[1]) if m else 0.0


def gaps(found: list[tuple[float, int]]) -> list[float]:
    return [b - a for (a, _), (b, _) in itertools.pairwise(found)]


def global_setting(board: emu.Board, name: str, detents: int) -> None:
    tap(board, "encoder")
    menu_set(board, name, detents)
    tap(board, "back")


def jack_pulse(board: emu.Board) -> None:
    board.sync(0, True)
    board.run(5)
    board.sync(0, False)


def check_mk_sync_midi() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    global_setting(board, "Clock Receive", 3)  # MIDI Din
    assert "Ext Tempo: --" in small_text(board), small_text(board)
    board.midi_in.append(0xFA)
    board.midi_out.clear()
    slow = clocked(board, 60000 / 120 / 24, 96)  # four beats at 120 BPM
    shown = ext_tempo(board)
    clocks_out = bytes(board.midi_out).count(0xF8)
    fast = clocked(board, 60000 / 150 / 24, 96)
    assert [n for _t, n in slow[:5]] == [53, 55, 57, 58, 53], slow
    assert slow[0][0] < 3, f"the first step comes with the first clock: {slow[0]}"
    assert all(near(g, 125, 2) for g in gaps(slow)), gaps(slow)
    assert all(near(g, 100, 2) for g in gaps(fast)[1:]), gaps(fast)
    assert near(shown, 120, 0.15), f"the home screen shows the tempo followed: {shown}"
    assert 95 <= clocks_out <= 96, f"one clock out per clock in, none doubled: {clocks_out}"
    return f"follows DIN clock at 120 and 150 BPM: steps {gaps(slow)[1]:.1f} and {gaps(fast)[1]:.1f} ms apart"


def check_mk_sync_transport() -> str:
    board = mk2000()
    host = board.usb.host
    host.enumerate()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    global_setting(board, "Clock Receive", 2)  # USB
    interval = 60000 / 120 / 24
    host.midi_send(bytes([0xFA]))
    first = clocked(board, interval, 48, lambda: host.midi_send(bytes([0xF8])))
    assert [n for _t, n in first[:4]] == [53, 55, 57, 58], first
    host.midi_send(bytes([0xFC]))
    board.run(20)
    assert bytes(board.midi_out).count(0xFC) == 1, "Stop from USB stops and is sent on"
    stopped = clocked(board, interval, 24, lambda: host.midi_send(bytes([0xF8])))
    assert stopped == [], stopped
    host.midi_send(bytes([0xFA]))
    again = clocked(board, interval, 24, lambda: host.midi_send(bytes([0xF8])))
    assert [n for _t, n in again[:2]] == [53, 55], f"Start plays from the top: {again}"
    tap(board, "stop")
    tap(board, "play")  # Play while following: armed until the next clock
    board.midi_out.clear()
    board.run(200)
    assert notes(board) == [], "Play waits for the clock it follows"
    armed = clocked(board, interval, 12, lambda: host.midi_send(bytes([0xF8])))
    assert armed and armed[0][1] == 53 and armed[0][0] < 3, armed
    return "Start, Stop and Start again from USB; local Play waits for the next clock"


def check_mk_sync_jack() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    global_setting(board, "Clock Receive", 4)  # Sync In
    board.sync(0, False)  # a cable in the jack
    tap(board, "play")
    board.midi_out.clear()
    board.run(300)
    assert notes(board) == [], "Play waits for the first pulse"
    slow = clocked(board, 100, 12, lambda: jack_pulse(board))  # sixteenths at 150 BPM
    clocks_out = bytes(board.midi_out).count(0xF8)
    fast = clocked(board, 62.5, 12, lambda: jack_pulse(board))  # 240 BPM
    assert [n for _t, n in slow[:5]] == [53, 55, 57, 58, 53], slow
    assert all(near(g, 100, 2) for g in gaps(slow)), gaps(slow)
    assert all(near(g, 62.5, 2) for g in gaps(fast)[1:]), gaps(fast)
    assert 70 <= clocks_out <= 72, f"six MIDI clocks out per pulse: {clocks_out}"
    tap(board, "stop")
    return f"follows the sync jack, a step per pulse: {gaps(slow)[1]:.1f} and {gaps(fast)[1]:.1f} ms apart"


def check_mk_sync_auto() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "k2", "k4", "k5"))
    board.midi_in.append(0xFA)  # Auto: the first port to send Start or clock
    board.midi_out.clear()
    found = clocked(board, 60000 / 150 / 24, 48)
    assert near(ext_tempo(board), 150, 0.15), small_text(board)
    assert all(near(g, 100, 2) for g in gaps(found)), gaps(found)
    board.midi_in.append(0xFC)
    board.run(2500)
    assert "Tempo: 120.0" in small_text(board) and "Ext" not in small_text(board), small_text(board)
    tap(board, "play", 40, 0)
    board.midi_out.clear()
    board.run(1000)
    assert len(notes(board)) == 8, "back on its own clock once the clock went quiet"
    tap(board, "stop")
    return "Auto follows DIN Start and clock, and its own clock again 2 s after the clock stops"


def framed(data: bytes) -> list[bytes]:
    """The messages of a MIDI stream with real time bytes left out; fails if one is cut short."""
    out, at, data = [], 0, bytes(b for b in data if b < 0xF8)
    while at < len(data):
        status = data[at]
        assert status & 0x80, f"data byte without a status at {at}: {data.hex(' ')}"
        if status == 0xF0:
            end = data.index(0xF7, at)
            assert all(b < 0x80 for b in data[at + 1 : end]), f"SysEx broken up: {data[at : end + 1].hex(' ')}"
            size = end + 1 - at
        else:
            size = 2 if status & 0xE0 == 0xC0 else 3
            assert all(b < 0x80 for b in data[at + 1 : at + size]), f"message cut short: {data.hex(' ')}"
        out.append(data[at : at + size])
        at += size
    return out


def check_mk_thru() -> str:
    board = mk2000()
    host = board.usb.host
    host.enumerate()
    for track, keys in enumerate((("k0", "k2", "k4", "k5"), ("k7", "k9"))):
        hold(board, "seq")
        tap(board, f"k{track}")
        let_go(board, "seq")
        mk_record_steps(board, keys)
    global_setting(board, "Clock Receive", 1)  # Internal
    global_setting(board, "MIDI Thru", 1)
    global_setting(board, "USB to MIDI", 1)
    tap(board, "play", 40, 0)
    board.midi_out.clear()
    started = board.us / 1000
    sysex = bytes([0xF0, 0x7D, 1, 2, 3, 4, 5, 6, 7, 8, 0xF7])
    din_in = [0x99, 40, 100, 0xFE, 41, 100, 0xB9, 7, 90, 0xF8, *sysex[:5], 0xF8, *sysex[5:], 0x89, 40, 0, 41, 0]
    usb_out = b""
    for burst in range(8):
        board.midi_in += din_in
        host.midi_send(bytes([0x9A, 50 + burst, 90, 0x8A, 50 + burst, 0]))
        usb_out += host.midi_receive(60)
        board.run(40)
        usb_out += host.midi_receive(20)
    own = (board.us / 1000 - started) / (60000 / 120 / 24)
    tap(board, "stop")
    usb_out += host.midi_receive(40)
    din = framed(bytes(board.midi_out))
    usb = framed(usb_out)
    forwarded = [bytes([0x99, 40, 100]), bytes([0x99, 41, 100]), bytes([0xB9, 7, 90]), sysex]
    for messages, name in ((din, "DIN"), (usb, "USB")):
        for m in forwarded:
            assert messages.count(m) == 8, f"{name} out: {m.hex(' ')} {messages.count(m)} times"
    assert sum(m == bytes([0x9A, 50, 90]) for m in din) == 1 and not any(m[0] & 15 == 10 for m in usb)
    assert balanced(bytes(b"".join(m for m in din if len(m) == 3))), "every note off follows its note on"
    assert {0, 1} <= {m[0] & 15 for m in din if m[0] & 0xF0 == 0x90}, "the sequence played meanwhile"
    clocks = bytes(board.midi_out).count(0xF8)
    assert own - 2 <= clocks <= own + 3, f"{clocks} clocks in the time of {own:.0f}: incoming ones passed on"
    assert bytes(board.midi_out).count(0xFE) == 8 and 0xFE in usb_out
    board.midi_out.clear()
    assert request(board, 7, OP_VERSION) == b"\x670.1.0.0"
    assert bytes(board.midi_out) == b"", "the unit's own SysEx requests stay off DIN"
    global_setting(board, "MIDI Thru", -1)
    board.midi_out.clear()
    board.midi_in += [0x90, 60, 100, 0xF8]
    board.run(50)
    assert bytes(board.midi_out) == b"", "MIDI Thru off"
    return f"DIN in to DIN and USB, USB in to DIN, whole messages among the sequence's, its own {clocks} clocks only"


def note_ons(board: emu.Board) -> list[int]:
    data = bytes(b for b in board.midi_out if b < 0xF8)
    return [data[i + 1] for i in range(0, len(data) - 2, 3) if data[i] & 0xF0 == 0x90]


def record_live_at(board: emu.Board, key: str, note: int, after: float) -> float:
    """Play, Record on, `key` played `after` ms past the track's first note (F3), Record off, Stop.
    Returns when the key's note went out, in ms past that F3; checks it sounded once in that pass."""
    tap(board, "play", 40, 0)
    tap(board, "record")
    board.midi_out.clear()
    for _ in range(1000):
        board.run(1)
        if 53 in note_ons(board):
            break
    first = board.us / 1000
    board.midi_out.clear()
    board.run(first + after - board.us / 1000)
    board.press(key)
    for _ in range(50):
        board.run(1)
        if note in note_ons(board):
            break
    live = board.us / 1000 - first
    board.run(60)
    board.release(key)
    board.run(first + 480 - board.us / 1000)
    assert note_ons(board).count(note) == 1, f"heard once in the pass it was played: {note_ons(board)}"
    tap(board, "record")
    tap(board, "stop")
    board.midi_out.clear()
    return live


def after_first(board: emu.Board, note: int) -> list[float]:
    """Play two passes of a 500 ms track: ms from each pass's F3 to `note`."""
    events = [(t, n) for t, _c, n, v in play_from_top(board, 1040) if v]
    starts = [t for t, n in events if n == 53]
    return [t - max(s for s in starts if s <= t) for t, n in events if n == note and starts[0] <= t]


def check_mk_rec_quantize() -> str:
    board = mk2000()
    mk_record_steps(board, ("k0", "hold", "k4", "hold"))  # F3, rest, A3, rest: 500 ms
    shift_key(board, "record")  # Overdub: the live notes go into the rests
    global_setting(board, "Rec Quantize", -1)
    live = record_live_at(board, "k7", 60, 148)  # the key scan adds some 7 ms: 30 ms into step 2
    assert near(live, 155, 6), f"the key went out {live} ms after the first step"
    late = after_first(board, 60)
    assert len(late) == 2 and all(near(t, live, 6) for t in late), f"played back as played, {live}: {late}"
    shift_key(board, "k28")
    assert shows(board, "Track Quantized"), all_text(board)
    grid = after_first(board, 60)
    assert len(grid) == 2 and all(near(t, 125, 3) for t in grid), f"Quant puts it on the step: {grid}"
    shift_key(board, "k31")
    undone = after_first(board, 60)
    assert len(undone) == 2 and all(near(t, live, 6) for t in undone), f"Undo puts it back: {undone}"
    hold(board, "seq")
    tap(board, "encoder")
    let_go(board, "seq")
    menu_to(board, "Quantize")
    tap(board, "encoder")
    turn(board, -1)  # 50 %, as the stock Seq Settings offer
    tap(board, "encoder")
    tap(board, "back")
    half = after_first(board, 60)
    want = 125 + (live - 125) / 2
    assert len(half) == 2 and all(
        near(t, want, 6) for t in half
    )  # steps start on ticks of 5.2 ms, f"Quantize 50 %: half way, {want}: {half}"
    shift_key(board, "k31")
    save_pattern(board)
    board.run(2500)  # and the settings, a moment after
    again = mk2000(flash=bytes(board.flash.mem))
    kept = after_first(again, 60)
    assert len(kept) == 2 and all(near(t, live, 6) for t in kept), f"off the grid after a restart: {kept}"
    global_setting(again, "Rec Quantize", 1)
    record_live_at(again, "k9", 62, 405)  # 30 ms into step 4, quantized
    snapped = after_first(again, 62)
    assert len(snapped) == 2 and all(near(t, 375, 3) for t in snapped), f"Rec Quantize On: on the step: {snapped}"
    hold(again, "seq")
    turn(again, 1)
    let_go(again, "seq")
    tap(again, "k6")  # step 3, A3
    tap(again, "encoder")
    turn(again, 6)  # Offset, after Tie
    tap(again, "encoder")
    turn(again, -5)
    tap(again, "encoder")
    assert shows(again, "Offset") and shows(again, "-25 %"), all_text(again)
    tap(again, "back")
    tap(again, "back")
    early = after_first(again, 57)
    assert len(early) == 2 and all(near(t, 250 - 31.25, 3) for t in early), f"Offset -25 %: {early}"
    return f"Rec Quantize Off keeps a note {live - 125:.0f} ms late, Quant, Undo, a restart, On snaps, step Offset"


def check_mk_count_in() -> str:
    board = mk2000()
    tap(board, "record")
    board.press("play")
    started, pressed = None, board.us
    for ms in range(2300):
        board.run(1)
        if ms == 40:
            board.release("play")
        if ms == 1200:
            assert shows(board, "Count-in: 2"), all_text(board)
            tap(board, "k0", 30, 0)  # a key during the count-in sounds and is not recorded
        if started is None and 0xFA in board.midi_out:
            started = (board.us - pressed) / 1000
    assert started is not None and near(started, 2000, 12), f"the pattern starts after a bar of 2 s: {started}"
    board.run(started + 2250 - (board.us - pressed) / 1000 - 30)  # to the third step of the second pass
    tap(board, "k7", 60)
    tap(board, "record")
    board.run(1500)
    board.midi_out.clear()
    times = [(t, n) for t, n in timed_notes(board, 4000)]
    assert {n for _t, n in times} == {60} and len(times) == 2, f"only the note played after the count-in: {times}"
    assert near(times[1][0] - times[0][0], 2000, 3), times
    tap(board, "stop")
    tap(board, "record")
    tap(board, "play")
    board.run(500)
    tap(board, "stop")
    board.run(100)
    board.midi_out.clear()
    board.run(2500)
    assert 0xFA not in board.midi_out, "Stop during the count-in calls it off"
    return "Record then Play counts a bar, starts on the bar, records after it; Stop calls it off"


CHECKS = {
    "stock usb": check_stock_usb,
    "stock keys cv": check_stock_keys_cv,
    "stock clock": check_stock_clock,
    "stock sync in": check_stock_sync_in,
    "stock din in": check_stock_din_in,
    "stock 37 keys": check_stock_37,
    "sdk usb": check_sdk_usb,
    "sdk cv": check_sdk_cv,
    "sdk seq4": check_sdk_seq4,
    "sdk seq4 live": check_sdk_seq4_live,
    "sdk store": check_sdk_store,
    "sdk flash": check_sdk_flash,
    "mk2000 identity": check_mk_identity,
    "mk2000 channels": check_mk_channels,
    "mk2000 sequence": check_mk_sequence,
    "mk2000 patterns": check_mk_patterns,
    "mk2000 arp": check_mk_arp,
    "mk2000 step page": check_mk_step_page,
    "mk2000 track settings": check_mk_track_settings,
    "mk2000 global settings": check_mk_global_settings,
    "mk2000 chain": check_mk_chain,
    "mk2000 edit keys": check_mk_edit_keys,
    "mk2000 solo tempo transpose": check_mk_solo_tempo_transpose,
    "mk2000 step fields": check_mk_step_fields,
    "mk2000 track menu": check_mk_track_menu,
    "mk2000 global menu": check_mk_global_menu,
    "mk2000 scale": check_mk_scale,
    "mk2000 chord": check_mk_chord,
    "mk2000 arp settings": check_mk_arp_settings,
    "mk2000 count-in": check_mk_count_in,
    "mk2000 rec quantize": check_mk_rec_quantize,
    "mk2000 sync midi": check_mk_sync_midi,
    "mk2000 sync transport": check_mk_sync_transport,
    "mk2000 sync jack": check_mk_sync_jack,
    "mk2000 sync auto": check_mk_sync_auto,
    "mk2000 thru": check_mk_thru,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("words", nargs="*", help="run only the checks whose name contains one of these")
    parser.add_argument(
        "--patterns", type=Path, help="a .keystep2 export: check that the stock firmware reads it from flash"
    )
    args = parser.parse_args()
    checks = dict(CHECKS)
    if args.patterns:
        checks["stock patterns"] = lambda: check_patterns(args.patterns)
    failures = 0
    for name, check in checks.items():
        if args.words and not any(word in name for word in args.words):
            continue
        try:
            print(f"ok    {name}: {check()}", flush=True)
        except (AssertionError, FileNotFoundError) as e:
            failures += 1
            print(f"FAIL  {name}: {e}", flush=True)
    print(f"{failures} failed" if failures else "all passed")
    return failures


if __name__ == "__main__":
    sys.exit(main())
