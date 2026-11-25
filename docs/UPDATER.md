# How MIDI Control Center installs firmware

Written from static analysis, 2026-11-23: for that analysis MCC was not launched and nothing was
sent to a device or to Arturia's servers. Claims are marked **verified** (read from the binary or
from files on disk) or **inferred** (a conclusion drawn from verified facts, not seen directly).
Rows that a unit has since answered say so and point at [BOOTLOADER.md](BOOTLOADER.md).

Since then the update has been run on a unit, with MIDI Control Center and with
`tools/ksflash.py` sending the sequence below, and the bootloader has been read
([BOOTLOADER.md](BOOTLOADER.md), "Seen on a device" and "The bootloader, read"). What MIDI Control
Center showed there: a unit entered with Tempo is listed as KeyStep mk2 despite its noise serial,
its `Firmware Revision` field shows the bootloader's `bcdDevice` (`0x0200` as `0.2.0`), and it
offers the latest version but takes a package chosen by hand.

| | |
|---|---|
| Binary | `/Applications/Arturia/MIDI Control Center.app/Contents/MacOS/MIDI Control Center` |
| Version | 1.21.1.209, built 2025-11-05, x86_64 only (no arm64 slice) |
| SHA-256 | `846710e88c5f6ccc644e40275296028d1936e4f15bddca82c05c6dbaa811822e` |
| Tools | `llvm-objdump` disassembly, Ghidra 12.1.4 decompilation of selected functions |

Addresses are virtual addresses in that binary. The binary is stripped; function names below are
descriptive, class names come from RTTI strings.

## Summary

- The `mfreak` updater is **USB DFU 1.1 over libusb control transfers**, with three
  Arturia-specific class requests (`0xF2`, `0xF3`, `0xF4`) in place of `DFU_DETACH` and reset. No
  SysEx is involved in the update itself.
- MCC sends the image file **byte for byte, including the 64-byte header**, in blocks of the
  device's `wTransferSize`. There are no DfuSe address or erase commands.
- MCC **never looks inside the image**. Magic, VID/PID, load address, XOR sums and version in the
  header are not read by MCC, so whatever checks them is the bootloader.
- MCC's own checks are: file name pattern, a readable zip with a valid `info.json`,
  `method == "dfu"`, an image entry whose `productid` equals the device's USB product id, and that
  the image file exists.
- `productCode` and `productKey` are **not used** by the `mfreak` updater.
- MCC shows **no text about entering update mode by hand** for the KeyStep mk2.

## 1. Transport and sequence

### Which code runs

| Step | Evidence | Status |
|------|----------|--------|
| Updater name `mfreak` creates `mcc::MFreakFWUpdater` | Factory at `0x10061c490` compares the name against `worlde`, `midiplus`, `midiplus_packaged`, `huaxin`, `spark`, `mbrute`, `mfreak`; the `mfreak` branch (`0x10061c811`) builds the wrapper `0x10061c9a0`, which creates the updater at `0x10061ea30` (log string "Creating FreakFWUpdater.", vtable `0x101500708`, RTTI `N3mcc15MFreakFWUpdaterE`) | verified |
| `MFreakFWUpdater::update(path)` calls `Arturia::DFUUpdater` | `0x10061edb0` calls `0x1004594e0` with a device record `{vid = -1, pid = this+0x58}` and the package path | verified |
| `DFUUpdater` drives a USB device manager built on a statically linked libusb | `0x1004594e0` (strings "DFUUpdater Init with PID", "Burning ... on device ..."); manager object from `0x101146cb0`, vtable `0x1015f8f80`; libusb strings (`libusb_control_transfer` wrapper at `0x1001dc8a0`, darwin backend) | verified |
| The same updater serves KeyStep Pro, MiniLab 3, KeyLab mk3, KeyLab Essential mk3, MicroLab mk3, MicroFreak | `"name": "mfreak"` in their JSON files under `/Library/Arturia/MIDI Control Center/Resources/` | verified |

