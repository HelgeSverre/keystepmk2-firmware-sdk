/* KeyStep MK2000: buttons, layers and what the keys do. The gestures are listed in docs/MK2000.md. */
#include "app.h"

void *memcpy(void *to, const void *from, size_t n);

#define TAP_MS 350
enum { KEY_FREE, KEY_NOTE, KEY_SOLO, KEY_ARP, KEY_LATCHED };

static uint32_t previous;
static uint32_t seq_at, stop_at;        /* when a button that may turn out to be a tap went down */
static uint32_t arp_at, chord_at;
static uint8_t arp_alone, chord_alone;
static uint8_t seq_alone, combo_used;   /* Seq went down by itself; something was done while a layer was held */
/* What each key that is down plays: one note, or in chord mode the chord, in the order it is
 * strummed. `key_sent` of them have been switched on; the rest follow, one every strum time. */
static uint8_t key_role[KS_KEYS], key_channel[KS_KEYS], key_velocity[KS_KEYS];
static uint8_t key_notes[KS_KEYS][MK_CHORD_NOTES], key_count[KS_KEYS], key_sent[KS_KEYS];
static uint32_t key_due[KS_KEYS];       /* when the next note of its chord is to start */
/* Two keys can hold the same pitch in chord mode: how many do, and where it sounds, so it ends
 * when the last of them lets go. The same for the pitches the keys have put into the arp. */
static uint8_t sounding[128], sounding_channel[128], in_arp[128];
static uint8_t learned[MK_CHORD_NOTES]; /* Chord held: the keys played into the user chord */
static uint8_t chord_keys, chord_tie;   /* step recording: keys of the chord still down; Hold was pressed with them */
static uint8_t rec_started;             /* this recording has taken its first note */
static uint8_t rec_replacing;           /* and it started the track over */
static uint32_t tempo_taps[4];          /* when Tempo went down, oldest first */
static uint8_t tempo_tapped;

static mk_track *track(void) { return &mk_engine_pattern()->track[app.track]; }

static void edited(void) {
    mk_engine_edited(app.track);
    app.dirty = 1;
    app.redraw = 1;
}

static void clear_steps(mk_track *t) {
    static mk_pattern blank;                            /* for the defaults of a step */
    if (!blank.tempo) mk_pattern_init(&blank);
    uint32_t state = app_lock();
    memcpy(t->step, blank.track[0].step, sizeof t->step);
    app_unlock(state);
}

static void step_add(mk_step *step, uint8_t note, uint8_t velocity) {
    for (int i = 0; i < MK_POLY; i++) {
        if (step->note[i] == note) return;
        if (!step->note[i]) {
            step->velocity[i] = velocity;
            step->note[i] = note;
            return;
        }
    }
}

static void step_empty(mk_step *step) {
    uint32_t state = app_lock();
    for (int i = 0; i < MK_POLY; i++) step->note[i] = 0;
    step->flags = 0;
    step->offset = 0;
    app_unlock(state);
}

/* ---- recording ---- */

static void record_stop(void) {
    if (app.recording == REC_STEP && rec_replacing && rec_started) {
        track()->length = app.rec_cursor ? app.rec_cursor : 1;
        edited();
    }
    app.recording = REC_OFF;
    app.redraw = 1;
}

static void record_start(void) {
    app.recording = mk_engine_playing() ? REC_LIVE : REC_STEP;
    app.rec_cursor = 0;
    rec_started = rec_replacing = chord_keys = chord_tie = 0;
    app.redraw = 1;
}

/* The first note of a recording: this is where a track is started over, not when Record is
 * pressed, so a stray press costs nothing. */
static void record_first_note(void) {
    mk_track *t = track();
    int was_used = mk_track_used(t);
    rec_started = 1;
    app_snapshot();
    if (app.settings.overdub && was_used) return;
    rec_replacing = 1;
    clear_steps(t);
    if (app.recording == REC_LIVE && !was_used) {       /* a new track loops with track 1 */
        const mk_track *first = &mk_engine_pattern()->track[0];
        t->length = app.track && mk_track_used(first) ? first->length : 16;
    } else if (app.recording == REC_STEP) {
        t->length = 1;
    }
    mk_engine_edited(app.track);
}

