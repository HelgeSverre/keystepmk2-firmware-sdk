#!/usr/bin/env python3
"""Send a firmware image to a KeyStep mk2 the way MIDI Control Center does (docs/UPDATER.md).

    uv run --with unicorn tools/ksflash.py sim sdk/build/hello/hello.img      # in the emulator
    uv run --with unicorn tools/ksflash.py selftest                           # the refusals, in the emulator
    uv run tools/ksflash.py check IMAGE                                       # the image checks only
    uv run --with pyusb tools/ksflash.py probe                                # read a real device's descriptors
    uv run --with pyusb tools/ksflash.py verify IMAGE                         # compare the flash (reads only)
    uv run --with pyusb tools/ksflash.py read OUT                             # save the flash (reads only)
    uv run --with pyusb tools/ksflash.py flash IMAGE --real                   # a real device

What is and is not known:
- The sequence (0xF2 enter, 0xF4 select image, DFU download blocks with status polls, a
  zero-length block, 0xF3 leave) is read from MIDI Control Center's code. The bootloader that
  answers it has not been read.
- `sim` and `selftest` run against FakeBootloader below, which is that reading turned into
  code. They test this tool's logic and refusals, not the real bootloader.
- `flash --real` has flashed stock 1.0.2 to one unit, from an update mode entered with Tempo,
  and the read-back was identical (docs/BOOTLOADER.md, 2026-11-24). Nothing but a stock image has
  been sent to a unit with it.

The tool refuses: an image that fails tools/fwpack.py's checks, a device that is not
1C75:0228/0229 with a firmware-update interface, and a device in update mode whose identity
differs from the one that was asked to enter it. It only ever sends the image file; where the
bytes land is the bootloader's doing.
"""

from __future__ import annotations

import argparse
import contextlib
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fwpack

VENDOR = fwpack.VENDOR_ID
PRODUCTS = fwpack.PRODUCT_IDS
DFU_CLASS = (0xFE, 0x01)
ENTER, LEAVE, SELECT = 0xF2, 0xF3, 0xF4
DNLOAD, UPLOAD, GETSTATUS, ABORT = 1, 2, 3, 6
DFU_IDLE, DN_BUSY, UPLOAD_IDLE, DFU_ERROR = 2, 4, 9, 10
STUCK = 0x0F  # where the bootloader goes when upload block 1 is asked for; only a power cycle clears it
FIRST_READ_BLOCK = 2  # upload block 0 is the command list, block 1 must never be sent
STATES = {
    0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE", 3: "dfuDNLOAD-SYNC", 4: "dfuDNBUSY", 5: "dfuDNLOAD-IDLE",
    6: "dfuMANIFEST-SYNC", 7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET", 9: "dfuUPLOAD-IDLE", 10: "dfuERROR",
}  # fmt: skip
STATUS = {
    0: "OK", 1: "file is not targeted for use by this device", 2: "file fails a vendor-specific verification",
    3: "device is unable to write memory", 4: "memory erase failed", 5: "memory erase check failed",
    6: "program memory function failed", 7: "programmed memory failed verification",
    8: "address out of range", 9: "download ended early", 10: "firmware is corrupt", 14: "unexpected request",
}  # fmt: skip


class Refused(Exception):
    """The tool will not go on. Nothing more is sent after this."""


@dataclass
class Device:
    """One USB device as the flasher sees it. `control` sends a request on endpoint 0."""

    vendor: int
    product: int
    serial: str
    interfaces: list[tuple[int, int, int]]  # class, subclass, protocol
    control: object = field(repr=False, default=None)
    name: str = ""

    @property
    def dfu_interface(self) -> int | None:
        for index, interface in enumerate(self.interfaces):
            if interface[:2] == DFU_CLASS:
                return index
        return None

    @property
    def update_mode(self) -> bool:
        """MIDI Control Center's test: exactly one interface, and it is the firmware-update one."""
        return len(self.interfaces) == 1 and self.dfu_interface == 0

    def describe(self) -> str:
        mode = "update mode" if self.update_mode else "application"
        return f"{self.vendor:04X}:{self.product:04X} {self.name!r} serial {self.serial!r}, {len(self.interfaces)} interface(s): {mode}"