The other strings that look relevant ("<-- Sending start update msg to bootloader", "Sending
chunk", "wait for ack") belong to the SysEx-based updaters for older products, not to `mfreak`
(inferred from their position after "Creating BruteFWUpdater." and from `MFreakFWUpdater::update`
containing no MIDI calls).

### Device selection

| Fact | Evidence | Status |
|------|----------|--------|
| Only devices with vendor id `0x1C75` are enumerated | `cmp ..., 0x1c75` at `0x101149b77` (device list) and `0x10114a4a4` | verified |
| The device is chosen by product id alone: first enumerated device whose PID equals the requested one | Loop at `0x10045af50`; on failure "Device not found. Please verify that you have specified the correct PID, or try unplugging/re-plugging the device." | verified |
| The requested PID is the first entry of the PID list of the device definition's `firmwareUpdater` | `0x10061eaf1` (virtual call, slot `0x30`, result stored at `this+0x58`); MicroFreak's `updaterUsbProductId` `0x601` is special-cased against the same field at `0x10061f059` | verified that it is a PID list from the definition; inferred that it is `updaterUsbProductId` |
| For the KeyStep mk2 that PID is `0x0228` | `KeyStep2.json`: `usbProductId` 552 and `updaterUsbProductId` 552 | verified |
| MCC expects the **same VID, PID and serial number string** before and after the switch to DFU mode | Hotplug callback `0x101147990`: an arriving device counts as "the dfu device" only if VID, PID and serial equal the stored target (comparison at `0x101147c16`; on arrival of a match it sets the flag at `manager+0x160` and wakes the waiting thread) | verified |

### Wire protocol

All requests are control transfers on endpoint 0 with timeout 0 (wait forever). The wrapper is
virtual slot `0x180` of the device class (vtable `0x1015f8d10`, function `0x101146800`), which
calls libusb's control transfer as `(handle, bmRequestType, bRequest, wValue, wIndex, data,
wLength, timeout)`. Each request is bracketed by two libusb calls on the interface
(`0x1001cefa0` before, `0x1001cf050` after), taken to be claim and release (inferred from the
nearby string "can t call claim interface with NULL handle").

| Request | bmRequestType | bRequest | wValue | wIndex | Data | Sent from | Meaning |
|---------|---------------|----------|--------|--------|------|-----------|---------|
| Enter DFU | `0x21` | `0xF2` | 0 | index of the DFU interface in the active configuration | none | `0x10114c3e5` (in `0x10114bb90`) | Arturia request, replaces `DFU_DETACH` |
| Select image | `0x21` | `0xF4` | `image_num` from `info.json` | 0 | none | `0x101145770` | Arturia request, once before the first block |
| `DFU_DNLOAD` | `0x21` | `0x01` | block number, from 0 | 0 | `wTransferSize` bytes; last block may be shorter | `0x101146017`, `0x1011457f0` | standard |
| `DFU_DNLOAD`, zero length | `0x21` | `0x01` | next block number | 0 | none | `0x10114614a` | end of download |
| `DFU_GETSTATUS` | `0xA1` | `0x03` | 0 | 0 | 6 bytes in | `0x1011452e0` | standard |
| Leave DFU | `0x21` | `0xF3` | 0 | 0 | none | `0x101145690` | Arturia request, followed by one `GETSTATUS` |
| `DFU_UPLOAD` | `0xA1` | `0x02` | block number | 0 | 64 bytes in | `0x10114648d` (in `0x101146210`) | present in the binary, not used by the update path |

All verified. A scan of every call through slot `0x180` in the DFU code (`0x10113a000`–
`0x101150000`) finds only these request codes: there is no `DFU_DETACH` (0), `DFU_CLRSTATUS` (4),
`DFU_GETSTATE` (5) or `DFU_ABORT` (6), and no DfuSe command bytes (`0x21` set address, `0x41`
erase) are ever placed in a download buffer.

The DFU interface is recognised by `bInterfaceClass == 0xFE` and `bInterfaceSubClass == 0x01`
(`0x10114c353`, `0x101145a79`). `bInterfaceProtocol` is not checked.

### Sequence

Per package, in `DFUUpdater` (`0x1004594e0`), for each image that survives the checks in section 2
(one image for the KeyStep mk2):

1. **Find the device** (VID `0x1C75`, PID `0x0228`).
2. **Enter DFU mode**, first image only (manager slot `0x68`, `0x10114bb90`):
   - Read the active configuration. If it has **exactly one interface** and that interface is the
     DFU one, the device is taken to be in DFU mode already and nothing is sent
     (`cmp eax, 1; sete [manager+0x160]` at `0x10114c35f`).
   - Otherwise send `0xF2` to the DFU interface, let go of the device and wait up to **30 s**
     (`0x6fc23ac00` ns at `0x10114c4e5`) for a device with the same VID, PID and serial to arrive.
   - On failure: "Failed to enter DFU mode for device.", then the leave step below.
3. **Record the image path** (slot `0x48`, `0x10114b5d0`). No file content is read here.
4. **Download** on a worker thread (slot `0x70`, `0x10114c8d0`, thread body `0x101145880`):
   - Take `wTransferSize` from the DFU functional descriptor of the first interface with class
     `FE`/`01` (`word [extra + 5]` at `0x101145acf`). Zero aborts with error `0xB`.
   - Open the file, get its size, seek to offset 0.
   - Send `0xF4` with `wValue = image_num`.
   - For each full block: zero the buffer, read `wTransferSize` bytes, `DFU_DNLOAD` with the block
     number, then `DFU_GETSTATUS` repeatedly while the device reports `dfuDNBUSY`. Stop on
     `dfuERROR`.
   - Send the remainder (file size mod `wTransferSize`) as a short block, same status loop.
   - Send a zero-length `DFU_DNLOAD`, one `DFU_GETSTATUS`, then mark the transfer finished
     **whatever state the device reported** (`mov dword [r15], 6` at `0x10114615f`).
5. **Watch progress** from the calling thread: poll every 25 ms (slot `0x80`). If the state is
   unchanged and is not "in progress" for about **30 s** (`0x7517` ms at `0x10045a23a`), give up:
   "The device is blocked in "<state>" during more than 30 seconds. FW Update is aborted." and
   "Firmware Update failed. Please reboot your device and retry."
6. **Leave DFU mode** after the last image (slot `0x88`, `0x10114d6c0`): join the worker, send
   `0xF3`, one `DFU_GETSTATUS`, then wait up to **10 s** (`0x2540be400` ns at `0x10114da27`) for
   a device with the same VID, PID and serial to arrive. If none arrives the step returns error
   code 4. `DFUUpdater` has the log string "DFU Status is OK. The device may have been
   disconected." (`0x10045aab6`); which branch prints it was not traced.

All verified from the code at the addresses given.

Details that matter for a custom flasher:

| Fact | Evidence | Status |
|------|----------|--------|
| The whole file is sent, header included, from offset 0 | `seekg(0)` then sequential reads in `0x101145880`; nothing between `fopen` and the first `DNLOAD` inspects the data | verified |
| Block size is the device's `wTransferSize`, not a constant | `0x101145acf` | verified |
| For the KeyStep mk2 that is 64 bytes: a 1.0.1 or 1.0.2 image (429,448 bytes) is 6,710 blocks of 64 plus a final block of 8 | MCC reads the descriptor after the switch to DFU mode. The application's descriptor in the image is `09 21 0B FF 00 40 00 10 01` (offset `0x6861E` in the 1.0.1 image); the bootloader's, read on a unit, is `09 21 0B FF 00 40 00 1A 01`, also 64, and `ksflash.py` flashed 1.0.2 in 6711 blocks ([BOOTLOADER.md](BOOTLOADER.md)) | verified on a unit |
| `bwPollTimeout` is ignored; MCC polls `GETSTATUS` with no delay | `0x1011452e0` uses only byte 0 (`bStatus`) and byte 4 (`bState`) of the reply | verified |
| MCC does not wait for manifestation and does not issue a USB reset; the device restarts on `0xF3` | Steps 4 and 6 above; the bootloader restarts by itself after `0xF3` when the download was complete and correct ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read", 6) | verified for MCC's side; the device's side verified in its code and on a unit |
| A final short block is sent short, not padded | `0x1011460e7` passes the remainder as `wLength` | verified |
| The block counter is 16 bits and is not reset | `movzx ecx, r14w` | verified |

MCC maps `bState` to its own state names with the table at `0x101400494`:

| bState | DFU name | MCC state |
|--------|----------|-----------|
| 0 | appIDLE | Idle |
| 1 | appDETACH | Entering Dfu Mode |
| 2, 3, 5, 9 | dfuIDLE, dfuDNLOAD-SYNC, dfuDNLOAD-IDLE, dfuUPLOAD-IDLE | In Progress |
| 4 | dfuDNBUSY | Busy |
| 6, 7 | dfuMANIFEST-SYNC, dfuMANIFEST | Finished |
| 8 | dfuMANIFEST-WAIT-RESET | Entering App Mode |
| 10 | dfuERROR | Failed |

On "Failed" MCC reports the standard DFU `bStatus` text ("File is not targeted for use by this
device.", "File is for this device but fails some vendor-specific verification test", "Memory
erase function failed.", "Programmed memory failed verification.", "Cannot program memory due to
received address that is out of range.", and so on; strings near `0x1011c8ee6`). Verified. These
are the only way the bootloader's verdict on an image reaches the user.

## 2. Checks

### Before flashing

| Check | Where | Status |
|-------|-------|--------|
| A device is connected (matched by USB VID/PID). Otherwise "Device should be plugged before upgrading." | `0x1004837a0` | verified |
| The chosen path exists, is not a directory, and its name matches `firmwareFilePattern` (`*.ks2fw`). Otherwise "Firmware files must fit this format: ..." | `0x10061fc80`, wildcard match `0x100f07ed0` | verified |
| The user confirms a dialog ending "Click 'OK' when ready, or 'Cancel' to abort the firmware update." | `0x10061fc80` | verified |
| MCC tries to open the device's MIDI ports. If that fails it logs "Can't open device with normal USB Ids. Maybe the device is already in bootloader mode." and asks the user to replug, with a retry counter | `0x10061fc80` (`0x100620e93`) | verified that the code exists. On a unit entered with Tempo MCC showed `Error: Failed to open the device.` at start and still flashed it ([BOOTLOADER.md](BOOTLOADER.md)); whether that message comes from this check is not determined |
| The package unzips into a temporary directory (`%%%%-%%%%-%%%%-%%%%`) and contains `info.json` that parses as JSON. Otherwise "Error while extracting the firware.", "Invalid JSON file." or "Invalid Firmware File" | `0x10045b840` | verified |
| `method` is `"dfu"`. `"bulk"` is recognised and rejected with "Bulk update not implemented."; anything else gives "Invalid value "method" in JSON" | `0x10045bfe3`–`0x10045c0bc`, `0x1004599d8` | verified |
| For each entry of `images`: `productid` (decimal, or hex with `0x` prefix; parser `0x10045d970`) must equal the device's PID or the entry is skipped. A missing `productid` falls back to a top-level `productid`, then to the device PID for four legacy products (`0x218`, `0x406`, `0x601`, `0x2218`), else "Invalid info.json in firmware package. Image productid unknown." | `0x10045c206`–`0x10045c388`, `0x10045bed0` | verified |
| Each selected entry needs `image_num` and `file_name`, and the file must exist in the package. Otherwise "Missing update image file: ..." | `0x10045c3e8`–`0x10045ca10` | verified |

That is the complete list on the `mfreak` path. Specifically **not** checked by MCC (verified by
the absence of any such code between package load and the last `DNLOAD`):

| Not checked | Note |
|-------------|------|
| Image header magic `0010` | The file is never parsed |
| Header VID/PID, load and end address, size | Same |
| Body XOR (`0x18`) and header XOR (`0x3F`) | Same |
| Header version bytes (`0x1A`, `0x1B`) | Same |
| `vendorid`, `version_number`, `date` in `info.json` | Keys never read by the loader `0x10045b840` |
| Any signature, encryption or challenge | The DFU path has no cryptographic calls; the only bytes sent are the file's |

`productCode` and `productKey` are read at one place only, `0x10061d2d6` and `0x10061d2ff`, inside
the constructor `0x10061d250` that the factory uses for `midiplus` and `midiplus_packaged`. The
`mfreak` branch never reaches it (verified). MiniLab 3's JSON carries the identical values (63 and
`[90,87,110,40,60,78,63]`), so they look copied from a template (inferred).

The string "This file is not a crypted firmware file." belongs to the Brute-family updater, not to
this path (inferred from its neighbours "Unable to allocate memory for this data file." and the
Brute strings).

### Version comparison and downgrade

| Fact | Evidence | Status |
|------|----------|--------|
| The manual path (choose a file) compares no versions | `0x10061fc80` contains only the pattern and existence checks; `0x1004594e0` and `0x10045b840` never read `version_number` | verified |
| The automatic path offers an update only when the device's version is lower than the latest known one, or lower than `minimalVersionRequired` (`1.0.0.0` for the KeyStep mk2) | `0x10048faa0`: comparison `0x1004ad0b0`, log strings "[Firmware Update] new update available", "Required update needed", "FW file found but outdated" | verified |
| So MCC itself will send an older image if the user selects the file | Follows from the two rows above | inferred |
| Whether the bootloader refuses an older version | It does not read the header's version bytes ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read", 2). Flashing a version older than the installed one was not tried on a unit | verified in the bootloader's code |

### After flashing

MCC does not read the device back or compare versions inside the updater. It sends `0xF3`, waits
for the device to reappear, and reports "Firmware download successful for device ...". The
MicroFreak alone gets extra waiting (`0x10061ef6e`, `0x10061f059`). The KeyStep mk2 definition has
no `postUpdateMessage`. Verified.

### Device already in update mode

There are two separate mechanisms.

| Mechanism | How it works | For the KeyStep mk2 | Status |
|-----------|--------------|---------------------|--------|
| Inside the DFU code | If the active configuration has one interface and it is the DFU one, the `0xF2` request is skipped and the download starts at once | Applies. This is the recovery path: update mode shows one `FE/01/02` interface, and MCC flashed a unit entered with Tempo (2026-11-24) | verified for the code and on a unit ([BOOTLOADER.md](BOOTLOADER.md), "Seen on a device") |
| In the device list | A connected USB PID is looked up in each product's normal PID list, then in its updater PID list. A hit in the updater list alone sets the "bootloader" flag, and MCC shows "Device currently in bootloader mode. Please update firmware." and "Bootloader..." instead of synchronising | Does not apply: both lists hold `0x0228`, the normal list is searched first, so the flag is never set | verified (`0x1005c03f0`; flag copied to the device object at `0x10047efe8`; messages in `0x10047e840`) |

Consequence: MCC cannot tell a KeyStep mk2 in DFU mode from one running its application by USB
ids. Seen on a unit in update mode (2026-11-24, [BOOTLOADER.md](BOOTLOADER.md)): MCC listed it as
KeyStep mk2 with `Firmware Revision: 0.2.0`, the bootloader's `bcdDevice`, offered an update, and
its firmware update ran from a package chosen by hand.

The Firmware Upgrade dialog (`0x1006854c0`, class `mcc::FirmwareUpgradeWindow`) returns three
results handled in `0x1004837a0`: 7 = install the latest version (downloading if needed), 5 =
install from a file, 6 = a force mode ("WARNING: This mode will force a specific firmware update.
Use it ONLY if you are told by the Arturia support. Which device do you want to upgrade ?"), which
lets the user name the product instead of relying on detection. Verified. What makes the dialog
offer the force mode is not determined.

## 3. Manual entry into update mode

| Fact | Evidence | Status |
|------|----------|--------|
| `KeyStep2.json` has no `userRebootMessage` and no `postUpdateMessage` | The file | verified |
| The other `mfreak` products have an empty `userRebootMessage` or none (MicroLab mk3). Only the KeyStep Pro has a `postUpdateMessage`: "Please reboot your device, and press OK." | All eight JSON files that name `mfreak` | verified |
| The binary holds no button-combination text for this family | String search for bootloader, update mode, reboot, "WHILE HOLDING". Every button instruction found lives in the JSON of an older product (KeyLab 25/49/61: Sound + Multi + Edit; MiniLab: Shift; KeyLab: Octave Up + Octave Down after the update) | verified |

So MCC gives no hint of a power-on key combination for the KeyStep mk2. The combination exists
outside MCC: Arturia's support article says to hold Tempo while switching on, the bootloader reads
PA15 (Tempo) at start, and it works on a unit ([BOOTLOADER.md](BOOTLOADER.md), "Entering update
mode").

## 4. How MCC finds updates

MCC embeds Arturia's shared update manager (log tag `update_manager`).

| Fact | Evidence | Status |
|------|----------|--------|
| Servers: `https://resources.arturia.net`, alternative `https://patches.resources.arturia.net` | Constructor call at `0x10047cf4e`; the choice is made by `0x10047dfe0` | verified; the condition for the alternative is not determined |
| The manager is created with the application id `mcc` | `0x10047cf29` (`mov dword ..., 0x63636d`) | verified |
| Request path is built as `<server>/api/resources/<type>/<sku>[;<sku>...]/<segment>/latest` plus `?architecture=ARM` or `?architecture=intel` | URL builder `0x10075b990`, caller `0x10073b1a0` (`latest` at `0x10073b2c4`, architecture from `sysctl.proc_translated` at `0x10073b333`) | verified |
| `<type>` is `firmware` for device firmware | Global string initialised at `0x100742273`, stored as the product's type at `0x10048fe7c` | verified |
| `<segment>` is `mcc`; a second request replaces it with `macARM` when the process runs translated | `0x1007306c0` (two calls of `0x10073b1a0`), `macARM` initialised at `0x1007422fa` | verified |
| `<sku>` is the `sku` of the device definition, `keystep-mk2` | `KeyStep2.json`; the product record is built at `0x10048faa0` from the selected device | inferred |
| Derived request for this Mac (Intel binary under Rosetta): `GET https://resources.arturia.net/api/resources/firmware/keystep-mk2/mcc/latest?architecture=ARM` | Assembled from the rows above | inferred, never sent |
| The reply is JSON; fields read include `product_sku`, `name`, `permalink`, `version`, `links.product_page.href`, `links.product_resources.href`, `release_notes_html`, `platform_min_version`, `platform_max_version` | Key strings in `0x10075b990`'s neighbours (`0x10075c272`...) and `0x10073b1a0` | verified that the keys are read; the reply's shape is inferred |
| Downloads are checked against an expected size and checksum from the reply ("Bad checksum for ...", "No expected checksum for ... Can't verify download.") | `0x1007460ef` | verified |
| MCC also looks for a local file named `<sku>_Firmware_Update_<version>*` in `/Library/Arturia/MIDI Control Center/Firmware` and uses the newest ("Upgrading to LOCAL latest available FW : ") | `0x10048faa0` (`_Firmware_Update_*` at `0x100490b55`), `0x100484744` | verified |
| When the server reports a newer version MCC asks "A new firmware update (X) is available for your ... Do you want to install it now ?" | `0x1004870e0` | verified |
| `--disable-auto-update` on the command line turns the check off | String at `0x10072fe66` | inferred from the name |

