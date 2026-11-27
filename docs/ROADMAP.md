# Roadmap: custom firmware for the KeyStep mk2

Goal: flash our own firmware safely, with an SDK, a toolchain and an emulator, and a multi-track
MIDI sequencer as the demonstration.

Status, 2026-11-25. Phases 3 and 4 are done on one unit: update mode entered with Tempo, the
flash read back through DFU upload, and stock 1.0.2 flashed from update mode first through MIDI
Control Center and then with `tools/ksflash.py`, each verified by reading the flash back
([BOOTLOADER.md](BOOTLOADER.md), "Seen on a device"). Phase 5, 2026-11-25: a stock image with one
byte changed ran, so there is no signature. The same day `flashdump` copied the unit's external
flash, `bootdump` read the bootloader (phase 6), MK2000 was flashed and played, and the self-test
ran on the unit: display, lights, buttons, encoder, keys, strips, aftertouch, pedal, USB and
flash reads pass ([SELFTEST.md](SELFTEST.md)). Next: the outputs not yet tried on the unit (MIDI
DIN, CV, gate, sync), the SDK's flash write and `ks_store`, and the newest MK2000 build there.

## The gating question

Flashing is safe once two things are known:

1. There is a way into update mode that does not need a working application.
2. What the bootloader accepts, so a tool can refuse an image the bootloader would reject, or
   worse, accept and fail to start.

| Question | Answer so far | Evidence |
|----------|---------------|----------|
| Recovery entry | Hold Tempo while switching on | Done on a unit, at least three times; MIDI Control Center and `ksflash.py` flash a unit entered this way |
| Entry from the application | USB class request `0xF2`, then a RAM marker and a reset | Application and bootloader code ([BOOTLOADER.md](BOOTLOADER.md)); used on a unit for every flash from a running firmware, stock and SDK |
| Image integrity | Two XOR sums in a 64-byte header, no encryption | All four stock images; `tools/fwpack.py selftest` rebuilds each header byte for byte |
| Signature | None | A stock image with one byte changed and the sums rebuilt was flashed and ran (2026-11-25) |
| Update protocol | USB DFU blocks plus Arturia requests `0xF2` (enter), `0xF4` (select image; in the bootloader it resets the body sum), `0xF3` (leave) | MCC's code ([UPDATER.md](UPDATER.md)), the bootloader's code; `tools/ksflash.py` sends that sequence and a unit took it |
| Host-side checks | None on the image: MCC only matches the package's product id | [UPDATER.md](UPDATER.md) |
| Bootloader contents | Read out and disassembled | `sdk/examples/bootdump` on a unit, 2026-11-25 ([BOOTLOADER.md](BOOTLOADER.md), "The bootloader, read") |

## Phases

1. **Map the stock firmware (no risk).** Ghidra export, SysEx dispatcher, version diff: done.
   Board map from the code: [HARDWARE.md](HARDWARE.md). Function names: `ghidra/names-1.0.2.tsv`.
2. **Update path, host side (no risk).** What MIDI Control Center sends and checks:
   [UPDATER.md](UPDATER.md).
3. **Observe update mode (low risk, reads only).** Hold Tempo at power-on, record the USB
   descriptors of the update-mode device (`just probe`), read what DFU upload returns, leave by
   switching off. Done: upload returns flash from `0x08008000` only, not the bootloader.
4. **Stock round trip (low risk).** Flash the unmodified 1.0.2 package through MCC, then the same
   image with our own tool. Done, without a capture of MCC's USB traffic.
5. **One-byte test (the first real risk).** A stock image with one menu string changed and the
   sums recomputed; Tempo entry must have worked first (phase 3). Done: the unit started it, so
   there is no signature.
6. **Read the bootloader.** DFU upload does not reach it, so an SDK firmware (`bootdump`) sends
   it over SysEx. Done; the boot decision is in [BOOTLOADER.md](BOOTLOADER.md).
7. **Emulator.** Working: `tools/emu.py` runs stock 1.0.2 with every control, and
   `tools/panel.py` shows it as a clickable KeyStep. USB is modelled with a host on the other end
   (`tools/usbmodel.py`): the stock firmware enumerates and answers SysEx over USB MIDI. The CV,
   gate and sync pins, the analog inputs, MIDI in and the tempo timer chain are modelled, and
   `--keys37` straps the board as the 37-key model. `tools/emutest.py` (`just test`) checks the
   stock firmware and the SDK examples against it. Missing: the 37-key panel.
8. **SDK.** Started: `sdk/` builds C firmware with `arm-none-eabi-gcc` and runs it in the
   emulator and the panel. Drivers: clock, display, buttons, Tempo, encoder, keys with velocity,
   touch strips, LEDs, MIDI DIN, external flash (writes confined to the 124 KiB the stock
   firmware never uses, unless the firmware lifts the guard). USB MIDI with the stock identity
   and the update request (so a flashed SDK firmware can be replaced without the Tempo button),
   the SysEx version reply, CV,
   gate, sync, aftertouch and pedal as raw values, and two proportional display fonts
   laid out like the stock ones (`tools/bdffont.py`). Missing: the 37-key model. Checked on a unit with
   `sdk/examples/selftest`, all but MIDI DIN, CV, gate, sync, flash erase and write (used by
   MK2000 on the unit) and `ks_store` ([SELFTEST.md](SELFTEST.md)).
   Tools around it: `tools/ksflash.py` (flash, read back, verify; used on a unit, see
   [FLASHING.md](FLASHING.md)), `sdk/examples/flashdump` with `tools/flashdump.py` (a read-only
   copy of the external flash over MIDI) and `sdk/examples/bootdump` (the same for the
   bootloader). All three have run on a unit.
9. **Demonstration: multi-track MIDI sequencer.** `sdk/examples/mk2000`, KeyStep MK2000
   ([MK2000.md](MK2000.md)): four tracks, per-step ratchet, probability and conditions, channel
   changes on the numbered keys, arpeggiator, chains, power-cut-safe storage, external clock,
   thru, controller recording, Mutate and Spice, scales and chords, CV. Flashed to a unit and
   played there; what has and has not been tried on it is at the top of MK2000.md.

## Rules

- No write to flash below `0x08008000`, ever. The bootloader is the only recovery path.
- No write to option bytes. Read protection level 2 is permanent.
- The flasher refuses any image that fails `tools/fwpack.py`'s checks, and any device whose USB
  identity is not the expected update-mode device.
- Keep a copy of stock 1.0.2 and a verified way to flash it before flashing anything else.
- No write to the external flash outside `0x41000`–`0x5FFFF` before that unit's flash has been
  read out and kept. The calibration at `0x60000` is per unit.
- The stock firmware is Arturia's. Share tools and patches, not their binaries.
