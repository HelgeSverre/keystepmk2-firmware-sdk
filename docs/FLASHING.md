# Flashing a KeyStep mk2

How to put firmware on a unit, check it, and get back to stock. Everything here was done on one
32-key unit on 2026-11-24 and 25; the evidence is in [BOOTLOADER.md](BOOTLOADER.md) ("Seen on a
device" and "The bootloader, read").

## What is known to work

| Step | Done with |
|---|---|
| Enter update mode with no working application | Hold **Tempo** while switching on |
| Enter update mode from a running firmware | `ksflash.py` sends request `0xF2`; stock and SDK firmware both answer it |
| Flash stock from update mode | MIDI Control Center, and `ksflash.py` |
| Flash a modified stock image | `ksflash.py`: one byte changed, sums rebuilt; it ran |
| Flash SDK firmware | `ksflash.py`: `flashdump`, `bootdump`, `mk2000` |
| Check what is in flash | `ksflash.py verify IMAGE`, and every `flash` before it leaves update mode |
| Copy the external flash | `sdk/examples/flashdump` + `tools/flashdump.py receive` |
| Copy the bootloader | `sdk/examples/bootdump`, the same way |

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

1. `just probe`: the unit should show as `1C75:0228 KeyStep mk2`.
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
  mark only after the whole new image has passed its check.

## What the bootloader checks

The 64-byte header (magic, product id `0228`, load address at or above `0x08008000`, sizes that
agree, a zero reserved word, the header's XOR sum), every block read back after it is written,
and the body's XOR sum at the end. No signature. It does not look at the application before
starting it: an image that passes the sums but does not run is started, and Tempo at power-on is
the way out.

## Never send

| What | Why |
|---|---|
| DFU upload block 1 | The bootloader goes to a state (`0x0F`) that refuses uploads; seen on the unit, cleared by a power cycle |
| An image ending above `0x0807F800` | The last page holds the bootloader's mark and is erased first; `fwpack` refuses such an image |
| A header whose end is below its load address | Passes the size test by wrap-around, erases the mark page, then fails |
| A second download in one session without `0xF4` first | The body sum is not reset; the final check fails. `ksflash.py` always sends `0xF4` |

Nothing a host can send makes the bootloader erase or write below `0x08008000` or touch the
option bytes (read from its code).

## The two flash memories

- **The processor's flash** holds the firmware. A flash replaces `0x08008000` up to the end of the
  image and nothing else.
- **The external flash chip** holds patterns, settings and calibration. No firmware update
  touches it. Stock 1.0.2, 1.0.3 and 1.0.4 use `0x00000`–`0x40FFF` and `0x60000`–`0x7FFFF`; the
  SDK writes only `0x41000`–`0x5FFFF`, which the stock versions never address
  ([HARDWARE.md](HARDWARE.md), "The SDK's area across versions"). So stock's patterns and an SDK
  firmware's data both survive switching between the two.

There is no tool yet that writes a saved copy of the external flash back to a unit.
