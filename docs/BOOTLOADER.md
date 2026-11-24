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

## Seen on a device, 2026-11-24

`just probe` (descriptor reads only) on one unit, serial `4651400812010772`, before and after
holding Tempo while switching on.

| | Application | Update mode (Tempo held) |
|---|---|---|
| VID:PID | `1C75:0228` | `1C75:0228` |
| Product string | `KeyStep mk2` | `KeyStep mk2 Updater` |
| Interfaces | 3: `FE/01/02`, `01/01/00`, `01/03/00` | 1: `FE/01/02` |
| Functional descriptor | `09 21 0B FF 00 40 00 10 01` | `09 21 0B FF 00 40 00 1A 01` |
| Serial string | `4651400812010772` | nine characters of noise |

- Holding Tempo while switching on enters update mode with the application untouched: the
  recovery entry works (the first gating question in ROADMAP.md).
- Update mode declares download, upload and will-detach, 64-byte transfers, and DFU version
  `0x011A` where the application's stub says `0x0110`.
- The serial string is noise after a Tempo entry. That fits the reading above: the application
  hands its serial string to the bootloader in RAM when it is asked for update mode, and nothing
  does after a cold start. MIDI Control Center matches the update-mode device by serial, so it
  may not recognise a unit entered this way; not tried.

### Upload: the bootloader reads flash back

Requests sent in update mode on the same unit: `DFU_GETSTATE`, `DFU_GETSTATUS`, `DFU_UPLOAD` and
`DFU_ABORT`. Nothing else.

| Request | Reply |
|---|---|
| Get state, get status | `02`, `00 00 00 00 02 00`: dfuIDLE, no error |
| Upload, block 0 | 3 bytes: `00 21 41` |
| Upload, block 1 | Stalled. The state becomes `0x0F`, which is not a DFU state; uploads are then refused and `DFU_ABORT` does not clear it. Switching off and on with Tempo held does |
| Upload, block 2 and up, from idle | 64 bytes of flash per block, from `0x08008000` |
| Abort, after uploads | Back to dfuIDLE (from dfuUPLOAD-IDLE, `09`) |

- `00 21 41` is the command list an ST DfuSe bootloader returns for block 0: get commands, set
  address pointer (`0x21`), erase (`0x41`). Block 1 is invalid there too, and reads start at block
  2 from the address pointer, as here. So the bootloader is DfuSe-shaped, with `0x08008000` as the
  pointer after start-up. Set address pointer and erase were not sent.
- 7680 blocks from block 2 (`0x08008000` to the end of flash at `0x08080000`) were read. The first
  429,384 bytes are stock 1.0.2 without its 64-byte header, byte for byte: the load address is
  `0x08008000`, the header is not stored there, and this unit runs 1.0.2.
- After the image: `FF` to the end of its 2 KiB page (`0x08071000`); `00` from `0x08071000` to
  `0x0807F800` (whether that is what the flash holds or what the bootloader returns for it is not
  known); `FF` in the last page up to `0x0807FFF8`; and there `DE C0 0D 60 0A 00 00 00`:
  `0x600DC0DE` and the number 10. Read as a mark the bootloader leaves for an image it accepted
  (I); what the 10 counts is not known.
- The image's header was not found anywhere in the range read.

### An update through MIDI Control Center, started from update mode

Same unit, same day. Entered with Tempo held, MIDI Control Center 1.x as installed (its offer to
update itself to 1.23.1.152 declined), package `keystep_mk2__1_0_2.ks2fw` chosen by hand.

- On start it showed `Error: Failed to open the device.`, then listed the unit as **KeyStep mk2**
  with `Firmware Revision: 0.2.0` and `Update Available (1.0.4)`, and offered to install 1.0.4
  (declined). The noise serial did not stop it. `0.2.0` is the bootloader's USB device version,
  `bcdDevice` `0x0200`, shown as a firmware revision.
- The update ran and ended with `The burn process completed successfully.` The unit restarted
  into the application by itself, with its own serial again.
- Read back afterwards (Tempo held again): all 491,520 bytes from `0x08008000` are identical to
  the read before the update, the image identical to stock 1.0.2, and the last eight bytes still
  `DE C0 0D 60 0A 00 00 00`. So the 10 is not a count of updates.

This is the recovery path, proven once: no working application is needed to enter update mode,
and Arturia's tool flashes a unit found that way.

`tools/ksflash.py verify IMAGE` and `read OUT` do the read-back (uploads from block 2, status and
abort only); `verify` confirmed the unit against the 1.0.2 package.

### The same update with `tools/ksflash.py`

Same unit, same day, from an update mode entered with Tempo: `ksflash.py flash
firmware/keystep_mk2__1_0_2.ks2fw --real`.

- `0xF4` (select image), 6711 download blocks of 64 bytes with a status poll after each, a
  zero-length block and `0xF3`: accepted, and the unit restarted into the application with its
  own serial. The sequence read from MIDI Control Center's code is what the bootloader takes.
- `ksflash.py verify`: the 429,384 bytes from `0x08008000` identical to the package; the last
  eight bytes of flash unchanged.
- Request `0xF2` to the running application: the unit came back as `KeyStep mk2 Updater` with
  the application's serial, `4651400812010772`. So the serial is handed over through RAM, as read
  from the code, and is noise only after a cold start.
- `0xF3` with no download before it does not start the application: the state becomes 8
  (dfuMANIFEST-WAIT-RESET) and stays. A USB bus reset then gives dfuIDLE, still in update mode.
  Switching off and on starts the application.
- While the device changes identity, its string descriptors cannot be read for a moment
  (pyusb: "The device has no langid").

### The one-byte test: no signature

Same unit, 2026-11-25. Stock 1.0.2 with one byte changed (`Bank: ` to `Bonk: ` at `0x0806D251`)
and the header rebuilt by `tools/fwpack.py image`: three bytes differ from the stock image, the
letter and the two sums (header offsets `0x18` and `0x3F`).

`ksflash.py flash` from the running application (`0xF2`, then the update): accepted, the unit
restarted into the application, and its home screen reads `Bonk: 1 | Preset: 1`.

So the bootloader takes an image that Arturia did not make. Whatever it checks, it is nothing
beyond what `tools/fwpack.py` rebuilds: there is no signature.

Stock 1.0.2 was flashed back straight afterwards with `ksflash.py flash`, which now reads the
flash back before it sends `0xF3`: after the last block the bootloader is in dfuIDLE, the upload
returned the image byte for byte, and `0xF3` after those uploads still started the application.

### SDK firmware on the unit

2026-11-25: `sdk/examples/flashdump` was flashed, read back and started. First SDK code on a
unit.

- It enumerates as `1C75:0228 KeyStep mk2` with the SDK's placeholder serial `0123456789ABCDEF`,
  the same three interfaces as the stock application, and macOS lists a MIDI port `KeyStep mk2`.
- Play sent the external flash over USB MIDI; `tools/flashdump.py receive` reassembled 524,288
  bytes with every block's CRC-16 and the whole chip's CRC-32 right. Chip id `C8 40 13`.
- The copy: all 64 pattern sectors in use, the settings sector, five of the 32 file-system
  sectors, and **nothing in `0x41000`-`0x5FFFF`**: the area the SDK keeps to is untouched by the
  stock firmware on this unit, as read from its code.

