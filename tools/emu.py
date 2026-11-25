#!/usr/bin/env python3
"""Run a KeyStep mk2 application image on the host.

A Cortex-M0+ core (Unicorn) with the STM32G0B1 memory map and models of the parts of the board
the firmware needs: clock flags, timers, DMA, the OLED, the external flash, the LED chains, the
keybed matrix, the button shift register, the encoders and the I2C touch controller.
docs/HARDWARE.md says where each fact comes from. tools/panel.py puts a window on top of this.

    uv run --with unicorn tools/emu.py firmware/keystep_mk2__1_0_2.ks2fw --ms 5000
    uv run --with unicorn tools/emu.py image.bin --ms 3000 --script "1500 press=b3; 1700 release=b3"

Script actions, each after a time in ms: press=<control>, release=<control>, tap=<control>,
turn=<detents>, show (print the display), leds. Controls: a name from BUTTONS, tempo, b0..b15
(button shift-register bits) or k0..k31 (keys).
"""

from __future__ import annotations

import argparse
import signal
import struct
import sys
from collections import Counter
from pathlib import Path
from types import MappingProxyType

import fwpack
import usbmodel

try:
    from unicorn import (
        UC_ARCH_ARM,
        UC_HOOK_INTR,
        UC_HOOK_MEM_READ,
        UC_HOOK_MEM_READ_UNMAPPED,
        UC_HOOK_MEM_WRITE,
        UC_HOOK_MEM_WRITE_UNMAPPED,
        UC_MODE_MCLASS,
        UC_MODE_THUMB,
        Uc,
        UcError,
    )
    from unicorn.arm_const import (
        UC_ARM_REG_LR,
        UC_ARM_REG_PC,
        UC_ARM_REG_PRIMASK,
        UC_ARM_REG_R0,
        UC_ARM_REG_R1,
        UC_ARM_REG_R2,
        UC_ARM_REG_R3,
        UC_ARM_REG_R12,
        UC_ARM_REG_SP,
        UC_ARM_REG_XPSR,
    )
except ModuleNotFoundError:
    sys.exit("error: needs the 'unicorn' module. Run: uv run --with unicorn tools/emu.py ...")

FLASH_BASE, FLASH_SIZE = 0x08000000, 0x80000
RAM_BASE, RAM_SIZE = 0x20000000, 0x24000
SYSTICK_CSR = 0xE000E010
NVIC_ISER, NVIC_ICER, NVIC_ISPR = 0xE000E100, 0xE000E180, 0xE000E200
SCB_ICSR, SCB_VTOR, SCB_AIRCR = 0xE000ED04, 0xE000ED08, 0xE000ED0C
GPIOA, GPIOB, GPIOC, GPIOD = 0x50000000, 0x50000400, 0x50000800, 0x50000C00
DMA1, ADC, RCC, EXTI = 0x40020000, 0x40012400, 0x40021000, 0x40021800
INSNS_PER_US = 40  # how much code runs per simulated microsecond
SLICE_INSNS = 2000  # longest run between looks at the clock: 50 us
# Handlers return here. The stub tells the emulator the handler is done by writing to HANDLER_DONE,
# then restores the interrupted registers from the stack:
#   ldr r0, =HANDLER_DONE; str r0, [r0]; pop {r0-r2}; mov r12, r0; mov lr, r1; msr APSR, r2; pop {r0-r3, pc}
RETURN_STUB, HANDLER_DONE = 0x1FFF0000, 0x4002FFF0
RETURN_CODE = bytes.fromhex("0348 0060 07bc 8446 8e46 82f3 0088 0fbd f0ff0240")
KEYS = 32
LED_CHAINS = (39, 15)

# STM32G0B1 peripheral blocks, for naming register accesses in the report.
PERIPHERALS = {
    0x40000000: "TIM2", 0x40000400: "TIM3", 0x40000800: "TIM4", 0x40001000: "TIM6", 0x40001400: "TIM7",
    0x40002000: "TIM14", 0x40002800: "RTC", 0x40002C00: "WWDG", 0x40003000: "IWDG", 0x40003800: "SPI2",
    0x40003C00: "SPI3", 0x40004400: "USART2", 0x40004800: "USART3", 0x40004C00: "USART4",
    0x40005000: "USART5", 0x40005400: "I2C1", 0x40005800: "I2C2", 0x40005C00: "USB", 0x40006400: "FDCAN1",
    0x40006800: "FDCAN2", 0x40006C00: "CRS", 0x40007000: "PWR", 0x40007400: "DAC", 0x40007800: "CEC",
    0x40007C00: "LPTIM1", 0x40008000: "LPUART1", 0x40008400: "LPUART2", 0x40008800: "I2C3",
    0x40009400: "LPTIM2", 0x40009800: "USB_RAM", 0x40009C00: "USB_RAM", 0x4000A000: "UCPD1",
    0x4000A400: "UCPD2", 0x4000B000: "TAMP", 0x40010000: "SYSCFG", 0x40010200: "COMP",
    0x40012400: "ADC", 0x40012800: "ADC", 0x40012C00: "TIM1", 0x40013000: "SPI1", 0x40013800: "USART1",
    0x40013C00: "USART6", 0x40014000: "TIM15", 0x40014400: "TIM16", 0x40014800: "TIM17",
    0x40015800: "DBG", 0x40020000: "DMA1", 0x40020400: "DMA2", 0x40020800: "DMAMUX",
    0x40021000: "RCC", 0x40021800: "EXTI", 0x40022000: "FLASH", 0x40023000: "CRC", 0x40025000: "RNG",
    0x50000000: "GPIOA", 0x50000400: "GPIOB", 0x50000800: "GPIOC", 0x50000C00: "GPIOD",
    0x50001000: "GPIOE", 0x50001400: "GPIOF",
}  # fmt: skip
SPI1, SPI2, SPI3 = 0x40013000, 0x40003800, 0x40003C00
TIM2, TIM3 = 0x40000000, 0x40000400
I2C1, USART1 = 0x40005400, 0x40013800
TIMER_IRQ = {
    0x40012C00: 13, 0x40000000: 15, 0x40000400: 16, 0x40000800: 16, 0x40001000: 17, 0x40001400: 18,
    0x40002000: 19, 0x40014000: 20, 0x40014400: 21, 0x40014800: 22,
}  # fmt: skip
TIMER_CLOCK_MHZ = 64