static void step_advance(void) {
    mk_track *t = track();
    if (chord_tie) t->step[app.rec_cursor].flags |= MK_STEP_TIE;
    chord_tie = 0;
    app.rec_cursor++;
    if (rec_replacing) {
        if (app.rec_cursor >= MK_STEPS) {
            record_stop();
            return;
        }
        t->length = app.rec_cursor;
    } else if (app.rec_cursor >= t->length) {
        app.rec_cursor = 0;
    }
    edited();
}

/* What one key played: a note, or its chord in chord mode. */
static void record_key(const uint8_t *notes, int n, uint8_t velocity) {
    if (app.recording == REC_LIVE && !mk_engine_playing()) return;     /* still counting in */
    if (!rec_started) record_first_note();
    if (app.recording == REC_LIVE) {
        for (int i = 0; i < n; i++) mk_engine_record(app.track, notes[i], velocity, !(app.settings.options & MK_OPT_REC_UNQUANTIZED));
        app.dirty = 1;
        return;
    }
    mk_step *step = &track()->step[app.rec_cursor];
    if (!chord_keys++) step_empty(step);                /* a new chord replaces what the step held */
    for (int i = 0; i < n; i++) step_add(step, notes[i], velocity);
    if (rec_replacing && track()->length <= app.rec_cursor) track()->length = (uint8_t)(app.rec_cursor + 1);
    edited();
}

static void record_rest(void) {
    if (!rec_started) record_first_note();
    step_empty(&track()->step[app.rec_cursor]);
    step_advance();
}

/* ---- the Seq Edit keys, on the selected track ---- */

static void set_division(int group, int variant) {
    if (group < 0 || group > 4) return;
    track()->division = (uint8_t)(group * 3 + variant);
    edited();
    app_message(mk_div_name[track()->division]);
}

static void nudge(int by) {
    mk_track *t = track();
    static mk_step carried;
    int n = t->length;
    if (n < 2) return;
    app_snapshot();
    uint32_t state = app_lock();
    if (by > 0) {
        carried = t->step[n - 1];
        for (int s = n - 1; s > 0; s--) t->step[s] = t->step[s - 1];
        t->step[0] = carried;
    } else {
        carried = t->step[0];
        for (int s = 0; s < n - 1; s++) t->step[s] = t->step[s + 1];
        t->step[n - 1] = carried;
    }
    app_unlock(state);
    edited();
    app_message(by > 0 ? "Nudge +1 step" : "Nudge -1 step");
}

/* Moves every step of the selected track towards the grid: all the way, or half way as the stock
 * firmware's 50 % does. Timing only, so nothing sounding needs cutting. */
void ui_quantize(int percent) {
    mk_track *t = track();
    app_snapshot();
    uint32_t state = app_lock();
    for (int s = 0; s < MK_STEPS; s++) t->step[s].offset = (int8_t)(percent >= 100 ? 0 : t->step[s].offset / 2);
    app_unlock(state);
    app.dirty = 1;
    app_message(percent >= 100 ? "Track Quantized" : "Quantized 50 %");
}

