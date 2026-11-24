/* seq4: a four-track step sequencer on the DIN output and USB.
 *
 * Four tracks on MIDI channels 1 to 4, up to 16 steps each, up to four notes per step. All
 * tracks run together, each over its own length, with MIDI clock (24 per quarter note), Start
 * and Stop. Track 1 also plays on the pitch CV and gate outputs, and the sync output carries one
 * pulse per step and the run line.
 *
 *   Seq + key 1-4       pick the track (the "bank" keys)
 *   Seq + key 5-20      move the cursor to that step (the "pattern" keys)
 *   Shift + key 1-4     mute or unmute a track
 *   Shift + key 5-20    set the track's length to that step
 *   Record              recording on or off
 *     stopped: keys     write the held keys into the cursor step, advance on release
 *     stopped: Hold     write a rest and advance
 *     stopped: Back     step back and clear that step
 *     playing: keys     add the note to the nearest step as it passes
 *   Shift + Record      clear the track
 *   Shift + Seq         save all tracks, tempo and mutes to the flash (stops playback)
 *   Play, Stop          start from step 1, stop
 *   Tempo + encoder     tempo, 30 to 240 BPM
 *   encoder             move the cursor
 *   Oct -, Oct +        shift the keys by octaves
 *
 * The saved state is loaded at power-on; without one, a demo pattern is.
 *
 * The display shows everything. On the 37-key model the lights above keys 1-4 also show the
 * tracks and those above keys 5-20 the steps of the current track; the 32-key model has none. */
#include <stddef.h>
#include "keystep.h"

void *memcpy(void *to, const void *from, size_t n);

#define TRACKS 4
#define STEPS 16
#define POLY 4
#define FIRST_STEP_KEY 4            /* keys 4 to 19 stand for steps 1 to 16 */
#define TICKS_PER_STEP 6            /* 24 clocks per quarter note, four steps per quarter */
#define GATE_TICKS 3

typedef struct { uint8_t note[POLY], velocity[POLY]; } step_t;

static step_t pattern[TRACKS][STEPS];
static uint8_t sounding[TRACKS][POLY];      /* notes the sequencer has switched on */
static uint8_t key_note[KS_KEYS];           /* the note each held key was sent as */
static uint8_t key_track[KS_KEYS];          /* and the track it was sent on */
static uint8_t muted;
static uint8_t length[TRACKS] = { STEPS, STEPS, STEPS, STEPS };
static uint8_t pos[TRACKS];                 /* the step each track is on */
static uint8_t cv_keys, cv_seq;             /* what holds the gate up: keys on track 1, the sequencer */
static int track, cursor, octave, bpm = 120;
static int playing, recording, tick;
static int chord_keys;                      /* keys held in the current recording gesture */
static uint32_t acc_us;                     /* time owed to the clock */
static const char *message;                 /* shown in place of the notes until message_until */
static uint32_t message_until;

#define SAVE_SLOT 0
#define SAVE_VERSION 2
static struct { uint8_t version, bpm, muted, reserved, length[TRACKS]; step_t pattern[TRACKS][STEPS]; } saved;

static const uint32_t track_colour[TRACKS] = { 0x00402000, 0x00003040, 0x00300040, 0x00104000 };

/* Everything goes out on the DIN socket and over USB. */
static void out(uint8_t status, uint8_t d1, uint8_t d2) {
    ks_midi_send(status, d1, d2);
    ks_usb_midi_send(status, d1, d2);
}

static void out_byte(uint8_t byte) {
    ks_midi_send_byte(byte);
    ks_usb_midi_send_byte(byte);
}

/* Track 1 on the CV outputs. The pitch scale is the one the stock firmware puts out with a blank
 * calibration: 65536 counts for ten octaves, zero at note 36. */
static void cv_note(uint8_t note) {
    int value = (note - 36) * 65536 / 120;
    ks_cv_pitch((uint16_t)(value < 0 ? 0 : value > 65535 ? 65535 : value));
}

static void cv_gate(void) { ks_gate(cv_keys || cv_seq); }

static int step_used(int t, int s) {
    for (int i = 0; i < POLY; i++)
        if (pattern[t][s].note[i]) return 1;
    return 0;
}

static void step_clear(int t, int s) {
    for (int i = 0; i < POLY; i++) pattern[t][s].note[i] = 0;
}

