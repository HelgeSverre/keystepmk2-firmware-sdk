"""The USB device controller of the STM32G0B1, and a host at the other end of the cable.

`Usb` models the registers at 0x40005C00 and the packet memory at 0x40009800 as the firmware
sees them. `Host` drives it: bus reset, enumeration, control requests and bulk transfers, each
running the processor until the device answers.

    board = emu.Board(...)
    board.run(3000)                     # let the firmware start and pull D+ up
    host = board.usb.host
    info = host.enumerate()             # descriptors, address and configuration set
    host.midi_send(bytes)               # MIDI bytes as USB-MIDI packets to the bulk OUT endpoint
    host.midi_receive(ms=50)            # MIDI bytes from the bulk IN endpoint

Not modelled: start-of-frame, suspend and resume, isochronous and double-buffered endpoints,
data toggle checking, and bus timing (a transfer takes as long as the firmware takes to answer).
"""

from __future__ import annotations

import struct

USB = 0x40005C00
USB_RAM = 0x40009800
USB_IRQ = 8

# CHEPnR bits
EA, STAT_TX, DTOG_TX, VTTX, SETUP, STAT_RX, DTOG_RX, VTRX = 0xF, 0x30, 0x40, 0x80, 0x800, 0x3000, 0x4000, 0x8000
PLAIN = 0x070F  # address, kind and type: written as they are
TOGGLE = STAT_TX | DTOG_TX | STAT_RX | DTOG_RX  # writing 1 flips the bit
DISABLED, STALL, NAK, VALID = 0, 1, 2, 3
# ISTR / CNTR bits
CTR, RESET = 0x8000, 0x0400
ISTR_CLEARABLE = 0x17F80
DPPU = 0x8000


class UsbError(OSError):
    pass


class Usb:
    def __init__(self, board):
        self.board = board
        self.chep = [0] * 8
        self.cntr = 0x3
        self.istr = 0
        self.daddr = 0
        self.bcdr = 0
        self.other: dict[int, int] = {}
        self.host = Host(self)

    # ---- the firmware's side ----

    def read(self, off: int) -> int:
        if off < 0x20:
            return self.chep[off >> 2]
        if off == 0x40:
            return self.cntr
        if off == 0x44:
            return self.status()
        if off == 0x4C:
            return self.daddr
        if off == 0x58:
            return self.bcdr
        return self.other.get(off, 0)

    def write(self, off: int, value: int) -> None:
        if off < 0x20:
            old = self.chep[off >> 2]
            new = old & ~PLAIN | value & PLAIN
            new ^= value & TOGGLE
            new &= value | ~(VTTX | VTRX)  # the two transfer flags clear when written as 0
            self.chep[off >> 2] = new
        elif off == 0x40:
            if value & 1 and not self.cntr & 1:  # controller reset
                self.chep = [0] * 8
                self.istr = self.daddr = 0
            self.cntr = value
        elif off == 0x44:
            self.istr &= value | ~ISTR_CLEARABLE
        elif off == 0x4C:
            self.daddr = value & 0xFF
        elif off == 0x58:
            self.bcdr = value
        else:
            self.other[off] = value

    def status(self) -> int:
        """ISTR: the stored flags, plus CTR with the endpoint and direction of the oldest transfer."""
        for n, reg in enumerate(self.chep):
            if reg & (VTTX | VTRX):
                return self.istr | CTR | n | (0x10 if reg & VTRX else 0)
        return self.istr

    def interrupt(self) -> bool:
        return bool(self.status() & self.cntr & 0x1FF80)

    @property
    def attached(self) -> bool:
        return bool(self.bcdr & DPPU) and not self.cntr & 0x3

    # ---- the bus's side ----

    def endpoint(self, number: int) -> int:
        for n, reg in enumerate(self.chep):
            if reg & EA == number and reg & (STAT_TX | STAT_RX):
                return n
        raise UsbError(f"endpoint {number} is not configured")

    def descriptor(self, n: int) -> tuple[int, int]:
        mem = self.board.uc.mem_read(USB_RAM + 8 * n, 8)
        return struct.unpack("<II", mem)

    def deliver(self, number: int, data: bytes, setup: bool = False) -> str:
        """A SETUP or OUT packet from the host. Returns ack, nak or stall."""
        n = self.endpoint(number)
        reg = self.chep[n]
        state = reg >> 12 & 3
        if not setup and state != VALID:
            return "stall" if state == STALL else "nak"
        _tx, rx = self.descriptor(n)
        self.board.uc.mem_write(USB_RAM + (rx & 0xFFFF), data + b"\0" * (-len(data) % 4))
        self.board.uc.mem_write(USB_RAM + 8 * n + 4, struct.pack("<I", rx & ~0x03FF0000 | len(data) << 16))
        reg = reg & ~STAT_RX | NAK << 12 | VTRX
        reg = reg | SETUP if setup else reg & ~SETUP
        if setup:
            reg = reg & ~STAT_TX | NAK << 4  # a SETUP cancels a pending answer
        self.chep[n] = reg
        return "ack"

    def collect(self, number: int) -> bytes | str:
        """An IN token from the host. Returns the packet, or nak or stall."""
        n = self.endpoint(number)
        reg = self.chep[n]
        state = reg >> 4 & 3
        if state != VALID:
            return "stall" if state == STALL else "nak"
        tx, _rx = self.descriptor(n)
        data = bytes(self.board.uc.mem_read(USB_RAM + (tx & 0xFFFF), tx >> 16 & 0x3FF))
        self.chep[n] = reg & ~STAT_TX | NAK << 4 | VTTX
        return data