static void edit_key(int key) {
    mk_track *t = track();
    int group = t->division / 3, variant = t->division % 3;
    switch (key) {
    case 20: set_division(group - 1, variant); break;
    case 21: set_division(group + 1, variant); break;
    case 22: set_division(group, variant == 2 ? 1 : 2); break;
    case 23: nudge(-1); break;
    case 24: nudge(1); break;
    case 25:
    case 26: {
        int shift = t->shift + (key == 26 ? 12 : -12);
        if (shift < -48 || shift > 48) break;
        t->shift = (int8_t)shift;
        edited();
        app_message(key == 26 ? "Transpose +12 st" : "Transpose -12 st");
        break;
    }
    case 27:
        if (t->length * 2 > MK_STEPS) break;
        app_snapshot();
        memcpy(&t->step[t->length], &t->step[0], t->length * sizeof t->step[0]);
        t->length = (uint8_t)(t->length * 2);
        edited();
        app_message("Track Doubled");
        break;
    case 28: ui_quantize(100); break;
    case 29: app_reload(); break;
    case KEY_CLEAR:
        app_snapshot();
        clear_steps(t);
        edited();
        app_message("Track Cleared");
        break;
    case 31: app_undo(); break;
    default: break;
    }
}

/* ---- keys ---- */

static void set_track_channel(int which, int channel) {
    mk_engine_pattern()->track[which].channel = (uint8_t)channel;      /* sounding notes end on the old one */
    app.dirty = 1;
}

static uint8_t transpose_root(void) {
    const mk_track *t = track();
    for (int s = 0; s < t->length; s++)
        if (t->step[s].note[0]) return t->step[s].note[0];
    return 60;
}

static int keys_down_as(uint8_t role) {
    int n = 0;
    for (int key = 0; key < KS_KEYS; key++) n += key_role[key] == role;
    return n;
}

/* A pitch goes on for one more key. One already sounding is struck again. */
static void pitch_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (sounding[note]) app_note(sounding_channel[note], note, 0);
    sounding[note]++;
    sounding_channel[note] = channel;
    app_note(channel, note, velocity);
}

static void pitch_off(uint8_t note) {
    if (sounding[note] && !--sounding[note]) app_note(sounding_channel[note], note, 0);
}

/* The notes of a key's chord that are due. With no strum time that is all of them at once. */
static void strum(int key, uint32_t now) {
    while (key_sent[key] < key_count[key] && (int32_t)(now - key_due[key]) >= 0) {
        pitch_on(key_channel[key], key_notes[key][key_sent[key]++], key_velocity[key]);
        key_due[key] += app.settings.chord.strum;
    }
}

/* End what a key played. Notes of its chord not yet strummed never start. */
static void key_silence(int key) {
    for (int i = 0; i < key_sent[key]; i++) pitch_off(key_notes[key][i]);
    key_count[key] = key_sent[key] = 0;
}

/* Hold with the arp off: notes stay on after their keys are let go, until this. */
static void release_latched(void) {
    for (int key = 0; key < KS_KEYS; key++) {
        if (key_role[key] != KEY_LATCHED) continue;
        key_role[key] = KEY_FREE;
        key_silence(key);
    }
}

static void arp_forget(void) {
    mk_arp_clear();
    for (int note = 0; note < 128; note++) in_arp[note] = 0;
}