static void step_add(int t, int s, uint8_t note, uint8_t velocity) {
    step_t *st = &pattern[t][s];
    for (int i = 0; i < POLY; i++) {
        if (st->note[i] == note) return;
        if (!st->note[i]) {
            st->note[i] = note;
            st->velocity[i] = velocity;
            return;
        }
    }
}

static void notes_off(int t) {
    for (int i = 0; i < POLY; i++) {
        if (sounding[t][i]) out((uint8_t)(0x80 | t), sounding[t][i], 0);
        sounding[t][i] = 0;
    }
    if (t == 0) {
        cv_seq = 0;
        cv_gate();
    }
}

static void play_current_step(void) {
    for (int t = 0; t < TRACKS; t++) {
        notes_off(t);
        if (muted & (1 << t)) continue;
        const step_t *st = &pattern[t][pos[t]];
        for (int i = 0; i < POLY; i++) {
            if (!st->note[i]) continue;
            out((uint8_t)(0x90 | t), st->note[i], st->velocity[i]);
            sounding[t][i] = st->note[i];
            if (t == 0 && !cv_seq) {
                cv_note(st->note[i]);
                cv_seq = 1;
                cv_gate();
            }
        }
    }
}

/* One MIDI clock. Returns 1 when the step changed. */
static int clock_tick(void) {
    int stepped = 0;
    out_byte(0xF8);
    if (tick == 0) {
        play_current_step();
        ks_sync_out(KS_SYNC_CLOCK, 1);
        stepped = 1;
    } else if (tick == GATE_TICKS) {
        for (int t = 0; t < TRACKS; t++) notes_off(t);
        ks_sync_out(KS_SYNC_CLOCK, 0);
    }
    if (++tick == TICKS_PER_STEP) {
        tick = 0;
        for (int t = 0; t < TRACKS; t++) pos[t] = (uint8_t)((pos[t] + 1) % length[t]);
    }
    return stepped;
}

static void start(void) {
    for (int t = 0; t < TRACKS; t++) {
        notes_off(t);
        pos[t] = 0;
    }
    tick = 0;
    playing = 1;
    acc_us = 2500000u / (uint32_t)bpm;          /* the first step sounds at once */
    out_byte(0xFA);
    ks_sync_out(KS_SYNC_RUN, 1);
}

static void stop(void) {
    for (int t = 0; t < TRACKS; t++) notes_off(t);
    if (playing) out_byte(0xFC);
    playing = 0;
    for (int t = 0; t < TRACKS; t++) pos[t] = 0;
    tick = 0;
    ks_sync_out(KS_SYNC_RUN, 0);
    ks_sync_out(KS_SYNC_CLOCK, 0);
}

static void demo_pattern(void) {
    static const uint8_t bass[STEPS] = { 41, 0, 0, 41, 0, 0, 48, 0, 44, 0, 0, 44, 0, 0, 51, 0 };
    static const uint8_t lead[STEPS] = { 65, 0, 68, 0, 72, 0, 68, 0, 63, 0, 68, 0, 72, 0, 75, 0 };
    for (int s = 0; s < STEPS; s++) {
        if (bass[s]) step_add(0, s, bass[s], 100);
        if (lead[s]) step_add(1, s, lead[s], 80);
    }
    step_add(2, 0, 60, 70);
    step_add(2, 0, 63, 70);
    step_add(2, 0, 68, 70);
    step_add(2, 8, 60, 70);
    step_add(2, 8, 63, 70);
    step_add(2, 8, 67, 70);
}

static void stop(void);

static void save(void) {
    stop();                                         /* the erase takes too long to keep time */
    saved.version = SAVE_VERSION;
    saved.bpm = (uint8_t)bpm;
    saved.muted = muted;
    memcpy(saved.length, length, sizeof length);
    memcpy(saved.pattern, pattern, sizeof pattern);
    message = ks_store_save(SAVE_SLOT, &saved, sizeof saved) ? "save failed" : "saved";
    message_until = ks_millis() + 1200;
}

static int load(void) {
    if (ks_store_load(SAVE_SLOT, &saved, sizeof saved) != (int)sizeof saved) return 0;
    if (saved.version != SAVE_VERSION || saved.bpm < 30 || saved.bpm > 240) return 0;
    for (int t = 0; t < TRACKS; t++)
        if (saved.length[t] < 1 || saved.length[t] > STEPS) return 0;
    memcpy(length, saved.length, sizeof length);
    bpm = saved.bpm;
    muted = saved.muted;
    memcpy(pattern, saved.pattern, sizeof pattern);
    return 1;
}