def check_identity(device: Device) -> None:
    if device.vendor != VENDOR or device.product not in PRODUCTS:
        raise Refused(f"not a KeyStep mk2: {device.vendor:04X}:{device.product:04X}")
    if device.dfu_interface is None:
        raise Refused("the device has no firmware-update interface")


def get_status(device: Device) -> tuple[int, int]:
    reply = device.control(0xA1, GETSTATUS, 0, 0, 6)
    if reply is None or len(reply) < 5:
        raise Refused("the device gave no status")
    return reply[0], reply[4]


def settle(device: Device, what: str) -> None:
    """Poll the status until the device is no longer busy; stop on an error."""
    for _ in range(20000):
        status, state = get_status(device)
        if state == DFU_ERROR:
            raise Refused(f"the device reports an error after {what}: {STATUS.get(status, f'status {status}')}")
        if state != DN_BUSY:
            return
    raise Refused(f"the device stayed busy after {what}")


def read_back(device: Device, length: int, say=print) -> bytes:
    """Read `length` bytes of flash from the load address (0x08008000) through DFU upload.

    Seen on a unit (docs/BOOTLOADER.md): upload block 2 returns flash from 0x08008000, 64 bytes a
    block; block 0 returns three bytes, block 1 leaves the bootloader stuck until it is switched
    off. Only upload, status and abort requests are sent: nothing is written."""
    check_identity(device)
    if not device.update_mode:
        raise Refused("the device is not in update mode: hold Tempo while switching it on")
    status, state = get_status(device)
    if state != DFU_IDLE or status:
        raise Refused(f"the bootloader is not idle (state {state}, status {status}): switch off and on with Tempo held")
    functional = device.control(0x81, 6, 0x2100, 0, 9)
    block = struct.unpack_from("<H", functional, 5)[0] if functional and len(functional) >= 7 else 0
    if not 8 <= block <= 4096:
        raise Refused(f"the device's transfer size is {block}")
    if not 0 < length <= fwpack.FLASH_END - fwpack.LOAD_ADDRESS:
        raise Refused(f"{length} bytes is not within the application area")
    count = -(-length // block)
    say(f"reading {length} bytes from {fwpack.LOAD_ADDRESS:#010x} in blocks of {block}")
    data = bytearray()
    for number in range(FIRST_READ_BLOCK, FIRST_READ_BLOCK + count):
        chunk = device.control(0xA1, UPLOAD, number, 0, block)
        if chunk is None or len(chunk) != block:
            raise Refused(f"upload block {number} returned {0 if chunk is None else len(chunk)} bytes")
        data += chunk
    device.control(0x21, ABORT, 0, 0, b"")
    return bytes(data[:length])


def flash(
    transport, image: bytes, image_num: int = 0, say=print, skip_image_checks: bool = False, verify: bool = False
) -> Device:
    """The whole update. Returns the device as it comes back afterwards.

    `skip_image_checks` exists for the self-test, to see what the device model does with a bad
    image. The command line never sets it."""
    problems = [] if skip_image_checks else fwpack.check(image)
    if problems:
        raise Refused("the image fails the checks:\n  - " + "\n  - ".join(problems))

    device = transport.find()
    if device is None:
        raise Refused("no device found")
    say(f"found {device.describe()}")
    check_identity(device)
    serial = device.serial
    # The application that comes back cannot be matched by serial: after Tempo at power-on the
    # bootloader's serial is noise (docs/BOOTLOADER.md), and an SDK firmware has a serial of its own.
    back_as = None

    if not device.update_mode:
        say("asking the application for update mode (request 0xF2)")
        device.control(0x21, ENTER, 0, device.dfu_interface, b"")
        device = transport.wait(serial, update_mode=True, timeout_s=30)
        if device is None:
            raise Refused("the device did not come back in update mode within 30 s; nothing was written")
        say(f"now   {device.describe()}")
    check_identity(device)
    if not device.update_mode or device.serial != serial:
        raise Refused("the device in update mode is not the one that was asked; nothing was written")

    functional = device.control(0x81, 6, 0x2100, 0, 9)
    block = struct.unpack_from("<H", functional, 5)[0] if functional and len(functional) >= 7 else 0
    if not 8 <= block <= 4096:
        raise Refused(f"the device's transfer size is {block}; nothing was written")

    say(f"sending {len(image)} bytes in blocks of {block}")
    device.control(0x21, SELECT, image_num, 0, b"")
    number = 0
    for at in range(0, len(image), block):
        device.control(0x21, DNLOAD, number & 0xFFFF, 0, image[at : at + block])
        settle(device, f"block {number}")
        number += 1
    device.control(0x21, DNLOAD, number & 0xFFFF, 0, b"")
    settle(device, "the end of the download")

    if verify:  # before the application starts: a wrong flash is then still in update mode
        _status, state = get_status(device)
        if state != DFU_IDLE:
            say(f"not verified: the bootloader is in state {state} after the download, not idle")
        else:
            body = image[fwpack.HEADER_SIZE :]
            if read_back(device, len(body), say) != body:
                raise Refused("the flash does not hold the image that was sent. The unit is still in update mode.")
            say("verified: the flash holds the image")

    say("leaving update mode (request 0xF3)")
    device.control(0x21, LEAVE, 0, 0, b"")
    with contextlib.suppress(Refused, OSError):  # the device may already be gone
        get_status(device)
    device = transport.wait(back_as, update_mode=False, timeout_s=10)
    if device is None:
        raise Refused(
            "the image was sent, but the device did not come back as an application within 10 s. "
            "If it is in update mode, send the stock image again."
        )
    say(f"back  {device.describe()}")
    return device


# ---- the emulator: an application in tools/emu.py, and a stand-in for the bootloader ----


class FakeBootloader:
    """A guess at the factory bootloader, built from what MIDI Control Center expects of it.

    Assumptions, none of them checked on a unit: same USB ids and serial as the application, one
    interface (FE/01), 64-byte blocks, the application area is erased when the first block
    arrives, the image is judged at the end by the header rules in tools/fwpack.py, and request
    0xF3 starts the application if there is a valid one."""

    BLOCK = 64

    def __init__(self, serial: str, product: int = fwpack.PRODUCT_ID, vendor: int = VENDOR):
        self.serial, self.product, self.vendor = serial, product, vendor
        self.received = bytearray()
        self.expect = 0
        self.state, self.status = 2, 0
        self.busy = 0
        self.app: bytes | None = None  # a valid image, once one has been taken
        self.app_erased = False
        self.left = False
        self.uploads: list[int] = []  # the block numbers asked for

    def device(self) -> Device:
        return Device(self.vendor, self.product, self.serial, [(0xFE, 0x01, 0x02)], self.control, "update mode (model)")

    def fail(self, status: int) -> None:
        self.state, self.status = DFU_ERROR, status

    def control(self, request_type, request, value=0, index=0, data=b""):
        if request_type == 0x81 and request == 6 and value >> 8 == 0x21:
            return bytes([9, 0x21, 0x0B, 0xFF, 0x00]) + struct.pack("<H", self.BLOCK) + b"\x10\x01"
        if request_type == 0xA1 and request == GETSTATUS:
            state = self.state
            if self.busy:
                self.busy -= 1
                state = DN_BUSY
            return bytes([self.status, 0, 0, 0, state, 0])
        if request_type == 0xA1 and request == UPLOAD:  # as seen on a unit
            self.uploads.append(value)
            if self.state not in (DFU_IDLE, UPLOAD_IDLE):
                raise Refused("the bootloader model stalls the upload")
            if value == 0:
                return b"\x00\x21\x41"
            if value == 1:
                self.state = STUCK
                raise Refused("the bootloader model stalls upload block 1 and is stuck")
            body = (self.app or b"")[fwpack.HEADER_SIZE :]
            at = (value - FIRST_READ_BLOCK) * self.BLOCK
            self.state = UPLOAD_IDLE
            return body[at : at + self.BLOCK].ljust(self.BLOCK, b"\xff")
        if request_type != 0x21:
            raise Refused(f"the bootloader model stalls request {request:#04x}")
        if request == ABORT:
            if self.state == UPLOAD_IDLE:
                self.state = DFU_IDLE
            return None
        if request == SELECT:
            self.received, self.expect, self.state, self.status = bytearray(), 0, 2, 0
        elif request == DNLOAD and self.state != DFU_ERROR:
            if value != self.expect:
                self.fail(14)
            elif data:
                self.app_erased = True  # from here on the old application is gone
                self.app = None
                self.received += data
                self.expect += 1
                self.state, self.busy = 5, 1
                if len(self.received) > fwpack.FLASH_END - fwpack.LOAD_ADDRESS + fwpack.HEADER_SIZE:
                    self.fail(8)
            else:
                problems = fwpack.check(bytes(self.received))
                if problems:
                    self.fail(1 if "USB id" in " ".join(problems) else 10)
                else:
                    self.app = bytes(self.received)
                    self.state = 2
        elif request == LEAVE:
            self.left = True
        return None


class SimTransport:
    """An application running in the emulator, with FakeBootloader behind the reset."""

    def __init__(self, app_image: bytes, say=print):
        self.say = say
        self.boot_model: FakeBootloader | None = None
        self.in_update = False
        self.board = None
        self.start_app(app_image)

    def start_app(self, image: bytes) -> None:
        import emu

        self.app_image = image
        self.board = emu.Board(image[fwpack.HEADER_SIZE :])
        self.board.run(3000)
        if not self.board.usb.attached:
            raise Refused("the firmware in the emulator did not connect to USB")
        self.info = self.board.usb.host.enumerate()
        self.in_update = False

    def app_device(self) -> Device:
        host, info = self.board.usb.host, self.info
        interfaces = [i["class"] for i in info["interfaces"]]
        return Device(info["vendor"], info["product"], info["serial"], interfaces, host.control, info["product_name"])

    def find(self) -> Device | None:
        return self.boot_model.device() if self.in_update else self.app_device()

    def wait(self, serial: str, update_mode: bool, timeout_s: float) -> Device | None:
        if update_mode:
            self.board.run(timeout_s * 1000)  # stops early at the reset
            if self.board.stopped != "reset" or self.board.peek(0x20000000) != 0xDEADBEEF:
                return None
            text = bytes(self.board.uc.mem_read(0x20000004, 16)).decode("ascii", "replace")
            self.say(f"      (emulator: reset with the update marker, serial {text!r}; bootloader model takes over)")
            if self.boot_model is None:
                self.boot_model = FakeBootloader(text, self.info["product"])
            self.in_update = True
            return self.boot_model.device()
        model = self.boot_model
        if not model.left or model.app is None:
            return None  # the model stays in update mode without a valid application
        self.say("      (emulator: bootloader model starts the new application)")
        self.start_app(model.app)
        self.boot_model = None
        device = self.app_device()
        return device if serial is None or device.serial == serial else None


# ---- a real device, through libusb. Reading descriptors is all that has a reason to run yet. ----


class UsbTransport:
    def __init__(self):
        import usb.core  # needs: uv run --with pyusb, and libusb installed
        import usb.util

        self.core, self.util = usb.core, usb.util

    def devices(self) -> list[Device]:
        found = []
        for dev in self.core.find(find_all=True, idVendor=VENDOR):
            config = dev.get_active_configuration()
            interfaces = [(i.bInterfaceClass, i.bInterfaceSubClass, i.bInterfaceProtocol) for i in config]

            def control(request_type, request, value=0, index=0, data=b"", dev=dev):
                result = dev.ctrl_transfer(request_type, request, value, index, data, timeout=5000)
                return bytes(result) if request_type & 0x80 else None

            serial = self.util.get_string(dev, dev.iSerialNumber) if dev.iSerialNumber else ""
            name = self.util.get_string(dev, dev.iProduct) if dev.iProduct else ""
            found.append(Device(dev.idVendor, dev.idProduct, serial, interfaces, control, name))
        return found

    def find(self) -> Device | None:
        matches = [d for d in self.devices() if d.product in PRODUCTS]
        if len(matches) > 1:
            raise Refused("more than one KeyStep mk2 is connected; unplug all but one")
        return matches[0] if matches else None

    def wait(self, serial: str, update_mode: bool, timeout_s: float) -> Device | None:
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            time.sleep(0.25)
            try:
                for device in self.devices():
                    if device.product not in PRODUCTS or device.update_mode != update_mode:
                        continue
                    if serial is None or device.serial == serial:
                        return device
            except (OSError, ValueError):  # the device is between two identities: no strings yet
                continue
        return None


# ---- commands ----


def load_image(path: Path) -> tuple[bytes, int]:
    image, info = fwpack.read_image(path)
    return image, int(info["images"][0].get("image_num", 0)) if info else 0


def cmd_check(args) -> int:
    image, _ = load_image(args.image)
    problems = fwpack.check(image)
    print(f"{args.image}: {len(image)} bytes" + ("" if problems else ", passes the checks"))
    for p in problems:
        print(f"  - {p}")
    return 1 if problems else 0


def cmd_sim(args) -> int:
    image, number = load_image(args.image)
    running, _ = load_image(args.running)
    print(f"emulator: starting {args.running.name}")
    transport = SimTransport(running)
    flash(transport, image, number)
    transport.board.run(300)
    transport.board.show_display()
    return 0


def cmd_selftest(_args) -> int:
    stock, _ = load_image(fwpack.ROOT / "firmware" / "keystep_mk2__1_0_2.ks2fw")
    sdk = fwpack.ROOT / "sdk" / "build" / "hello" / "hello.img"
    if not sdk.exists():
        sys.exit("error: build the SDK example first: make -C sdk APP=hello")
    hello = sdk.read_bytes()

    def quiet(*_):
        pass

    results = []

    def expect(name: str, ok: bool, detail: str = "") -> None:
        results.append(ok)
        print(f"{'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")

    def refusal(name: str, needle: str, run) -> None:
        try:
            run()
            expect(name, False, "was not refused")
        except Refused as e:
            expect(name, needle in str(e), str(e).splitlines()[0])

    t = SimTransport(stock, quiet)
    device = flash(t, hello, say=quiet)
    expect("stock -> SDK hello", t.app_image == hello and not device.update_mode)
    device = flash(t, stock, say=quiet)
    expect("SDK hello -> stock (through the SDK's own 0xF2)", t.app_image == stock and device.name == "KeyStep mk2")
    said: list[str] = []
    device = flash(t, hello, say=said.append, verify=True)
    expect(
        "stock -> SDK hello, read back before leaving",
        t.app_image == hello and "verified: the flash holds the image" in said,
    )
    device = flash(t, stock, say=quiet, verify=True)

    bad = bytearray(hello)
    bad[200] ^= 1
    sent = len(t.boot_model.received) if t.boot_model else 0
    refusal(
        "image with a wrong sum is refused before anything is sent",
        "fails the checks",
        lambda: flash(t, bytes(bad), say=quiet),
    )
    expect("  and the device was left alone", not t.in_update and sent == 0 and t.board.stopped == "")
    refusal("truncated image", "fails the checks", lambda: flash(t, hello[:-4], say=quiet))
    wrong = bytearray(hello)
    wrong[4:8] = struct.pack("<HH", 0x1234, 0x5678)
    wrong[0x3F] = fwpack.xor8(bytes(wrong[:63]))
    refusal("image for another product", "USB id", lambda: flash(t, bytes(wrong), say=quiet))

    refusal(
        "bad image forced past the tool: the bootloader model rejects it",
        "reports an error",
        lambda: flash(t, bytes(bad), say=quiet, skip_image_checks=True),
    )
    expect("  and the device stays in update mode", t.in_update and t.boot_model.app is None)
    device = flash(t, stock, say=quiet)
    expect("  and stock can be flashed from there", t.app_image == stock and not device.update_mode)

    model = FakeBootloader("noise")
    model.app = hello
    body = hello[fwpack.HEADER_SIZE :]
    expect("read back through upload", read_back(model.device(), len(body), quiet) == body and model.state == DFU_IDLE)
    expect("  and block 1 is never asked for", 1 not in model.uploads and min(model.uploads) == FIRST_READ_BLOCK)
    refusal("reading back from the application", "not in update mode", lambda: read_back(t.find(), 64, quiet))
    model.state = STUCK
    refusal("reading back from a stuck bootloader", "not idle", lambda: read_back(model.device(), 64, quiet))

    class Other(SimTransport):
        def find(self):
            device = super().find()
            device.product = 0x0218
            return device

    refusal(
        "a device with another product id", "not a KeyStep mk2", lambda: flash(Other(stock, quiet), hello, say=quiet)
    )

    class Swapped(SimTransport):
        def wait(self, serial, update_mode, timeout_s):
            device = super().wait(serial, update_mode, timeout_s)
            if device and update_mode:
                device.serial = "SOMEONE-ELSE"
            return device

    refusal(
        "a different unit appearing in update mode",
        "not the one that was asked",
        lambda: flash(Swapped(stock, quiet), hello, say=quiet),
    )
    print(f"{sum(results)} of {len(results)} passed")
    return 0 if all(results) else 1


def cmd_probe(_args) -> int:
    devices = UsbTransport().devices()
    if not devices:
        print("no Arturia device found")
        return 1
    for device in devices:
        print(device.describe())
        for index, interface in enumerate(device.interfaces):
            print(f"  interface {index}: class {interface[0]:02X}/{interface[1]:02X}/{interface[2]:02X}")
        if device.dfu_interface is not None:
            try:
                functional = device.control(0x81, 6, 0x2100, device.dfu_interface, 9)
                print(f"  firmware-update descriptor: {functional.hex(' ')}")
            except OSError as e:
                print(f"  firmware-update descriptor: not readable ({e})")
    return 0


def read_device(length: int) -> bytes:
    """Read back from the connected unit. A running application is asked for update mode first
    (request 0xF2, as MIDI Control Center does); the unit then stays there until switched off."""
    transport = UsbTransport()
    device = transport.find()
    if device is None:
        raise Refused("no device found")
    print(f"found {device.describe()}")
    check_identity(device)
    entered = not device.update_mode
    if entered:
        serial = device.serial
        print("asking the application for update mode (request 0xF2)")
        device.control(0x21, ENTER, 0, device.dfu_interface, b"")
        device = transport.wait(serial, update_mode=True, timeout_s=30)
        if device is None:
            raise Refused("the device did not come back in update mode within 30 s")
        print(f"now   {device.describe()}")
    data = read_back(device, length)
    if entered:  # 0xF3 only starts the application after a download (docs/BOOTLOADER.md)
        print("the unit stays in update mode: switch it off and on to start the application")
    return data


def cmd_read(args) -> int:
    data = read_device(args.length)
    args.out.write_bytes(data)
    print(f"wrote {args.out} ({len(data)} bytes)")
    return 0


def cmd_verify(args) -> int:
    image, _number = load_image(args.image)
    body = image[fwpack.HEADER_SIZE :]
    data = read_device(len(body))
    if data == body:
        print(f"the flash holds {args.image.name}: {len(body)} bytes identical")
        return 0
    first = next(i for i in range(len(body)) if data[i] != body[i])
    differing = sum(a != b for a, b in zip(data, body, strict=True))
    print(f"DIFFERENT: {differing} bytes, the first at {fwpack.LOAD_ADDRESS + first:#010x}")
    return 1


def cmd_flash(args) -> int:
    if not args.real:
        sys.exit("error: this would write to a real device. Use `sim`, or add --real.")
    image, number = load_image(args.image)
    print(f"This writes {args.image.name} to the connected KeyStep. Type FLASH to go on: ", end="", flush=True)
    if sys.stdin.readline().strip() != "FLASH":
        sys.exit("stopped")
    flash(UsbTransport(), image, number, verify=not args.no_verify)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("check", help="run the image checks")
    p.add_argument("image", type=Path)
    p = sub.add_parser("sim", help="flash in the emulator, against the bootloader model")
    p.add_argument("image", type=Path)
    p.add_argument("--running", type=Path, default=fwpack.ROOT / "firmware" / "keystep_mk2__1_0_2.ks2fw",
                   help="the firmware the emulated device runs before the update (default: stock 1.0.2)")  # fmt: skip
    sub.add_parser("selftest", help="the update and every refusal, in the emulator")
    sub.add_parser("probe", help="print the descriptors of connected Arturia devices (reads only)")
    p = sub.add_parser("read", help="save the flash from 0x08008000 to a file (reads only)")
    p.add_argument("out", type=Path)
    p.add_argument("--length", type=lambda v: int(v, 0), default=fwpack.FLASH_END - fwpack.LOAD_ADDRESS)
    p = sub.add_parser("verify", help="compare the flash with an image (reads only)")
    p.add_argument("image", type=Path)
    p = sub.add_parser("flash", help="flash a real device")
    p.add_argument("image", type=Path)
    p.add_argument("--real", action="store_true")
    p.add_argument("--no-verify", action="store_true", help="do not read the flash back before leaving update mode")
    args = parser.parse_args()
    try:
        commands = {
            "check": cmd_check,
            "sim": cmd_sim,
            "selftest": cmd_selftest,
            "probe": cmd_probe,
            "read": cmd_read,
            "verify": cmd_verify,
            "flash": cmd_flash,
        }
        return commands[args.command](args)
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"stopped on a USB error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
