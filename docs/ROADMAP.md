# Roadmap: custom firmware for the KeyStep mk2

Goal: flash our own firmware safely, with an SDK, a toolchain and an emulator, and a multi-track
MIDI sequencer as the demonstration. The method follows `../reverse-engineering/jt4000`.

Status as of 2026-11-23. Nothing has been flashed. The only device traffic so far is read-only
SysEx (see [SYSEX.md](SYSEX.md)).

## The gating question

Flashing is safe once two things are known:

1. There is a way into update mode that does not need a working application.
2. What the bootloader accepts, so a tool can refuse an image the bootloader would reject, or
   worse, accept and fail to start.

| Question | Answer so far | Evidence |
|----------|---------------|----------|
| Recovery entry | Hold Tempo while switching on | Arturia's support article; not tried here |
| Entry from the application | USB class request `0xF2`, then a RAM marker and a reset | Application code ([BOOTLOADER.md](BOOTLOADER.md)) |
| Image integrity | Two XOR sums in a 64-byte header, no encryption | All four stock images; `tools/fwpack.py selftest` rebuilds each header byte for byte |
| Signature | None visible in the image or header | The bootloader could still hold a check that is not visible from outside |
| Update protocol | USB DFU blocks plus Arturia requests `0xF2` (enter), `0xF4` (select image), `0xF3` (leave) | MCC's code ([UPDATER.md](UPDATER.md)); not yet seen on the wire |
| Host-side checks | None on the image: MCC only matches the package's product id | [UPDATER.md](UPDATER.md) |
| Bootloader contents | Unknown | Not in the package |

## Phases

1. **Map the stock firmware (no risk).** Ghidra export, SysEx dispatcher, version diff: done.
   Board map from the code: [HARDWARE.md](HARDWARE.md). Function names: `ghidra/names-1.0.2.tsv`.
2. **Update path, host side (no risk).** What MIDI Control Center sends and checks:
   [UPDATER.md](UPDATER.md).
3. **Observe update mode (low risk, reads only).** Hold Tempo at power-on. Record the USB
   descriptors of the update-mode device (`just probe`). If it offers DFU upload, read what it returns; that may
   be the application or the whole flash including the bootloader. Leave by switching off.
4. **Stock round trip (low risk).** Flash the unmodified 1.0.2 package through MCC while logging
   USB traffic, to capture a known-good update. Then flash the same image with our own tool.
5. **One-byte test (the first real risk).** A stock image with one menu string changed and the
   sums recomputed. If the unit starts it, there is no signature. If it refuses, it should stay in
   update mode and take stock again; phase 3 must have shown that Tempo entry works first.
6. **Read the bootloader.** Through DFU upload if phase 3 allows it; otherwise a stock image
   patched to return flash over SysEx, as on the JT-4000. Document the boot decision in
   [BOOTLOADER.md](BOOTLOADER.md).
7. **Emulator.** Working: `tools/emu.py` runs stock 1.0.2 with every control, and
   `tools/panel.py` shows it as a clickable KeyStep. USB is modelled with a host on the other end
   (`tools/usbmodel.py`): the stock firmware enumerates and answers SysEx over USB MIDI. The CV,
   gate and sync pins, the analog inputs, MIDI in and the tempo timer chain are modelled, and
   `--keys37` straps the board as the 37-key model. `tools/emutest.py` (`just test`) checks the
   stock firmware and the SDK examples against it. Missing: the 37-key panel.
8. **SDK.** Started: `sdk/` builds C firmware with `arm-none-eabi-gcc` and runs it in the
   emulator and the panel. Drivers: clock, display, buttons, Tempo, encoder, keys with velocity,
   touch strips, LEDs, MIDI DIN, external flash (confined to the 124 KiB the stock firmware never
   uses). USB MIDI with the stock identity and the update request (so a
   flashed SDK firmware can be replaced without the Tempo button), the SysEx version reply, CV,
   gate, sync, aftertouch and pedal as raw values, and optional use of the stock fonts
   (`tools/fontextract.py`). Missing: calibrated CV, the 37-key model.
   Tools around it: `tools/ksflash.py` (the update, rehearsed in the emulator against a model of
   the bootloader; the real-device path is untested) and `sdk/examples/flashdump` with
   `tools/flashdump.py` (a read-only copy of the external flash over MIDI).
9. **Demonstration: multi-track MIDI sequencer.** `sdk/examples/mk2000`, KeyStep MK2000
   ([MK2000.md](MK2000.md)): four tracks, per-step ratchet, probability and conditions, channel
   changes on the numbered keys, arpeggiator, chains, power-cut-safe storage. Runs in the emulator
   only. The earlier sketch is `sdk/examples/seq4`. Missing: external clock, thru and merge,
   controller recording, Mutate and Spice, scales and chords.

## Rules

- No write to flash below `0x08008000`, ever. The bootloader is the only recovery path.
- No write to option bytes. Read protection level 2 is permanent.
- The flasher refuses any image that fails `tools/fwpack.py`'s checks, and any device whose USB
  identity is not the expected update-mode device.
- Keep a copy of stock 1.0.2 and a verified way to flash it before flashing anything else.
- No write to the external flash outside `0x41000`–`0x5FFFF` before that unit's flash has been
  read out and kept. The calibration at `0x60000` is per unit.
- The stock firmware is Arturia's. Share tools and patches, not their binaries.