# Button shift-register bit for each front-panel control, found by pressing each bit in the
# emulator and reading the display (docs/HARDWARE.md, "Buttons"). Bits 7, 8 and 15 do nothing on
# the 32-key model. Tempo is not on the shift register: it is wired to PA15, active high.
BUTTONS: dict[str, int] = {
    "mutate": 0, "arp": 1, "seq": 2, "shift": 3, "hold": 4, "oct+": 5, "oct-": 6,
    "encoder": 9, "back": 10, "play": 11, "stop": 12, "record": 13, "chord": 14,
}  # fmt: skip
TEMPO = 16  # a bit above the shift register, for the PA15 line
# LED on chain 1 for each button, from the same experiments.
BUTTON_LEDS = {
    "oct-": 0, "oct+": 1, "hold": 2, "shift": 3, "seq": 4, "arp": 5, "mutate": 6, "chord": 7,
    "tempo": 8, "record": 9, "stop": 10, "play": 11, "encoder": 12,
}  # fmt: skip


FRAME_REGS = (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_R12, UC_ARM_REG_LR, UC_ARM_REG_SP, UC_ARM_REG_XPSR,
)  # fmt: skip


def reg_name(addr: int) -> str:
    block = addr & ~0x3FF
    return f"{PERIPHERALS.get(block, f'{block:08X}')}+{addr & 0x3FF:03X}"


class Spi:
    """One SPI controller: every byte written to DR is answered by the attached device."""

    def __init__(self):
        self.rx: list[int] = []
        self.device = None  # callable(byte) -> byte
        self.source = None  # callable() -> byte, for a bus the firmware only receives on

    def write(self, value: int) -> None:
        answer = self.device(value & 0xFF) if self.device else 0xFF
        if len(self.rx) < 4:  # the receive FIFO holds four bytes; more are lost
            self.rx.append(answer)

    def status(self) -> int:
        # TXE, RXNE, and the FIFO level the HAL polls to drain what a transmit left behind
        return 0x02 | (1 if self.rx or self.source else 0) | min(len(self.rx), 3) << 9

    def read(self) -> int:
        if self.rx:
            return self.rx.pop(0)
        return self.source() if self.source else 0xFF


class I2c:
    """STM32 I2C (the CR2/ISR/TXDR/RXDR kind) as a master; logs each transfer."""

    def __init__(self, clock):
        self.left = 0
        self.reading = self.autoend = self.reload = self.stopf = self.done = False
        self.addr = 0
        self.preload: int | None = None
        self.data = bytearray()
        self.written = bytearray()  # bytes written since the last stop: the register address for a read
        self.log: list[tuple[int, bool, bytes, int]] = []
        self.device = None  # callable(addr, bytes written before the read, index) -> byte
        self.present = lambda _addr: True  # does a device answer at this address? If not: no acknowledge
        self.nack = False
        self.clock = clock

    def control(self, cr2: int) -> None:
        """A write to CR2: start, continue (reload) or stop a transfer."""
        if cr2 & 1 << 13:
            self.finish()
            self.addr, self.reading = cr2 >> 1 & 0x7F, bool(cr2 & 1 << 10)
            self.data = bytearray()
            self.nack = not self.present(self.addr)
        self.left = cr2 >> 16 & 0xFF
        self.autoend, self.reload = bool(cr2 & 1 << 25), bool(cr2 & 1 << 24)
        self.done = self.stopf = False
        if self.preload is not None and cr2 & 1 << 13 and not self.reading and self.left:
            self.data.append(self.preload)  # the HAL loads the first byte before it sends the start
            self.written.append(self.preload)
            self.left -= 1
        self.preload = None
        if cr2 & 1 << 14:
            self.stop()
        elif not self.left:
            self.end()

    def end(self) -> None:
        self.done = True
        if self.autoend and not self.reload:
            self.stop()

    def stop(self) -> None:
        self.done = self.stopf = True
        self.left = 0
        self.finish()
        self.written = bytearray()

    def finish(self) -> None:
        if self.data:
            if len(self.log) < 2000:
                self.log.append((self.addr, self.reading, bytes(self.data), self.clock()))
            self.data = bytearray()

    def status(self) -> int:
        v = 0x01  # TXE
        if self.nack:
            return v | 0x10  # NACKF: nothing answered the address
        if self.left:
            v |= 0x04 if self.reading else 0x02  # RXNE or TXIS
        elif self.stopf:
            v |= 0x20
        elif self.done:
            v |= 0x80 if self.reload else 0x40  # TCR or TC
        return v

    def tx(self, value: int) -> None:
        if not self.left:
            self.preload = value & 0xFF
            return
        self.data.append(value & 0xFF)
        self.written.append(value & 0xFF)
        self.left -= 1
        if self.left <= 0:
            self.left = 0
            self.end()

    def rx(self) -> int:
        value = self.device(self.addr, bytes(self.written), len(self.data)) if self.device else 0x00
        self.data.append(value & 0xFF)
        self.left -= 1
        if self.left <= 0:
            self.left = 0
            self.end()
        return value & 0xFF


class NorFlash:
    """The 512 KiB SPI flash on SPI2 (chip select PA8). Answers as a GD25Q40: id C8 40 13.

    A page program keeps it busy for 0.7 ms and an erase for 45 ms (4 KiB), 150 ms (32 KiB) or
    250 ms (64 KiB), about what the part's data sheet gives as typical, on `clock` (microseconds)."""

    BUSY_US = MappingProxyType({0x02: 700, 0x20: 45_000, 0x52: 150_000, 0xD8: 250_000})

    SIZE = 0x80000
    JEDEC = (0xC8, 0x40, 0x13)

    def __init__(self, image: bytes | None = None):
        self.mem = bytearray(image) if image else bytearray(b"\xff" * self.SIZE)
        self.cmd: list[int] = []
        self.addr = 0
        self.writable = False
        self.clock = lambda: 0.0  # the board's time, set by Board
        self.busy_until = 0.0
        self.dirty = False
        self.stats: Counter[str] = Counter()

    def deselect(self) -> None:
        if self.cmd and self.cmd[0] == 0x02:
            self.writable = False
            self.busy_until = self.clock() + self.BUSY_US[0x02]
        self.cmd = []

    def transfer(self, b: int) -> int:
        cmd = self.cmd
        cmd.append(b)
        op, n = cmd[0], len(cmd)
        if op == 0x9F:
            return self.JEDEC[n - 2] if 2 <= n <= 4 else 0xFF
        if op == 0x05:
            if n == 1:
                return 0xFF
            return (0x02 if self.writable else 0x00) | (self.clock() < self.busy_until)
        if op == 0x06:
            self.writable = True
        elif op == 0x04:
            self.writable = False
        elif op in (0x03, 0x0B, 0x02, 0x20, 0x52, 0xD8):
            if n == 4:
                self.addr = (cmd[1] << 16 | cmd[2] << 8 | cmd[3]) % self.SIZE
                if op in (0x20, 0x52, 0xD8) and self.writable:
                    size = {0x20: 0x1000, 0x52: 0x8000, 0xD8: 0x10000}[op]
                    start = self.addr & ~(size - 1)
                    self.mem[start : start + size] = b"\xff" * size
                    self.stats["erase"] += 1
                    self.writable, self.dirty = False, True
                    self.busy_until = self.clock() + self.BUSY_US[op]
            elif n > 4 + (op == 0x0B):
                if op == 0x02:
                    if self.writable:
                        self.mem[self.addr] &= b
                        self.stats["program"] += 1
                        self.dirty = True
                    b = 0xFF
                else:
                    self.stats["read"] += 1
                    b = self.mem[self.addr]
                self.addr = (self.addr + 1) % self.SIZE
                return b
        return 0xFF


