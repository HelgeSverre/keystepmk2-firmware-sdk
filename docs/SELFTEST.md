# Self-test

`sdk/examples/selftest` is a firmware that exercises every part of the SDK on a unit. It shows
OK, FAIL or SKIP per check on the display, tells the person at the unit what to do, and sends
every result with its measured numbers to the computer over USB MIDI, where `tools/selftest.py`
prints and saves them.

**Status: emulator only.** `tools/selftest_emu.py` runs it from start to summary in the
emulator. It has not run on a unit, and passing in the emulator says nothing about the
hardware: finding out is what this firmware is for.

## Run it on a unit

```
just flash selftest
uv run --with mido --with python-rtmidi tools/selftest.py receive --port "KeyStep mk2" -o report.json
```

Start the second command right after the first; either order works. The tool waits up to a
minute for the port, then asks the unit to send again whatever it reported before the tool was
listening. It prints each result as it arrives, ends with a table when the run is over, and
writes `report.json`. Ctrl-C ends it early and still prints and saves what came.

A whole run takes about five minutes. Nothing needs to be plugged in besides USB; three
optional checks want a pedal, a MIDI cable and a voltmeter.

Before flashing: the automatic part puts eight pulses on the sync output and the CV check puts
voltages on the CV and gate sockets, so unplug gear that would mind.

To go back to another firmware, flash it the usual way ([FLASHING.md](FLASHING.md)); the
self-test honours the update request like every SDK firmware.

## What the person does

1. **Wait.** The automatic part takes about four seconds (seven if no computer is found) and
   ends on a screen like `Auto: OK 9  FAIL 0 / SKIP 0  INFO 2`.
2. **Press Play** to start the guided part.
3. **Do what each screen says.** The top line is the check's number and name and a countdown
   in seconds. After each check the display shows OK, FAIL or SKIP large, with a reason below a
   FAIL.
4. At the end the screen reads `Done: OK … FAIL … / SKIP … INFO …`.

The answers are the same everywhere:

| Control | Means |
|---|---|
| Play | Yes, it is right. In an optional check: yes, test this. |
| Stop | No, it is wrong (FAIL). In an optional check's first question: skip it. In a check that waits for you to play or turn something: give up (FAIL). |
| nothing for 15 s | The check ends by itself: FAIL, or SKIP for an optional check's question. |

On both summary screens (after the automatic part and at the end):

| Control | Does |
|---|---|
| Knob | Lists the results one by one: number, name and status, then the reason, then the numbers. Turn back to the left for the totals. |
| Record | Sends the whole report again. |
| Play | Goes on: to the guided part, or, at the end, to a whole new run. |

If someone has to read the display to you, the totals line and, for each FAIL, the three lines
the knob shows for it are everything there is.

## The checks

The number is the check's id in the report and on the display. *auto* needs no hands, *guided*
needs the person, *optional* begins with a question and is skipped with Stop.

### Automatic