static void key_down(int key, uint8_t velocity, uint32_t buttons, uint32_t now) {
    int number = key - FIRST_NUMBERED;                  /* 0-15 on the numbered keys */
    int numbered = key >= FIRST_NUMBERED && key < FIRST_EDIT;
    app.redraw = 1;
    switch (app.layer) {
    case LAYER_CHANNEL:
        combo_used = 1;
        if (key < MK_TRACKS) {
            app.channel_target = (int8_t)key;
        } else if (numbered && (app.channel_target >= 0 || buttons & KS_BTN_SEQ)) {
            set_track_channel(app.channel_target >= 0 ? app.channel_target : app.track, number);
        } else if (numbered) {
            app.settings.home_channel = (uint8_t)number;        /* picking a home channel puts you in Home */
            app.settings.keys_on_track = 0;
            app.settings_dirty = 1;
        }
        return;
    case LAYER_TRACKS:
        combo_used = 1;
        if (key < MK_TRACKS) {
            app.track = (uint8_t)key;
            if (app.recording) record_stop();
        } else if (number < MK_TRACKS) {
            mk_track *t = &mk_engine_pattern()->track[number];
            t->muted = !t->muted;
            mk_engine_mute_changed(number);
            app.dirty = 1;
        } else if (number < 2 * MK_TRACKS) {
            app.solo |= (uint8_t)(1 << (number - MK_TRACKS));
            key_role[key] = KEY_SOLO;
            mk_engine_solo(app.solo);
        }
        return;
    case LAYER_LENGTH:
        combo_used = 1;
        if (key < MK_BANKS) {
            app.length_page = (uint8_t)key;
        } else if (numbered) {
            track()->length = (uint8_t)(app.length_page * 16 + number + 1);
            edited();
        }
        return;
    case LAYER_SAVE:
        if (key < MK_BANKS) app.save_slot = (uint8_t)(key * 16 + app.save_slot % 16);
        else if (numbered) app.save_slot = (uint8_t)(app.save_slot / 16 * 16 + number);
        return;
    case LAYER_SHIFT:
        combo_used = 1;
        if (key == KEY_CLEAR && pages_clear()) return;  /* the chain page's Clear */
        if (key < MK_BANKS) app_select_slot(key * 16 + app.slot % 16);
        else if (numbered) app_select_slot(app.slot / 16 * 16 + number);
        else edit_key(key);
        return;
    case LAYER_ARP:
        combo_used = 1;
        if (key < 4) {
            app.settings.arp.octaves = (uint8_t)(key + 1);
        } else if (numbered && number < MK_ARP_MODES) {
            app.settings.arp.mode = (uint8_t)number;
        }
        app.settings_dirty = 1;
        return;
    case LAYER_CHORD: {                                 /* the keys played while Chord is held become the user chord */
        int played = 53 + key + 12 * app.octave;
        combo_used = 1;
        if (played >= 0 && played <= 127 && app.learning < MK_CHORD_NOTES) learned[app.learning++] = (uint8_t)played;
        return;
    }
    default:
        break;
    }

    int note = 53 + key + 12 * app.octave;              /* key 0 is F3 */
    if (note < 0 || note > 127) return;
    int root, scale = app_scale(&root);
    note = mk_scale_fit(note, scale, root);
    int use = pages_key(key, (uint8_t)note, velocity, (buttons & KS_BTN_SHIFT) != 0);
    if (use == PAGE_KEY_USED) return;
    if (use == PAGE_KEY_PLAYS && app.transpose_hold && !app.recording) {
        mk_engine_transpose(app.track, note - transpose_root());
        return;
    }
    if (key_role[key] == KEY_LATCHED) key_silence(key); /* a latched key played again starts afresh */
    uint8_t *notes = key_notes[key];
    int n = 1;
    notes[0] = (uint8_t)note;
    if (use == PAGE_KEY_PLAYS && app.settings.chord.on) n = mk_chord_notes(&app.settings.chord, note, scale, root, notes);
    key_count[key] = (uint8_t)n;
    key_sent[key] = 0;
    key_velocity[key] = velocity;
    if (use == PAGE_KEY_PLAYS && app.settings.arp_on && !app.recording) {
        key_role[key] = KEY_ARP;
        for (int i = 0; i < n; i++) {
            in_arp[notes[i]]++;
            mk_arp_key(notes[i], velocity);
        }
        return;
    }
    if (app.latch && !keys_down_as(KEY_NOTE)) release_latched();    /* a new chord replaces the latched one */
    key_role[key] = KEY_NOTE;
    key_channel[key] = use == PAGE_KEY_TRACK ? track()->channel : app_keys_channel();
    if (use == PAGE_KEY_PLAYS && app.recording) record_key(notes, n, velocity);     /* low to high, at once */
    mk_chord_order(notes, n, app.settings.chord.strum_type, app_random);
    key_due[key] = now;
    strum(key, now);
}

