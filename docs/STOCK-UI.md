# Stock firmware: what each gesture does

Recorded 2026-11-23 by running stock 1.0.2 in the emulator as the 32-key model and reading the
display back as text (`tools/screentext.py`). Each gesture started from a fresh boot in Seq mode,
stopped, with a blank external flash. Nothing here has been checked on a device.

The reader matches the stock fonts, in which `l` and `I` look alike; the texts below are
corrected by hand.

## Buttons

| Gesture | Display | Notes |
|---|---|---|
| Seq (tap) | `Seq mode` / `Long press: Settings`, then `Tempo: 120.0  1/16` / `Bank: 1 \| Preset: 1` | Seq light on, Stop light on |
| Arp (tap) | `Tempo: 120.0  1/16` / `Arp \| Up` | Arp light on |
| Seq (hold 1.5 s) | `Seq Settings` menu: `Pattern Init`, `Clear`, … , `All Patterns Init` | Encoder light on; turn to move |
| Arp (hold) | `Arp Settings` menu: `Arp Length`, `Arp Retrigger` (see Arp, chord and scale settings) | |
| Tempo (tap) | `Tempo: 120.0 BPM` / `Long press: Settings` | |
| Tempo (hold) | `Timing` menu, first entry `Tempo` | |
| Tempo held + turn | `Tempo` / new value | Three detents moved 120.0 to 189.1 (accelerated). Re-measured 2026-11-25 with `emutest.turn` and with three detents 30 ms apart: 121.1 both times |
| Chord (hold) | `Chord` menu: `Preset`, `Strum (ms)`, `Strum Type`, `Voicing` | |
| Hold (hold 1.5 s) | `Transpose Hold` on release | Hold light on |
| Mutate (press) | A new variation of the sequence; an animation while held (see Mutate) | Mutate light on |
| Encoder click | `Global Settings` menu: `MIDI`, `CV Pitch`, `CV Mod`, … | Back goes up one level |
| Encoder turn at home | No change | |
| Encoder or Back (hold) | No change | |

## With Shift

| Gesture | Display | Notes |
|---|---|---|
| Shift alone | No change | Shift light on |
| Shift + Seq | `Save Pattern` / `Bank: 1 \| Preset: 1` | A page: the destination can be chosen |
| Shift + Arp | No change in Seq mode | Legend: To Seq |
| Shift + Mutate | `Mutate Reset` | |
| Shift + Chord | `Scale` menu: `Global Scale`, `Pattern Scale`, `Root` | |
| Shift + Tempo | `Seq Length` / `16 steps`; turn to change, click to confirm | Stays open after release |
| Shift + Record | `Overdub On` | A switch, not a momentary action |
| Shift + Stop | `Output Channel`; turn, click to confirm | Stays open after release |
| Shift + Play | `Pattern Chaining` / `Select Patterns`, then `Press Pattern Keys` | Back leaves |
| Shift + Hold | `Transpose Hold` | Hold light on |
| Shift + Oct + / − | `Octave: +1` / `Octave: -1` | |
| Shift + encoder click | `Global Settings` | Same as without Shift |
| Shift + Back | No change | |

## Shift + keys

Keys are numbered from 0 at the low F.

| Key | Legend | Display |
|---|---|---|
| 0–3 | Bank 1–4 | `Bank: N` |
| 4–19 | Pattern 1–16 | `Bank: 1 \| Preset: N` |
| 20, 21 | ‹ Time Div › | `Time Div: 1/8`, `Time Div: 1/32` (from 1/16) |
| 22 | 3 | `Time Div: 1/16T` |
| 23, 24 | ‹ Nudge › | `Nudge -1 step`, `Nudge +1 step` |
| 25, 26 | ‹ Octave › | `Transpose -12 st`, `Transpose +12 st` |
| 27 | Double | `Pattern Doubled` / `to 32 steps` |
| 28 | Quant | `Pattern Quantized` |
| 29 | Reload | `Pattern Reloaded` |
| 30 | Clear | `Pattern Cleared` |
| 31 | Undo | No change (nothing to undo) |

## Seq Settings and quantizing

