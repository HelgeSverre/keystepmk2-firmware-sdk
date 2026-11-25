#!/usr/bin/env python3
"""Read the report that sdk/examples/selftest sends: one result per check, with its numbers.

    uv run --with mido --with python-rtmidi tools/selftest.py receive --port "KeyStep mk2" -o report.json
    uv run tools/selftest.py decode capture.syx [-o report.json]      # a recorded MIDI stream

`receive` waits for the port, asks the unit to send again whatever it already reported, prints
each result as it arrives and ends with a table when the run is over (Ctrl-C ends it early). It
also pings the unit every two seconds: that is the unit's USB-in check, and the answers compare
its clock with this computer's. The exit status is 0 only for a complete run without a FAIL.

The message format is described at the top of sdk/examples/selftest/main.c, docs/SELFTEST.md
says what each check does.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flashdump import crc16, messages, unpack7

PREFIX = bytes([0x7D, ord("K"), ord("S")])
START, RESULT, END, PONG, PING, REPLAY = 0x10, 0x11, 0x12, 0x13, 0x20, 0x21
STATUS = ("OK", "FAIL", "SKIP", "INFO")
FORMAT = 1

# id: (name, kind, the names of its values; ":x" marks one shown in hex). The ids and the order
# of the values are the firmware's (selftest.h, auto.c, guided.c).
_BUTTONS = ("seq", "arp", "mutate", "chord", "tempo", "record", "stop", "play", "shift", "hold", "oct-", "oct+", "back")
_REST = ("min", "max", "mean", "samples", "conversions")
_STRIP = ("min", "max", "first", "last", "released", "other_strip_touched")
CHECKS: dict[int, tuple[str, str, tuple[str, ...]]] = {
    1: ("usb.ready", "auto", ("ready", "waited_ms")),
    2: ("time.millis", "auto", ("millis_ms", "micros_us", "delay_100ms_us", "counters_apart_us")),
    3: (
        "time.timer",
        "auto",
        ("ticks_at_1000us", "window_1000_us", "ticks_at_250us", "window_250_us", "ticks_after_stop"),
    ),
    4: ("flash.id", "auto", ("jedec:x",)),
    5: (
        "flash.read",
        "auto",
        ("sdk_sectors_blank", "settings_bytes_used", "fs_bytes_used", "littlefs", "reads_differ"),
    ),
    6: (
        "flash.write",
        "auto",
        ("sector:x", "write_result", "bytes_wrong", "erase_result", "bytes_left", "write_ms", "erase_ms"),
    ),
    7: (
        "flash.store",
        "auto",
        ("slot", "save_result", "load_result", "bytes_wrong", "erase_result", "bytes_left", "load_after_erase"),
    ),
    8: ("sync.loop", "auto", ("sent", "counted", "hook_count", "levels_followed", "run_followed", "idle_levels")),
    9: ("aftertouch.rest", "auto", _REST),
    10: ("pedal.rest", "auto", _REST),
    11: ("touch.rest", "auto", ("mod", "bend", "controller_answers")),
    12: ("display", "guided", ("answer",)),
    13: ("leds.red", "guided", ("colour:x",)),
    14: ("leds.green", "guided", ("colour:x",)),
    15: ("leds.blue", "guided", ("colour:x",)),
    16: ("leds.white", "guided", ("colour:x",)),
    **{
        17 + i: (f"button.{name}", "guided", ("expected:x", "seen:x", "held_before:x"))
        for i, name in enumerate(_BUTTONS)
    },
    30: ("button.encoder", "guided", ("expected:x", "seen:x", "held_before:x")),
    31: ("encoder", "guided", ("right", "left")),
    32: ("keys", "guided", ("fired:x", "released:x", "misordered", "velocity_min", "velocity_max", "repeats")),
    33: ("strip.bend", "guided", _STRIP),
    34: ("strip.mod", "guided", _STRIP),
    35: ("aftertouch", "guided", ("rest", "peak", "direction", "min", "max", "threshold")),
    36: ("pedal", "optional", ("rest", "start", "min", "max")),
    37: ("midi.din", "optional", ("sent", "received", "matching", "clocks_heard")),
    38: ("cv", "optional", ("steps_shown", "answer")),
    39: ("usb.in", "optional", ("host_messages",)),
}


def check_name(check: int) -> str:
    return CHECKS[check][0] if check in CHECKS else f"check {check}"


@dataclass
class Result:
    sequence: int
    check: int
    status: str
    values: list[int]
    text: str
    device_ms: int

    @property
    def name(self) -> str:
        return check_name(self.check)

    @property
    def kind(self) -> str:
        return CHECKS[self.check][1] if self.check in CHECKS else "unknown"

    def named(self) -> dict[str, int] | None:
        """The values by name, when the check is known and sent all of them (a SKIP sends fewer)."""
        fields = CHECKS.get(self.check, ("", "", ()))[2]
        if len(fields) != len(self.values):
            return None
        return {name.removesuffix(":x"): value for name, value in zip(fields, self.values, strict=True)}

    def numbers(self) -> str:
        fields = CHECKS.get(self.check, ("", "", ()))[2]
        if len(fields) != len(self.values):
            return " ".join(str(v) for v in self.values)
        return " ".join(
            f"{name[:-2]}={value & 0xFFFFFFFF:#x}" if name.endswith(":x") else f"{name}={value}"
            for name, value in zip(fields, self.values, strict=True)
        )

    def line(self) -> str:
        text = f'  "{self.text}"' if self.text else ""
        return f"{self.status:<4}  {self.check:>2} {self.name:<16} {self.numbers()}{text}"


@dataclass
class Report:
    """Collects messages. `feed` returns what to print for each; `problems()` says what is wrong."""

    start: dict | None = None
    results: dict[int, Result] = field(default_factory=dict)  # by sequence number
    end: dict | None = None
    faults: list[str] = field(default_factory=list)
    pongs: list[tuple[int, int, int, float | None]] = field(default_factory=list)  # number, ms, us, when

    def feed(self, message: bytes, at: float | None = None) -> list[str]:
        """One SysEx message without F0 and F7; `at` is when it arrived, for the pongs."""
        if not message.startswith(PREFIX) or len(message) < 4 or not START <= message[3] <= PONG:
            return []
        kind, body = message[3], unpack7(message[4:])
        if len(body) < 3 or crc16(body[:-2]) != struct.unpack("<H", body[-2:])[0]:
            self.faults.append(f"a message of type {kind:#04x} failed its checksum")
            return []
        body = body[:-2]
        if kind == START and len(body) >= 13:
            form, major, minor, run, run_id, ms, checks = struct.unpack_from("<BBBBIIB", body)
            if self.start and self.start["run_id"] == run_id:
                return []  # a replay of the run we already have
            self.start = {
                "format": form, "version": f"{major}.{minor}", "run": run, "run_id": run_id,
                "device_ms": ms, "checks": checks,
            }  # fmt: skip
            self.results, self.end, self.faults = {}, None, []
            if form != FORMAT:
                self.faults.append(f"report format {form}: this tool reads format {FORMAT}")
            return [f"self-test {major}.{minor}, run {run}: {checks} checks"]
        if self.start is None:
            if "results before a start message" not in self.faults:
                self.faults.append("results before a start message")
            return []
        if kind == RESULT and len(body) >= 8:
            sequence, check, status, count, ms = struct.unpack_from("<BBBBI", body)
            if sequence in self.results or status >= len(STATUS) or len(body) < 8 + 4 * count:
                return []  # a replay, or not a message of this format
            values = list(struct.unpack_from(f"<{count}i", body, 8))
            text = body[8 + 4 * count :].decode("ascii", "replace")
            result = self.results[sequence] = Result(sequence, check, STATUS[status], values, text, ms)
            return [result.line()]
        if kind == END and len(body) >= 10 and self.end is None:
            count, ok, fail, skip, info, dropped, ms = struct.unpack_from("<BBBBBBI", body)
            self.end = {
                "results": count, "OK": ok, "FAIL": fail, "SKIP": skip, "INFO": info, "dropped": dropped,
                "device_ms": ms,
            }  # fmt: skip
            return [f"end of run: {ok} OK, {fail} FAIL, {skip} SKIP, {info} INFO"]
        if kind == PONG and len(body) >= 9:
            number, ms, us = struct.unpack_from("<BII", body)
            self.pongs.append((number, ms, us, at))
        return []

    def ordered(self) -> list[Result]:
        return [self.results[s] for s in sorted(self.results)]

    def totals(self) -> dict[str, int]:
        return {status: sum(r.status == status for r in self.results.values()) for status in STATUS}

    def complete(self) -> bool:
        return self.end is not None and sorted(self.results) == list(range(self.end["results"]))

    def problems(self) -> list[str]:
        """What is wrong with the report itself, as opposed to with the unit."""
        problems = list(self.faults)
        if self.start is None:
            return problems + ["no start message"]
        if self.end is None:
            problems.append("no end message: the run is not over, or the report was cut short")
        else:
            missing = sorted(set(range(self.end["results"])) - set(self.results))
            if missing:
                problems.append(f"results missing (sequence numbers {missing}): ask again with Record on the unit")
            elif {s: self.end[s] for s in STATUS} != self.totals():
                problems.append(f"the unit's totals {self.end} differ from the results received")
            if self.end["dropped"]:
                problems.append(f"the unit dropped {self.end['dropped']} messages while nobody read them")
            seen = [r.check for r in self.results.values()]
            absent = [check_name(c) for c in CHECKS if c not in seen and c <= self.start["checks"]]
            if absent and not missing:
                problems.append("no result for: " + ", ".join(absent))
            if len(seen) != len(set(seen)):
                problems.append("a check reported twice")
        return problems

    def table(self) -> str:
        lines = [r.line() for r in sorted(self.results.values(), key=lambda r: (r.check, r.sequence))]
        totals = self.totals()
        lines.append("")
        lines.append("  ".join(f"{status} {totals[status]}" for status in STATUS) + f"  of {len(self.results)} results")
        failed = [r.name for r in self.ordered() if r.status == "FAIL"]
        if failed:
            lines.append("FAILED: " + ", ".join(failed))
        return "\n".join(lines)

    def as_json(self, clock: dict | None = None) -> dict:
        return {
            "start": self.start,
            "end": self.end,
            "complete": self.complete(),
            "totals": self.totals(),
            "problems": self.problems(),
            "clock": clock,
            "results": [
                {
                    "sequence": r.sequence,
                    "id": r.check,
                    "check": r.name,
                    "kind": r.kind,
                    "status": r.status,
                    "values": r.named(),
                    "raw": r.values,
                    "text": r.text,
                    "device_ms": r.device_ms,
                }
                for r in self.ordered()
            ],
        }


def decode_stream(stream: bytes, echo=None) -> Report:
    """A report from raw MIDI bytes. `echo` is called with each line as `receive` would print it."""
    report = Report()
    for message in messages(stream):
        for line in report.feed(message):
            if echo:
                echo(line)
    return report


def clock_check(pongs: list[tuple[int, int, int, float | None]], sent: dict[int, float]) -> dict | None:
    """The unit's clock against this computer's, from the first and the last ping that came
    straight back (the unit answers late while a check keeps it busy)."""
    quick = []
    for number, _ms, us, at in pongs:
        if at is not None and number in sent and at - sent[number] < 0.03:
            quick.append(((at + sent[number]) / 2, us))
    if len(quick) < 2 or quick[-1][0] - quick[0][0] < 5:
        return None
    host = quick[-1][0] - quick[0][0]
    device = ((quick[-1][1] - quick[0][1]) & 0xFFFFFFFF) / 1e6
    return {
        "host_seconds": round(host, 4), "device_seconds": round(device, 4),
        "device_fast_ppm": round((device / host - 1) * 1e6), "pings": len(quick),
    }  # fmt: skip


def finish(report: Report, out: Path | None, clock: dict | None = None) -> int:
    print()
    print(report.table())
    if clock:
        print(
            f"clock: the unit counted {clock['device_seconds']} s while this computer counted {clock['host_seconds']} s "
            f"({clock['device_fast_ppm']:+d} ppm, from {clock['pings']} pings; good to about 100 ppm a minute)"
        )
    problems = report.problems()
    for problem in problems:
        print(f"report: {problem}", file=sys.stderr)
    if out:
        out.write_text(json.dumps(report.as_json(clock), indent=2) + "\n")
        print(f"wrote {out}")
    return 0 if report.complete() and not problems and not report.totals()["FAIL"] else 1


def find_port(names: list[str], wanted: str) -> str | None:
    exact = [n for n in names if n == wanted]
    return (exact or [n for n in names if wanted.lower() in n.lower()] or [None])[0]


def receive(wanted: str, out: Path | None, wait: float) -> int:
    import mido  # needs: uv run --with mido --with python-rtmidi

    deadline = time.monotonic() + wait
    while (name := find_port(mido.get_input_names(), wanted)) is None:
        if time.monotonic() > deadline:
            sys.exit(f"error: no MIDI input called {wanted!r}; found: {', '.join(mido.get_input_names()) or 'none'}")
        time.sleep(0.5)
    back = find_port(mido.get_output_names(), wanted)
    report, sent, number, next_ping = Report(), {}, 0, 0.0
    print(f"listening on {name}" + ("" if back else "; no output port of that name, so no pings") + " (Ctrl-C to end)")
    with mido.open_input(name) as source:
        sink = mido.open_output(back) if back else None
        try:
            if sink:  # whatever the unit reported before we were listening
                sink.send(mido.Message("sysex", data=[*PREFIX, REPLAY]))
            while report.end is None:
                for message in source.iter_pending():
                    if message.type == "sysex":
                        for line in report.feed(bytes(message.data), time.monotonic()):
                            print(line, flush=True)
                if sink and time.monotonic() >= next_ping:
                    number = number % 127 + 1
                    sent[number] = time.monotonic()
                    sink.send(mido.Message("sysex", data=[*PREFIX, PING, number]))
                    next_ping = sent[number] + 2
                time.sleep(0.001)
        except KeyboardInterrupt:
            print("\nstopped before the end of the run")
        finally:
            if sink:
                sink.close()
    return finish(report, out, clock_check(report.pongs, sent))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("receive", help="read the report from a MIDI input as the test runs")
    p.add_argument("--port", default="KeyStep mk2", help='the MIDI port, or part of its name (default "KeyStep mk2")')
    p.add_argument("--wait", type=float, default=60, help="seconds to wait for the port to appear (default 60)")
    p.add_argument("-o", "--out", type=Path, help="write the report as JSON")
    p = sub.add_parser("decode", help="read the report from a file of raw MIDI bytes")
    p.add_argument("capture", type=Path)
    p.add_argument("-o", "--out", type=Path, help="write the report as JSON")
    args = parser.parse_args(argv)

    if args.command == "receive":
        return receive(args.port, args.out, args.wait)
    return finish(decode_stream(args.capture.read_bytes(), echo=print), args.out)


if __name__ == "__main__":
    sys.exit(main())