static char *note_name(uint8_t note, char *out) {
    static const char names[] = "C C#D D#E F F#G G#A A#B ";
    char *p = out;
    *p++ = names[note % 12 * 2];
    if (names[note % 12 * 2 + 1] != ' ') *p++ = '#';
    int oct = note / 12 - 1;
    if (oct < 0) {
        *p++ = '-';
        oct = -oct;
    }
    *p++ = (char)('0' + oct);
    *p = 0;
    return out;
}

static void two_digits(int value, char *out) {
    out[0] = (char)('0' + value / 10);
    out[1] = (char)('0' + value % 10);
    out[2] = 0;
}

static void draw(void) {
    char text[8];
    ks_oled_clear();
    /* top line: track, tempo, state, cursor step */
    text[0] = 'T';
    text[1] = (char)('1' + track);
    text[2] = 0;
    ks_oled_text(0, 0, text);
    ks_oled_text(24, 0, ks_utoa((uint32_t)bpm, text));
    ks_oled_text(56, 0, recording ? "REC" : playing ? "RUN" : "---");
    ks_oled_text(88, 0, "st");
    two_digits(cursor + 1, text);
    ks_oled_text(108, 0, text);
    /* second line: the notes in the cursor step */
    const step_t *st = &pattern[track][cursor];
    int x = 0;
    if (message) {
        ks_oled_text(0, 10, message);
        x = 1;
    }
    for (int i = 0; i < POLY && !message; i++) {
        if (!st->note[i]) continue;
        ks_oled_text(x, 10, note_name(st->note[i], text));
        x += 32;
    }
    if (!x) ks_oled_text(0, 10, muted & (1 << track) ? "muted" : "rest");
    /* grid: one row per track, one cell per step */
    for (int t = 0; t < TRACKS; t++) {
        int y = 20 + 3 * t;
        for (int s = 0; s < length[t]; s++)
            if (step_used(t, s)) ks_oled_rect(s * 8 + 1, y, 6, 2, 1);
        if (t == track) ks_oled_rect(cursor * 8 + 3, y, 2, 2, !step_used(t, cursor));
        if (muted & (1 << t)) ks_oled_rect(0, y, 1, 2, 1);
        if (length[t] < STEPS) ks_oled_rect(length[t] * 8, y, 1, 2, 1);        /* where the track ends */
        if (playing) ks_oled_rect(pos[t] * 8, y, 1, 3, 1);
    }
    ks_oled_flush();
}

static void lights(uint32_t buttons) {
    for (int t = 0; t < TRACKS; t++) {
        uint32_t c = muted & (1 << t) ? 0x00200000 : track_colour[t];
        ks_led(KS_LED_KEY(t), t == track ? 0x00606060 : c);
    }
    for (int s = 0; s < STEPS; s++) {
        uint32_t c = s >= length[track] ? 0 : step_used(track, s) ? track_colour[track] : 0x00040404;
        if (s == cursor) c = recording ? 0x008D0000 : 0x00404040;
        if (playing && s == pos[track]) c = 0x00008D20;
        ks_led(KS_LED_KEY(FIRST_STEP_KEY + s), c);
    }
    ks_led(KS_LED_PLAY, playing ? 0x00008D00 : 0);
    ks_led(KS_LED_STOP, playing ? 0 : 0x00303030);
    ks_led(KS_LED_RECORD, recording ? 0x008D0000 : 0);
    ks_led(KS_LED_SEQ, buttons & KS_BTN_SEQ ? 0x008D4000 : 0x00201000);
    ks_led(KS_LED_SHIFT, buttons & KS_BTN_SHIFT ? 0x00404040 : 0);
    ks_led(KS_LED_TEMPO, buttons & KS_BTN_TEMPO ? 0x008D4000 : 0);
    ks_led(KS_LED_OCT_DOWN, octave < 0 ? 0x00404040 : 0);
    ks_led(KS_LED_OCT_UP, octave > 0 ? 0x00404040 : 0);
    ks_leds_show();
}

static int wrap(int value, int size) {
    value %= size;
    return value < 0 ? value + size : value;
}

