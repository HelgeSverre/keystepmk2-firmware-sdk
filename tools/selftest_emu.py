#!/usr/bin/env python3
"""Run sdk/examples/selftest in the emulator from start to summary, doing what a person would.

    just build selftest
    uv run --with unicorn tools/selftest_emu.py            # every part
    uv run --with unicorn tools/selftest_emu.py good       # only the parts whose name contains a word

The script reads the instructions off the emulated display and presses, turns and touches as
they say, collects the SysEx the firmware sends over USB, and gives it to tools/selftest.py.
Parts: `good` (everything done right: every check must be OK), `bad` (a second run on the same
board with mistakes and faults: the right checks must FAIL or SKIP), `din` (no USB host: the
report must come out of the DIN socket). Prints one line per assertion; the exit status is 1
if any failed.

This shows the firmware against the emulator's model of the board. It says nothing about a unit.
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu
import emutest
import selftest

SYSEX = bytes([0xF0, *selftest.PREFIX])
LAST = max(selftest.CHECKS)
NAMES = {"Oct -": "oct-", "Oct +": "oct+", "Knob": "encoder", "the knob": "encoder"}  # display -> emulator
REST_PEDAL, REST_AFTERTOUCH = 900, 120  # the emulated inputs with nothing pressed
BLANK_SECTOR = 0x5E000  # the highest blank one of the flash below

failures = 0


def check(name: str, passed: bool, detail: object = "") -> None:
    global failures
    failures += not passed
    print(f"{'ok  ' if passed else 'FAIL'}  {name}" + (f": {detail}" if detail != "" else ""), flush=True)


def unit_flash() -> bytes:
    """A chip like a unit's: patterns, settings, the file system's two name blocks, and in the
    SDK's area a saved record at each end, so that 0x5E000 is the highest blank sector."""
    image = bytearray(b"\xff" * emu.NorFlash.SIZE)
    for sector in range(0, 0x40000, 0x1000):
        image[sector : sector + 3480] = bytes((sector >> 12) + i & 0x7F for i in range(3480))
    image[0x40000:0x40055] = bytes(range(0x55))
    for block in (0x60000, 0x61000):
        image[block : block + 0x40] = bytes(8) + b"littlefs" + bytes(range(0x30))
    image[0x41000:0x41040] = b"KSS1" + bytes(range(60))
    image[0x5F000:0x5F040] = b"KSS1" + bytes(range(60))
    return bytes(image)


