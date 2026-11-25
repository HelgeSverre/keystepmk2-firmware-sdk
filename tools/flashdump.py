#!/usr/bin/env python3
"""Reassemble the external flash from the SysEx that sdk/examples/flashdump sends.

    uv run tools/flashdump.py decode capture.syx -o flash.bin     # a recorded MIDI stream
    uv run --with mido --with python-rtmidi tools/flashdump.py receive -o flash.bin [--port NAME]
    uv run tools/flashdump.py info flash.bin                      # what the image holds

The message format is described at the top of sdk/examples/flashdump/main.c. Every data message
carries a CRC-16 and the stream ends with a CRC-32 of the whole chip; an image is written only
if all of it arrived and both checks pass. sdk/examples/bootdump sends the processor's first
32 KiB, the bootloader, in the same format.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

PREFIX = bytes([0x7D, ord("K"), ord("S")])
REGIONS = [
    (0x00000, 0x40000, "stock patterns (64 x 4 KiB)"),
    (0x40000, 0x41000, "stock settings"),
    (0x41000, 0x60000, "SDK area"),
    (0x60000, 0x80000, "stock file system (calibration)"),
]


def unpack7(body: bytes) -> bytes:
    """Eight 7-bit bytes -> seven bytes; the first of each eight holds the top bits."""
    out = bytearray()
    for i in range(0, len(body), 8):
        top, rest = body[i], body[i + 1 : i + 8]
        out += bytes(b | (top >> k & 1) << 7 for k, b in enumerate(rest))
    return bytes(out)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


class Dump:
    """Collects messages. `done` turns true at the end message; `problems` lists what is wrong."""

    def __init__(self):
        self.chip_id = b""
        self.size = 0
        self.image = bytearray()
        self.have = bytearray()  # one flag per byte
        self.done = False
        self.crc32: int | None = None
        self.problems: list[str] = []

    def feed(self, message: bytes) -> None:
        """One SysEx message without F0 and F7."""
        if not message.startswith(PREFIX) or len(message) < 4:
            return
        kind, body = message[3], unpack7(message[4:])
        if kind == 1 and len(body) >= 9:
            self.chip_id = body[:3]
            self.size, _block = struct.unpack_from("<IH", body, 3)
            self.image = bytearray(b"\xff" * self.size)
            self.have = bytearray(self.size)
            self.done, self.crc32 = False, None
        elif not self.size:
            self.problems.append("data before the start message")
        elif kind in (2, 3) and len(body) >= 6:
            payload, (check,) = body[:-2], struct.unpack("<H", body[-2:])
            (addr,) = struct.unpack_from("<I", payload)
            if crc16(payload) != check:
                self.problems.append(f"bad checksum in the message for {addr:#07x}")
                return
            if kind == 2:
                data = payload[4:]
            else:
                data = b"\xff" * struct.unpack_from("<I", payload, 4)[0]
            if addr + len(data) > self.size:
                self.problems.append(f"message for {addr:#07x} runs past the end")
                return
            self.image[addr : addr + len(data)] = data
            self.have[addr : addr + len(data)] = b"\x01" * len(data)
        elif kind == 4 and len(body) >= 4:
            (self.crc32,) = struct.unpack_from("<I", body)
            self.done = True

    def check(self) -> list[str]:
        problems = list(self.problems)
        if not self.size:
            return problems + ["no start message"]
        missing = self.size - sum(self.have)
        if missing:
            first = self.have.index(0)
            problems.append(f"{missing} bytes missing, first at {first:#07x}")
        if not self.done:
            problems.append("no end message: the dump was cut short")
        elif not missing and zlib.crc32(self.image) != self.crc32:
            problems.append(f"CRC-32 differs: sent {self.crc32:08x}, received {zlib.crc32(self.image):08x}")
        return problems


def messages(stream: bytes):
    """The SysEx messages in a raw MIDI byte stream, without F0 and F7. Real-time bytes are dropped."""
    current: bytearray | None = None
    for b in stream:
        if b >= 0xF8:
            continue
        if b == 0xF0:
            current = bytearray()
        elif b == 0xF7:
            if current is not None:
                yield bytes(current)
            current = None
        elif b & 0x80:
            current = None
        elif current is not None:
            current.append(b)


def describe(image: bytes) -> str:
    lines = []
    for start, stop, name in REGIONS:
        part = image[start:stop]
        used = sum(1 for i in range(0, len(part), 0x1000) if part[i : i + 0x1000].strip(b"\xff"))
        lines.append(f"  {start:#07x}-{stop - 1:#07x}  {name:<34} {used} of {len(part) // 0x1000} sectors used")
    return "\n".join(lines)


def finish(dump: Dump, out: Path) -> int:
    problems = dump.check()
    if problems:
        print("error: the dump is not usable:\n  - " + "\n  - ".join(problems), file=sys.stderr)
        return 1
    out.write_bytes(dump.image)
    print(f"wrote {out} ({dump.size} bytes, chip id {dump.chip_id.hex(' ')}, crc32 {dump.crc32:08x})")
    print(describe(dump.image))
    return 0


def receive(port: str | None, out: Path) -> int:
    import mido  # needs: uv run --with mido --with python-rtmidi

    names = mido.get_input_names()
    if not names:
        sys.exit("error: no MIDI inputs found")
    if port is None:
        if len(names) > 1:
            sys.exit("error: several MIDI inputs; pick one with --port:\n  " + "\n  ".join(names))
        port = names[0]
    dump = Dump()
    print(f"listening on {port}; press Play on the KeyStep (Ctrl-C to give up)")
    with mido.open_input(port) as source:
        for message in source:
            if message.type != "sysex":
                continue
            dump.feed(bytes(message.data))
            if dump.size:
                print(f"\r{sum(dump.have) * 100 // dump.size:3d} %", end="", flush=True)
            if dump.done:
                break
    print()
    return finish(dump, out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("decode", help="reassemble from a file of raw MIDI bytes")
    p.add_argument("capture", type=Path)
    p.add_argument("-o", "--out", type=Path, required=True)
    p = sub.add_parser("receive", help="reassemble from a MIDI input")
    p.add_argument("--port")
    p.add_argument("-o", "--out", type=Path, required=True)
    p = sub.add_parser("info", help="summarise a flash image")
    p.add_argument("image", type=Path)
    args = parser.parse_args()

    if args.command == "decode":
        dump = Dump()
        for message in messages(args.capture.read_bytes()):
            dump.feed(message)
        return finish(dump, args.out)
    if args.command == "receive":
        return receive(args.port, args.out)
    image = args.image.read_bytes()
    print(f"{args.image}: {len(image)} bytes, crc32 {zlib.crc32(image):08x}")
    print(describe(image))
    return 0


if __name__ == "__main__":
    sys.exit(main())