class Ssd1306:
    """128 x 32 OLED. The firmware sends 512 data bytes per refresh and lets the controller wrap."""

    def __init__(self):
        self.ram = bytearray(128 * 4)
        self.at = 0
        self.frames = 0
        self.on = False
        self.contrast = 0x7F
        self.arg = 0

    def command(self, b: int) -> None:
        if self.arg == 0x81:
            self.contrast = b
        elif b in (0xAE, 0xAF):
            self.on = b == 0xAF
        self.arg = b if self.arg == 0 and b in (0x81, 0xD5, 0xA8, 0xD3, 0x8D, 0x20, 0xDA, 0xD9, 0xDB) else 0
        self.at = 0

    def data(self, b: int) -> None:
        self.ram[self.at] = b
        self.at = (self.at + 1) % len(self.ram)
        if self.at == 0:
            self.frames += 1

    def pixel(self, x: int, y: int) -> int:
        return self.ram[(y >> 3) * 128 + x] >> (y & 7) & 1

    def render(self) -> str:
        rows = []
        for y in range(0, 32, 2):
            rows.append("".join(" ▀▄█"[self.pixel(x, y) | self.pixel(x, y + 1) << 1] for x in range(128)).rstrip())
        return "\n".join(rows)


class Timer:
    """Counter and update interrupt of one STM32 timer; time is the board's microsecond clock."""

    def __init__(self, irq: int):
        self.irq = irq
        self.sr = 0
        self.running = self.irq_on = False
        self.tick_us = self.period_us = 1.0
        self.started = self.next = 0.0
        self.arr = 0xFFFF

    def configure(self, cr1: int, dier: int, psc: int, arr: int, now: float) -> None:
        was = self.running
        self.running, self.irq_on = bool(cr1 & 1), bool(dier & 1)
        self.arr = arr
        self.tick_us = (psc + 1) / TIMER_CLOCK_MHZ
        self.period_us = self.tick_us * (arr + 1)
        if self.running and not was:
            self.started, self.next = now, now + self.period_us

    def count(self, now: float) -> int:
        return int((now - self.started) / self.tick_us) % (self.arr + 1) if self.running else 0


class Encoder:
    """A quadrature encoder read through a select line; one detent is four phase changes."""

    STATES = (0b11, 0b01, 0b00, 0b10)

    def __init__(self):
        self.position = 0  # in quarter steps
        self.target = 0

    def turn(self, detents: int) -> None:
        self.target += 4 * detents

    def advance(self) -> None:
        if self.position != self.target:
            self.position += 1 if self.target > self.position else -1

    def phases(self) -> int:
        return self.STATES[self.position & 3]


