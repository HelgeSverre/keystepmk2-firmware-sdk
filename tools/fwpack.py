#!/usr/bin/env python3
"""Inspect, unpack and build KeyStep mk2 firmware images and .ks2fw packages.

An image is a 64-byte header followed by the code loaded at 0x08008000 (docs/FIRMWARE.md).
A package is an uncompressed zip holding info.json and one image.

    fwpack.py info    <image.bin | package.ks2fw>
    fwpack.py body    <image.bin | package.ks2fw> -o body.bin
    fwpack.py image   <body.bin> --version 1.0.2 -o image.bin
    fwpack.py package <image.bin> --version 1.0.2.782 --date 2025_11_13 -o out.ks2fw
    fwpack.py selftest            # rebuild every stock header and compare
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zipfile
from dataclasses import dataclass
from functools import reduce
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER_SIZE = 64
MAGIC = 0x0010
VENDOR_ID = 0x1C75
PRODUCT_ID = 0x0228
PRODUCT_IDS = (0x0228, 0x0229)  # the USB ids of the KeyStep mk2 and the KeyStep 37 mk2: both take the same image
LOAD_ADDRESS = 0x08008000
RAM_START, RAM_END = 0x20000000, 0x20024000
FLASH_END = 0x08080000  # the end of the 512 KiB flash
# The bootloader keeps its "valid image" mark in the last page and erases that page before the
# image's own pages (docs/BOOTLOADER.md, "The bootloader, read"), so an image must end before it.
IMAGE_END = 0x0807F800


def xor8(data: bytes) -> int:
    return reduce(lambda a, b: a ^ b, data, 0)


@dataclass
class Header:
    magic: int
    vendor: int
    product: int
    load: int
    end: int
    size: int
    body_xor: int
    patch: int
    major: int
    header_xor: int

    @classmethod
    def parse(cls, raw: bytes) -> Header:
        magic, vendor, product, _zero, load, end, size, body_xor, _r, patch, major = struct.unpack_from(
            "<HHIIIIIBBBB", raw, 0
        )
        return cls(magic, vendor, product, load, end, size, body_xor, patch, major, raw[63])


def build_header(body: bytes, major: int, patch: int, product: int = PRODUCT_ID) -> bytes:
    head = bytearray(HEADER_SIZE)
    struct.pack_into(
        "<HHIIIIIBBBB",
        head,
        0,
        MAGIC,
        VENDOR_ID,
        product,
        0,
        LOAD_ADDRESS,
        LOAD_ADDRESS + len(body),
        len(body),
        xor8(body),
        0,
        patch,
        major,
    )
    head[63] = xor8(head[:63])
    return bytes(head)


def check(image: bytes) -> list[str]:
    """Problems that would make an image wrong; an empty list means it matches the stock rules."""
    if len(image) <= HEADER_SIZE + 8:
        return ["file is too short to be an image"]
    h, body, problems = Header.parse(image), image[HEADER_SIZE:], []
    if h.magic != MAGIC:
        problems.append(f"magic is {h.magic:04X}, expected {MAGIC:04X}")
    if h.vendor != VENDOR_ID or h.product != PRODUCT_ID:  # the bootloader wants 0228 on both models
        problems.append(f"USB id is {h.vendor:04X}:{h.product:04X}")
    if h.load != LOAD_ADDRESS:
        problems.append(f"load address is {h.load:08X}")
    if h.size != len(body) or h.end != h.load + len(body):
        problems.append(f"size {h.size:#x} / end {h.end:08X} do not match a {len(body):#x}-byte body")
    if h.end > IMAGE_END:
        problems.append(
            f"image ends at {h.end:08X}, past {IMAGE_END:08X}: it would reach the page of the bootloader's mark"
        )
    if h.body_xor != xor8(body):
        problems.append(f"body XOR is {h.body_xor:02X}, computed {xor8(body):02X}")
    if h.header_xor != xor8(image[:63]):
        problems.append(f"header XOR is {h.header_xor:02X}, computed {xor8(image[:63]):02X}")
    if any(image[0x1C:0x3F]) or image[0x19] or any(image[8:12]):
        problems.append("reserved header bytes are not zero")
    sp, reset = struct.unpack_from("<II", body, 0)
    if not RAM_START < sp <= RAM_END or sp % 4:
        problems.append(f"initial stack pointer {sp:08X} is outside RAM")
    if not (reset & 1) or not LOAD_ADDRESS < reset < LOAD_ADDRESS + len(body):
        problems.append(f"reset vector {reset:08X} is not Thumb code inside the image")
    return problems


def read_image(path: Path) -> tuple[bytes, dict | None]:
    """The image bytes from an image file or a package, plus the package's info.json if any."""
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            info = json.loads(z.read("info.json"))
            return z.read(info["images"][0]["file_name"]), info
    return path.read_bytes(), None