static void key_up(int key) {
    uint8_t role = key_role[key];
    if (role == KEY_LATCHED) return;                    /* pressed again while latched: already handled */
    key_role[key] = KEY_FREE;
    if (role == KEY_ARP) {                              /* a pitch another key still holds stays in */
        for (int i = 0; i < key_count[key]; i++) {
            uint8_t note = key_notes[key][i];
            if (in_arp[note] && !--in_arp[note]) mk_arp_key(note, 0);
        }
        key_count[key] = 0;
        return;
    }
    if (role == KEY_NOTE && app.latch && !app.recording) {
        key_role[key] = KEY_LATCHED;
        return;
    }
    if (role == KEY_SOLO) {
        app.solo &= (uint8_t)~(1 << (key - FIRST_NUMBERED - MK_TRACKS));
        mk_engine_solo(app.solo);
        app.redraw = 1;
    } else if (role == KEY_NOTE) {
        key_silence(key);                               /* on the channel it was played on */
        pages_key_up(key);
        if (app.recording == REC_STEP && chord_keys && !--chord_keys) step_advance();
    }
}

/* ---- buttons ---- */

static void set_tempo(int tenths) {
    if (tenths < 300) tenths = 300;
    if (tenths > 2400) tenths = 2400;
    mk_engine_pattern()->tempo = (uint16_t)tenths;
    app.dirty = 1;
    app.redraw = 1;
}

/* Tap tempo: the mean of the last taps, up to four, each within two seconds of the one before. */
static void tempo_tap(uint32_t now) {
    if (tempo_tapped && now - tempo_taps[tempo_tapped - 1] > 2000) tempo_tapped = 0;
    if (tempo_tapped == 4) {
        for (int i = 0; i < 3; i++) tempo_taps[i] = tempo_taps[i + 1];
        tempo_tapped = 3;
    }
    tempo_taps[tempo_tapped++] = now;
    if (tempo_tapped < 2) return;
    uint32_t beat = (now - tempo_taps[0]) / (uint32_t)(tempo_tapped - 1);
    if (beat) set_tempo((int)(600000u / beat));
}

static void turned(int by, uint32_t buttons) {
    mk_track *t = track();
    combo_used = 1;
    app.redraw = 1;
    if (app.layer == LAYER_SAVE) {
        app.save_slot = (uint8_t)((app.save_slot + by + MK_SLOTS) % MK_SLOTS);
    } else if (app.layer == LAYER_CHANNEL) {
        app.settings.home_channel = (uint8_t)((app.settings.home_channel + by + 16) % 16);
        app.settings.keys_on_track = 0;
        app.settings_dirty = 1;
    } else if (app.layer == LAYER_LENGTH) {
        int length = t->length + by;
        t->length = (uint8_t)(length < 1 ? 1 : length > MK_STEPS ? MK_STEPS : length);
        edited();
    } else if (app.layer == LAYER_TRACKS) {
        pages_open(PAGE_STEPS, buttons);
    } else if (app.layer == LAYER_CHORD) {
        int preset = app.settings.chord.preset + by;
        app.settings.chord.preset = (uint8_t)(preset < 0 ? 0 : preset >= MK_CHORD_COUNT ? MK_CHORD_COUNT - 1 : preset);
        app.settings_dirty = 1;
    } else if (buttons & KS_BTN_TEMPO) {
        tempo_tapped = 0;                               /* that press was to turn, not a tap */
        set_tempo(mk_engine_pattern()->tempo + 10 * by);
    } else if (app.layer == LAYER_HOME && pages_active()) {
        pages_turn(by);
    } else if (app.recording == REC_STEP && rec_started && !rec_replacing) {
        app.rec_cursor = (uint8_t)((app.rec_cursor + by + 4 * t->length) % t->length);
    }
}

