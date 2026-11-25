# Firmware packages and image format

From the four published packages, 2026-11-22. Claims are marked **verified** (checked against all
four images) or **inferred**. Facts learned later from the bootloader and a unit are marked with
their source: [BOOTLOADER.md](BOOTLOADER.md), [UPDATER.md](UPDATER.md).

## Where the firmware comes from

- **Published packages.** Arturia's resources page for the KeyStep mk2 lists each release. The
  download buttons point at
  `https://www.arturia.com/api/download/products/keystep-mk2/firmware/keystep_mk2__1_0_N.ks2fw`.
  No login is needed. `tools/fwfetch.py` downloads and unpacks them.
- **Bundled with MIDI Control Center.** MCC installs
  `/Library/Arturia/MIDI Control Center/Firmware/keystep-mk2_Firmware_Update_1.0.1.760.ks2fw`.
  It is byte-identical to the published 1.0.1 package (verified by SHA-256).
- **MCC's own update check** has not been observed. Its binary names `https://resources.arturia.net`
  and `https://patches.resources.arturia.net` and the path `/api/resources/`; simple guesses at
  that API return 404. The request read from MCC's code is in [UPDATER.md](UPDATER.md), section 4;
  it was not sent.

| Version | Build | Built | Released | Image size |
|---------|-------|-------|----------|------------|
| 1.0.1 | 760 | 2025-11-02 | 2025-11-03 | 429,448 |
| 1.0.2 | 782 | 2025-11-13 | 2025-11-14 | 429,448 |
| 1.0.3 | 799 | 2025-11-19 | 2026-02-26 | 434,276 |
| 1.0.4 | 920 | 2026-02-25 | 2026-02-26 | 435,292 |

The KeyStep 37 mk2 is offered the same file names from its own product path. Hashes are in
`firmware/listing.json`.

## Package (`.ks2fw`)

A zip archive, stored without compression, with two members (verified):

- `info.json`: version, build date, USB vendor id `0x1C75`, `"method": "dfu"`, and a list of
  images. The one image is listed twice, for product ids `0x228` (KeyStep mk2) and `0x229`
  (inferred: KeyStep 37 mk2, which the image names in its strings).
- `ks2_embedded__fw<version>__<date>_image_552.bin`: the image. 552 is `0x228` in decimal.

MCC's device definition (`/Library/Arturia/MIDI Control Center/Resources/KeyStep2.json`) names the
updater `mfreak` with `productCode: 63` and a 7-byte `productKey`. The `mfreak` updater does not
use them ([UPDATER.md](UPDATER.md), section 2). The image is not encrypted.

## Image

A 64-byte header followed by a plain ARM Cortex-M image (verified: vector table, Thumb code,
readable strings).

| Offset | Size | 1.0.2 value | Meaning |
|--------|------|-------------|---------|
| 0x00 | 2 | `0010` | Magic, constant; the bootloader requires it |
| 0x02 | 2 | `1C75` | USB vendor id |
| 0x04 | 4 | `00000228` | USB product id; the bootloader compares the low 16 bits with `0x0228` on both models |
| 0x08 | 4 | 0 | Constant. The bootloader requires the 16-bit word at 0x08 to be zero; meaning not determined |
| 0x0C | 4 | `08008000` | Load address |
| 0x10 | 4 | `08070D48` | End address (load address + body size) |
| 0x14 | 4 | `00068D48` | Body size (file size − 64) |
| 0x18 | 1 | `4D` | XOR of every body byte (verified on all four) |
| 0x19 | 1 | 0 | Not determined; zero in all four; not read by the bootloader |
| 0x1A | 1 | `02` | Patch version |
| 0x1B | 1 | `01` | Major version (inferred) |
| 0x1C | 35 | 0 | Zero |
| 0x3F | 1 | `1C` | XOR of header bytes 0x00-0x3E (verified on all four) |

Little-endian throughout. Both integrity fields are plain XOR sums. The bootloader checks the
magic, vendor and product id, the word at 0x08, the addresses and size, and both sums; it checks
no signature, no trailing word and no version ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader,
read", 2). A stock image with one byte changed and its sums rebuilt was flashed to a unit and ran.

Body, loaded at `0x08008000`:

- **Vector table** at the start: initial stack pointer `0x20024000`, reset handler `0x08021EC5`.
- **Flash layout:** the application starts 32 KiB into flash; the bootloader occupies
  `0x08000000-0x08007FFF`. It is not in the package; it was read out of a unit
  ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read").
- **Processor:** STM32G0B1 class, a Cortex-M0+. The code is the 16-bit Thumb instruction set, and
  its literal pools reference that part's peripherals: reset and clock control at `0x40021000`,
  GPIO ports at `0x50000000`, the USB device controller at `0x40005C00` with packet memory at
  `0x40009800`, two DMA controllers, and the identity words at `0x1FFF7500`-`0x1FFF75E0`. 144 KiB of
  RAM matches the initial stack pointer. Flash is at least 512 KiB: a unit's bootloader returned
  `0x08008000`-`0x08080000` over DFU upload. The exact package and flash size are not determined.
- **Last word** of the image is a RAM address (`0x2001B05D` in 1.0.1 and 1.0.2). Not a checksum:
  it is the same in two images that differ.

Strings show a C++ application ("Keystep 2 App"), a "Firmware Updater" USB string, USB audio
descriptors, an external flash chip holding calibration files (`calib/aft_factory`,
`calib/aft_user`, `calib/expr_pdl`), and the on-screen menu text.

## 1.0.1 to 1.0.2

The two images differ in 15 bytes (verified): the header's version and its two checksums, the
version and date strings, and one data byte at `0x0807068C` (1 → 2). The code is identical.
Whatever 1.0.2 changed, it is that single byte or nothing in the application.

## Analysis pipeline

```bash
ghidra/analyze.sh 1.0.4                                  # import, analyse, export to ghidra/out/1.0.4/
uv run python ghidra/scripts/verdiff.py 1.0.2 1.0.4      # function-level diff of two exports
```

`analyze.sh` strips the 64-byte header, loads the body at `0x08008000`, adds the memory map, seeds
functions from pointer tables and prologues (`KsSeed.java`: 590 more than auto-analysis finds), and
writes `decomp.c`, `functions.tsv` and `strings.tsv`. It needs Ghidra 12 and a JDK; paths are at
the top of the script.

Results are in [SYSEX.md](SYSEX.md) and [VERSIONS.md](VERSIONS.md).

## Follow-up work

The list left open on 2026-11-22, and where each item stands:

| Item | Status |
|---|---|
| Name functions and map the peripherals to the board | Done: `ghidra/names-1.0.2.tsv`, [HARDWARE.md](HARDWARE.md) |
| The five SysEx operations MIDI Control Center does not use | Handlers read from code, [SYSEX.md](SYSEX.md); only `EA F9` sent to a unit |
| Go through the 1.0.2 to 1.0.4 function differences one by one | Not done ([VERSIONS.md](VERSIONS.md)) |
| Work out the DFU transfer MCC uses, and what the bootloader verifies | Done: [UPDATER.md](UPDATER.md), [BOOTLOADER.md](BOOTLOADER.md) |
| Dump the bootloader | Done on a unit, 2026-11-25 ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read") |