## Not determined

- **Force mode trigger**, and whether the MIDI-port check is what printed `Error: Failed to open
  the device.` for a unit already in DFU mode.
- **The update API's reply.** The request above was derived, not sent.

## Open on 2026-11-23, answered since

Kept as history: the device-side questions this analysis could not answer, with the answers.

| Question | Answer |
|---|---|
| What the bootloader does with `0xF2`, `0xF3`, `0xF4` and the header: which fields it verifies, whether it refuses older versions, when it erases, when it checks the body XOR | Header magic, ids, addresses, sizes and header XOR at block 0; no version check; erase right after a good header, before any data; body XOR at `0xF3` ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read", 2, 3 and 6) |
| The bootloader's USB descriptors and transfer size | One interface `FE/01/02`, VID:PID `1C75:0228` on the 32-key unit, `wTransferSize` 64; the serial is the application's when entered by `0xF2`, noise after a Tempo entry (BOOTLOADER.md, "Seen on a device") |
| Product id `0x021B` in the image | The application overwrites it at start-up (`0x0800F87C`) with `0x0228` for the KeyStep mk2 or `0x0229` for the KeyStep 37 mk2 (BOOTLOADER.md, "USB identities") |
| PID `0x0229` (`info.json` lists the same image for `0x228` and `0x229`; `KeyStep2.json` knows only 552) | The KeyStep 37 mk2: the application and the bootloader both take `0x0229` when PB11 reads low, the 37-key strap (BOOTLOADER.md, "USB identities" and "The bootloader, read", 1). The bootloader still demands `0x0228` in the image header |
| `0xF4` semantics (on the MicroFreak `image_num` 0 to 4 selects chips and a wavetable) | Selects nothing: any `wValue` but 0 is refused; it clears the header flag and the body sum (BOOTLOADER.md, "The bootloader, read", 3) |
| `DFU_UPLOAD` (MCC has upload code, `0x101146210`, reached through manager slot `0x78`, `0x10114ce00`, unused on the update path) | The bootloader answers: block 0 `00 21 41`, block 1 stalls into state `0x0F`, block n ≥ 2 returns flash from `0x08008000 + (n − 2) × 64`; used on a unit to read the application back (BOOTLOADER.md, "Upload: the bootloader reads flash back") |

## Reproducing

Disassembly: `llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel --macho <binary>`.
Decompilation: Ghidra headless import with default analysis (about six minutes), then
`DecompInterface` on the functions named above. Start points for anyone repeating this: the
strings "Creating FreakFWUpdater.", "DFUUpdater Init with PID ", "could not find the DFU
interface", "... Downloading image (%s) to device" and "Invalid info.json in firmware package.
Image productid unknown.".