int main(void) {
    uint32_t previous = 0, last_ms;
    int dirty = 1;
    ks_init();
    if (!load()) demo_pattern();
    last_ms = ks_millis();
    for (;;) {
        uint32_t buttons = ks_buttons(), pressed = buttons & ~previous;
        if (buttons != previous) dirty = 1;
        previous = buttons;

        if (pressed & KS_BTN_SEQ && buttons & KS_BTN_SHIFT) save();
        if (message && (int32_t)(ks_millis() - message_until) >= 0) {
            message = 0;
            dirty = 1;
        }
        if (pressed & KS_BTN_PLAY) start();
        if (pressed & KS_BTN_STOP) stop();
        if (pressed & KS_BTN_RECORD) {
            if (buttons & KS_BTN_SHIFT) {
                notes_off(track);
                for (int s = 0; s < STEPS; s++) step_clear(track, s);
            } else {
                recording = !recording;
                chord_keys = 0;
                if (cursor >= length[track]) cursor = length[track] - 1;
            }
        }
        if (pressed & KS_BTN_OCT_UP && octave < 2) octave++;
        if (pressed & KS_BTN_OCT_DOWN && octave > -2) octave--;
        if (recording && !playing && pressed & KS_BTN_HOLD) {
            step_clear(track, cursor);
            cursor = wrap(cursor + 1, length[track]);
        }
        if (recording && !playing && pressed & KS_BTN_BACK) {
            cursor = wrap(cursor - 1, length[track]);
            step_clear(track, cursor);
        }

        int turned = ks_encoder();
        if (turned) {
            if (buttons & KS_BTN_TEMPO) {
                bpm += turned;
                if (bpm < 30) bpm = 30;
                if (bpm > 240) bpm = 240;
            } else {
                cursor = wrap(cursor + turned, length[track]);
            }
            dirty = 1;
        }

        ks_key_event e;
        while (ks_key_get(&e)) {
            dirty = 1;
            if (buttons & (KS_BTN_SEQ | KS_BTN_SHIFT)) {        /* the key is a command */
                if (!e.on) continue;
                if (e.key < TRACKS) {
                    if (buttons & KS_BTN_SEQ) track = e.key;
                    else muted ^= (uint8_t)(1 << e.key);
                    if (muted & (1 << e.key)) notes_off(e.key);
                } else if (e.key < FIRST_STEP_KEY + STEPS && buttons & KS_BTN_SEQ) {
                    cursor = e.key - FIRST_STEP_KEY;
                    if (cursor >= length[track]) cursor = length[track] - 1;
                } else if (e.key < FIRST_STEP_KEY + STEPS) {             /* Shift: the track ends here */
                    length[track] = (uint8_t)(e.key - FIRST_STEP_KEY + 1);
                    if (cursor >= length[track]) cursor = length[track] - 1;
                    if (pos[track] >= length[track]) pos[track] = 0;
                }
                continue;
            }
            if (e.on) {
                uint8_t note = (uint8_t)(53 + e.key + 12 * octave);     /* key 0 is F3 */
                key_note[e.key] = note;
                key_track[e.key] = (uint8_t)track;
                out((uint8_t)(0x90 | track), note, e.velocity);
                if (track == 0) {
                    cv_note(note);
                    cv_keys++;
                    cv_gate();
                }
                if (recording && playing) {                             /* live: the nearest step */
                    int s = tick > TICKS_PER_STEP / 2 ? (pos[track] + 1) % length[track] : pos[track];
                    step_add(track, s, note, e.velocity);
                } else if (recording) {
                    if (!chord_keys++) step_clear(track, cursor);
                    step_add(track, cursor, note, e.velocity);
                }
            } else if (key_note[e.key]) {
                out((uint8_t)(0x80 | (key_track[e.key])), key_note[e.key], 0);
                key_note[e.key] = 0;
                if (key_track[e.key] == 0 && cv_keys) {
                    cv_keys--;
                    cv_gate();
                }
                if (recording && !playing && chord_keys && !--chord_keys) cursor = wrap(cursor + 1, length[track]);
            }
        }

        uint32_t now = ks_millis();
        if (playing) {
            uint32_t tick_us = 2500000u / (uint32_t)bpm;        /* 60 s / (bpm * 24) */
            acc_us += (now - last_ms) * 1000u;
            while (acc_us >= tick_us) {
                acc_us -= tick_us;
                dirty |= clock_tick();
            }
        } else {
            acc_us = 0;
        }
        last_ms = now;

        if (dirty) {
            dirty = 0;
            lights(buttons);
            draw();
        }
    }
}