static void pressed_button(uint32_t button, uint32_t buttons, uint32_t now) {
    uint32_t others = buttons & ~button;
    int shift = (buttons & KS_BTN_SHIFT) != 0;
    switch (button) {
    case KS_BTN_PLAY:
        if (others == KS_BTN_SHIFT) {
            pages_open(PAGE_CHAIN, buttons);
            break;
        }
        if (others) break;
        if (app.recording == REC_STEP) {                /* Record then Play: a bar to get ready, then live */
            record_stop();
            app_count_in();
            record_start();
            app.recording = REC_LIVE;
        } else {
            app_start();
        }
        break;
    case KS_BTN_STOP:
        if (shift) {                                    /* the channel layer; a tap flips Home and Track */
            stop_at = now;
            combo_used = 0;
            if (app.layer == LAYER_SAVE) app.layer = LAYER_HOME;
            break;
        }
        if (others) break;
        app_stop();
        if (app.recording == REC_LIVE) record_stop();
        break;
    case KS_BTN_SEQ:
        if (shift && !(buttons & KS_BTN_STOP)) {        /* Save Pattern */
            app.layer = LAYER_SAVE;
            app.save_slot = app.slot;
            break;
        }
        seq_alone = !others;
        seq_at = now;
        combo_used = 0;
        break;
    case KS_BTN_RECORD:
        if (shift) {
            app.settings.overdub = !app.settings.overdub;
            app.settings_dirty = 1;
            app_message(app.settings.overdub ? "Overdub On" : "Overdub Off");
        } else if (app.recording) {
            record_stop();
        } else {
            record_start();
        }
        break;
    case KS_BTN_HOLD:
        if (shift) {
            app.transpose_hold = !app.transpose_hold;
            app_message(app.transpose_hold ? "Transpose Hold" : "Transpose Off");
        } else if (app.layer == LAYER_HOME && pages_hold()) {
            break;                                      /* the chain page: remove the last pattern */
        } else if (app.recording == REC_STEP) {
            if (chord_keys) chord_tie = 1;              /* keys + Hold: tie */
            else record_rest();
        } else {                                        /* latch the arp, or the played notes */
            app.latch = !app.latch;
            mk_arp_hold(app.latch);
            if (!app.latch) release_latched();
        }
        break;
    case KS_BTN_TEMPO:
        if (!others) tempo_tap(now);
        break;
    case KS_BTN_ARP:
        arp_alone = !others;
        arp_at = now;
        combo_used = 0;
        break;
    case KS_BTN_MUTATE:                                 /* both as in the stock firmware */
        if (others == KS_BTN_SHIFT) app_mutate_reset();
        else if (!others && !app.recording) app_mutate();
        break;
    case KS_BTN_CHORD:
        if (others == KS_BTN_SHIFT) {                   /* the stock gesture */
            pages_open(PAGE_SCALE, buttons);
            break;
        }
        chord_alone = !others;
        chord_at = now;
        combo_used = 0;
        app.learning = 0;
        break;
    case KS_BTN_BACK:
        if (app.layer == LAYER_SAVE) app.layer = LAYER_HOME;
        else if (pages_active()) pages_back();
        else if (app.recording) record_stop();
        break;
    case KS_BTN_ENCODER:
        if (app.layer == LAYER_SAVE) {
            app.layer = LAYER_HOME;
            app_save(app.save_slot);
        } else if (app.layer == LAYER_TRACKS) {
            combo_used = 1;
            pages_open(PAGE_TRACK, buttons);
        } else if (app.layer == LAYER_ARP || app.layer == LAYER_CHORD) {
            combo_used = 1;
            pages_open(app.layer == LAYER_ARP ? PAGE_ARP : PAGE_CHORD, buttons);
        } else if (app.layer != LAYER_HOME) {
            break;
        } else if (pages_active()) {
            pages_click();
        } else if (!others) {
            pages_open(PAGE_GLOBAL, buttons);
        }
        break;
    case KS_BTN_OCT_UP:
        if (!shift && app.octave < 3) app.octave++;
        break;
    case KS_BTN_OCT_DOWN:
        if (!shift && app.octave > -3) app.octave--;
        break;
    default:
        break;
    }
}

