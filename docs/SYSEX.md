# SysEx handler

Static analysis of the application image, 2026-11-22. The wire format (7-bit packing, sequence
byte, CBOR arguments) and the nine operations seen in MIDI Control Center's traffic are documented
in the `keystep2-editor` project's README; this file adds what the firmware itself shows.

Addresses are for 1.0.4 unless noted. Nothing here was sent to a device.

## Dispatcher

One function handles every operation: `0x0802CA18` in 1.0.4, `0x0802CAA8` in 1.0.2. It compares
the operation code against 14 constants in its literal pool. Ghidra's auto-analysis does not find
it, because it is only reached through a C++ virtual call; `ghidra/scripts/KsSeed.java` recovers it.

The firmware reads the two operation bytes as one little-endian 16-bit number, so the constant in
the code is the wire bytes swapped: `3E 69` on the wire is `0x693E` in the image.

| Wire | In code | Seen in MCC | What the handler does |
|------|---------|-------------|-----------------------|
| `3E 69` | `693E` | yes | Select pattern: reads one byte, loads that pattern, returns a status |
| `6C D6` | `D66C` | yes | Read a parameter of the selected pattern |
| `78 48` | `4878` | yes | Write a parameter of the selected pattern |
| `08 DF` | `DF08` | yes | Read next block: replies 32 bytes, or an empty byte string at the end |
| `6E 86` | `866E` | yes | Start writing a pattern file |
| `10 AC` | `AC10` | yes | Write next 32-byte block |
| `1B 5D` | `5D1B` | yes | Commit the pattern |
| `2A 89` | `892A` | yes | Read a global setting |
| `36 FB` | `FB36` | yes | Write a global setting |
| `EA F9` | `F9EA` | no | Replies with the firmware version string (`1.0.4.920`) |
| `19 92` | `9219` | no | No arguments. Arms a 100 ms timer; in the emulator the settings block at flash `0x40000` is written afterwards, so this saves the global settings |
| `83 93` | `9383` | no | Replies through a lookup keyed by the string `unlock`. Factory or service use (inferred) |
| `79 9B` | `9B79` | no | Same, keyed by `asn`. MCC's own strings use ASN for a serial number (inferred) |
| `29 32` | `3229` | no | Takes a value 1-5; one branch loops over indices 26-79 setting each to `0xFF000000` (inferred: an LED or display test) |

The same 14 codes are present in 1.0.2. No operation was added or removed by 1.0.4.

`EA F9` was sent to a device on 1.0.2 (2026-11-22). The reply is a value reply holding one CBOR
text string: `69 31 2E 30 2E 32 2E 37 38 32`, that is `1.0.2.782` (version and build). The other
four have not been sent; they change state or belong to factory tooling and should be left alone
until understood.

### Seen in the emulator (2026-11-23)

- Requests are answered on the USB MIDI port only. The same bytes on the DIN input get no reply.
- The universal identity request (`F0 7E 7F 06 01 F7`) gets no reply on either port. The image
  does hold a routine that builds an identity reply (`F0 7E 7F 06 02 00 20 6B` + family, member
  and version, at `0x08018E04` in 1.0.2), but nothing was found that calls it. MIDI Control
  Center's device definition lists the matching family and member ids, so how it reads the
  firmware revision of a KeyStep mk2 is still open.
- `36 FB` changes a setting in RAM; `19 92` writes the settings to flash
  ([HARDWARE.md](HARDWARE.md), settings block).
- The SDK answers `EA F9` in the same reply format with its own version string.

## Parameter ids

A parameter id is 16 bits: the top three bits select a group, the low 13 bits an index. The
handler rejects indices past a per-group limit.

**Pattern parameters** (read `6C D6`, write `78 48`), identical in 1.0.2 and 1.0.4:

