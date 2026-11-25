# Version differences

From the published images, 2026-11-22. "Identical" means identical decompiled code after
normalising addresses and generated names (`ghidra/scripts/verdiff.py`).

| From | To | Code | What changed |
|------|----|------|--------------|
| 1.0.1 | 1.0.2 | Identical bytes except 15 | Version and date strings, header checksums, one data byte at `0x0807068C` (1 → 2) |
| 1.0.2 | 1.0.4 | 2042 of about 2570 functions identical | See below |

1.0.3 has not been decompiled; its strings are compared below.

Arturia's resources page lists the packages without release notes (looked at on 2026-11-25), so
what a version adds is read from the images here.

Where things are stored on the external flash did not change from 1.0.2 to 1.0.4: the same names,
the same blocks, identical storage code in 1.0.4, and the same sectors touched by all three in
the emulator ([HARDWARE.md](HARDWARE.md), "The SDK's area across versions").

## 1.0.2 to 1.0.4

- **Functions:** 2570 in 1.0.2, 2583 in 1.0.4. 187 runs of functions differ, 12 were added and 12
  removed. Many of the differing ones are the same code with shifted jump targets; they are not
  reviewed one by one. The full list is in `ghidra/out/diff-1.0.2-1.0.4.txt` after running
  the pipeline.
- **SysEx:** the same 14 operation codes. Pattern parameter groups and their sizes are unchanged.
  The global group `0x6000` grows from 12 settings to 24.
- **Pattern format:** nothing in the dispatcher suggests a change: the step-data group is still
  2240 values and the block reader still ends at 4096 bytes. Not verified on a device running 1.0.4.

## New on-screen text

Text present in a later image and absent from the one before. It shows where features landed.

| Version | Added | Removed |
|---------|-------|---------|
| 1.0.3 | `CC Bank A` to `CC Bank D`, `Bank`, `Knob 1` to `Knob 4`, `Fixed 32`, `Fixed 64`, `Fixed 100`, `Fixed 127`, `Long Press: Edit`, `Setting Updated` | `Knob1 CC` to `Knob4 CC`, `CC1` to `CC4`, `Fixed` |
| 1.0.4 | `MIDI Thru`, `F.Leg`, `F.Trg`, `Free`, `S.Leg`, `S.Trg` | |

Read as: 1.0.3 adds banks of knob CC assignments (the knobs exist on the KeyStep 37 mk2) and a
choice of fixed velocities; 1.0.4 adds a MIDI Thru setting and gate or trigger modes (`Free`
sits between `F.Trg` and `S.Leg` in the image, next to the existing `Sync`). These are inferences
from the strings; Arturia publishes no release notes for these versions (see above).