static void released_button(uint32_t button, uint32_t now) {
    if (button == KS_BTN_SEQ && seq_alone && !combo_used && now - seq_at < TAP_MS) {
        app.settings.seq_on = !app.settings.seq_on;
        app.settings_dirty = 1;
        mk_engine_enable(app.settings.seq_on);
    }
    if ((button == KS_BTN_STOP || button == KS_BTN_SHIFT) && stop_at) {
        if (!combo_used && now - stop_at < TAP_MS) {
            app.settings.keys_on_track = !app.settings.keys_on_track;
            app.settings_dirty = 1;
        }
        stop_at = 0;
    }
    if (button == KS_BTN_ARP && arp_alone && !combo_used && now - arp_at < TAP_MS) {
        app.settings.arp_on = !app.settings.arp_on;
        app.settings_dirty = 1;
        if (!app.settings.arp_on) arp_forget();
        app_message(app.settings.arp_on ? mk_arp_mode_name[app.settings.arp.mode] : "Arp Off");
    }
    if (button == KS_BTN_CHORD && chord_alone) {
        mk_chord_settings *chord = &app.settings.chord;
        if (app.learning) {                             /* keys were played: they are the user chord, as in stock */
            mk_chord_learn(chord, learned, app.learning);
            chord->preset = MK_CHORD_USER;
            chord->on = 1;
            app.settings_dirty = 1;
            app_message("User Chord Recorded");
        } else if (!combo_used && now - chord_at < TAP_MS) {
            chord->on = !chord->on;
            app.settings_dirty = 1;
            app_message(chord->on ? "Chord On" : "Chord Off");
        }
        app.learning = 0;
    }
    if (button == KS_BTN_SEQ) seq_alone = 0;
    if (button == KS_BTN_ARP) arp_alone = 0;
    if (button == KS_BTN_CHORD) chord_alone = 0;
}

/* A page keeps every control while the buttons that opened it are still down: Shift + Play
 * leaves Shift held, and the numbered keys must then go to the chain, not pick a pattern. */
static uint8_t layer_for(uint32_t buttons) {
    if (app.layer == LAYER_SAVE) return LAYER_SAVE;
    if (pages_holding()) return LAYER_HOME;
    if (buttons & KS_BTN_SHIFT) {
        if (buttons & KS_BTN_STOP && stop_at) return LAYER_CHANNEL;
        if (buttons & KS_BTN_TEMPO) return LAYER_LENGTH;
        return LAYER_SHIFT;
    }
    if (buttons & KS_BTN_SEQ) return LAYER_TRACKS;
    if (buttons & KS_BTN_ARP && arp_alone) return LAYER_ARP;
    if (buttons & KS_BTN_CHORD && chord_alone) return LAYER_CHORD;
    return LAYER_HOME;
}

void ui_poll(void) {
    uint32_t buttons = ks_buttons(), now = ks_millis();
    uint32_t down = buttons & ~previous, up = previous & ~buttons;
    previous = buttons;
    for (uint32_t bit = 1; bit && (down || up); bit <<= 1) {
        if (down & bit) pressed_button(bit, buttons, now);
        if (up & bit) released_button(bit, now);
    }
    if (down || up) app.redraw = 1;
    pages_buttons(buttons);

    uint8_t layer = layer_for(buttons);
    if (layer != app.layer) {
        if (app.layer == LAYER_CHANNEL) app.channel_target = -1;
        if (layer == LAYER_LENGTH) app.length_page = (uint8_t)((track()->length - 1) / 16);
        app.layer = layer;
        app.redraw = 1;
    }

    if (app.recording == REC_LIVE && !app_counting() && !mk_engine_playing()) record_stop();   /* stopped from outside */

    int by = ks_encoder();
    if (by) turned(by, buttons);

    ks_key_event event;
    while (ks_key_get(&event)) {
        if (event.on) key_down(event.key, event.velocity, buttons, now);
        else key_up(event.key);
    }
    for (int key = 0; key < KS_KEYS; key++)             /* chords being strummed */
        if (key_sent[key] < key_count[key] && (key_role[key] == KEY_NOTE || key_role[key] == KEY_LATCHED)) strum(key, now);
}
