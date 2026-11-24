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
| Arp (hold) | `Arp Settings` menu: `Arp Length`, `Arp Retrigger`, … | |
| Tempo (tap) | `Tempo: 120.0 BPM` / `Long press: Settings` | |
| Tempo (hold) | `Timing` menu, first entry `Tempo` | |
| Tempo held + turn | `Tempo` / new value | Three detents moved 120.0 to 189.1 (accelerated) |
| Chord (hold) | `Chord` / `Preset` menu | |
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
| Shift + Chord | `Scale` menu: `Global Scale`, `Root`, … | |
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
