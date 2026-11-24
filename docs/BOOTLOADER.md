# KeyStep mk2 bootloader and update mode

The bootloader occupies flash below `0x08008000` (32 KiB). It is not in the firmware package.
The first sections below were written before it had been seen, from the application image
(1.0.2), Arturia's support pages and the image header. On 2026-11-25 it was read out of a unit
and disassembled: **"The bootloader, read"** at the end has what it really does, and where that
differs from an earlier section, the later one is right.

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

## Unknown until the bootloader is read (answered since: see "The bootloader, read")

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
  2, as here. It looked DfuSe-shaped from outside; the code shows the three bytes are a leftover
  of ST's library and that there are no commands and no pointer ("The bootloader, read", 4). Set
  address pointer and erase were not sent.
- 7680 blocks from block 2 (`0x08008000` to the end of flash at `0x08080000`) were read. The first
  429,384 bytes are stock 1.0.2 without its 64-byte header, byte for byte: the load address is
  `0x08008000`, the header is not stored there, and this unit runs 1.0.2.
- After the image: `FF` to the end of its 2 KiB page (`0x08071000`); `00` from `0x08071000` to
  `0x0807F800` (whether that is what the flash holds or what the bootloader returns for it is not
  known); `FF` in the last page up to `0x0807FFF8`; and there `DE C0 0D 60 0A 00 00 00`:
  `0x600DC0DE` and the number 10. It is the mark the bootloader leaves for an image it accepted;
  the 10 is a newline that happens to follow the constant in the bootloader ("The bootloader,
  read", 5).
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


## The bootloader, read

Read on 2026-11-25 from `firmware/device/bootloader-0x08000000.bin` (32 KiB, read from the unit;
code ends at `0x08005FB0`, initialised data follows to `0x080061C0`). Its own strings call it
`KS2 Bootloader` `0.0.1`, built `Jun 25 2025 12:05:45`. It is ST's USB device library with the
ST DFU class, modified, under a small C++ layer of Arturia's. This section replaces the guesses
above where they differ. **V** = read in the code at the address given; **I** = inferred;
nothing in this section was tried on a unit unless it says so.

RAM it uses, for reading the addresses below:

| Address | Meaning |
|---|---|
| `0x20000098` | Write pointer; `0xFFFFFFFF` when no download is open |
| `0x20000294` | Header-accepted flag |
| `0x20000298` | Copy of the last 64-byte header received |
| `0x200002D8` | Product id a header must carry: `0x0228`, set once at `0x080010A2` |
| `0x200002DD` | Running XOR of the body bytes received |
| `0x20000468` | Download result: 2 after reset, 1 erased, 0 failed, 3 complete and correct |
| `0x20000478` | Restart request, polled by a scheduler task (`0x080002BC`) |
| `0x20000479` | Arturia's own error code; first error sticks; never reaches the host |
| DFU handle (heap) | `+0x40` block number, `+0x44` length, `+0x48` upload base, `+0x50` the six status bytes, `+0x58` state |

### 1. Boot decision

`main` (`0x08002A44`) sets up clocks, option bytes (see 7), pins, then `0x080005C0` decides, in
this order:

| # | Test | Result | Where | |
|---|---|---|---|---|
| 1 | Word at `0x20000000` is `0xDEADBEEF` | Clear it, stay in update mode | `0x080005C6`–`D2` | V |
| 2 | Word at `0x0807FFF8` is not `0x600DC0DE` | Stay in update mode | `0x08000612` → `0x08000320` | V |
| 3 | PA15 (Tempo) reads high | Stay in update mode | `0x0800061C` → `0x0800258E` | V |
| 4 | Otherwise | Pulse every peripheral reset (`0x08003E28`), load SP from `0x08008000`, call `[0x08008004]` | `0x080025A4` | V |

- The application's vector table is **not** examined: no stack-pointer or reset-vector test
  (`0x080025A4`–`B6`). The mark is the only evidence of a good image.
- Only the first word of the mark is compared (`0x08000334`: add `0x9FF23F22`, test for zero).
- PA15 is made an input with pull-down just before (`0x08002B34`–`48`), so "high" is "pressed".
- The 17 serial bytes at `0x20000004` are copied to the USB serial string in **every** case, with
  byte 16 forced to zero (`0x080005FA`–`0x0800060A`), not only when the marker is present. That is
  why the serial is noise after a cold start.
- PB11 picks the identity (`0x080005D4`): high gives `0x0228` "KeyStep mk2 Updater", low gives
  `0x0229` "KeyStep 37 mk2 Updater". It changes the USB descriptor only (`0x08002390`), not the
  product id demanded of an image.

### 2. Image acceptance

Three checks, at three moments. No hash, no signature, nothing about the vector table.

**Header, when block 0 arrives** (`0x08000A58`). All must hold, or the block is refused:

| Check | Where |
|---|---|
| The block is exactly 64 bytes | `0x08000A6A` |
| 16-bit word at offset 4 equals `0x0228` | `0x08000A78`–`7E` |
| Word at offset 0 equals `0x1C750010` (magic `0010`, vendor `1C75`) | `0x08000A80`–`86` |
| `end` − `load` equals `size` (offsets `0x10`, `0x0C`, `0x14`) | `0x08000A88`–`92` |
| XOR of bytes 0–62 equals byte 63 | `0x08000A94`–`AC` |
| 16-bit word at offset 8 is zero | `0x08000AAE` |
| `load` > `0x08007FFF` and `end` ≤ `0x08080000` | `0x08000AB4`–`BE` |

**Each data block** (`0x08000BB0`): a header was accepted, and `load` ≤ write pointer ≤ `end`
(`0x08000BC4`–`D4`). The block is XORed into the running sum, programmed, read back and compared
(`0x08000AF8`).

**At `0xF3`** (`0x08000960`): write pointer equals `end`, and the running sum equals header byte
`0x18`. Nothing is checked at the zero-length block.

Differences from `tools/fwpack.py` `check()`:

| | Bootloader | `fwpack.py` |
|---|---|---|
| Product id | `0x0228` only, on both models; bytes 6–7 ignored | Also accepts `0x0229`, which the bootloader refuses |
| Load address | Anything from `0x08008000` up | Exactly `0x08008000`. Correct in effect: the jump target is fixed |
| End | ≤ `0x08080000` | Same limit, but see below: the real limit is `0x0807F800` |
| Bytes 10–11, `0x19`, `0x1A`–`0x3E` | Not looked at | Reserved ones must be zero (stricter, harmless) |
| Stack pointer and reset vector | Not looked at | Checked: this is the only guard there is |
| Body length against the file | Pointer must land exactly on `end` | `size` equals the body length (equivalent) |

An image that reaches into the last page (`0x0807F800` up) is accepted, but the mark is programmed
over its last eight bytes afterwards with no check of the result (`0x0800035C`–`68`); unless
those bytes are erased the mark is not what boot looks for and the unit stays in update mode
(I, the programming outcome was not tested). Keep images at or below `0x0807F800`.

**Status the host sees** (the six `GETSTATUS` bytes are `+0x50`–`+0x55`):

| Event | `bStatus` | `bState` | `iString` | Where |
|---|---|---|---|---|
| Header refused, or a data block refused or failed to program or verify | `0x0B` | `0x0A` dfuERROR | 3 | `0x0800120C`–`1A`, `0x08001244` |
| Erase failed after a good header | `0x0B` | `0x0A` (the internal state is left at 3) | 3 | `0x0800122A`–`36` |
| `DFU_CLRSTATUS` in any state but dfuERROR | `0x0E` | `0x0A` | 0 | `0x080014F6` |
| `0xF4` refused | `0x01`, and the request stalls; state unchanged | | | `0x0800159E` |
| Upload of block 1 | unchanged, stall | `0x0F` | | `0x080013F2` |

Arturia's own codes go to `0x20000479` (`0x0B` header, 4 erase, 8 block without header or
pointer out of range, 3 program or verify, 7 final check, 1 `0xF4`). The status call copies that
byte into `bStatus` and the next call zeroes it again (`0x0800249C`, `0x08002694`), so the host
never sees it.