class Human:
    """Someone at the unit with a computer attached: reads the display, uses the controls, and
    keeps what arrives over USB."""

    def __init__(self, board: emu.Board):
        self.board = board
        self.capture = bytearray()
        self.cable = False  # a MIDI cable from Out to In
        self.looped = 0
        self.read: tuple[bytes, str] = (b"", "")
        self.flash_writes: list[tuple[int, int]] = []  # (command, address) of every program and erase
        inner = board.flash.transfer

        def transfer(byte: int) -> int:
            answer = inner(byte)
            command = board.flash.cmd
            if len(command) == 4 and command[0] in (0x02, 0x20, 0x52, 0xD8):
                self.flash_writes.append((command[0], board.flash.addr))
            return answer

        board.spi[emu.SPI2].device = transfer

    def run(self, ms: float) -> None:
        board, end = self.board, self.board.us + ms * 1000
        while board.us < end:
            if board.usb.host.midi_in_ep is None:
                board.run(1)
            else:
                self.capture += board.usb.host.midi_receive(2)  # the computer reads all the time
            if self.cable:
                board.midi_in += board.midi_out[self.looped :]
            self.looped = len(board.midi_out)
            assert not board.stopped, f"the processor stopped: {board.stopped}"

    def screen(self) -> str:
        """The small-font text on the display, lines top to bottom."""
        ram = bytes(self.board.oled.ram)
        if ram != self.read[0]:  # the font's O and 0 are one shape: a 0 touching a letter is an O
            text = re.sub(r"0(?=[A-Za-z])|(?<=[A-Za-z])0", "O", emutest.small_text(self.board))
            self.read = (ram, text)
        return self.read[1]

    def wait_for(self, pattern: str, ms: float = 5000) -> re.Match:
        waited = 0
        while not (found := re.search(pattern, self.screen())):
            assert waited < ms, f"waited {ms} ms for {pattern!r}; the display shows {self.screen()!r}"
            self.run(20)
            waited += 20
        return found

    def step(self, check_id: int, pattern: str = "", ms: float = 5000) -> re.Match:
        """Wait for a check's screen, and for a text on it."""
        return self.wait_for(rf"\b{check_id}/{LAST} .*{pattern}", ms)

    def tap(self, *controls: str) -> None:
        for control in controls:
            self.board.press(control)
        self.run(80)
        for control in controls:
            self.board.release(control)
        self.run(40)

    def send(self, kind: int, *data: int) -> None:
        self.board.usb.host.midi_send(SYSEX + bytes([kind, *data, 0xF7]))

    def lit(self) -> dict[str, tuple[int, int, int]]:
        """The button lights that are on, by the emulator's name for the button."""
        leds = self.board.leds[1]
        return {name: leds[i] for name, i in emu.BUTTON_LEDS.items() if any(leds[i])}

    def report(self) -> selftest.Report:
        return selftest.decode_stream(bytes(self.capture))

    # ---- what a person does at each kind of check ----

    def lights(self, check_id: int, colour: str, rgb: tuple[int, int, int], answer: str) -> list[str]:
        """Watch one sweep: each light named on the display must be the one lit, in the colour."""
        wrong, seen = [], []
        while True:
            found = self.step(check_id, rf"{colour}: (all thirteen|Oct [-+]|\w+)")
            self.run(30)  # the lights and the display change within a few ms of each other
            if self.step(check_id, rf"{colour}: (all thirteen|Oct [-+]|\w+)").group(1) != found.group(1):
                continue
            name, lit = found.group(1), self.lit()
            if name == "all thirteen":
                if len(lit) != 13 or set(lit.values()) != {rgb}:
                    wrong.append(f"all: {lit}")
                break
            if name not in seen:
                seen.append(name)
                if lit != {NAMES.get(name, name.lower()): rgb}:
                    wrong.append(f"{name}: {lit}")
            self.run(60)
        if len(seen) != 13:
            wrong.append(f"only {len(seen)} lights named")
        self.step(check_id, "Play: right")
        self.tap(answer)
        return wrong

    def button(self, check_id: int) -> str:
        """The emulator's name of the control the display asks for."""
        name = self.step(check_id, r"Press (the knob|Oct [-+]|\w+)").group(1)
        return NAMES.get(name, name.lower())

    def keys(self, keys: list[int]) -> None:
        for n, key in enumerate(keys):  # the second contact 3 to 9 ms after the first: several velocities
            now = self.board.us
            self.board.key_events += [(now, key, 1), (now + 3000 + 2000 * (n % 4), key, 3)]
            self.run(50)
            self.board.release(f"k{key}")
            self.run(40)

    def slide(self, strip: int, start: float, end: float) -> None:
        for i in range(21):
            self.board.set_touch(strip, start + (end - start) * i / 20)
            self.run(15)
        self.board.set_touch(strip, None)
        self.run(300)

    def start(self) -> None:
        """From power-on, or from Play on the last summary, to the end of the automatic part."""
        self.wait_for("Play: go on", 12000)


def boot(enumerate_usb: bool = True) -> Human:
    human = Human(emutest.sdk("selftest", 20, flash=unit_flash()))
    human.board.adc[:] = [REST_PEDAL, REST_AFTERTOUCH]
    if enumerate_usb:
        info = human.board.usb.host.enumerate()
        assert info["product_name"] == "KeyStep mk2", info
    return human


def by_name(report: selftest.Report) -> dict[str, selftest.Result]:
    return {r.name: r for r in report.ordered()}