class Host:
    """What a computer does on the bus. Every call runs the processor while it waits."""

    STEP_MS = 0.25

    def __init__(self, usb: Usb):
        self.usb = usb
        self.max0 = 64
        self.midi_out_ep = self.midi_in_ep = None
        self.midi_pending = bytearray()

    def wait(self, ms: float = STEP_MS) -> None:
        if self.usb.board.run(ms):
            raise UsbError(f"the processor stopped: {self.usb.board.stopped}")

    def reset(self) -> None:
        if not self.usb.attached:
            raise UsbError("the device has not connected (no pull-up on D+)")
        self.usb.chep = [0] * 8
        self.usb.istr |= RESET
        self.wait(2)

    def send(self, ep: int, data: bytes, setup: bool = False, timeout_ms: float = 500) -> None:
        waited = 0.0
        while (answer := self.usb.deliver(ep, data, setup)) != "ack":
            if answer == "stall":
                raise UsbError(f"endpoint {ep} OUT stalled")
            if waited >= timeout_ms:
                raise UsbError(f"endpoint {ep} OUT: no answer in {timeout_ms} ms")
            self.wait()
            waited += self.STEP_MS
        self.wait()

    def receive(self, ep: int, timeout_ms: float = 500) -> bytes | None:
        """One packet, or None if the device has nothing to send within the timeout."""
        waited = 0.0
        while isinstance(answer := self.usb.collect(ep), str):
            if answer == "stall":
                raise UsbError(f"endpoint {ep} IN stalled")
            if waited >= timeout_ms:
                return None
            self.wait()
            waited += self.STEP_MS
        self.wait()
        return answer

    def control(self, request_type: int, request: int, value: int = 0, index: int = 0, data: bytes | int = b""):
        """A control transfer on endpoint 0. `data` is the bytes to send, or the length to read."""
        reading = bool(request_type & 0x80)
        length = data if reading else len(data)
        self.send(0, struct.pack("<BBHHH", request_type, request, value, index, length), setup=True)
        if reading:
            got = bytearray()
            while len(got) < length:
                packet = self.receive(0)
                if packet is None:
                    raise UsbError(f"control request {request:#04x}: no data")
                got += packet
                if len(packet) < self.max0:
                    break
            self.send(0, b"")  # status
            return bytes(got[:length])
        for i in range(0, len(data), self.max0):
            self.send(0, data[i : i + self.max0])
        if self.receive(0) is None:  # status
            raise UsbError(f"control request {request:#04x}: no status")
        return None

    def string(self, index: int) -> str:
        if not index:
            return ""
        raw = self.control(0x80, 6, 0x0300 | index, 0x0409, 255)
        return raw[2 : raw[0]].decode("utf-16-le", "replace")

    def enumerate(self) -> dict:
        """Reset, read the descriptors, set address 1 and the first configuration."""
        self.reset()
        device = self.control(0x80, 6, 0x0100, 0, 18)
        self.max0 = device[7]
        self.control(0x00, 5, 1)
        vid, pid, bcd, i_man, i_prod, i_serial = struct.unpack_from("<HHHBBB", device, 8)
        head = self.control(0x80, 6, 0x0200, 0, 9)
        config = self.control(0x80, 6, 0x0200, 0, struct.unpack_from("<H", head, 2)[0])
        interfaces, endpoints, at, current = [], [], 0, None
        while at + 2 <= len(config) and config[at]:
            size, kind = config[at], config[at + 1]
            if kind == 4:
                current = {
                    "number": config[at + 2], "class": tuple(config[at + 5 : at + 8]),
                    "name": self.string(config[at + 8]), "endpoints": [],
                }  # fmt: skip
                interfaces.append(current)
            elif kind == 5 and current is not None:
                ep = {"address": config[at + 2], "type": config[at + 3] & 3,
                      "size": struct.unpack_from("<H", config, at + 4)[0]}  # fmt: skip
                current["endpoints"].append(ep)
                endpoints.append((current, ep))
            at += size
        self.control(0x00, 9, config[5])
        for interface, ep in endpoints:
            if interface["class"][:2] == (1, 3) and ep["type"] == 2:  # audio, MIDI streaming, bulk
                if ep["address"] & 0x80:
                    self.midi_in_ep = ep["address"] & 0x7F
                else:
                    self.midi_out_ep = ep["address"]
        return {
            "vendor": vid, "product": pid, "release": bcd, "manufacturer": self.string(i_man),
            "product_name": self.string(i_prod), "serial": self.string(i_serial),
            "max_power_ma": config[8] * 2, "device": device, "configuration": config, "interfaces": interfaces,
        }  # fmt: skip

    # ---- USB-MIDI ----

    def midi_send(self, data: bytes) -> None:
        """MIDI bytes as 4-byte USB-MIDI packets (cable 0)."""
        packets = bytearray()
        i = 0
        in_sysex = False
        while i < len(data):
            status = data[i]
            if status == 0xF0 or (in_sysex and (status < 0x80 or status == 0xF7)):  # SysEx, its end included
                chunk = data[i : i + 3]
                if 0xF7 in chunk:
                    chunk = chunk[: chunk.index(0xF7) + 1]
                    code = 4 + len(chunk)  # 5, 6 or 7: the message ends with one, two or three bytes
                    in_sysex = False
                else:
                    code = 4
                    in_sysex = True
                i += len(chunk)
            elif status >= 0xF8:
                chunk, code, i = data[i : i + 1], 0xF, i + 1
            else:
                size = 2 if status & 0xF0 in (0xC0, 0xD0) else 3
                chunk, code, i = data[i : i + size], status >> 4, i + size
            packets += bytes([code]) + chunk + b"\0" * (3 - len(chunk))
        for at in range(0, len(packets), 64):
            self.send(self.midi_out_ep, bytes(packets[at : at + 64]))

    def midi_receive(self, ms: float = 50) -> bytes:
        """MIDI bytes the device sends within `ms`."""
        out = bytearray()
        left = ms
        while left > 0:
            packet = self.receive(self.midi_in_ep, timeout_ms=min(left, 5))
            left -= 5
            for at in range(0, len(packet or b""), 4):
                code = packet[at] & 0xF
                size = {0x5: 1, 0xF: 1, 0x2: 2, 0x6: 2, 0xC: 2, 0xD: 2}.get(code, 3 if code >= 2 else 0)
                out += packet[at + 1 : at + 1 + size]
        return bytes(out)
