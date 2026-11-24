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
| Tempo held + turn | `Tempo` / new value | Three detents moved 120.0 to 189.1 (accelerated) |
| Chord (hold) | `Chord` menu: `Preset`, `Strum (ms)`, `Strum Type`, `Voicing` | |
| Hold (hold 1.5 s) | `Transpose Hold` on release | Hold light on |
| Mutate (hold) | Blank while held | Mutate light on |
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
  `60 63 67`, `65 68 60`, `67 70 62`, `62 65 69`, `72 75 79`: some top notes an octave down. The
  rule was not worked out.
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