def expect(results: dict[str, selftest.Result], name: str, status: str, text: str = "", **values: int) -> None:
    """One result: its status, its text, and any of its values by name."""
    result = results.get(name)
    if result is None:
        check(f"{name} {status}", False, "no result")
        return
    named = result.named() or {}
    passed = result.status == status and text in result.text and all(named.get(k) == v for k, v in values.items())
    check(
        f"{name} {status}" + (f" ({text})" if text else ""), passed, result.line() if not passed else result.numbers()
    )


# ---- everything done right ----


def part_good() -> Human:
    human = boot()
    board = human.board
    before = bytes(board.flash.mem)
    human.start()
    human.send(selftest.PING, 5)  # what selftest.py does while it listens
    human.run(20)
    ready = by_name(human.report())
    check("automatic part reports before anyone touches the unit", len(ready) == 11, sorted(ready))
    pong = human.report().pongs
    check(
        "ping answered with the unit's time", bool(pong) and pong[0][0] == 5 and abs(pong[0][1] - board.us / 1000) < 30
    )
    human.tap("play")

    human.wait_for("Play=ok Stop=bad")  # the display check draws its own screen
    px = board.oled.pixel
    frame = all(px(x, y) for x in range(128) for y in (0, 31)) and all(px(x, y) for y in range(32) for x in (0, 127))
    dots = all(px(11, y) == (y % 2 == 0) and px(116, y) == (y % 2 == 0) for y in range(2, 30))
    check("display pattern: frame on the outermost pixels, dotted columns", frame and dots)
    human.tap("play")

    colours = {"Red": (0x8D, 0, 0), "Green": (0, 0x8D, 0), "Blue": (0, 0, 0x8D), "White": (0x50, 0x50, 0x50)}
    for check_id, (colour, rgb) in enumerate(colours.items(), 13):
        wrong = human.lights(check_id, colour, rgb, "play")
        check(f"lights {colour.lower()}: each named light is the one lit, then all", not wrong, "; ".join(wrong))

    asked = []
    for check_id in range(17, 31):
        asked.append(human.button(check_id))
        own = human.lit()
        if asked[-1] != "back" and list(own) != [asked[-1]]:
            check(f"button {asked[-1]}: its own light is lit while it is asked for", False, own)
        human.tap(asked[-1])
    check("buttons: fourteen asked for, each once", len(set(asked)) == 14, " ".join(asked))

    human.step(31, "clicks right")
    board.turn(4)
    human.step(31, "clicks left")
    board.turn(-4)
    human.step(32, "Play key 1 of 32")
    human.keys(list(range(32)))
    human.step(33, "Slide the Bend strip")
    human.slide(1, 0.0, 1.0)
    human.step(34, "Slide the Mod strip")
    human.slide(0, 0.0, 1.0)

    human.step(35, "Hold a key, press hard")
    board.press("k12")
    human.run(60)
    board.adc[1] = 3000
    human.run(60)
    board.adc[1] = REST_AFTERTOUCH
    board.release("k12")

    human.step(36, "Pedal plugged in")
    human.tap("play")
    human.step(36, "Press the pedal")
    board.adc[0] = 3500
    human.run(100)
    board.adc[0] = REST_PEDAL

    human.step(37, "MIDI cable Out > In", 8000)
    human.cable = True
    human.tap("play")

    human.step(38, "Meter on CV and gate")
    human.tap("play")
    seen, wrong = [], []
    for _ in range(8):
        found = human.step(38, r"(Pitch (\d+) Mods (\d+)   Gate (\d): (on|off))")
        pitch, mod, form, gate = int(found[2]), int(found[3]), int(found[4]), found[5] == "on"
        out = board.outputs()
        level = (not gate) if form == 0 else gate  # format 0 pulls the pin low for a gate
        want = {"pitch": pitch, "mod1": mod, "mod2": mod, "gate_format": form, "gate_pin": int(level)}
        if any(out[k] != v for k, v in want.items()):
            wrong.append(f"shown {found[1]!r}, outputs {out}")
        seen.append((pitch, mod, gate, form))
        human.tap("play")
        human.wait_for(rf"^(?!.*{re.escape(found[1])})")  # the next step, or the question
    check("cv: the outputs are what each of the eight steps shows", not wrong and len(set(seen)) == 8, wrong or seen)
    human.step(38, "Did the outputs follow")
    human.tap("play")
    human.wait_for(r"Done: OK \d+  FAIL \d+", 8000)
    human.cable = False

    report = human.report()
    results = by_name(report)
    check("report complete, nothing wrong with it", report.complete() and not report.problems(), report.problems())
    check("one result per check", sorted(r.check for r in report.ordered()) == list(selftest.CHECKS))
    for check_id, (name, _kind, _fields) in selftest.CHECKS.items():
        want = "INFO" if check_id in (9, 10) else "OK"  # the two rest levels are measurements
        expect(results, name, want)
    totals = report.totals()
    shown = human.wait_for(r"Done: OK (\d+)  FAIL (\d+)   SKIP (\d+)  INFO (\d+)")
    check("totals: 37 OK, 2 INFO, and the display says the same", [*totals.values()] == [37, 0, 0, 2], totals)
    check("summary screen shows the totals", [int(g) for g in shown.groups()] == [37, 0, 0, 2], shown[0])

    # the numbers, where the emulator fixes them
    expect(results, "usb.ready", "OK", ready=1)
    expect(results, "time.millis", "OK", millis_ms=500)
    check("time.millis: 500 ms measured as 500 ms", abs(results["time.millis"].values[1] - 500000) <= 200)
    expect(results, "time.timer", "OK", ticks_after_stop=0)
    expect(results, "flash.id", "OK", jedec=0xC84013)
    expect(results, "flash.read", "OK", sdk_sectors_blank=29, settings_bytes_used=0x55, littlefs=1, reads_differ=0)
    expect(results, "flash.write", "OK", sector=BLANK_SECTOR, bytes_wrong=0, bytes_left=0)
    expect(results, "flash.store", "OK", slot=(BLANK_SECTOR - 0x41000) >> 12, load_result=64, load_after_erase=-4)
    expect(results, "sync.loop", "OK", sent=8, counted=8, hook_count=8, levels_followed=8, run_followed=1)
    expect(results, "aftertouch.rest", "INFO", min=REST_AFTERTOUCH, max=REST_AFTERTOUCH, mean=REST_AFTERTOUCH)
    expect(results, "pedal.rest", "INFO", min=REST_PEDAL, max=REST_PEDAL, mean=REST_PEDAL)
    expect(results, "touch.rest", "OK", mod=-1, bend=-1)
    expect(results, "encoder", "OK", right=4, left=-4)
    expect(results, "keys", "OK", fired=-1, released=-1, misordered=0, repeats=0)
    low, high = results["keys"].values[3:5]
    check("keys: slower and faster strikes give a range of velocities", 1 <= low < high <= 127, f"{low} to {high}")
    for strip in ("strip.bend", "strip.mod"):
        expect(results, strip, "OK", min=0, max=255, first=0, last=255, released=1, other_strip_touched=0)
    expect(results, "aftertouch", "OK", rest=REST_AFTERTOUCH, peak=3000, direction=1)
    expect(results, "pedal", "OK", rest=REST_PEDAL, start=REST_PEDAL, min=REST_PEDAL, max=3500)
    expect(results, "midi.din", "OK", sent=16, received=16, matching=16, clocks_heard=2)
    expect(results, "cv", "OK", "confirmed by hand", steps_shown=8)
    expect(results, "usb.in", "OK", host_messages=1)

    # the flash: only the blank sector was written, and it is blank again
    sectors = {address & ~0xFFF for _command, address in human.flash_writes}
    check("flash: every program and erase was in the one blank sector", sectors == {BLANK_SECTOR}, sorted(sectors))
    check("flash: the chip holds what it held before", bytes(board.flash.mem) == before)
    out = board.outputs()
    check("outputs back at rest", (out["pitch"], out["mod1"], out["gate_pin"], out["gate_format"]) == (0, 0, 0, 1), out)
    check("lights off at the end", not human.lit(), human.lit())

    # the summary's list, a replay for a late listener, and the decoder on files
    board.turn(1)
    listed = human.wait_for(r"1 USB: OK")
    check("summary: the knob lists the results", bool(listed))
    board.turn(-1)
    human.wait_for("Done: OK")
    live = len(human.capture)
    human.send(selftest.REPLAY)
    human.run(400)
    again = selftest.decode_stream(bytes(human.capture[live:]))
    check("replay request: the whole report again", again.complete() and again.as_json() == report.as_json())
    check("replay on top of the live report changes nothing", human.report().as_json() == report.as_json())
    with tempfile.TemporaryDirectory() as folder:
        capture, out_file = Path(folder, "capture.syx"), Path(folder, "report.json")
        capture.write_bytes(human.capture[:live])
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            status = selftest.main(["decode", str(capture), "-o", str(out_file)])
        table = "OK 37  FAIL 0  SKIP 0  INFO 2  of 39 results" in printed.getvalue()
        written = json.loads(out_file.read_text())
        passed = status == 0 and table and written["totals"]["OK"] == 37 and written["complete"]
        check("decode from a file: exit status 0, the table and the JSON", passed, status)
    cut = selftest.decode_stream(bytes(human.capture[: live // 2]))
    check("a report cut short is not complete", not cut.complete() and any("no end" in p for p in cut.problems()))
    spoiled = bytearray(human.capture[:live])
    spoiled[spoiled.index(b"\xf0", 100) + 9] ^= 1
    bad = selftest.decode_stream(bytes(spoiled))
    check("a damaged message is refused", any("checksum" in p for p in bad.problems()) and not bad.complete())
    return human


# ---- a second run on the same board, with mistakes and faults ----


def part_bad(human: Human) -> None:
    board = human.board
    board.flash.mem[0x41000:0x60000] = bytes(0x1F000)  # every sector of the SDK's area in use
    before = bytes(board.flash.mem)
    board.sync(0, False)  # a cable in the sync socket, nothing coming in
    board.sync(1, False)
    human.flash_writes.clear()
    human.capture.clear()
    human.tap("play")  # Play on the summary: a new run
    human.start()
    human.tap("play")

    human.wait_for("Play=ok Stop=bad")
    human.tap("stop")  # "the display looks wrong"
    human.step(13, "Play: right", 9000)
    human.tap("stop")  # "the red lights are wrong"
    for check_id in (14, 15, 16):
        human.step(check_id, "Play: right", 9000)
        human.tap("play")

    for check_id in range(17, 31):
        control = human.button(check_id)
        if control == "seq":
            human.tap("arp")  # the wrong button
        elif control == "arp":
            human.step(check_id + 1, "Press", 20000)  # nothing at all: the check must give up by itself
        elif control == "mutate":
            human.tap("mutate", "chord")  # two at once
        else:
            human.tap(control)

    human.step(31, "clicks right")
    board.turn(-4)  # the wrong way
    human.step(32, "Play key 1 of 32", 9000)
    human.keys([k for k in range(32) if k != 5])  # one key dead
    human.step(33, "Slide the Bend strip")
    human.slide(1, 1.0, 0.0)  # top to bottom
    human.step(33, "again")
    human.tap("stop")
    human.step(34, "Slide the Mod strip")
    human.slide(1, 0.0, 1.0)  # the Bend strip again
    human.tap("stop")
    human.step(35, "Hold a key, press hard")
    board.press("k3")
    human.run(100)
    board.release("k3")  # played, never pressed hard
    human.run(60)
    human.tap("stop")
    human.step(36, "Pedal plugged in")
    human.tap("stop")
    human.step(37, "MIDI cable Out > In")
    human.tap("play")  # no cable
    human.step(38, "Meter on CV and gate")
    human.tap("play")
    human.step(38, "Pitch 0 Mods 0")
    human.tap("stop")  # "the meter shows something else"
    human.wait_for(r"Done: OK \d+  FAIL \d+", 8000)

    report = human.report()
    results = by_name(report)
    check("second run: a new run number, complete", report.complete() and report.start["run"] == 2, report.problems())
    expect(results, "flash.write", "SKIP", "no blank sector")
    expect(results, "flash.store", "SKIP")
    check("flash with no blank sector: nothing programmed or erased", not human.flash_writes, human.flash_writes)
    check("flash with no blank sector: the chip holds what it held", bytes(board.flash.mem) == before)
    expect(results, "sync.loop", "SKIP", "no pulse came back", sent=8, counted=0, run_followed=0)
    expect(results, "display", "FAIL", "said wrong")
    expect(results, "leds.red", "FAIL", "said wrong")
    expect(results, "leds.green", "OK")
    expect(results, "button.seq", "FAIL", "saw Arp", expected=1 << 2, seen=1 << 1)
    expect(results, "button.arp", "FAIL", "nothing pressed", seen=0)
    expect(results, "button.mutate", "FAIL", "saw Mutate+Chord", seen=1 | 1 << 14)
    expect(results, "button.chord", "OK")
    expect(results, "button.encoder", "OK")
    expect(results, "encoder", "FAIL", "counts the other way", right=-4, left=0)
    expect(results, "keys", "FAIL", "missing 1 misordered 1", fired=-1 - (1 << 5), misordered=1)
    expect(results, "strip.bend", "FAIL", "reads top to bottom", first=255, last=0)
    expect(results, "strip.mod", "FAIL", "the other strip moved", min=-1, other_strip_touched=1)
    expect(results, "aftertouch", "FAIL", "level did not move", direction=0)
    expect(results, "pedal", "SKIP", "skipped")
    expect(results, "midi.din", "SKIP", "nothing came back", sent=16, received=0)
    expect(results, "cv", "FAIL", "said wrong", steps_shown=1, answer=0)
    expect(results, "usb.in", "SKIP", "no ping")
    failed = sorted(r.name for r in report.ordered() if r.status == "FAIL")
    wanted = sorted(
        ["display", "leds.red", "button.seq", "button.arp", "button.mutate", "encoder", "keys", "strip.bend"]
        + ["strip.mod", "aftertouch", "cv"]
    )
    check("exactly the eleven spoiled checks FAIL", failed == wanted, failed)
    skipped = sorted(r.name for r in report.ordered() if r.status == "SKIP")
    check("six SKIP", skipped == sorted(["flash.write", "flash.store", "sync.loop", "pedal", "midi.din", "usb.in"]))
    shown = human.wait_for(r"Done: OK (\d+)  FAIL (\d+)   SKIP (\d+)  INFO (\d+)")
    check("summary screen shows 11 FAIL and 6 SKIP", shown.groups()[1:3] == ("11", "6"), shown[0])


# ---- no USB host: the report goes out of the DIN socket ----


def part_din() -> None:
    human = boot(enumerate_usb=False)
    human.start()
    human.run(100)
    report = selftest.decode_stream(bytes(human.board.midi_out))
    results = by_name(report)
    check("no host: eleven results on the DIN output", len(results) == 11 and not report.faults, sorted(results))
    expect(results, "usb.ready", "FAIL", "no host", ready=0)
    expect(results, "flash.write", "OK")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("words", nargs="*", help="run only the parts whose name contains one of these")
    args = parser.parse_args()

    def wanted(name: str) -> bool:
        return not args.words or any(word in name for word in args.words)

    human = None
    for name, part in (("good", part_good), ("bad", None), ("din", part_din)):
        if not wanted(name):
            continue
        print(f"--- {name}", flush=True)
        try:
            if part:
                result = part()
                human = result or human
            else:  # the second run needs a board that finished a first one
                part_bad(human or part_good())
        except (AssertionError, FileNotFoundError, ValueError, emu.usbmodel.UsbError) as e:
            check(f"{name} run", False, e)
    print(f"{failures} failed" if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