Seq held for 1.5 s opens `Seq Settings`: `Pattern Init`, `Quantize`, `Rec Count In` (Off, On;
Off as shipped), `Rec Autom` (On as shipped), `Wait to Load` (Instant, 1 Bar), `All Patterns Init`.

- `Quantize` offers `50%` and `100%`; the click shows `Pattern Quantized`. Shift + Quant shows the
  same message at once.
- `Rec Quantize` (On as shipped) is not in any menu on the device; MIDI Control Center sets it
  (global setting `0xC006`). On: a key played 30 ms before or after a step boundary plays back
  exactly on it. Off: it plays back as played (+29 and −27 ms measured).
- Shift + Quant is the 100 % quantize. `50%` moves each note half way: 40 ms late became 20 ms.
- MIDI Control Center also gives `Wait to Load` a third value, `At End`.

Measured in the emulator on 1.0.2, with `Rec Quantize` switched over SysEx.

## Recording

| Gesture | Result |
|---|---|
| Record, stopped | Record light on; `Keys: Add Notes` / `Hold: Add Rest` / `Keys+Hold: Tie` |
| Keys | Each key sounds and is entered as a step |
| Hold | Enters a rest |
| Back | Leaves recording; back to the home screen |
| Record again | Shows the note of a step (`F3`) |
| Play | Start is sent and the entered notes play in order |
| Keys while playing | The keys sound alongside the sequence; they do not transpose it |
| Record while playing | Switches recording off and on without stopping |
| Stop | Stop is sent; Stop light on |

## Arp, chord and scale settings

Measured on stock 1.0.2 in the emulator, like the rest of this file; not on a device. One detent
of the emulated encoder moves a value by two when turned at its usual speed, so the lists were
read with half detents.

**Arp Settings** (Arp held 1.5 s): `Arp Length` (1 to 64 steps, shows 16) and `Arp Retrigger`
(On, Off; On by default).

- With three keys in Up, a length of 4 plays `C E G C`, `C E G C`: the pattern starts again every
  Arp Length steps. 8 and 17 do the same at 8 and 17 steps.
- With Arp Retrigger Off the pattern runs on whatever the length.
- A length of 16 that has not been edited does not restart the pattern (22 steps of `C E G`
  without a break). Changing it to 17 and back to 16 makes it restart every 16 steps.

**Chord** (Chord held 1.5 s): `Preset`, `Strum (ms)`, `Strum Type`, `Voicing`. A tap of Chord is
chord mode.

| Preset | Semitones above the key |
|---|---|
| User | what was played (below); the key alone until then |
| Oct | 0 12 |
| 5th | 0 7 |
| Maj, min | 0 4 7, 0 3 7 |
| sus2, sus4 | 0 2 7, 0 5 7 |
| Maj7, min7 | 0 4 7 11, 0 3 7 10 |
| Maj9, min9 | 0 4 7 11 14, 0 3 7 10 14 |
| Maj11, min11 | 0 4 7 11 14 17, 0 3 7 10 14 17 |

- `Strum (ms)`: 0 to 80. At 20 ms the notes of `min` start 20 ms apart.
- `Strum Type`: Up, Alt Up, Down, Alt Down, Random. `min` on C: Up `C Eb G`, Alt Up `C G Eb`,
  Down `G Eb C`.
- `Voicing`: Off, On, Bass -1, Bass -2, Bass -3. Bass -1 adds the root an octave below the chord,
  Bass -3 three octaves below. With On, `min` on C, F, G, D, C5 played in that order came out as
  `60 63 67`, `65 68 60`, `67 70 62`, `62 65 69`, `72 75 79`: see "Voicing On" below.
- User chord: with Chord held, play keys (`User Chord   3 notes`), let go (`User Chord
  Recorded`). C Eb G recorded, then the D key plays D F A.
- In a scale every note of the chord is fitted: `min` on C in C major plays `C D G`.
- With the arp on, one key's chord is what the arp runs over.