| # | Name | What it does | OK when |
|---|---|---|---|
| 1 | `usb.ready` | Waits up to 3 s for a computer to configure the device. | A host did. On FAIL the report goes out of the MIDI Out socket instead. |
| 2 | `time.millis` | Counts 500 ms on `ks_millis` and measures them with `ks_micros`; measures `ks_delay_ms(100)`; compares `ks_micros16` with `ks_micros`. | 499–501 ms, 98.5–100.5 ms, and the two counters under 0.5 ms apart. |
| 3 | `time.timer` | `ks_timer_start` at 1000 µs for 200 ms, `ks_timer_period` to 250 µs for 100 ms, then `ks_timer_stop`; counts the callback's ticks. | Each count fits its measured time within three periods, and no tick after the stop. |
| 4 | `flash.id` | Reads the chip's JEDEC id. | `C8 40 13` or `1F 84 01`, the two the stock firmware accepts. |
| 5 | `flash.read` | Reads the SDK's area (31 sectors) and counts the blank ones; reads the settings sector and the first two sectors of the stock file system twice. | The two reads agree and the name `littlefs` is where the stock firmware's file system has it. INFO if there is no such name (a chip the stock firmware never ran on): the read cannot prove itself then. FAIL if the two reads differ. |
| 6 | `flash.write` | In the highest sector of the SDK's area that reads entirely `FF`: writes 300 bytes across two page ends, reads them back, erases the sector, reads it blank. | All of that. SKIP with no blank sector or an unknown chip. |
| 7 | `flash.store` | In the same sector: `ks_store_save`, `ks_store_load`, erase, and a load that must now find nothing. | All of that. SKIP unless check 6 passed. |
| 8 | `sync.loop` | Eight 5 ms pulses on the sync clock output and one on the run line, with nothing in the sync socket. Counts them with `ks_sync_pulses`, the `ks_sync_clock_in` hook and `ks_sync_in`. | All eight clock pulses arrive each way. SKIP if none arrives at all; FAIL if some do. The run line is reported (`run_followed`) and not judged. |
| 9 | `aftertouch.rest` | 250 readings of `ks_aftertouch` over a second with nothing pressed. | Never OK: INFO with the numbers. FAIL for a value outside 0–4095, or if the converter did not run meanwhile. |
| 10 | `pedal.rest` | The same for `ks_pedal`. | As 9. |
| 11 | `touch.rest` | Asks the touch controller for its register 4, then reads both strips for half a second. | The controller answers, and both strips say "no finger" throughout. |

Flash safety: checks 6 and 7 are the only writes in the firmware. They go to one sector between
`0x41000` and `0x5FFFF` that was just read as blank, and that sector is erased and read back
before the check ends. The stock firmware's areas are only read. The SDK's own guard against
writes outside its area is not put to the test, because testing it means asking for such a
write.

What the automatic part cannot know: check 8 passing with nothing plugged in rests on the
emulator's model, in which an empty sync socket reads the unit's own output
([MK2000.md](MK2000.md), Sync and routing). A SKIP there on a real unit means that model is
wrong for the hardware, or that something is in the socket. Check 11 catches a phantom touch,
but a touch controller that never answers also reads "no finger"; checks 33 and 34 settle that.

### Guided