### 3. Download handling

A `DFU_DNLOAD` with data is taken only in dfuIDLE or dfuDNLOAD-IDLE and at most 64 bytes are
kept (`0x08001304`–`1E`). Nothing happens to it until the host asks for status: `GETSTATUS` in
dfuDNLOAD-SYNC answers dfuDNBUSY with a poll time of 0, and the block is processed when that
answer has been sent (`0x08001454`, `0x080011C8`). **A block not followed by `GETSTATUS` is never
written.** All V.

| Block | Action | Where |
|---|---|---|
| `wValue` = 0 | The header. Checked as above; not stored in flash. If good: erase, then write pointer = `load` | `0x080011F8`, `0x08000A58`, `0x080009A4` |
| `wValue` ≠ 0 | Data, programmed at the write pointer, which then advances by the block length. **The block number is not used**: order of arrival is all that counts | `0x08001238`, `0x08000BB0` |
| Length 0 | State becomes dfuIDLE. Nothing else: no check, no write | `0x08001336`–`50` |

- **Erase** happens once, right after a good header, inside the same call: first the page holding
  the mark (`0x0807F800`, `0x080009C0`–`C8`), then every 2 KiB page from `load` to `end`
  (`0x0800286C`). So from the moment block 0 is accepted the unit has no mark and will not start
  an application until a download completes. A refused header erases nothing.