**Scale** (Shift + Chord): `Global Scale` (Off and the fifteen scales), `Pattern Scale` (Off,
Global, the scales, `User   (Keys to edit)`), `Root`. `Root` did not move the global scale: with
Global Scale Freygish, the keys played the same notes at Root C, C# and D#. `Global Root` is a
setting in MIDI Control Center (`KeyStep2.json`).

A key outside the scale plays the nearest note of the scale, and the lower one when two are as
near. The keys C to B with root C, in semitones from C (12 is the C above):

| Scale | C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Major | 0 | 0 | 2 | 2 | 4 | 5 | 5 | 7 | 7 | 9 | 9 | 11 |
| Minor | 0 | 0 | 2 | 3 | 3 | 5 | 5 | 7 | 8 | 8 | 10 | 10 |
| Dorian | 0 | 0 | 2 | 3 | 3 | 5 | 5 | 7 | 7 | 9 | 10 | 10 |
| Phrygian | 0 | 1 | 1 | 3 | 3 | 5 | 5 | 7 | 8 | 8 | 10 | 10 |
| Lydian | 0 | 0 | 2 | 2 | 4 | 4 | 6 | 7 | 7 | 9 | 9 | 11 |
| Mixolydian | 0 | 0 | 2 | 2 | 4 | 5 | 5 | 7 | 7 | 9 | 10 | 10 |
| Locrian | 0 | 1 | 1 | 3 | 3 | 5 | 6 | 6 | 8 | 8 | 10 | 10 |
| Harmonic Minor | 0 | 0 | 2 | 3 | 3 | 5 | 5 | 7 | 8 | 8 | 11 | 11 |
| Blues | 0 | 0 | 3 | 3 | 3 | 5 | 6 | 7 | 7 | 10 | 10 | 10 |
| Pentatonic Major | 0 | 0 | 2 | 2 | 4 | 4 | 7 | 7 | 7 | 9 | 9 | 12 |
| Pentatonic Minor | 0 | 0 | 3 | 3 | 3 | 5 | 5 | 7 | 7 | 10 | 10 | 10 |
| Japanese | 0 | 0 | 2 | 3 | 3 | 3 | 7 | 7 | 8 | 8 | 8 | 12 |
| Gypsy | 0 | 0 | 2 | 3 | 3 | 6 | 6 | 7 | 8 | 8 | 10 | 10 |
| Arabic | 0 | 1 | 1 | 4 | 4 | 5 | 6 | 6 | 8 | 8 | 11 | 11 |
| Freygish | 0 | 1 | 1 | 4 | 4 | 5 | 5 | 7 | 8 | 8 | 10 | 10 |

## Strips, aftertouch and pedal

Stock 1.0.2 in the emulator, DIN output, default settings.

- **Bend strip**: pitch bend, 14 bits, by position: three quarters up `E0 42 5E` (12098), the
  top `E0 7F 7F`, a quarter up `E0 44 1F` (4036), the bottom `E0 00 00`. A touch in the middle
  sent nothing, also when it came from the bottom. Let go at the bottom, it came back to the
  centre in 21 messages, about 410 apart, within 150 ms, ending on `E0 00 40`.
- **Mod strip**: `B0 01 xx` by position (`3F` in the middle, `7F` at the top); nothing when let
  go.
- **Aftertouch** and **pedal**: nothing was sent for any level put on either ADC channel, with or
  without a key down. Re-measured 2026-11-25 with aftertouch on channel 0 and the pedal on
  channel 1 ([HARDWARE.md](HARDWARE.md)), the pedal input at full scale as on a unit with nothing
  plugged in, aftertouch 0 to 4095 with a key held, the pedal 0 to 4095: still nothing. Whether
  the emulator's model of the two inputs or a setting or threshold in the firmware is the reason
  is not known.

## Mutate

Stock 1.0.2 in the emulator, Seq mode, eight steps recorded: C4 D4 E4 F4 G4 A4 B4 C5.

- Each press of Mutate replaces what the sequence plays with a variation, which then repeats
  unchanged. While the button is held the display shows a double helix turning, and the Mutate
  light goes round the colours from red; there is no menu, and turning the encoder with it held
  changed nothing that was seen.
