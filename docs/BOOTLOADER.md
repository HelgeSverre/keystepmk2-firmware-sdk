# KeyStep mk2 bootloader and update mode

The bootloader occupies flash below `0x08008000` (32 KiB). It is not in the firmware package and
has not been read out, so everything here comes from the application image (1.0.2), Arturia's
support pages, and the image header. What the bootloader itself checks is still unknown.

## Entering update mode

| Way in | Source | Status |
|--------|--------|--------|
| Hold **Tempo** while switching the unit on | Arturia support article "MIDI Control Center (MCC) - Firmware Update", row "KeyStep mk2 / KeyStep 37 mk2". The article adds: "once in bootloader mode, all LEDs and the screen should be OFF" | Documented by Arturia; not tried here |
| USB class request `0xF2` to the running application | Application code, below | Read from code; not tried here |

The Tempo entry is Arturia's recovery path after a failed update, so it must work without a
running application. That is the property that makes flashing recoverable. It should be confirmed
on the device (hold Tempo, check which USB device appears) before anything is flashed.

## What the application does: request `0xF2`

The application's USB device has three interfaces (configuration descriptor at `0x080705CC`):

| Interface | Class | Purpose |
|-----------|-------|---------|
| 0 | `FE`/`01`/`02`, string "Firmware Updater" | DFU-shaped stub |
| 1 | Audio control | |
| 2 | MIDI streaming, endpoints `02` OUT and `81` IN, 64 bytes | Notes and SysEx |

Interface 0 carries a DFU functional descriptor (`09 21 0B FF 00 40 00 10 01`: download, upload,
will-detach, transfer size 64, DFU 1.1), but the application implements none of the DFU requests.
Its class setup handler (`0x0800E674`) does this:

| Request | Action |
|---------|--------|
| Class request `0xF2` | Sets the byte at `0x20018492` and acknowledges |
| Class requests `0xF0`, `0xF1`, `0xF3`-`0xF5` | Acknowledged, no action |
| Any other class request (including DFU's `0`-`6`) | Stalled |
| Standard `GET_DESCRIPTOR` type `0x21` | Returns the 9-byte functional descriptor |

The main loop polls that byte (`0x0800DF7C` → `0x0800E790`). When it is set, `0x0800F8F4` runs:

1. Wait 500 ms.
2. Write `0xDEADBEEF` to `0x20000000`.
3. Copy 17 bytes from `0x20000268` to `0x20000004` (the buffer is filled by `0x0800F84C` from a
   USB string; inferred to be the serial-number string, so the device keeps its identity across
   the switch).
4. Write `0x05FA0004` to `SCB->AIRCR`: system reset.

The application's start-up code (`0x08021EC4`) initialises RAM from `0x20000020` upward only, so
`0x20000000`-`0x2000001F` is a 32-byte block shared with the bootloader. The bootloader is expected
to find the marker there after the reset and stay in update mode.

## What the host sends

From MIDI Control Center's code ([UPDATER.md](UPDATER.md)); not yet observed on the wire.

| Step | Request | Notes |
|------|---------|-------|
| Enter update mode | `0x21 / 0xF2`, `wIndex` = DFU interface | Skipped if the device already shows a single DFU interface |
| Select image | `0x21 / 0xF4`, `wValue` = `image_num` (0) | Other values are untested and may address other targets |
| Download | DFU `DNLOAD` blocks from 0, each followed by `GETSTATUS` | Block size from the device's functional descriptor; the whole image file including its 64-byte header |
| Finish | Zero-length `DNLOAD`, `GETSTATUS` | |
| Leave | `0x21 / 0xF3` | The device restarts |

MCC never parses the image. Magic, ids, addresses, XOR sums and version are checked by the
bootloader or not at all.

## USB identities

One image serves both models; `0x0800F87C` picks the identity at start-up.

| Model | Product string | Product id | Declared current |
|-------|----------------|------------|------------------|
| KeyStep mk2 | "KeyStep mk2" | `0x0228` | 100 mA |
| KeyStep 37 mk2 | "KeyStep 37 mk2" | `0x0229` | 500 mA |

The device descriptor stored in flash has `0x021B`; it is patched at start-up. The image header
carries vendor `1C75` and product `0228`.

## Option bytes

`0x08019764` runs at start-up: if the DUAL_BANK option bit is set, it clears it, programs the
option bytes and reloads them. The application changes nothing else there; in particular it does
not set read protection. Whether the bootloader or the factory set it is unknown.

## Unknown until the bootloader is read

- What the bootloader verifies before starting an image: the header's magic, vendor and product
  ids, the two XOR sums, the addresses.
- Whether an image that fails those checks leaves the unit in update mode.
- The update-mode USB identity and protocol (see [UPDATER.md](UPDATER.md) for the host side).
- Whether the update-mode DFU upload request returns flash contents, and which range.
- The read-protection level.

## Checked in the emulator

Run on 2026-11-23 with stock 1.0.2 in `tools/emu.py` and the USB host in `tools/usbmodel.py`.
This confirms the reading of the application code above; it says nothing about the bootloader.

| Request to the running application | Result |
|---|---|
| Class request `0xF2` | Acknowledged. 540 ms later the firmware writes `0xDEADBEEF` to `0x20000000`, copies 17 bytes to `0x20000004` and resets |
| The 17 bytes | The USB serial number string with its terminator (`0123456789ABCDEF` in the emulator). MIDI Control Center expects the same serial in update mode, so the string is presumably how the bootloader keeps it |
| Class requests `0xF0`, `0xF4` | Acknowledged, no effect |
| DFU `GETSTATUS` (`0xA1`/`3`) | Stalled |
| `GET_DESCRIPTOR` type `0x21` | `09 21 0B FF 00 40 00 10 01` |

The SDK's USB driver (`sdk/src/usb.c`) behaves the same way for all five rows.

## The flasher

`tools/ksflash.py` implements the host side from [UPDATER.md](UPDATER.md). It is tested only in
the emulator, against `FakeBootloader`: a model built from what MIDI Control Center expects, not
from the real bootloader. `just flash-selftest` runs the update in both directions and every
refusal. `just probe` reads a connected unit's descriptors and sends nothing else; it is the
tool for the first look at update mode.