| # | Name | The screen says | You do | FAIL when |
|---|---|---|---|---|
| 12 | `display` | A test pattern (below). | Play if it matches, Stop if anything is off. | Stop or no answer. |
| 13–16 | `leds.red`, `.green`, `.blue`, `.white` | `Red: Seq`, `Red: Arp`, … then `Red: all thirteen` | Watch: the one light named must be lit, alone, in that colour; then all thirteen. Then Play or Stop. The sweep repeats until you answer; the answer counts once every light has had its turn. | Stop or no answer. |
| 17–30 | `button.seq`, `.arp`, `.mutate`, `.chord`, `.tempo`, `.record`, `.stop`, `.play`, `.shift`, `.hold`, `.oct-`, `.oct+`, `.back`, `.encoder` | `Press Seq` … `Press the knob` | Press that one. Its own light is lit dim white while it waits (Back has none). | Another button went down first (`saw Arp`), several together, or none. |
| 31 | `encoder` | `Knob: 4 clicks right`, then `Knob: 4 clicks left` | Turn it. | It counts the other way, or not far enough before the time is up. |
| 32 | `keys` | `Play key 1 of 32` | Every key once, lowest to highest, one at a time. Vary how hard you strike. The line below shows the key and velocity just read. | A key never fired, never came up, or fired out of turn. After a slip the check carries on from the key that fired. |
| 33, 34 | `strip.bend`, `strip.mod` | `Slide the Bend strip` | One slide from the bottom to the top, then lift. A slide that does not qualify shows `again`. | It never reached within a fifth of both ends, it read top to bottom, or the other strip answered. |
| 35 | `aftertouch` | `Hold a key, press hard` | Hold any key down and lean on it, then let go. | The level never moved 200 counts from its rest level (or four times the rest level's noise, if that is more). |

The display pattern: a frame one pixel wide on the outermost pixels all the way round; the
digits `1` top left, `2` top right, `3` bottom left, `4` bottom right, in the SDK's own font;
beside them two dotted vertical lines with every other row lit, which show a missing or doubled
row as a gap or a blob; `Play=ok Stop=bad` in the small stock font; and `Display` in the large
stock font, dark on a lit bar. Wrong orientation moves the digits, an offset cuts the frame.

### Optional

Each starts with a question: Play to test, Stop (or 15 s) to SKIP.

| # | Name | Question | Then | Result |
|---|---|---|---|---|
| 36 | `pedal` | `Pedal plugged in?` | Press the pedal down and let go. | OK if the level moved by 500 counts; FAIL if not. |
| 37 | `midi.din` | `MIDI cable Out > In?` | Nothing: 16 bytes go out, two of them clock bytes for the `ks_midi_realtime` hook. | OK if all 16 come back in order; SKIP if nothing comes back; FAIL if something else does. |
| 38 | `cv` | `Meter on CV and gate?` | Eight steps, Play for the next. Each shows what is on the outputs, such as `Pitch 32768 Mods 2048` / `Gate 1: off` (gate format 1, gate off). Then `Did the outputs follow?` | OK only if you answer Play at the end; Stop at any step is FAIL; no answer is SKIP. |
| 39 | `usb.in` | none | Nothing: `selftest.py receive` pings the unit every two seconds. | OK if a ping arrived during the run; SKIP if none did (no tool listening). |

The CV steps, as raw values (what voltage each gives is exactly what is not known yet, so write
down what the meter shows at each step):

| Step | Pitch (0–65535) | Mod 1 and Mod 2 (0–4095) | Gate |
|---|---|---|---|
| 1 | 0 | 0 | format 1, off |
| 2 | 32768 | 2048 | format 1, off |
| 3 | 65535 | 4095 | format 1, off |
| 4 | 32768 | 2048 | format 1, on |
| 5 | 32768 | 2048 | format 0, off |
| 6 | 32768 | 2048 | format 0, on |
| 7 | 32768 | 2048 | format 2, off |
| 8 | 32768 | 2048 | format 2, on |

## Statuses

| Status | Means |
|---|---|
| OK | The check could have caught a fault and did not. |
| FAIL | It caught one, the person said so, or a required check got no answer. |
| SKIP | Not tested: an optional check declined, a precondition missing, or no signal at all where none may be normal. |
| INFO | Measured, with no verdict, because nobody knows the right value yet. |

## The numbers

`selftest.py` prints them by name; `report.json` has them under `values`, and as sent under
`raw`.

| Check | Values |
|---|---|
| `usb.ready` | `ready` 0 or 1; `waited_ms` until a host appeared. |
| `time.millis` | `millis_ms` 500; `micros_us` how long that was by the microsecond counter; `delay_100ms_us`; `counters_apart_us`. Both counters run from one crystal, so this shows a wrong divider, not a wrong crystal: see Clock below. |
| `time.timer` | `ticks_at_1000us` in `window_1000_us`, `ticks_at_250us` in `window_250_us`, `ticks_after_stop`. |
| `flash.id` | `jedec` in hex. |
| `flash.read` | `sdk_sectors_blank` of 31 (MK2000 keeps a pattern or its settings in each used one); `settings_bytes_used` in `0x40000`–`0x40FFF`; `fs_bytes_used` in `0x60000`–`0x61FFF`; `littlefs` 1 if the name was found; `reads_differ`. |
| `flash.write` | `sector` address; `write_result`, `erase_result` (0, or a `KS_FLASH_` code: −2 is a timeout); `bytes_wrong` on read-back; `bytes_left` not blank after the erase; `write_ms`, `erase_ms`. The erase time is worth having: the SDK gives an erase 500 ms. |
| `flash.store` | `slot`; `save_result`; `load_result` (64, the record's length); `bytes_wrong`; `erase_result`; `bytes_left`; `load_after_erase` (−4, empty). |
| `sync.loop` | `sent` 8; `counted` by `ks_sync_pulses`; `hook_count` by `ks_sync_clock_in`; `levels_followed`, pulses during which `ks_sync_in` read the level put out; `run_followed` 0 or 1; `idle_levels` before the first pulse (bit 0 clock, bit 1 run; 0 expected). |
| `aftertouch.rest`, `pedal.rest` | `min`, `max`, `mean` of `samples` readings, 0–4095; `conversions`: how many the converter finished meanwhile. |
| `touch.rest` | `mod`, `bend`: the last readings, −1 for no finger; `controller_answers`: 1 or 0. |
| `display` | `answer`: 1 Play, 0 Stop, −1 none. |
| `leds.*` | `colour` sent, `0xRRGGBB`. |
| `button.*` | `expected` and `seen` as `KS_BTN_` bits; `held_before`, buttons already down when the check began (a stuck one shows here). |
| `encoder` | `right`, `left`: detents counted in each half, clockwise positive, so 4 and −4 when right. |
| `keys` | `fired` and `released`: one bit per key, bit 0 the lowest; `misordered`; `velocity_min`, `velocity_max` over every strike (1–127); `repeats`, strikes of the key just struck (bounce, or a second try). |
| `strip.*` | `min`, `max`, `first`, `last` of the last slide, 0 (bottom) to 255 (top), −1 if never touched; `released`; `other_strip_touched`. |
| `aftertouch` | `rest` from check 9; `peak`, the reading farthest from it; `direction` +1 if pressing raises the level, −1 if it lowers it; `min`, `max` while a key was down; `threshold`. |
| `pedal` | `rest` from check 10 (pedal unplugged, usually); `start` when the check began; `min`, `max`. |
| `midi.din` | `sent` 16; `received`; `matching`; `clocks_heard` by the hook (2). |
| `cv` | `steps_shown`; `answer` as for the display. |
| `usb.in` | `host_messages`: pings and replay requests heard. |

### Clock

While it listens, `selftest.py` pings the unit every two seconds and gets the unit's time back.
From the first and last ping that came straight back it prints how long the unit and the
computer each think the run took, and the difference in parts per million; `report.json` has it
under `clock`. This is the only measurement of the unit's clock against anything outside the
unit. It is good to about 100 ppm over a minute, better over a longer run.

## The report format

One SysEx message per event, `F0 7D 'K' 'S' <type> <body> F7`, described at the top of
`sdk/examples/selftest/main.c`: a start message with the firmware's version, one message per
result (check id, status, the time, the values, a short text) and an end message with the
totals. Every body carries a CRC-16. Results are numbered, so a report sent twice (Record, or
the tool's request) is not counted twice, and a missing message is noticed.

To keep a raw copy, record the port with any MIDI monitor that saves SysEx and decode it later:

```
uv run tools/selftest.py decode capture.syx -o report.json
```

With no computer on USB, the same messages come out of the MIDI Out socket, where an interface
can record them.

## In the emulator

```
just build selftest
uv run --with unicorn tools/selftest_emu.py
```

`selftest_emu.py` reads the instructions off the emulated display and does what they say. Three
parts: everything done right (every check OK, the two rest levels INFO); a second run on the
same board with mistakes and faults (wrong button, no button, two buttons, knob turned the
wrong way, a dead key, a strip slid the wrong way, the wrong strip, no pressure, no MIDI cable,
no blank flash sector, a plugged sync socket), which must give exactly the expected FAILs and
SKIPs; and a start with no USB host, where the report must come out of the DIN socket.

`just host selftest` opens it in the KeyStep window to try by hand.