- How long the button is held sets how far the variation goes (the rule is below). Of the eight
  steps, a tap changed 6 or 7, a hold of 0.3 s changed 3, and a hold of 1.25 s changed 4 to 8
  and in two of four tries left four longer notes in place of eight.
- Shift + Mutate shows `Mutate Reset` and the original plays again.
- Six presses in a row gave, per step (original `60 62 64 65 67 69 71 72`):

  | Press | Notes |
  |---|---|
  | 1 | 67 60 62 65 69 67 71 72 |
  | 2 | 60 67 64 64 67 62 65 72 |
  | 3 | 69 62 64 62 67 65 72 77 |
  | 4 | 67 69 64 62 64 76 81 72 |
  | 5 | 67 64 81 60 64 71 72 72 |
  | 6 | 64 62 60 74 72 69 71 76 |

  Two to four steps still hold their original note after every press, the sixth included, so a
  variation is made from the original, not from the variation before it.
- The velocities of a variation are the original's, in another order (100 five times, 99 twice,
  97 once in every row). Gate lengths did not change.
- With rests in the sequence (notes on steps 1, 3, 5, 6, 8), every variation had five notes, on
  other steps: 1 4 5 6 7, then 1 2 4 5 8, 1 3 5 6 8, 1 3 5 7 8.
- The notes are the sequence's own, some an octave up or down (77, 81, 55, 52, 48 above).
- `Dice Rolled`, `Clear Spice` and `Gate/Spice only in Seq/Arp mode` are in the firmware's
  strings; touching the Mod strip shows a `Pitch | Mod` display.

### The rule, read from the code

Read from the 1.0.2 code, not run step by step. The hold-time effect above is the one part
measured in the emulator.

- **When.** The press [0x0804CC94] notes the time, starts the picture and deals new dice for
  Spice. The variation is made when the button is let go [0x0804774C].
