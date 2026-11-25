# KeyStep MK2000 user guide

KeyStep MK2000 is a replacement firmware for the 32-key Arturia KeyStep mk2. It turns the unit
into a four-track step sequencer with an arpeggiator, chords, scales, pattern chains and MIDI
channels you change from the keys, while keeping what is printed on the panel meaning what it
says.

This is the plain-text guide; [user-guide.html](user-guide.html) is the same guide as a designed
page with the panel and the displays drawn. This guide is for playing it. How it is built is in [MK2000.md](MK2000.md); how to put it on a
unit and get back to Arturia's firmware is in [FLASHING.md](FLASHING.md).

> MK2000 is unofficial and new. It has run on one unit. Take the copy of your unit's memory that
> FLASHING.md describes before you install it, and keep Arturia's firmware at hand: `just
> flash-stock` puts it back, and your Arturia patterns are still there when you do.

## The panel

Everything you do while playing is a held button plus a key. The keys carry the same printing as
on the stock firmware, and this guide uses these names for them:

```keys
```

| Name | Keys | Used for |
|---|---|---|
| Bank keys 1–4 | the lowest four | Tracks, banks, pages of 16 |
| Numbered keys 1–16 | the next sixteen | Patterns, steps, channels, mutes |
| Edit keys | the top twelve | Time division, Nudge, Octave, Double, Quant, Reload, Clear, Undo |

The buttons: **Seq, Arp, Mutate, Chord, Tempo, Record, Stop, Play, Shift, Hold, Oct −, Oct +**,
the **encoder** (turn and click) and **Back**.

A release counts as a **tap** only within a third of a second of the press. Holding a button and
letting go later does nothing by itself, so you can hold Seq to look at the tracks without
switching anything.

## How it is organised

- A **pattern** has four **tracks** and a tempo.
- A **track** has a MIDI channel, a length of 1 to 64 steps, a time division, swing, gate length
  and direction of its own. Tracks of different lengths and speeds run side by side.
- A **step** holds up to eight notes, with velocity, gate, probability, ratchet, a condition, a
  tie and a timing offset.
- There are 64 pattern slots: 4 **banks** of 16.
- One **arpeggiator** and one **chord** mode work on the keys.

Three switches decide what happens when you play:

| Light | When lit |
|---|---|
| Seq | The tracks play while the clock runs. Its colour is the selected track: orange, teal, pink, green |
| Arp | The keys go through the arpeggiator |
| Chord | Each key plays a chord |

Play and Stop run the clock. The switches are independent: you can stop the tracks and keep the
arp running in time, or play chords into the arp.

### The home screen

```oled
Tempo: 120.0     D Min     1/16
Bank: 1 | Preset: 3*
[T1] T2  --  T4          KBD T1
```

- Top: the tempo, the scale in force if there is one, and the selected track's time division.
- Middle: the pattern in use. A `*` means it has edits that are not saved.
- Bottom: the four tracks. The selected one is boxed, an empty one is `--`, a muted one is struck
  through. At the right is where the keys go: `KBD T1` (the selected track's channel) or `KBD 9`
  (home channel 9).
- While it plays, a bar along the bottom shows where the selected track is.

## Your first pattern

1. Press **Record**. The display shows `T1 Rec` and `Keys: add notes`.
2. Play a note or a chord for each step. **Hold** adds a rest. Keys + **Hold** ties the step into
   the next.
3. Press **Record** again. The track is as long as the steps you entered.
4. Press **Play**.
5. Hold **Seq** and press **bank key 2** to select track 2, and record that the same way. Its
   length can be different.
6. **Shift + Seq**, then click the encoder, to save.

Nothing is saved by itself. The `*` on the home screen tells you there is something to save.

## Tracks

| To | Do |
|---|---|
| Select a track | Seq + bank key 1–4 |
| Mute or unmute track 1–4 | Seq + numbered key 1–4 |
| Solo track 1–4 while held | Seq + numbered key 5–8 |
| Open the track's settings | Seq + encoder click |
| Open the track's step page | Seq + encoder turn |
| Set the length | Shift + Tempo, then turn, or press a numbered key (a bank key first picks the page of 16) |
| Change the time division | Shift + the Time Div edit keys: slower, faster, triplet |
| Move every step one place | Shift + Nudge − or Nudge + |
| Move the track an octave | Shift + Octave − or Octave + |
| Repeat the track after itself | Shift + Double |
| Empty the track | Shift + Clear |
| Take back the last edit | Shift + Undo |

**Track settings** (Seq + encoder click): Channel, Length, Time Div, Swing (50–75 %), Gate
(10–90 %), Direction (Forward, Reverse, Pendulum, Random), Follows transpose, Shift (−48 to +48
semitones), Spice, Quantize and Clear Autom. Turn to move, click to edit, turn to change, click or Back
to finish.

### MIDI channels

The keys are in one of two states:

- **Track**: they play on the selected track's channel.
- **Home**: they play on the home channel, whatever track is selected.

| To | Do |
|---|---|
| Flip between Track and Home | Tap Shift + Stop |
| Set the home channel (and switch to Home) | Shift + Stop + numbered key, or Shift + Stop + turn |
| Set the selected track's channel | Shift + Stop + Seq + numbered key |
| Set another track's channel | Shift + Stop + bank key, then a numbered key |

While Record is on, the keys always play the selected track.

## Recording

| Start with | You get |
|---|---|
| Record while stopped | **Step recording**: a note or chord per step, Hold for a rest, keys + Hold for a tie. Back or Record ends it |
| Record while playing | **Live recording** into the selected track: each note goes to the nearest step and sounds as you play it |
| Record, then Play | A bar of **count-in** on the display and the lights, then live recording |

- Recording **replaces** the track unless Overdub is on (Shift + Record). The track is only
  cleared when you play the first note, so a stray press of Record costs nothing.
- A track first recorded live takes the length of track 1, or 16 steps.
- **Rec Quantize** (global settings, on by default): on, a live note lands on the grid; off, it
  keeps the moment you played it.
- **Quant** (Shift + Quant) puts every step of the track on the grid. Track settings has Quantize
  50 % for half way.
- **Rec Autom** (on by default): the Bend strip, the Mod strip, aftertouch and the pedal are
  recorded into the steps while you record live. Clear Autom in track settings takes them off
  again, one kind or all.

## Editing steps

Hold **Seq** and turn the encoder to open the **step page** of the selected track.

```oled
T1 Step 7                past end
C4 E4 G4
[#][ ][#][#]| .  .  # .  .  .  .
```

- **Turn** to move along the steps. You can go on past the end of the track, up to step 64.
- **Press a key** to put its note on the step. Hold several keys for a chord. You hear it on the
  track's channel, and the page stays on the step so you can try another.
- **Hold** makes the step a rest. Shift + Undo brings it back.
- **Click** opens the step's fields; **Back** returns.
- With **Seq held**: a numbered key jumps to that step of the 16 shown, the encoder moves 16
  steps at a time, and a bank key selects the track.

**Steps past the end.** A track plays steps 1 to its length. The steps after that keep whatever
you put there and are silent until the length reaches them. They are drawn as dots, or a small
block when they hold a note, with a bar after the last step that plays. Entering a note there
never changes the length: Shift + Tempo does that. This lets you prepare the second half of a
phrase and bring it in by lengthening the track.

**A step's fields:**

| Field | Values |
|---|---|
| Velocity | 1–127, for every note of the step |
| Gate | 5–100 % of the step, or `Track` to use the track's gate |
| Probability | 0–100 % |
| Ratchet | 1–4 evenly spaced hits |
| Condition | Always, 1 of 2, 2 of 2, 1 of 3, 1 of 4, First, Not first (counted in passes of the track) |
| Tie | The notes run into the next step |
| Offset | −50 to +50 % of a step: early or late |

Keys played while you are inside a step replace its notes.

### Mutate

**Mutate** makes a variation of the selected track: some steps change places with a neighbour,
some play the notes of another step, a few jump an octave. It keeps the same number of notes and
their velocities. Each press is a new variation of the original, not of the last one.
**Shift + Mutate** brings the original back. The Mutate light is on while the track is a
variation. Saving keeps the variation as the pattern.

## Patterns

| To | Do |
|---|---|
| Choose a bank | Shift + bank key |
| Choose a pattern | Shift + numbered key. It takes over when track 1 next wraps (or at once: see Pattern change) |
| Save | Shift + Seq, then click. A numbered key picks another slot first; Back cancels |
| Reload the saved pattern | Shift + Reload |
| Chain patterns | Shift + Play |

**Chains.** On the chain page, numbered keys add patterns of the current bank (up to 32), Hold
removes the last, Shift + Clear empties it, and turning switches the chain on or off. With a
chain on, Play starts on its first pattern and each hands over to the next when track 1 wraps.
Picking a pattern by hand switches the chain off.

**Tempo.** Hold Tempo and turn, 1 BPM a click, or tap Tempo two to four times. Each pattern
stores its tempo; *Tempo on change* in global settings decides whether a pattern brings its tempo
with it.

## Arpeggiator

| To | Do |
|---|---|
| Switch it on or off | Tap Arp |
| Choose the mode | Arp + numbered key 1–6: Up, Down, UpDown, Random, Order, Poly |
| Choose 1–4 octaves | Arp + bank key |
| Latch | Hold. New keys replace the latched ones |
| Open its settings | Arp + encoder click |

The arp plays on the channel the keys go to and stays in step with the tracks.

**Arp settings:** Arp Length (1–64 steps: the pattern starts again after that many), Arp
Retrigger (off lets it run on), Mode, Octaves, Time Division, Gate, Swing.

## Chords

| To | Do |
|---|---|
| Switch chord mode on or off | Tap Chord |
| Choose a preset | Chord + turn |
| Teach it your own chord | Hold Chord and play the keys; let go |
| Open the chord page | Chord + encoder click |

Presets: User, Oct, 5th, Maj, min, sus2, sus4, Maj7, min7, Maj9, min9, Maj11, min11.

**Chord page:** Preset, Strum (0–80 ms from one note to the next), Strum Type (Up, Alt Up, Down,
Alt Down, Random) and Voicing. Voicing `On` keeps a run of chords in one register: notes of each
new chord move an octave towards where the last chord sounded. `Bass −1`, `−2` or `−3` adds the
root that many octaves down.

Chords are recorded whole, and with the arp on, the arp runs over the chord of each key.

## Scales

**Shift + Chord** opens the scale page. With a scale on, a key outside it plays the nearest note
of the scale (the lower one when two are as near). This applies to everything the keys do: notes,
chords, the arp and recording.

| Setting | What it is |
|---|---|
| Global Scale, Global Root | Used by every pattern that does not have its own |
| Pattern Scale | `Global`, a scale of its own, `User`, or `Off`. Saved with the pattern |
| Pattern Root | The root of the pattern's own scale |

**User** is a scale you make. Open Pattern Scale for editing, turn to `User`, and press keys: each
key puts its note in or takes it out, in any octave. The display shows the twelve notes from the
root as cells, filled when they are in. The root always is.

Scales: Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Harmonic Minor, Blues,
Pentatonic Major, Pentatonic Minor, Japanese, Gypsy, Arabic, Freygish.

## Strips, pedal and aftertouch

| Control | Sends |
|---|---|
| Bend strip | Pitch bend. It springs back to the centre when you let go |
| Mod strip | Mod wheel (CC 1). It stays where you leave it |
| Pedal | Sustain (CC 64) |
| Aftertouch | Channel pressure. Aft Sensitivity in global settings: Off, Low, Medium, High |
| Shift + Bend strip | **Gate** of the selected track, short at the bottom, long at the top. Sends nothing |
| Shift + Mod strip | **Spice** of the selected track. Sends nothing |
| Shift + three quick taps on the Mod strip | **Dice** |

**Spice** gives every step of the track its own change of velocity and gate length: a little at
the bottom of the strip, a lot at the top. The change is the same every time round, so it is a
variation you can keep, not noise. **Dice** deals new changes. Spice is also in track settings.

**Shift + Hold** is Transpose Hold: the keys transpose the selected track instead of playing.

## Sync and MIDI

**Clock Receive** (global settings) chooses what the tempo follows: Auto, Internal, USB, MIDI Din
or Sync In. Auto uses its own clock until a clock arrives on USB or MIDI, follows that, and goes
back to its own two seconds after it stops. MK2000 always sends MIDI clock and the sync output,
so it passes the tempo on.

**MIDI Thru** passes what arrives on the MIDI input to the MIDI output and USB. **USB to MIDI**
passes what arrives on USB to the MIDI output.

## CV outputs

Whatever plays on **track 1's channel**, from the track, the keys or the arp, also plays on the
pitch and gate outputs. Global settings → **CV Settings**:

| Setting | Values |
|---|---|
| Pitch Format | VoltOct, HertzVolt |
| Pitch Ref | C-2 to C3 |
| Pitch Bend Range | 1–24 semitones |
| Gate Format | S-trig, V-trig 5V, V-trig 12V |
| Gate Retrig | Mono, Legato, Legato Slide |
| Mod 1 Source, Mod 2 Source | Velocity, Aftertouch, ModWheel, Random, Pulse |
| Mod 1 Max Volt, Mod 2 Max Volt | 1–10 V |
| Wheel Vibrato | 0–12 semitones: how far the Mod strip wobbles the pitch output. CV only |
| Vibrato Rate | 0.3–10 Hz |

The outputs use your unit's factory calibration, read from where Arturia's firmware keeps it.
About (in global settings) says `CV: calibrated` when it was found.

## Global settings

Click the encoder with nothing held.

| Setting | Values |
|---|---|
| Brightness | 1–10, of the button lights |
| Pattern change | At the wrap, At once |
| Tempo on change | When stopped, Always, Never |
| Overdub | Off, On |
| Rec Quantize | On, Off |
| Rec Autom | On, Off |
| Keys | Track, Home |
| Home channel | 1–16 |
| Aft Sensitivity | Off, Low, Medium, High |
| Clock Receive | Auto, Internal, USB, MIDI Din, Sync In |
| MIDI Thru | Off, On |
| USB to MIDI | Off, On |
| CV Settings | A page of its own |
| About | Version and free memory |

Settings are written a moment after a change, while stopped.

## Every gesture

| Gesture | Does |
|---|---|
| Play, Stop | Start from the top, stop |
| Seq tap | Tracks on or off |
| Seq + bank key | Select track |
| Seq + numbered key 1–4 | Mute track |
| Seq + numbered key 5–8 | Solo track while held |
| Seq + encoder turn | Step page |
| Seq + encoder click | Track settings |
| Arp tap | Arp on or off |
| Arp + numbered key 1–6 | Arp mode |
| Arp + bank key | Arp octaves |
| Arp + encoder click | Arp settings |
| Chord tap | Chord mode on or off |
| Chord + keys | Learn the user chord |
| Chord + encoder turn | Chord preset |
| Chord + encoder click | Chord page |
| Mutate | New variation of the track |
| Shift + Mutate | The original back |
| Shift + Chord | Scale page |
| Shift + Stop tap | Keys: Track or Home |
| Shift + Stop + numbered key | Home channel |
| Shift + Stop + Seq + numbered key | Selected track's channel |
| Shift + Stop + bank key, numbered key | That track's channel |
| Shift + bank key | Bank |
| Shift + numbered key | Pattern |
| Shift + Seq | Save |
| Shift + Play | Chain page |
| Shift + Tempo | Track length |
| Shift + edit key | Time division, Nudge, Octave, Double, Quant, Reload, Clear, Undo |
| Shift + Record | Overdub on or off |
| Shift + Hold | Transpose Hold |
| Tempo + turn, Tempo taps | Tempo |
| Record | Step recording (stopped) or live recording (playing) |
| Record, then Play | Count-in, then live recording |
| Hold | Latch; a rest in step recording and on the step page |
| Encoder click | Global settings |
| Back | Leave a page, end recording |

## Lights

| Light | Meaning |
|---|---|
| Seq | The selected track's colour; bright when tracks play, dim when off |
| Arp, Chord | On when that mode is on |
| Mutate | On while the selected track is a variation |
| Play | Blinks on the beat while running |
| Stop | Dim while stopped |
| Record | Red while recording, blinking during the count-in, dim when Overdub is on |
| Hold | On when latched, blinking in Transpose Hold |
| Oct − / + | Brighter per octave |
| Encoder | On while a page is open |

## Memory, and going back to Arturia's firmware

MK2000 keeps its patterns in a part of the unit's memory that Arturia's firmware never uses, and
never touches Arturia's patterns, settings or calibration. You can switch between the two
firmwares as often as you like: each finds its own patterns as it left them. Roughly 16 to 30
MK2000 patterns fit, depending on how full they are; About shows the free memory.
