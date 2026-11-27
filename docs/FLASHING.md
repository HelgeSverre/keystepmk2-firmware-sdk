# Flashing a KeyStep mk2

How to put firmware on a unit, check it, and get back to stock. Everything under "What is known
to work" was done on one 32-key unit between 2026-11-24 and 27 ([BOOTLOADER.md](BOOTLOADER.md), "Seen
on a device"). What the bootloader checks, and the "never send" list, are read from its code
([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read"); only upload block 1 was seen on the unit.

## What is known to work

| Step | Done with |
|---|---|
| Enter update mode with no working application | Hold **Tempo** while switching on |
| Enter update mode from a running firmware | `ksflash.py` sends request `0xF2`; stock and SDK firmware both answer it |
| Flash stock from update mode | MIDI Control Center, and `ksflash.py` |
| Flash a modified stock image | `ksflash.py`: one byte changed, sums rebuilt; it ran |
| Flash SDK firmware | `ksflash.py`: `flashdump`, `bootdump`, `selftest`, `mk2000` |
| Check what is in flash | `ksflash.py verify IMAGE`, and every `flash` before it leaves update mode |
| Copy the external flash | `sdk/examples/flashdump` + `tools/flashdump.py receive` |
| Copy the bootloader | `sdk/examples/bootdump`, the same way |
| Flash from Linux | `ksflash.py` on Ubuntu 24.04: `mk2000`, then stock, both verified |
| Flash from Windows | `ksflash.py` on Windows Server 2022 with WinUSB ([README](../README.md#linux-and-windows)): `mk2000`, verified. The unit was attached over USB/IP, not plugged in |

macOS needs no driver. Linux needs `sudo` or a udev rule, and Windows needs WinUSB on the unit;
the README's [Linux and Windows](../README.md#linux-and-windows) section has both.

## Commands

```sh
just probe                        # what is connected, and in which mode (reads only)
just build mk2000                 # build an SDK app
just flash mk2000                 # flash it: asks for update mode, sends, reads back, starts it
just flash-stock                  # stock 1.0.2 from firmware/, the same way
just verify                       # compare the unit's flash with stock 1.0.2 (reads only)
just flash selftest               # the self-test: then `just selftest-receive` and follow the display
```

`flash` refuses an image that fails `tools/fwpack.py`'s checks, asks you to type `FLASH`, and
compares the flash with the image before it starts the new firmware. `verify` on a running
firmware leaves the unit in update mode: switch it off and on afterwards.

## Before the first flash of a unit

1. `just probe`: the unit shows as `1C75:0228 'KeyStep mk2' … 3 interface(s): application`.
2. `just flash flashdump`, start `just dump-receive firmware/device/extflash.bin --port "KeyStep
   mk2"`, press **Play** on the unit. This copies the external flash: patterns, settings and the
   factory calibration, which exists nowhere else. Keep the file somewhere safe.
3. `just flash-stock` to go back, or go on to the firmware you want.

## Getting back to stock

- From any firmware that answers `0xF2` (stock, and everything built with the SDK):
  `just flash-stock`.
- From a firmware that does not start or does not answer: switch off, hold **Tempo**, switch on.
  The unit shows as `KeyStep mk2 Updater`. Then `just flash-stock`, or MIDI Control Center's
  firmware update with the `.ks2fw` package chosen by hand (decline its offer of the latest
  version if you want a particular one).
- An update that is cut short leaves the unit in update mode at the next power-on by itself:
  the bootloader erases its "valid image" mark before it erases the old image and writes the
  mark only after the whole new image has passed its check. Read from the code. Seen once on
  2026-11-27: a download that stopped partway on a USB timeout left the unit in update mode, and
  the next `just flash-stock` wrote and verified stock. The unit was not switched off in between,
  so the power-on part is still untried.

## What the bootloader checks

The 64-byte header (magic, product id `0228`, load address at or above `0x08008000`, end at or
below `0x08080000`, sizes that agree, a zero reserved word, the header's XOR sum), every block
read back after it is written, and at the end that the data reached the header's end address and
the body's XOR sum matches. No signature: a stock image with one byte changed and the sums
rebuilt ran on the unit. It does not look at the application before starting it: an image that
passes the sums but does not run is started, and Tempo at power-on is the way out.

## Never send

| What | Why |
|---|---|
| DFU upload block 1 | The bootloader goes to a state (`0x0F`) that refuses uploads; seen on the unit, cleared by a power cycle |
| An image ending above `0x0807F800` | The last page holds the bootloader's mark, which is programmed over the image's last eight bytes; the unit then stays in update mode (inferred from the code). `fwpack` refuses such an image |
| A header whose end is below its load address | Passes the size test by wrap-around, erases the mark page, then fails |
| A second download in one session without `0xF4` first | The body sum is not reset; the final check fails. `ksflash.py` always sends `0xF4` |

Nothing a host can send makes the bootloader erase or write below `0x08008000` or touch the
option bytes (read from its code).

## The two flash memories

- **The processor's flash** holds the firmware. A flash erases the 2 KiB pages from `0x08008000`
  to the end of the image and the last page (the mark), writes the image, and then the mark.
  Nothing else is touched.
- **The external flash chip** holds patterns, settings and calibration. No firmware update
  touches it. Stock 1.0.2, 1.0.3 and 1.0.4 use `0x00000`–`0x40FFF` and `0x60000`–`0x7FFFF`; the
  SDK writes only `0x41000`–`0x5FFFF`, which the stock versions never address
  ([HARDWARE.md](HARDWARE.md), "The SDK's area across versions": read from the code and checked
  in the emulator). So stock's patterns and an SDK firmware's data both survive switching between
  the two. On the unit, the external flash still held its patterns and settings after several
  updates.

No tool writes a saved copy of the external flash back to a unit.