- **Amount, 0 to 100.** Let go within 250 ms: 30 (a hidden setting's default). Held longer: from
  0 up to 100 over one second, back to 0 over 0.67 s, and round again. The line along the bottom
  of the picture is this amount. Let go at exactly 0, the variation playing becomes the pattern
  and the light goes out.
- **From the original.** The first Mutate keeps a copy of all 64 steps; every later one puts it
  back first [0x08035024]. Shift + Mutate puts it back and stops there.
- **Scale.** The Scale setting is not used. The pattern's own notes give a set of pitch classes
  [0x0805DD24]: what every fitting transposition of eight built-in scales has in common, or the
  notes themselves when none fits. Pitches move by members of that set ("degrees"). The root is
  the commonest note, unless that is a third or fifth above the next commonest [0x0805DDF0].
- **Operations.** A round runs amount / 20 + 1 of them (2 at a tap, 6 at 100), and a pattern of
  16 steps or fewer gets four rounds [0x08034F68, 0x080339C0]. Each is one of seven, picked by a
  random number, and covers a stretch of the pattern whose length grows with the amount:

  | Operation | What it does |
  |---|---|
  | Transpose | A stretch moves up or down by up to 3 degrees |
  | Invert | A stretch is mirrored about one degree |
  | Block swap | Two groups of whole steps change places |
  | Reverse | A stretch plays backwards |
  | Harmony | A second note 2 to 4 degrees above the first; only if the pattern has chords |
  | Run | A stretch becomes a run up, or down by twos, along the scale |
  | Shift | A note moves into a rest next to it |

  Block swap and Reverse are mostly turned down for a pattern with few notes; Run is mostly
  turned down always.
- **Afterwards.** A note more than an octave above the original's highest or below its lowest
  comes back an octave [0x08034058].
- **What stays.** Velocities travel with their steps; gates change only for harmony notes; a rest
  stays a rest unless a note moves into it. That fits what was measured above.
- **Seed.** A clock read at that moment [0x08035024]; it is not stored, so a variation cannot be
  made again.

## CV settings

Stock 1.0.2 in the emulator, blank calibration, each setting written over USB (`36 FB`, see
[SYSEX.md](SYSEX.md)). A setting with a list of values is written as its index scaled to
`0x7FFF`: index × 32767 / (values − 1); a small number such as 1 is read as the first value.
All numbers are register values, not volts.

- **Pitch Format VoltOct**: `(note − reference) × 65536 / 120`, never below 0. **Pitch Ref** C-2,
  C-1, C0, C1, C2, C3 puts the reference at note 0, 12, 24, 36, 48, 60. Notes 53, 60, 72, 84 at C1:
  9284, 13106, 19660, 26214.
- **HertzVolt**: 6553 at the reference note, doubling every octave, 65535 at most. The same notes
  at C1: 17494, 26214, 52428, 65535; at C3: 4372, 6552, 13106, 26214.
- **Pitch Bend Range**: the Bend strip at the top adds 546 per semitone of range to the pitch
  output: note 60 at C1 gives 13652 at 1 ST, 14198 at 2 ST (the default), 19658 at 12, 26212 at
  24.
- **Gate Retrig**, a second key with the first held: Legato (the default) leaves the gate up;
  Mono drops it for 2 ms; Legato Slide leaves it up and moves the pitch there gradually (13106,
  13174, 13266, 13350, 13428, 13498 … a millisecond apart, towards 14198).
- **Gate Format** was written and read back; what it does to the gate pins was not read cleanly.
- **Mod 1 Source**: Velocity gives the last note's velocity; ModWheel the Mod strip; Aftertouch
  stayed at 0 (see Strips, aftertouch and pedal); Random a new level at each note (758, 1233,
  599, 612); Pulse 1545 at 10 ms after the key and 0 at 20 ms.
- **Mod Max Volt**: the top of the output is 309 per volt: 309 at 1 V, 1545 at 5 V, and the strip
  half way at 10 V gives 1545.

## Gate, Spice and Dice

Stock 1.0.2 in the emulator, Seq mode, eight steps of a sixteenth playing at 120 BPM (125 ms a
step), default settings.

- **Shift + Bend strip** shows `Gate | Spice` with a bar and sets the gate length by where the
  finger is: 8 ms at the bottom, 22 ms a quarter up, 52 ms three quarters up, 69 ms at the top.
  The default (MIDI Control Center shows `Gate 50`) is 38 ms. So the gate is about 8 ms + 0.6 ms
  per point of a 0-to-100 value at this step length, and it stays where it was left. No MIDI is
  sent for the strip.
- **Shift + Mod strip** sets Spice the same way, and sends nothing. With Spice above 0 each step
  plays with another velocity and gate, the same on every pass:

  | Strip | Velocities (were 97 to 100) | Gates (were 38 ms) |
  |---|---|---|
  | a quarter up | 93 to 106 | 29 to 66 ms |
  | half way | 89 to 112 | 21 to 98 ms |
  | the top | 80 to 124 | 6 to 122 ms |

  A step's change is in proportion to the strip: the step that gained 6 at a quarter gained 12 at
  half and 24 at the top. Timing did not change. Back at the bottom the steps play as recorded.
- At the top of the strip some steps are left out and some play an octave away. Five patterns of
  dice on the eight steps `60 62 64 65 67 69 71 72` gave:

  | Dice | Notes played |
  |---|---|
  | 1 | 60 62 65 67 69 72 (two left out) |
  | 2 | 62 65 67 69 72 (three left out) |
  | 3 | 60 62 77 67 69 71 84 (two an octave up) |
  | 4 | 60 62 64 65 69 71 72 (one left out) |
  | 5 | 60 62 64 83 72 (three left out, one an octave up) |

  Gates there ran from 2 ms to 121 ms, with many at exactly 62 ms.
- **Dice**: with Shift held, three taps on the Mod strip within half a second of each other show
  `Dice Rolled` [0x0804F178 counts presses 501 ms apart, and on the third seeds a new random
  number]; the steps then vary by other amounts. Three taps without Shift, or on the Bend strip,
  do nothing. A Mutate press deals new dice too [0x0804CC94].

### The rule, read from the code

Read from the 1.0.2 code; the measurements above fit it.

- The dice are a 16-bit seed kept in the pattern (sequence setting 6, hidden in MIDI Control
  Center). From it the firmware makes two numbers from 0 to 128 for each of 64 steps
  [0x0805AB1C]. Spice changes what is played, never the pattern.
- With Spice `S` from 0 to 255 and a step's two numbers `a` and `b` [0x0805A958]:

  | Changed | Rule |
  |---|---|
  | Velocity | by `(b − 64) × S / 256` half-steps of velocity: at most 32 either way, turned back at 1 and 127 |
  | Gate | from the Gate setting towards `2 × a`, on a scale where 46 is nothing and 208 the whole step |
  | Left out | when that gate comes out at 46 or less |
  | Half a step | when it comes out at 208 or more: the gate is 50 % (the 62 ms above) |
  | Octave up | when `6 × b < a × S / 256` |
  | Octave down | otherwise, when `7 × a < b × S / 256` |

  At full Spice about one step in five is left out and one in seven plays an octave away. There
  is no level at which these begin: they get likelier as Spice rises.
- The Gate setting 10 to 91 gives 7 % to 56 % of the step by the same scale, which is the 8 ms to
  69 ms measured above.
- Also seen: Shift + Tempo shows `Seq Length`; the Shift + edit keys show `Time Div:`, `Nudge`,
  `Transpose`, `Pattern Doubled`, `Pattern Quantized`, `Pattern Reloaded`, `Pattern Cleared`,
  `Pattern Undo`.

## The user scale

Stock 1.0.2 in the emulator. In the Scale menu, `Pattern Scale` edited and turned to `User   (Keys
to edit)`: the display becomes a drawing of twelve keys, and a key pressed changes the drawing.
With C4, D4 and E4 pressed there, the keys C4 to B4 afterwards play `60 61 61 63 63 65 66 67 68 69
70 71`: D and E have gone out of the scale and play the note below, C has stayed. So the user
scale starts with all twelve notes, a key takes its note out or puts it back, and C (the root at
its default) stays in. Pressing C4 again changed nothing.

## Vibrato

Stock 1.0.2 in the emulator, settings written over USB (ids 16393 `Wheel Vibrato` and 16394
`Vibrato Rate`, values scaled to `0x7FFF` over their range), a key held, pitch output sampled
every 10 ms.

- With Wheel Vibrato above 0 and the Mod strip up, the pitch output swings around the note. At
  the top of the strip: 13090 counts peak to peak with the setting at its top (12 semitones
  either way), 6464 at half. With the strip half way the swing halves; at the bottom there is
  none.
- Vibrato Rate: one swing a second at its default, every 196 ms at half of its range, every
  100 ms at the top. So the setting's 3 to 100 is 0.3 to 10.0 Hz, and Wheel Vibrato's 0 to 120 is
  0 to 12.0 semitones.
- Nothing is sent in MIDI for it: no pitch bend. Mod 2 still follows the strip.

## Voicing On

Read from the code [`0x0805CF14`; state reset `0x0805D1D8`, called from the chord parameter
setter `0x0802DA18`] and checked against 24 chords played in the emulator, which a model of the
rule reproduces note for note (`sdk/examples/mk2000/tests/harmony_test.c` holds them).

- The firmware keeps a **window**: the lowest and highest note of the previous chord as it
  sounded. It is taken down when that chord's key is let go, and becomes the window in force at
  the next note-on whose lowest note differs from that chord's.
- A new chord is left alone unless an edge of the window lies within its own range. If one does,
  each note is considered on its own: a note below the window may go up an octave, one above it
  down an octave, and it does so if that puts it inside the window or nearer to either edge than
  it was. A note that would land on an earlier note of the chord stays where it was.
- The notes keep their order: the key, then the preset's intervals from low to high. The strum
  walks that order, not pitch order, so a voiced chord is strummed in an order of its own.
- Changing Voicing starts with no window. Changing the preset from the menu sends an empty
  release, which also empties the window. Switching chord mode off and on does not.
- Voicing On and the Bass values exclude each other.
- After voicing, in this order, come Spread (extra copies of the notes an octave up, or a cut, to
  reach a number of notes; no entry in the 32-key unit's menu), strum, and the scale last.