- A header with `end` below `load` passes the size test by wrap-around; the mark page is erased,
  the range erase then fails its bounds test (`0x080026A8`) and the download ends in error. The
  application stays, the mark is gone. Never send that.
- Programming is in 8-byte units (`0x08002736`–`48`). A block whose length is not a multiple of 8
  programs up to 7 stale bytes from the previous block after its own; fine for the last block,
  and the sum and the read-back cover only the real length. A short block in the middle leaves
  the pointer unaligned and the next block should fail (I).
- Data may run at most one block past `end` before the pointer test refuses it; `0xF3` then fails
  because the pointer is not on `end`.
- The whole erase runs inside the USB interrupt (`0x08002C8C` → `0x08004078` → `0x080011C8`), so
  the request after block 0's status poll gets no answer until about 211 pages are erased (I for
  the duration; `ksflash.py`'s 5 s per request has been enough).

**`0xF4`** (`0x0800158C` → `0x08000A10`): stores `wValue`, clears the header-accepted flag and the
running sum. Refused (stall, `bStatus` 1) if `wValue` is not 0, or if the last header received
had a non-zero word at offset 8. It selects nothing: there is one image. It is **not required**
for the first download after reset. It **is** what clears the running sum: block 0 does not
(`0x08000A58` clears `0x200002DC`, the header's sum, not `0x200002DD`). A second download in the
same session without `0xF4`, after one that ran to the end, fails the final check. Errors and
USB resets clear the sum too (`0x080003D4`). Always send it.

### 4. DfuSe commands: there are none

The bytes `00 21 41` for upload block 0 are ST's library code left in place (`0x080013AE`–`B8`).
Nothing interprets a download block as a command. The one leftover is at `0x0800147E`: block 0
with first byte `0x41` picks the "erase" poll-time callback instead of the "program" one, and
both return 0. The upload base `+0x48` is set to `0x08008000` when the interface is configured
(`0x080016C6`) and never written again. V.

**`DFU_DNLOAD`, `wValue` 0, data `21 00 00 00 08`, no `0xF4` first:**

1. Accepted into dfuDNLOAD-SYNC; nothing else yet.
2. At the next `GETSTATUS` it is handed to the header check, which fails on the first test
   (5 bytes, not 64).
3. Result: dfuERROR, status `0B 00 00 00 0A 03`. No pointer is set, **nothing is erased**, the
   mark and the application are untouched. `DFU_CLRSTATUS` returns to dfuIDLE.

The same goes for `41 …`. A 64-byte block 0 is the only thing that can start an erase, and only
if it passes every header test.

**Upload** (`0x08001354`), allowed in dfuIDLE and dfuUPLOAD-IDLE:

| Block | Reply | |
|---|---|---|
| 0 | `00 21 41` | V |
| 1 | Stall, state `0x0F`: ST's "unsupported block" branch, with a state number outside DFU's | V |
| n ≥ 2 | `wLength` (≤ 64) bytes straight from memory at `0x08008000 + (n − 2) × 64` | V |

- The read callback is a bare `bx lr` (`0x08002498`): no copy, no bounds test. Reachable range is
  `0x08008000` to `0x08407F80`; the bootloader's own 32 KiB cannot be read this way. Past the end
  of flash (block 7682 and up) the read should fault, and the fault handler is an endless loop
  (`0x08002C7C`): power cycle (I). `ksflash.py` stops at 7681.
- So the zeros at `0x08071000`–`0x0807F800` **are what the flash holds**; the bootloader does not
  substitute anything. It also never writes there: its only writes are image blocks inside
  `load`–`end` and the mark. Where the zeros came from is not determined (the factory image is
  the obvious candidate).
- State `0x0F` is not in `DFU_ABORT`'s list (states 2, 3, 5, 6, 9; mask at `0x08001512`), which
  is why abort did nothing. `DFU_CLRSTATUS` twice should clear it: the first gives dfuERROR with
  status `0x0E`, the second dfuIDLE (`0x080014DC`). V in code, not tried.

### 5. The mark at `0x0807FFF8`

- Written by `0xF3`/`0xF2` when the final check passed (`0x08000350`): 4 bytes requested from
  `0x08005EC8` to `0x0807FFF8`. The flash routine programs whole 8-byte units, so the four bytes
  after the constant go along. V.
- The second word is not a field. `0x08005ECC` holds the one-character string `"\n"` used by the
  log code (referenced at `0x08005ED0`), and it happens to sit behind `0x600DC0DE`. Hence
  `0A 00 00 00`. V.
- Erased at the start of every accepted download (see 3). Checked at boot, first word only
  (see 1). V.

### 6. `0xF3`

`0xF2` and `0xF3` are the same code in the bootloader (jump table at `0x080012DA`, both to
`0x08001584` → `0x0800115A`):

1. Final check (`0x08000960`). Pass: result 3. Fail: result 0, write pointer and sum reset.
2. If the result is 3, write the mark and raise the restart request (`0x08000350`).
3. Set dfuMANIFEST-WAIT-RESET, state 8 (`0x080011BE`), because the descriptor says "not
   manifestation tolerant". ST's reset at this point has been removed.
4. The scheduler task sees the request, stops USB, waits 100 ms and resets through `AIRCR`
   (`0x080002BC` → `0x080025C4`). After the reset the boot decision finds the mark and no
   marker, and starts the application, unless Tempo is held.

Without a completed download step 1 fails, no request is raised, and state 8 is all that
happens: what was seen. Uploads in between change neither pointer nor sum, so `0xF3` still works
after a read-back: also what was seen. Two `DFU_CLRSTATUS` should leave state 8 without a bus
reset (V in code, not tried).

### 7. Writes below `0x08008000` and to option bytes

| Path | Reachable from the host? | |
|---|---|---|
| Page erase (`0x08003B60`), two callers: the mark page `0x080028E8` and the image range `0x0800286C` | Range is `load`–`end` from a header that passed `load` > `0x08007FFF` | V |
| Program (`0x08003580`), one caller `0x080026FC`: image blocks at the write pointer (≥ `load`) and the mark | Same bound | V |
| Mass erase | Not called | V |
| Option bytes (`0x08003630`), one caller `0x08002A00` | No. At every start: if `DUAL_BANK` (bit 21 of the user option word) is set, clear it, program, reload. Same as the application | V |

No request makes the bootloader erase or program its own 32 KiB, or touch option bytes or read
protection. The flash layer itself only checks "inside flash" (`0x080026A8`, from `0x08000000`);
the protection is the header test at `0x08000AB4`. What a host **can** do with well-formed
requests: erase the application and the mark (any valid header), and leave the unit in update
mode until a complete image is sent. Update mode itself survives all of it.

### 8. For a flashing tool

- **No watchdog**: no reference to IWDG or WWDG anywhere (I: addresses searched as literals).
  No timeout in update mode; it waits forever.
- Poll time in `GETSTATUS` is always 0. After the status poll for a data block the state is still
  dfuDNBUSY in that reply; the block is processed before the next request is answered.
- After a refused block the next `DNLOAD` stalls (dfuERROR). `DFU_CLRSTATUS`, `0xF4`, start again
  from block 0.
- `DFU_ABORT` returns to dfuIDLE from states 2, 3, 5, 6, 9 but does not close a download: the
  write pointer stays (`0x08001506`).
- A USB reset or `DFU_DETACH` (which disconnects and reconnects, `0x0800156E`) closes the
  download: flag, pointer and sum are reset (`0x080003D4`), the flash stays as it is.
- States only a power cycle or bus reset is known to clear on a unit: `0x0F` and 8. By the code
  two `DFU_CLRSTATUS` do it; untried.
- Class requests `0xF0`, `0xF1`, `0xF5` are accepted and do nothing (`0x080012CE`–`D4`).
- Any fault ends in `b .` (`0x08002C7C`, `0x080053B4`): the unit hangs until switched off.
- If the erase fails, the internal state is left at dfuDNLOAD-SYNC with the length kept, so the
  next `GETSTATUS` runs block 0 again, erase included (`0x0800122A`). Not seen; noted.

### Not determined

- Where the zeros at `0x08071000`–`0x0807F800` come from.
- Whether programming all-`FF` data leaves a flash unit programmable (it decides whether an
  image may cover the mark's eight bytes).
- How long the erase holds off USB, and whether the scheduler tick is 1 ms (the restart task
  runs every 50 ticks).
- The read-protection level: the bootloader reads the option bytes only to clear `DUAL_BANK`.