class Board:
    """The KeyStep mk2: processor, peripherals, and the controls a person can touch."""

    def __init__(
        self,
        body: bytes,
        flash: bytes | None = None,
        stats: bool = False,
        quiet: bool = True,
        insns_per_us: int = INSNS_PER_US,
        keys37: bool = False,
    ):
        self.quiet, self.stats = quiet, stats
        self.keys37 = keys37  # the model strap on PB11 reads low: 37 keys and five encoders
        self.ipu = insns_per_us  # fewer instructions per microsecond runs faster and less faithfully
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.ctl_set_cpu_model(7)  # UC_CPU_ARM_CORTEX_M0
        uc.mem_map(FLASH_BASE, FLASH_SIZE)
        uc.mem_map(RAM_BASE, RAM_SIZE)
        # Address 0 aliases the start of flash (the bootloader, not in the package). The stock
        # firmware reads and writes through a null pointer at 0x08061EF0; on the chip that lands here.
        uc.mem_map(0, 0x8000)
        uc.mem_write(0, b"\xff" * 0x8000)
        uc.mem_map(0x1FFF0000, 0x10000)  # system memory, option bytes, identity
        uc.mem_map(0x40000000, 0x30000)
        uc.mem_map(0x50000000, 0x2000)
        uc.mem_map(0xE0000000, 0x100000)
        uc.mem_write(FLASH_BASE, b"\xff" * FLASH_SIZE)
        uc.mem_write(fwpack.LOAD_ADDRESS, body)
        uc.mem_write(RETURN_STUB, RETURN_CODE)
        uc.mem_write(0x1FFF75E0, struct.pack("<H", 512))  # flash size in KiB
        uc.mem_write(0x1FFF7590, b"KS2-EMULATOR")  # 96-bit unique id
        uc.mem_write(0x40022020, struct.pack("<I", 0xDFFFE1AA))  # FLASH_OPTR: single bank, RDP level 0

        # time and exceptions
        self.us = 0.0
        self.next_ms = 1000.0
        self.pending: set[int] = set()  # exception numbers
        self.in_handler = False
        self.nvic_enabled = 0
        self.systick_irq = False
        self.stopped = ""
        self.interrupted = False  # Ctrl-C or SIGTERM: end the run, but still report and save
        sp, self.pc = struct.unpack("<II", body[:8])
        uc.reg_write(UC_ARM_REG_SP, sp)
        self.poke(SCB_VTOR, fwpack.LOAD_ADDRESS)

        # peripherals
        self.timers = {base: Timer(irq) for base, irq in TIMER_IRQ.items()}
        self.spi = {SPI1: Spi(), SPI2: Spi(), SPI3: Spi()}
        self.i2c = {I2C1: I2c(lambda: int(self.us / 1000))}
        self.gpio_out = dict.fromkeys((GPIOA, GPIOB, GPIOC, GPIOD), 0)
        self.driven: dict[int, int] = {}  # mode register value -> mask of its output pins
        self.dma_isr = 0
        self.dma_repeat: dict[int, float] = {}  # circular peripheral-to-memory channels: next completion
        self.dma_due: dict[int, float] = {}  # transfers in flight: when each completes
        self.dma_seen: dict[int, str] = {}
        self.tim2_count = 0  # TIM2 while it counts TIM3's overflows instead of the clock
        self.adc = [0, 0]  # 12-bit levels at the pedal input (channel 0) and the aftertouch strip (1)
        self.sync_in: list[bool | None] = [None, None]  # clock, run: driven by a cable, or None
        self.usb = usbmodel.Usb(self)

        # the board
        self.oled = Ssd1306()
        self.flash = NorFlash(flash)
        self.flash.clock = lambda: self.us
        self.spi[SPI2].device = self.flash.transfer
        self.spi[SPI3].source = self.button_byte  # the stock firmware only receives
        self.spi[SPI3].device = lambda _byte: self.button_byte()  # the SDK clocks the bytes in by sending
        self.i2c[I2C1].device = self.touch_read
        self.touch_present = True  # False: the touch controller does not answer, as with a broken flex cable
        self.i2c[I2C1].present = lambda _addr: self.touch_present
        self.leds = [[(0, 0, 0)] * n for n in LED_CHAINS]
        self.led_updates = 0
        self.midi_out = bytearray()  # bytes sent on the DIN output
        self.midi_in: list[int] = []  # bytes waiting at the DIN input
        self.midi_rx_dma: tuple[int, int, int] | None = None  # channel, buffer, size while DMA receives
        self.midi_rx_next = 0.0
        self.buttons = 0  # the 16-bit shift register, bit set = pressed
        self.button_phase = 0
        self.key_contacts = [0] * 40  # per key: bit 0 first contact closed, bit 1 second
        self.key_events: list[tuple[float, int, int]] = []  # (time, key, contacts) still to happen
        self.encoders = [Encoder() for _ in range(5)]  # the 32-key model has only the first
        self.encoder = self.encoders[0]
        self.encoder_next = 0.0
        self.touch = [0, 0]  # the two strip channels as the controller reports them
        self.touch_pending = False
        self.exti_rising = 0
        self.exti_falling = 0  # EXTI falling-edge pending flags; the firmware polls bit 6 for the touch chip

        # statistics
        self.reads: Counter[int] = Counter()
        self.writes: Counter[int] = Counter()
        self.log: list[str] = []

        uc.hook_add(UC_HOOK_MEM_READ, self.on_read, begin=0x40000000, end=0x5FFFFFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=0x40000000, end=0x5FFFFFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_ppb_write, begin=0xE0000000, end=0xE00FFFFF)
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self.on_unmapped)
        uc.hook_add(UC_HOOK_INTR, self.on_intr)

    # ---- controls ----

    def press(self, control: str) -> None:
        kind, n = self.control(control)
        if kind == "b":
            self.buttons |= 1 << n
        else:  # a key: the second contact closes 6 ms after the first, a medium velocity
            self.key_events += [(self.us, n, 1), (self.us + 6000, n, 3)]

    def release(self, control: str) -> None:
        kind, n = self.control(control)
        if kind == "b":
            self.buttons &= ~(1 << n)
        else:
            self.key_events += [(self.us, n, 1), (self.us + 6000, n, 0)]

    def turn(self, detents: int, knob: int = 0) -> None:
        """Turn the encoder (or knob 1 to 4 of the 37-key model); positive is clockwise."""
        self.encoders[knob].turn(-detents)

    def set_touch(self, channel: int, position: float | None) -> None:
        """Touch a strip (0 = Mod, 1 = Bend) at 0.0 (bottom) to 1.0 (top), or None to let go.

        The controller reports a byte per strip: 0 for no finger, about 160 at the bottom falling
        to about 22 at the top (measured against the MIDI the firmware sends). A new report pulls
        PC6 low until it is read."""
        value = 0 if position is None else max(1, round(160 - 138 * min(1.0, max(0.0, position))))
        if self.touch[channel] == value:
            return
        self.touch[channel] = value
        self.touch_pending = True
        self.exti_falling |= 1 << 6

    @staticmethod
    def control(name: str) -> tuple[str, int]:
        if name == "tempo":
            return "b", TEMPO
        if name in BUTTONS:
            return "b", BUTTONS[name]
        if name[:1] in ("b", "k") and name[1:].isdigit():
            return name[0], int(name[1:])
        raise ValueError(f"no control called {name!r}")

    def button_byte(self) -> int:
        """The shift register is read as two bytes, low byte first, after a load pulse on PB4."""
        value = self.buttons >> (8 * self.button_phase) & 0xFF
        self.button_phase ^= 1
        return value

    def touch_read(self, _addr: int, written: bytes, index: int) -> int:
        if written:
            return 0  # start-up read of register 4
        report = struct.pack("<HH", *self.touch)
        if index == 3:
            self.touch_pending = False
        return report[index]

    def sync(self, line: int, active: bool | None) -> None:
        """Drive a sync input through a cable (line 0 = clock, PA2; 1 = run, PA3; active pulls the pin low), or
        None to unplug. A level change latches the edge and raises the EXTI interrupt if enabled."""
        pin = 2 + line
        before = self.gpio_in(GPIOA) >> pin & 1
        self.sync_in[line] = active
        after = self.gpio_in(GPIOA) >> pin & 1
        if before != after and self.peek(EXTI + (0x00 if after else 0x04)) >> pin & 1:  # edge selected
            if after:
                self.exti_rising |= 1 << pin
            else:
                self.exti_falling |= 1 << pin
            if self.peek(EXTI + 0x80) >> pin & 1:
                self.pending.add(16 + 6)

    def outputs(self) -> dict[str, int]:
        """What the firmware is putting on the CV, gate and sync pins, as raw register values."""
        b, c = self.gpio_out[GPIOB], self.gpio_out[GPIOC]
        dac = 0x40007400
        mod = [max(self.peek(dac + r) & 0xFFF, self.peek(dac + r + 4) >> 4 & 0xFFF) for r in (0x08, 0x14)]
        return {
            "pitch": (self.peek(0x40014038) & 0xFF) << 8 | self.peek(0x40014034) & 0xFF,
            "mod1": mod[0],
            "mod2": mod[1],
            "gate_pin": c >> 9 & 1,
            "gate_format": 2 if b & 2 else 1 if c & 0x100 else 0,
            "sync_clock": int(not c & 8),
            "sync_run": int(not c & 1),
        }

    # ---- memory helpers ----

    def poke(self, addr: int, value: int) -> None:
        self.uc.mem_write(addr, struct.pack("<I", value & 0xFFFFFFFF))

    def peek(self, addr: int) -> int:
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def note(self, text: str) -> None:
        line = f"[{self.us / 1000:8.1f} ms pc {self.uc.reg_read(UC_ARM_REG_PC):08x}] {text}"
        self.log.append(line)
        if not self.quiet:
            print(line)

    # ---- GPIO inputs ----

    def gpio_in(self, port: int) -> int:
        """Levels on a port's pins. Unconnected inputs read high (pull-ups)."""
        out, b_out = self.gpio_out[port], self.gpio_out[GPIOB]
        moder = self.peek(port)
        driven = self.driven.get(moder)
        if driven is None:
            driven = self.driven[moder] = sum(1 << i for i in range(16) if moder >> 2 * i & 3 == 1)
        level = 0xFFFF
        if port == GPIOD:  # keybed returns PD0-PD6 and PD8, low = contact closed
            phase = b_out >> 10 & 7
            if b_out & 0x4:
                phase += 2  # phases 8 and 9 of the 37-key model
            base = ((32, 24, 16, 8, 0) if self.keys37 else (24, 16, 8, 0, 32))[phase // 2]  # highest group first
            closed = 0
            contact = 1 << (phase & 1)
            for i in range(8):
                if self.key_contacts[base + i] & contact:
                    closed |= 0x80 >> i
            level &= ~(closed & 0x7F | (closed & 0x80) << 1)
        elif port == GPIOC:  # encoder phases on PC12/PC13
            if not self.keys37:
                level &= ~0x3000 | self.encoder.phases() << 12
            else:  # five encoders: a select line (PB8, PB9, PD9) low puts its encoders on the two pairs
                selects = (not b_out & 0x100, not b_out & 0x200, not self.gpio_out[GPIOD] & 0x200)
                for knob, (select, shift) in enumerate(((0, 12), (1, 12), (2, 12), (1, 14), (2, 14))):
                    if selects[select]:
                        level &= ~(3 << shift) | self.encoders[knob].phases() << shift
            if self.touch_pending:
                level &= ~0x40
        elif port == GPIOA:  # sync inputs follow the sync outputs when no cable is plugged in
            c_out = self.gpio_out[GPIOC]
            level = level & ~0xC | (c_out >> 3 & 1) << 2 | (c_out & 1) << 3
            for bit, active in zip((2, 3), self.sync_in, strict=True):  # a cable: active pulls the pin low
                if active is not None:
                    level = level & ~(1 << bit) | (0 if active else 1 << bit)
            if not self.buttons >> TEMPO & 1:
                level &= ~0x8000  # PA15: the Tempo button, pulled down, high when pressed
        elif port == GPIOB and self.keys37:
            level &= ~0x800  # the strap: PB11 tied low
        return (out & driven) | (level & ~driven)

    # ---- register hooks ----

    def on_read(self, uc, _access, addr, _size, _value, _data):
        if self.stats:
            self.reads[addr] += 1
        base = addr & ~3
        block, off = base & ~0x3FF, base & 0x3FF
        if block in self.gpio_out:
            if off == 0x10:
                self.poke(base, self.gpio_in(block))
            elif off in (0x18, 0x28):  # the set and reset registers are write-only and read as zero;
                self.poke(base, 0)  # the stock firmware does `BSRR |= bit`
        elif block in self.timers:
            if off == 0x10:
                self.poke(base, self.timers[block].sr)
            elif off == 0x24:
                self.poke(base, self.tim2_count if self.tim2_chained(block) else self.timers[block].count(self.us))
        elif block in self.spi:
            if off == 0x08:
                self.poke(base, self.spi[block].status())
            elif off == 0x0C:
                self.poke(base, self.spi[block].read())
        elif block in self.i2c:
            if off == 0x18:
                self.poke(base, self.i2c[block].status())
            elif off == 0x24:
                self.poke(base, self.i2c[block].rx())
        elif base == DMA1:
            self.poke(base, self.dma_isr)
        elif base == EXTI + 0x10:
            self.poke(base, self.exti_falling)
        elif base == EXTI + 0x0C:
            self.poke(base, self.exti_rising)
        elif base == RCC:  # RCC_CR: an oscillator is ready exactly while it is on
            v = self.peek(base) & ~0x02820400
            self.poke(base, v | (v & 0x100) << 2 | (v & 0x10000) << 1 | (v & 0x400000) << 1 | (v & 0x1000000) << 1)
        elif base == RCC + 0x08:  # RCC_CFGR: the selected clock is the active one
            v = self.peek(base)
            self.poke(base, v & ~0x38 | (v & 7) << 3)
        elif base in (RCC + 0x5C, RCC + 0x60):  # LSE and LSI ready
            v = self.peek(base)
            self.poke(base, v | (v & 1) << 1)
        elif base == 0x40007014:  # PWR_SR2: regulator settled
            self.poke(base, self.peek(base) & ~0x700)
        elif base == USART1 + 0x1C:  # ISR: transmitter always free, a byte waiting if MIDI came in
            self.poke(base, 0xC0 | (0x20 if self.midi_in and self.us >= self.midi_rx_next else 0))
        elif base == USART1 + 0x24:  # bytes arrive no faster than the DIN rate, one per 320 us
            ready = self.midi_in and self.us >= self.midi_rx_next
            if ready:
                self.midi_rx_next = self.us + 320
            self.poke(base, self.midi_in.pop(0) if ready else 0)
        elif base == ADC + 0x08:  # ADC_CR: calibration, stop and disable finish at once
            v = self.peek(base)
            if v & 0x2:
                v &= ~0x3
            self.poke(base, v & ~0x80000012)
        elif block == usbmodel.USB:
            self.poke(base, self.usb.read(off))
        elif base == 0x400100A0:  # SYSCFG_ITLINE8: which of the sources sharing interrupt 8 is asking
            self.poke(base, 4 if self.usb.interrupt() else 0)
        elif base == ADC:  # ADC_ISR: ready when enabled, a result whenever started
            cr = self.peek(ADC + 0x08)
            self.poke(base, 0x2000 | (0x1 if cr & 1 else 0) | (0xE if cr & 4 else 0))
        elif base == ADC + 0x40:  # the data register: the lowest selected channel; reading ends the conversion
            select = self.peek(ADC + 0x28) & 0x7FFFF
            self.poke(base, self.adc[1] if select & -select == 2 else self.adc[0])
            self.poke(ADC + 0x08, self.peek(ADC + 0x08) & ~4)

    def on_write(self, uc, _access, addr, _size, value, _data):
        if self.stats:
            self.writes[addr] += 1
        block, off = addr & ~0x3FF, addr & 0x3FF
        if addr == HANDLER_DONE:
            self.in_handler = False
        elif block in self.gpio_out:
            if off in (0x14, 0x18, 0x28):
                old = self.gpio_out[block]
                if off == 0x14:
                    new = value & 0xFFFF
                elif off == 0x18:
                    new = old & ~(value >> 16) | value & 0xFFFF  # set wins over reset
                else:
                    new = old & ~value
                self.gpio_out[block] = new
                if block == GPIOA and (old ^ new) & new & 0x100:  # PA8 rises: flash deselected
                    self.flash.deselect()
                elif block == GPIOB and (old ^ new) & new & 0x10:  # PB4 rises: buttons latched
                    self.button_phase = 0
        elif block in self.spi:
            if off == 0x0C:
                self.spi[block].write(value)
                if block == SPI1:  # the display: PB6 is its data/command line
                    (self.oled.data if self.gpio_out[GPIOB] & 0x40 else self.oled.command)(value & 0xFF)
        elif addr == EXTI + 0x10:
            self.exti_falling &= ~value
        elif addr == EXTI + 0x0C:
            self.exti_rising &= ~value
        elif block == usbmodel.USB:
            self.usb.write(off, value)
        elif block == DMA1:
            if off == 0x04:
                self.dma_isr &= ~value
            elif off >= 0x08 and (off - 0x08) % 0x14 == 0:
                ch = (off - 0x08) // 0x14 + 1
                if value & 1:
                    self.dma_start(ch, value)
                else:
                    self.dma_repeat.pop(ch, None)
                    self.dma_due.pop(ch, None)
                    if self.midi_rx_dma and self.midi_rx_dma[0] == ch:
                        self.midi_rx_dma = None
        elif block in self.timers:
            timer = self.timers[block]
            if off == 0x10:
                timer.sr &= value
            elif block == TIM2 and (off == 0x24 or off == 0x14 and value & 1):
                self.tim2_count = value if off == 0x24 else 0
            elif off in (0x00, 0x0C, 0x28, 0x2C):
                reg = {o: self.peek(block + o) for o in (0x00, 0x0C, 0x28, 0x2C)}
                reg[off] = value
                timer.configure(reg[0x00], reg[0x0C], reg[0x28], reg[0x2C], self.us)
        elif addr == USART1 + 0x28:  # a byte written straight to the transmitter
            self.midi_out.append(value & 0xFF)
        elif block in self.i2c:
            i2c = self.i2c[block]
            if off == 0x04:
                i2c.control(value)
            elif off == 0x28:
                i2c.tx(value)
            elif off == 0x1C and value & 0x20:
                i2c.stopf = i2c.done = False

    def on_ppb_write(self, uc, _access, addr, _size, value, _data):
        if addr == SYSTICK_CSR:
            self.systick_irq = value & 3 == 3
        elif addr == NVIC_ISER:
            self.nvic_enabled |= value
        elif addr == NVIC_ICER:
            self.nvic_enabled &= ~value
        elif addr == NVIC_ISPR:
            self.pending.update(16 + i for i in range(32) if value >> i & 1)
        elif addr == SCB_ICSR:
            if value & 1 << 28:
                self.pending.add(14)
            if value & 1 << 26:
                self.pending.add(15)
        elif addr == SCB_AIRCR and value & 0xFFFF0004 == 0x05FA0004:
            self.note(f"system reset requested (word at 0x20000000 = {self.peek(RAM_BASE):#010x})")
            self.stopped = "reset"
            uc.emu_stop()

    def on_unmapped(self, uc, _access, addr, _size, _value, _data):
        self.note(f"unmapped access at {addr:#010x}")
        return False

    def on_intr(self, uc, intno, _data):
        self.note(f"cpu exception {intno}")
        self.stopped = "fault"
        uc.emu_stop()

    # ---- DMA ----

    def dma_start(self, ch: int, ccr: int) -> None:
        base = DMA1 + 0x08 + 0x14 * (ch - 1)
        count, periph, memory = self.peek(base + 4), self.peek(base + 8), self.peek(base + 12)
        to_periph, circular = bool(ccr & 0x10), bool(ccr & 0x20)
        width = 1 << (ccr >> 10 & 3)
        self.dma_seen.setdefault(
            ch,
            f"{'memory -> ' if to_periph else ''}{reg_name(periph)}{'' if to_periph else ' -> memory'}, "
            f"{count} x {width} byte{', circular' if circular else ''}, interrupt {'on' if ccr & 2 else 'off'}",
        )
        if to_periph:
            data = bytes(self.uc.mem_read(memory, count * width))
            if periph == SPI1 + 0x0C:
                handler = self.oled.data if self.gpio_out[GPIOB] & 0x40 else self.oled.command
                for b in data[::width]:
                    handler(b)
            elif periph == SPI2 + 0x0C:
                for b in data[::width]:
                    self.flash.transfer(b)
            elif periph & ~0x3FF == 0x40014400:  # TIM16 compare: the LED bit stream
                self.led_frame(data)
                per_item = 1.25  # one 800 kHz period per compare value
            elif periph & ~0x3FF == 0x40013800:  # USART1 transmit: MIDI out
                self.midi_out += data[::width]
                per_item = 320.0  # ten bits at 31250 baud
            else:
                per_item = 1.0  # a byte at 8 MHz
            if not circular:
                self.poke(base + 4, 0)
            self.dma_due[ch] = self.us + count * per_item
        elif periph == ADC + 0x40:
            self.uc.mem_write(memory, b"".join(self.adc[i & 1].to_bytes(width, "little") for i in range(count)))
            if circular:
                self.dma_repeat[ch] = self.us + 1000
            else:
                self.poke(base + 4, 0)
            self.dma_due[ch] = self.us + 20.0 * count
        elif periph == USART1 + 0x24 and circular:  # MIDI in: bytes are put in the ring as they arrive
            self.midi_rx_dma = (ch, memory, count)
        # a read from any other peripheral never completes: nothing models the data source

    def dma_done(self, ch: int) -> None:
        self.dma_isr |= 0x3 << 4 * (ch - 1)  # GIF + TCIF
        if self.peek(DMA1 + 0x08 + 0x14 * (ch - 1)) & 2:
            self.pending.add(16 + (9 if ch == 1 else 10 if ch <= 3 else 11))

    def led_frame(self, data: bytes) -> None:
        """Compare values at 800 kHz: 38 is a 1 bit, 19 a 0 bit, after 176 slots of reset gap."""
        slots = struct.unpack(f"<{len(data) // 2}H", data)[176:]
        chain = 0 if not self.gpio_out[GPIOB] & 1 else 1 if not self.gpio_out[GPIOA] & 0x80 else None
        if chain is None:
            return
        leds = self.leds[chain]
        for i in range(min(len(leds), len(slots) // 24)):
            word = 0
            for s in slots[24 * i : 24 * i + 24]:
                word = word << 1 | (s > 28)
            leds[i] = (word >> 8 & 0xFF, word >> 16 & 0xFF, word & 0xFF)  # sent green, red, blue
        self.led_updates += 1

    # ---- time and exceptions ----

    def events(self) -> float:
        """Fire everything that is due and return the time of the next event."""
        now = self.us
        nxt = now + SLICE_INSNS / self.ipu
        if now >= self.next_ms:
            self.next_ms += 1000
            if self.systick_irq:
                self.pending.add(15)
            for ch, due in list(self.dma_repeat.items()):
                if now >= due:
                    self.dma_repeat[ch] = now + 1000
                    self.dma_done(ch)
        nxt = min(nxt, self.next_ms)
        for ch, due in list(self.dma_due.items()):
            if now >= due:
                del self.dma_due[ch]
                self.dma_done(ch)
            else:
                nxt = min(nxt, due)
        for base, timer in self.timers.items():
            if timer.running:
                if base == TIM2 and self.tim2_chained(base):
                    continue
                if now >= timer.next:
                    timer.next = max(timer.next + timer.period_us, now)
                    timer.sr |= 1
                    if timer.irq_on:
                        self.pending.add(16 + timer.irq)
                    if base == TIM3 and self.tim2_chained(TIM2):
                        self.tim2_step()
                if timer.irq_on:
                    nxt = min(nxt, timer.next)
        if self.usb.interrupt():
            self.pending.add(16 + usbmodel.USB_IRQ)
        if self.midi_rx_dma and self.midi_in:
            if now >= self.midi_rx_next:  # one byte every 320 us, the DIN rate
                ch, memory, size = self.midi_rx_dma
                left_at = DMA1 + 0x0C + 0x14 * (ch - 1)
                left = self.peek(left_at) or size
                self.uc.mem_write(memory + size - left, bytes([self.midi_in.pop(0)]))
                self.poke(left_at, left - 1 or size)
                self.midi_rx_next = now + 320
            nxt = min(nxt, self.midi_rx_next)
        if self.key_events:
            due = [e for e in self.key_events if e[0] <= now]
            if due:
                self.key_events = [e for e in self.key_events if e[0] > now]
                for _, key, contacts in due:
                    self.key_contacts[key] = contacts
        if now >= self.encoder_next:
            self.encoder_next = (
                now + 12000
            )  # a quarter step every 12 ms while a knob turns: slow enough for the debounce
            for encoder in self.encoders:
                encoder.advance()
        return nxt

    def tim2_chained(self, block: int) -> bool:
        """TIM2 in external clock mode 1: the stock firmware clocks it from TIM3's overflow."""
        return block == TIM2 and self.peek(TIM2 + 0x08) & 7 == 7

    def tim2_step(self) -> None:
        """One count of the chained TIM2, with its four compare channels."""
        timer = self.timers[TIM2]
        if not timer.running:
            return
        self.tim2_count = 0 if self.tim2_count >= self.peek(TIM2 + 0x2C) else self.tim2_count + 1
        dier = self.peek(TIM2 + 0x0C)
        for n in range(1, 5):
            if self.tim2_count == self.peek(TIM2 + 0x30 + 4 * n):
                timer.sr |= 1 << n
                if dier >> n & 1:
                    self.pending.add(16 + timer.irq)

    def enter_exception(self, number: int) -> None:
        """Start a handler. The interrupted registers go on the stack, and the handler returns
        into RETURN_STUB, which pops them and resumes; so the processor never has to stop for an
        exception return. Handlers therefore run in thread mode, and nothing here can nest."""
        uc = self.uc
        r0, r1, r2, r3, r12, lr, sp, xpsr = uc.reg_read_batch(FRAME_REGS)
        self.in_handler = True
        sp -= 32
        uc.mem_write(sp, struct.pack("<8I", r12, lr, xpsr, r0, r1, r2, r3, self.pc | 1))
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RETURN_STUB | 1)
        self.pc = self.peek(self.peek(SCB_VTOR) + 4 * number)

    def run(self, ms: float) -> str:
        """Run for a stretch of simulated time. Returns "" or why the processor stopped for good."""
        uc = self.uc
        end = self.us + ms * 1000
        while self.us < end and not self.stopped and not self.interrupted:
            nxt = self.events()
            if self.pending and not self.in_handler and not uc.reg_read(UC_ARM_REG_PRIMASK) & 1:
                number = min(self.pending)
                self.pending.discard(number)
                if number < 16 or self.nvic_enabled >> (number - 16) & 1:
                    self.enter_exception(number)
            count = max(1, min(SLICE_INSNS, int((nxt - self.us) * self.ipu)))
            if self.pending and not self.in_handler:
                # An interrupt is waiting behind PRIMASK. On the chip it is taken the instant the
                # mask goes; a whole slice here would let code that masks again straight away (a
                # loop on a guarded counter) hold it off for good. So look again very soon.
                count = min(count, 12)
            try:
                uc.emu_start(self.pc | 1, 0xFFFFFFF0, count=count)
            except UcError as e:
                self.note(f"stopped: {e}")
                self.stopped = "fault"
                break
            except KeyboardInterrupt:  # before stop_on_signals, or raised out of a hook
                self.interrupted = True
                break
            self.pc = uc.reg_read(UC_ARM_REG_PC)
            self.us += count / self.ipu
        return self.stopped

    # ---- reports ----

    def report(self, hot: int = 0) -> None:
        print(f"\nran {self.us / 1000:.0f} ms, pc {self.pc:#010x}")
        if self.stats:
            blocks: dict[str, list[int]] = {}
            for addr in sorted(set(self.reads) | set(self.writes)):
                blocks.setdefault(reg_name(addr).split("+")[0], []).append(addr)
            print(f"{'peripheral':<10} {'registers':>9} {'reads':>9} {'writes':>9}")
            for name, addrs in blocks.items():
                r, w = sum(self.reads[a] for a in addrs), sum(self.writes[a] for a in addrs)
                print(f"{name:<10} {len(addrs):>9} {r:>9} {w:>9}")
            for addr, n in self.reads.most_common(hot):
                print(f"  most read: {reg_name(addr):<14} {n:>9}")
        irqs = [i for i in range(32) if self.nvic_enabled >> i & 1]
        print(f"enabled interrupts: {irqs}")
        for base, timer in self.timers.items():
            if timer.running:
                print(
                    f"  {PERIPHERALS[base]:<6} period {timer.period_us:.1f} us, interrupt {'on' if timer.irq_on else 'off'}"
                )
        for ch, text in sorted(self.dma_seen.items()):
            print(f"  DMA1 channel {ch}: {text}")
        for addr, reading, data, ms in self.i2c[I2C1].log[:12]:
            print(f"  {ms:>6} ms I2C1 {'read ' if reading else 'write'} {addr:#04x}: {data.hex(' ')}")
        if self.flash.stats:
            print(f"external flash: {dict(self.flash.stats)}")
        if self.midi_out:
            print(f"MIDI out: {self.midi_out[:48].hex(' ')}")
        self.show_leds()
        self.show_display()

    def show_display(self) -> None:
        print(f"display ({self.oled.frames} frames, {'on' if self.oled.on else 'off'}):")
        print(self.oled.render())

    def show_leds(self) -> None:
        for chain, leds in enumerate(self.leds):
            lit = {i: f"{r:02x}{g:02x}{b:02x}" for i, (r, g, b) in enumerate(leds) if r | g | b}
            print(f"LED chain {chain} ({self.led_updates} updates): {lit or 'all off'}")


def load(path: Path, raw: bool = False) -> bytes:
    """The body of an image file or package, checked the way the flasher will check it."""
    if raw:
        return path.read_bytes()
    image, _ = fwpack.read_image(path)
    problems = fwpack.check(image)
    if problems:
        sys.exit("error: image fails the checks:\n  - " + "\n  - ".join(problems))
    return image[fwpack.HEADER_SIZE :]


def parse_script(text: str) -> list[tuple[float, str]]:
    """ "1500 press=b3; 1700 release=b3; 2000 show" -> [(1500, "press=b3"), ...]"""
    steps = []
    for part in filter(None, (p.strip() for p in text.split(";"))):
        when, action = part.split(None, 1)
        steps.append((float(when), action.strip()))
    return sorted(steps)


def act(board: Board, action: str) -> None:
    name, _, arg = action.partition("=")
    if name == "press":
        board.press(arg)
    elif name == "release":
        board.release(arg)
    elif name == "turn":
        board.turn(int(arg))
    elif name[:4] == "knob" and name[4:] in ("1", "2", "3", "4"):  # the 37-key model's knobs
        board.turn(int(arg), int(name[4:]))
    elif name in ("pedal", "aftertouch"):
        board.adc[name == "aftertouch"] = max(0, min(4095, int(arg)))
    elif name in ("clock", "run"):  # the sync input: clock=1, clock=0, or clock=none to unplug
        board.sync(name == "run", None if arg == "none" else arg == "1")
    elif name == "cv":
        print(f"--- {board.us / 1000:.0f} ms  " + "  ".join(f"{k} {v}" for k, v in board.outputs().items()))
    elif name == "show":
        print(f"--- {board.us / 1000:.0f} ms")
        board.show_display()
    elif name == "leds":
        print(f"--- {board.us / 1000:.0f} ms")
        board.show_leds()
    else:
        raise ValueError(f"unknown script action {action!r}")


def run_script(board: Board, steps: list[tuple[float, str]], until_ms: float) -> None:
    """Run to until_ms, doing each scripted action at its time. tap=X is press, then release 80 ms later."""
    expanded = []
    for when, action in steps:
        if action.startswith("tap="):
            expanded += [(when, "press=" + action[4:]), (when + 80, "release=" + action[4:])]
        else:
            expanded.append((when, action))
    for when, action in sorted(expanded):
        if when > until_ms or board.run(when - board.us / 1000) or board.interrupted:
            break
        act(board, action)
    board.run(until_ms - board.us / 1000)


def stop_on_signals(board: Board) -> None:
    """Make Ctrl-C and SIGTERM end the run the normal way. The handler only sets a flag, which
    Board.run checks between slices, because an exception raised inside a Unicorn hook may be
    swallowed or leave the emulator half-stopped."""

    def stop(signum, frame) -> None:
        board.interrupted = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=Path, help="an image, a .ks2fw package, or a raw body with --raw")
    parser.add_argument("--raw", action="store_true", help="the file is a body without the 64-byte header")
    parser.add_argument("--ms", type=float, default=1000, help="simulated time to run (default 1000)")
    parser.add_argument("--script", default="", help='timed actions, e.g. "1500 tap=b3; 2000 show"')
    parser.add_argument("--flash", type=Path, help="external flash contents: loaded if present, saved on exit")
    parser.add_argument("--stats", action="store_true", help="count register accesses per peripheral")
    parser.add_argument("--hot", type=int, default=8, help="with --stats, list the N most-read registers")
    parser.add_argument("--quiet", action="store_true", help="do not print notes as they happen")
    parser.add_argument("--keys37", action="store_true", help="strap the board as the 37-key model")
    args = parser.parse_args()

    flash = args.flash.read_bytes() if args.flash and args.flash.exists() else None
    board = Board(load(args.image, args.raw), flash, stats=args.stats, quiet=args.quiet, keys37=args.keys37)
    stop_on_signals(board)
    run_script(board, parse_script(args.script), args.ms)
    board.report(args.hot)
    if args.flash and board.flash.dirty:
        args.flash.write_bytes(board.flash.mem)
    print(f"result: {board.stopped or ('interrupted' if board.interrupted else 'ran to the end')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