def cmd_info(args: argparse.Namespace) -> int:
    image, info = read_image(args.file)
    h = Header.parse(image)
    if info:
        print(f"package   {info['version_number']}  {info['date']}  method {info['method']}")
    print(f"usb id    {h.vendor:04X}:{h.product:04X}")
    print(f"version   {h.major}.0.{h.patch} (header)")
    print(f"load      {h.load:08X}-{h.end:08X}  ({h.size} bytes)")
    sp, reset = struct.unpack_from("<II", image, HEADER_SIZE)
    print(f"vectors   sp {sp:08X}  reset {reset:08X}")
    problems = check(image)
    print("checks    " + ("ok" if not problems else "FAILED"))
    for p in problems:
        print(f"  - {p}")
    return 1 if problems else 0


def cmd_body(args: argparse.Namespace) -> int:
    image, _ = read_image(args.file)
    args.output.write_bytes(image[HEADER_SIZE:])
    print(f"wrote {args.output} ({len(image) - HEADER_SIZE} bytes, loads at {LOAD_ADDRESS:08X})")
    return 0


def parse_version(text: str) -> tuple[int, int]:
    parts = [int(p) for p in text.split(".")]
    return parts[0], parts[2]


def cmd_image(args: argparse.Namespace) -> int:
    body = args.body.read_bytes()
    major, patch = parse_version(args.version)
    image = build_header(body, major, patch) + body
    problems = check(image)
    if problems:
        print("refusing to write an image that fails the checks:", *problems, sep="\n  - ", file=sys.stderr)
        return 1
    args.output.write_bytes(image)
    print(f"wrote {args.output} ({len(image)} bytes)")
    return 0


def cmd_package(args: argparse.Namespace) -> int:
    image = args.image.read_bytes()
    problems = check(image)
    if problems:
        print("refusing to package an image that fails the checks:", *problems, sep="\n  - ", file=sys.stderr)
        return 1
    v = args.version.split(".")
    name = f"ks2_embedded__fw{v[0]}_{v[1]}_{v[2]}_{v[3]}__{args.date}_image_552.bin"
    info = {
        "version_number": args.version,
        "date": args.date,
        "vendorid": "0x1C75",
        "method": "dfu",
        "images": [{"image_num": 0, "file_name": name, "productid": f"0x{p:X}"} for p in PRODUCT_IDS],
    }
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_STORED) as z:
        z.writestr("info.json", json.dumps(info, indent=4))
        z.writestr(name, image)
    print(f"wrote {args.output}")
    return 0


def cmd_selftest(_args: argparse.Namespace) -> int:
    failed = 0
    for path in sorted((ROOT / "firmware").glob("[0-9]*/*.bin")):  # the versions, not firmware/device
        image = path.read_bytes()
        h = Header.parse(image)
        rebuilt = build_header(image[HEADER_SIZE:], h.major, h.patch) + image[HEADER_SIZE:]
        problems = check(image)
        ok = rebuilt == image and not problems
        failed += not ok
        print(
            f"{path.parent.name}  header rebuilt {'identical' if rebuilt == image else 'DIFFERENT'}"
            f"  checks {'ok' if not problems else problems}"
        )
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("info", help="print the header and run the checks")
    p.add_argument("file", type=Path)
    p.set_defaults(run=cmd_info)

    p = sub.add_parser("body", help="strip the header: the bytes loaded at 0x08008000")
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(run=cmd_body)

    p = sub.add_parser("image", help="put a header on a body")
    p.add_argument("body", type=Path)
    p.add_argument("--version", required=True, help="major.minor.patch, for example 1.0.2")
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(run=cmd_image)

    p = sub.add_parser("package", help="wrap an image in a .ks2fw package")
    p.add_argument("image", type=Path)
    p.add_argument("--version", required=True, help="major.minor.patch.build, for example 1.0.2.782")
    p.add_argument("--date", required=True, help="build date as YYYY_MM_DD")
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(run=cmd_package)

    p = sub.add_parser("selftest", help="rebuild every stock header in firmware/ and compare")
    p.set_defaults(run=cmd_selftest)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