| Group | Ids | Count | Content |
|-------|-----|-------|---------|
| 1 | `0x2000`+ | 24 | Chord settings, including the user chord (matches MCC's ids 8192-8215) |
| 2 | `0x4000`+ | 14 | Scale settings (MCC exposes 2; the rest are the user scale) |
| 3 | `0x6000`+ | 21 | Sequence settings (MCC exposes 9) |
| 4 | `0x8000`+ | 2240 | Raw bytes of the step objects in RAM, 40 per step |
| 5 | `0xA000`+ | 1024 | Raw signed 16-bit words of a second object array, 10 per object. Purpose unknown |

The three header groups have room for 32 words each in the pattern file.

### Groups 4 and 5 on a device

Read with `6C D6` from a device on 1.0.2 (2026-11-22), pattern 1-1 selected. Reads only; nothing
was written.

**Group 4** is a byte-wise window on an array of C++ step objects. Each object is 40 bytes:

| Index in object | Content |
|-----------------|---------|
| 0-3 | `74 AB 06 08`: the object's vtable pointer, `0x0806AB74` (a table of Thumb function pointers in the 1.0.2 image) |
| 4-39 | The step's 36-byte record, identical to the pattern file |

So `0x8000 + 40 × step + 4 + k` is byte `k` of a step record. Values come back as plain bytes, not
scaled to 15 bits. All 56 × 36 bytes matched the file read with `08 DF`.

- The limit of 2240 is 56 objects, so steps 57-64 are out of reach. (2240 is 64 × 35; the limit
  looks like it was computed for a 35-byte record.)
- Ids `0x88BF` and `0xA3FF` answer; `0x88C0` and `0xA400` get no reply.
- One read takes about 10 ms; all 2240 took 23 s. A whole-pattern block read is faster.

**Group 5** is a word-wise window on a second array. Each object is 10 words (20 bytes): words 0-1
are the vtable pointer `0x0806ABDC`, words 2-9 were all zero. Negative words come back as CBOR
negative integers. The file's 1024-byte region at offset 2497 has room for 64 × 16 bytes, which
fits 64 of these objects without their pointers.

**Do not write to these groups with `78 48` without checking the index.** Indices 0-3 of each
group-4 object and words 0-1 of each group-5 object are vtable pointers; overwriting one would
most likely crash the firmware. Writing has not been tried, and whether a written value is saved
without a commit is unknown.

**Global settings** (read `2A 89`, write `36 FB`):

| Group | Ids | Count in 1.0.2 | Count in 1.0.4 |
|-------|-----|----------------|----------------|
| 1 | `0x2000`+ | 5 | 5 |
| 2 | `0x4000`+ | 15 | 15 |
| 3 | `0x6000`+ | 12 | 24 |
| 4 | `0x8000`+ | bit-masked | bit-masked |
| 5 | `0xA000`+ | bit-masked | bit-masked |
| 6 | `0xC000`+ | 7 | 7 |
| 7 | `0xE000`+ | 6 | 6 |

Read and write take the id as a CBOR integer, the write followed by the value: `2A 89` + `19 C0 06`
reads `Rec Quantize`, `36 FB` + `19 C0 06 00` switches it off (no reply). A switch that is on reads
back as `0x7FFF`. The ids are the `globalParamId` values in MIDI Control Center's
`/Library/Arturia/MIDI Control Center/Resources/KeyStep2.json`, which also lists each setting's
values and default. Tried in the emulator only.

The value of a setting that has a list of choices is its index scaled to `0x7FFF`: index × 32767 /
(choices − 1), written as a CBOR integer. So a two-way switch is `0` or `0x7FFF`, and Mod Max Volt
`5V` (index 4 of 10) is `0x38E3`. A small number such as 1 is read as the first choice (V in
emulation, from the CV settings in [STOCK-UI.md](STOCK-UI.md)).

Group 3 is the only limit that differs between the versions: 1.0.4 has twelve more global
settings there.

## The block reader

The read-block handler (`0x080537C0`) begins:

```c
if (offset > 0xFFF && current_pattern == requested) return 0;
```

That is the behaviour found by experiment: after a pattern's 4096 bytes have been read, further
reads for the same pattern return nothing until a different one is requested.

The handlers build a decimal string from the pattern index before opening it, and the image has
strings for an external flash chip and file names such as `calib/aft_factory`. Patterns are
therefore files on a file system in external flash (inferred), not fixed regions of memory.
