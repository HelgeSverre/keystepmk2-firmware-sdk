/* KeyStep MK2000: the pages that stay on the display until Back: the step page, track settings,
 * global settings, pattern chaining, and the arp, chord and scale settings (docs/MK2000.md). ui.c hands them the encoder, Back,
 * Hold and the keys; Play, Stop and the held layers keep their meaning.
 *
 * The step page has two levels: the list of steps, then the fields of one step. The menus show
 * one entry at a time like the stock ones: turn to move, click to edit, turn to change, click or
 * Back to finish. */
#include "app.h"

#define SMALL small_font
#define LARGE large_font
#define BRIGHTEST 0x8D          /* the stock firmware never drives a light harder */
#define NO_TRACK 0xFF

enum { LIST, INSIDE };          /* the step page's levels */
enum { F_VELOCITY, F_GATE, F_PROBABILITY, F_RATCHET, F_CONDITION, F_TIE, F_OFFSET, FIELDS };
enum { T_CHANNEL, T_LENGTH, T_DIVISION, T_SWING, T_GATE, T_DIRECTION, T_FOLLOWS, T_SHIFT, T_QUANTIZE, T_CLEAR_AUTOM,
       TRACK_ENTRIES };
enum { G_BRIGHTNESS, G_PATTERN_CHANGE, G_TEMPO, G_OVERDUB, G_REC_QUANTIZE, G_REC_AUTOM, G_KEYS, G_HOME_CHANNEL,
       G_AFTERTOUCH, G_SYNC, G_DIN_THRU, G_USB_THRU, G_ABOUT, GLOBAL_ENTRIES };
enum { A_LENGTH, A_RETRIGGER, A_MODE, A_OCTAVES, A_DIVISION, A_GATE, A_SWING, ARP_ENTRIES };
enum { C_PRESET, C_STRUM, C_STRUM_TYPE, C_VOICING, CHORD_ENTRIES };
enum { S_GLOBAL, S_GLOBAL_ROOT, S_PATTERN, S_PATTERN_ROOT, SCALE_ENTRIES };

typedef struct {
    const char *name;
    int16_t low, high;
    uint8_t step;               /* per detent; 0 = cannot be changed */
    const char *const *words;   /* a word per value from `low`, or NULL to show the number */
    const char *unit;           /* after the number */
    const char *zero;           /* shown instead of 0 */
} entry;

typedef struct {
    const entry *entries;
    int count;
    int (*get)(int which);
    void (*set)(int which, int value);
} menu;

static const char *const off_on[] = { "Off", "On" };
static const char *const conditions[MK_COND_COUNT] = { "Always", "1 of 2", "2 of 2", "1 of 3", "1 of 4", "First", "Not first" };
static const char *const directions[MK_DIR_COUNT] = { "Forward", "Reverse", "Pendulum", "Random" };
static const char *const pattern_changes[] = { "At the wrap", "At once" };
static const char *const tempo_changes[] = { "When stopped", "Always", "Never" };
static const char *const key_states[] = { "Home", "Track" };
static const char *const sync_sources[MK_SYNC_COUNT] = { "Auto", "Internal", "USB", "MIDI Din", "Sync In" };
static const char *const version[] = { ks_version };
static const char *const sensitivities[] = { "Off", "Low", "Medium", "High" };
static const char *const automations[MK_CONTROLLERS + 1] = { "Bend", "Mod", "Aftertouch", "Pedal", "All" };
static const char *const voicings[] = { "Off", "Bass -1", "Bass -2", "Bass -3" };
/* The pattern's scale in the order the stock menu has it: Off, Global, then the scales. */
static const char *const pattern_scales[MK_SCALE_COUNT + 1] = {
    "Off", "Global", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harmonic Minor",
    "Blues", "Pentatonic Major", "Pentatonic Minor", "Japanese", "Gypsy", "Arabic", "Freygish",
};

/* Gate, probability and offset move in fives: a hundred detents would be too many to turn. The
 * offset is shown in per cent of a step, -50 to +50. */
static const entry step_fields[FIELDS] = {
    { "Velocity", 1, 127, 1, 0, 0, 0 },
    { "Gate", 0, 100, 5, 0, " %", "Track" },
    { "Probability", 0, 100, 5, 0, " %", 0 },
    { "Ratchet", 1, 4, 1, 0, 0, 0 },
    { "Condition", 0, MK_COND_COUNT - 1, 1, conditions, 0, 0 },
    { "Tie", 0, 1, 1, off_on, 0, 0 },
    { "Offset", -50, 50, 5, 0, " %", 0 },
};

static const entry track_entries[TRACK_ENTRIES] = {
    { "Channel", 1, 16, 1, 0, 0, 0 },
    { "Length", 1, MK_STEPS, 1, 0, " steps", 0 },
    { "Time Div", 0, MK_DIV_COUNT - 1, 1, mk_div_name, 0, 0 },
    { "Swing", 50, 75, 1, 0, " %", 0 },
    { "Gate", 10, 90, 5, 0, " %", 0 },
    { "Direction", 0, MK_DIR_COUNT - 1, 1, directions, 0, 0 },
    { "Follows transpose", 0, 1, 1, off_on, 0, 0 },
    { "Shift", -48, 48, 1, 0, " st", 0 },
    { "Quantize", 50, 100, 50, 0, " %", 0 },     /* an action: the click that ends the edit does it */
    { "Clear Autom", 0, MK_CONTROLLERS, 1, automations, 0, 0 },    /* an action too */
};

static const entry global_entries[GLOBAL_ENTRIES] = {
    { "Brightness", 1, 10, 1, 0, " of 10", 0 },
    { "Pattern change", 0, 1, 1, pattern_changes, 0, 0 },
    { "Tempo on change", 0, 2, 1, tempo_changes, 0, 0 },
    { "Overdub", 0, 1, 1, off_on, 0, 0 },
    { "Rec Quantize", 0, 1, 1, off_on, 0, 0 },
    { "Rec Autom", 0, 1, 1, off_on, 0, 0 },
    { "Keys", 0, 1, 1, key_states, 0, 0 },
    { "Home channel", 1, 16, 1, 0, 0, 0 },
    { "Aft Sensitivity", 0, 3, 1, sensitivities, 0, 0 },
    { "Clock Receive", 0, MK_SYNC_COUNT - 1, 1, sync_sources, 0, 0 },
    { "MIDI Thru", 0, 1, 1, off_on, 0, 0 },
    { "USB to MIDI", 0, 1, 1, off_on, 0, 0 },
    { "About", 0, 0, 0, version, 0, 0 },
};

/* Arp Length and Arp Retrigger are the stock menu's two entries; the rest has no menu there. */
static const entry arp_entries[ARP_ENTRIES] = {
    { "Arp Length", 1, 64, 1, 0, " steps", 0 },
    { "Arp Retrigger", 0, 1, 1, off_on, 0, 0 },
    { "Mode", 0, MK_ARP_MODES - 1, 1, mk_arp_mode_name, 0, 0 },
    { "Octaves", 1, 4, 1, 0, 0, 0 },
    { "Time Division", 0, MK_DIV_COUNT - 1, 1, mk_div_name, 0, 0 },
    { "Gate", 10, 90, 5, 0, " %", 0 },
    { "Swing", 50, 75, 1, 0, " %", 0 },
};

static const entry chord_entries[CHORD_ENTRIES] = {
    { "Preset", 0, MK_CHORD_COUNT - 1, 1, mk_chord_name, 0, 0 },
    { "Strum (ms)", 0, MK_STRUM_MAX, 2, 0, " ms", 0 },
    { "Strum Type", 0, MK_STRUM_COUNT - 1, 1, mk_strum_name, 0, 0 },
    { "Voicing", 0, 3, 1, voicings, 0, 0 },
};

static const entry scale_entries[SCALE_ENTRIES] = {
    { "Global Scale", 0, MK_SCALE_COUNT - 1, 1, mk_scale_name, 0, 0 },
    { "Global Root", 0, 11, 1, mk_root_name, 0, 0 },
    { "Pattern Scale", 0, MK_SCALE_COUNT, 1, pattern_scales, 0, 0 },
    { "Pattern Root", 0, 11, 1, mk_root_name, 0, 0 },
};

static uint8_t kind;            /* PAGE_ */
static uint32_t holding;        /* the buttons that opened the page, while they stay down */
static uint8_t level;           /* step page: LIST or INSIDE */
static uint8_t cursor;          /* step page: the step */
static uint8_t shown;           /* the menu entry, or the field of the step */
static uint8_t editing;         /* turning changes the value rather than moving */
static uint8_t snapped;         /* the track an undo snapshot was taken of since the page opened */
static uint32_t chord;          /* inside a step: keys of the chord being entered, still down */

static mk_track *track(void) { return &mk_engine_pattern()->track[app.track]; }

/* The selected track may have changed or got shorter since the cursor was put there. */
static mk_step *step(void) {
    if (cursor >= track()->length) cursor = (uint8_t)(track()->length - 1);
    return &track()->step[cursor];
}

/* ---- editing the selected track ---- */

static void before_edit(void) {
    if (snapped == app.track) return;
    app_snapshot();
    snapped = app.track;
}

static void after_edit(void) {
    mk_engine_edited(app.track);
    app.dirty = 1;
    app.redraw = 1;
}

static int step_get(int which) {
    const mk_step *s = step();
    switch (which) {
    case F_VELOCITY: return s->velocity[0] ? s->velocity[0] : 100;
    case F_GATE: return s->gate;
    case F_PROBABILITY: return s->probability;
    case F_RATCHET: return s->ratchet;
    case F_CONDITION: return s->condition;
    case F_TIE: return (s->flags & MK_STEP_TIE) != 0;
    default: return s->offset < 0 ? -((-s->offset * 100 + 128) / 256) : (s->offset * 100 + 128) / 256;
    }
}

/* Velocity goes to every note of the step, and to the unused slots so it holds for a note added
 * later from the list. */
static void step_set(int which, int value) {
    mk_step *s = step();
    before_edit();
    uint32_t state = app_lock();
    switch (which) {
    case F_VELOCITY:
        for (int i = 0; i < MK_POLY; i++) s->velocity[i] = (uint8_t)value;
        break;
    case F_GATE: s->gate = (uint8_t)value; break;
    case F_PROBABILITY: s->probability = (uint8_t)value; break;
    case F_RATCHET: s->ratchet = (uint8_t)value; break;
    case F_CONDITION: s->condition = (uint8_t)value; break;
    case F_TIE: s->flags = (uint8_t)(value ? s->flags | MK_STEP_TIE : s->flags & ~MK_STEP_TIE); break;
    default: {
        int offset = value < 0 ? -((-value * 256 + 50) / 100) : (value * 256 + 50) / 100;
        s->offset = (int8_t)(offset < -128 ? -128 : offset > 127 ? 127 : offset);
        break;
    }
    }
    app_unlock(state);
    after_edit();
}

/* A key inside a step: the first key of a chord replaces the step's notes, as step recording does. */
static void enter_note(int key, uint8_t note, uint8_t velocity) {
    mk_step *s = step();
    before_edit();
    uint32_t state = app_lock();
    if (!chord)
        for (int i = 0; i < MK_POLY; i++) s->note[i] = 0;
    for (int i = 0; i < MK_POLY && s->note[i] != note; i++) {
        if (s->note[i]) continue;
        s->velocity[i] = velocity;
        s->note[i] = note;
        break;
    }
    app_unlock(state);
    chord |= 1u << key;
    after_edit();
}

static uint8_t quantize_strength = 100;
static uint8_t autom_choice = MK_CONTROLLERS;   /* Clear Autom: one controller, or all */

/* Takes what was recorded from a strip, pressure or the pedal off every step of the track. */
static void clear_autom(int which) {
    mk_track *t = track();
    before_edit();
    uint32_t state = app_lock();
    for (int s = 0; s < MK_STEPS; s++)
        for (int c = 0; c < MK_CONTROLLERS; c++)
            if (which == c || which == MK_CONTROLLERS) t->step[s].controller[c] = MK_CTL_NONE;
    app_unlock(state);
    after_edit();
    app_message("Autom Cleared");
}

static int track_get(int which) {
    const mk_track *t = track();
    switch (which) {
    case T_CHANNEL: return t->channel + 1;
    case T_LENGTH: return t->length;
    case T_DIVISION: return t->division;
    case T_SWING: return t->swing;
    case T_GATE: return t->gate;
    case T_DIRECTION: return t->direction;
    case T_FOLLOWS: return t->follows_transpose;
    case T_QUANTIZE: return quantize_strength;
    case T_CLEAR_AUTOM: return autom_choice;
    default: return t->shift;
    }
}

static void track_set(int which, int value) {
    mk_track *t = track();
    if (which == T_QUANTIZE) {
        quantize_strength = (uint8_t)value;
        return;
    }
    if (which == T_CLEAR_AUTOM) {
        autom_choice = (uint8_t)value;
        return;
    }
    before_edit();
    switch (which) {
    case T_CHANNEL: t->channel = (uint8_t)(value - 1); break;      /* sounding notes end on the old one */
    case T_LENGTH: t->length = (uint8_t)value; break;
    case T_DIVISION: t->division = (uint8_t)value; break;
    case T_SWING: t->swing = (uint8_t)value; break;
    case T_GATE: t->gate = (uint8_t)value; break;
    case T_DIRECTION: t->direction = (uint8_t)value; break;
    case T_FOLLOWS: t->follows_transpose = (uint8_t)value; break;
    default: t->shift = (int8_t)value; break;
    }
    after_edit();
}

/* ---- global settings ---- */

static int global_get(int which) {
    const mk_settings *s = &app.settings;
    switch (which) {
    case G_BRIGHTNESS: {
        int level = (s->brightness * 10 + BRIGHTEST / 2) / BRIGHTEST;
        return level < 1 ? 1 : level > 10 ? 10 : level;
    }
    case G_PATTERN_CHANGE: return s->pattern_change;
    case G_TEMPO: return s->tempo_follows_pattern;
    case G_OVERDUB: return s->overdub;
    case G_REC_QUANTIZE: return !(s->options & MK_OPT_REC_UNQUANTIZED);
    case G_REC_AUTOM: return !(s->options & MK_OPT_NO_AUTOM);
    case G_AFTERTOUCH: {                                /* stored: 0 medium, 1 low, 2 high, 3 off */
        static const uint8_t shown_as[4] = { 2, 1, 3, 0 };
        return shown_as[(s->options & MK_OPT_AFTERTOUCH) >> MK_OPT_AFTERTOUCH_SHIFT];
    }
    case G_KEYS: return s->keys_on_track;
    case G_HOME_CHANNEL: return s->home_channel + 1;
    case G_SYNC: return s->sync_source < MK_SYNC_COUNT ? s->sync_source : MK_SYNC_AUTO;
    case G_DIN_THRU: return s->din_thru;
    case G_USB_THRU: return s->usb_thru;
    default: return 0;
    }
}

/* main.c writes the settings a moment later, while stopped. The lights follow on the redraw. */
static void global_set(int which, int value) {
    mk_settings *s = &app.settings;
    switch (which) {
    case G_BRIGHTNESS: s->brightness = (uint8_t)(value * BRIGHTEST / 10); break;
    case G_PATTERN_CHANGE: s->pattern_change = (uint8_t)value; break;
    case G_TEMPO: s->tempo_follows_pattern = (uint8_t)value; break;
    case G_OVERDUB: s->overdub = (uint8_t)value; break;
    case G_REC_QUANTIZE: s->options = (uint8_t)(value ? s->options & ~MK_OPT_REC_UNQUANTIZED : s->options | MK_OPT_REC_UNQUANTIZED); break;
    case G_REC_AUTOM: s->options = (uint8_t)(value ? s->options & ~MK_OPT_NO_AUTOM : s->options | MK_OPT_NO_AUTOM); break;
    case G_AFTERTOUCH: {
        static const uint8_t stored_as[4] = { 3, 1, 0, 2 };
        s->options = (uint8_t)((s->options & ~MK_OPT_AFTERTOUCH) | stored_as[value] << MK_OPT_AFTERTOUCH_SHIFT);
        break;
    }
    case G_KEYS: s->keys_on_track = (uint8_t)value; break;
    case G_HOME_CHANNEL: s->home_channel = (uint8_t)(value - 1); break;
    case G_SYNC: s->sync_source = (uint8_t)value; break;          /* main.c follows the new one at once */
    case G_DIN_THRU: s->din_thru = (uint8_t)value; break;
    case G_USB_THRU: s->usb_thru = (uint8_t)value; break;
    default: return;
    }
    app.settings_dirty = 1;
}

/* ---- arp, chord and scale settings ---- */

static int arp_get(int which) {
    const mk_arp_settings *a = &app.settings.arp;
    switch (which) {
    case A_LENGTH: return a->length < 1 ? 16 : a->length > 64 ? 64 : a->length;
    case A_RETRIGGER: return !a->free_run;
    case A_MODE: return a->mode < MK_ARP_MODES ? a->mode : MK_ARP_UP;
    case A_OCTAVES: return a->octaves;
    case A_DIVISION: return a->division < MK_DIV_COUNT ? a->division : MK_DIV_16;
    case A_GATE: return a->gate;
    default: return a->swing;
    }
}

/* The arp reads its settings afresh on every step. */
static void arp_set(int which, int value) {
    mk_arp_settings *a = &app.settings.arp;
    switch (which) {
    case A_LENGTH: a->length = (uint8_t)value; break;
    case A_RETRIGGER: a->free_run = (uint8_t)!value; break;
    case A_MODE: a->mode = (uint8_t)value; break;
    case A_OCTAVES: a->octaves = (uint8_t)value; break;
    case A_DIVISION: a->division = (uint8_t)value; break;
    case A_GATE: a->gate = (uint8_t)value; break;
    default: a->swing = (uint8_t)value; break;
    }
    app.settings_dirty = 1;
}

static int chord_get(int which) {
    const mk_chord_settings *c = &app.settings.chord;
    switch (which) {
    case C_PRESET: return c->preset < MK_CHORD_COUNT ? c->preset : MK_CHORD_USER;
    case C_STRUM: return c->strum > MK_STRUM_MAX ? MK_STRUM_MAX : c->strum;
    case C_STRUM_TYPE: return c->strum_type < MK_STRUM_COUNT ? c->strum_type : MK_STRUM_UP;
    default: return c->voicing > 3 ? 0 : c->voicing;
    }
}

static void chord_set(int which, int value) {
    mk_chord_settings *c = &app.settings.chord;
    switch (which) {
    case C_PRESET: c->preset = (uint8_t)value; break;
    case C_STRUM: c->strum = (uint8_t)value; break;
    case C_STRUM_TYPE: c->strum_type = (uint8_t)value; break;
    default: c->voicing = (uint8_t)value; break;
    }
    app.settings_dirty = 1;
}

/* The pattern's scale is stored as 0 = Global, a scale, or MK_PATTERN_SCALE_OFF; the menu has Off
 * first, then Global. */
static int scale_get(int which) {
    const mk_pattern *p = mk_engine_pattern();
    switch (which) {
    case S_GLOBAL: return app.settings.scale < MK_SCALE_COUNT ? app.settings.scale : MK_SCALE_OFF;
    case S_GLOBAL_ROOT: return app.settings.root % 12;
    case S_PATTERN: return p->scale == MK_PATTERN_SCALE_GLOBAL ? 1 : p->scale < MK_SCALE_COUNT ? p->scale + 1 : 0;
    default: return p->root % 12;
    }
}

static void scale_set(int which, int value) {
    mk_pattern *p = mk_engine_pattern();
    switch (which) {
    case S_GLOBAL: app.settings.scale = (uint8_t)value; break;
    case S_GLOBAL_ROOT: app.settings.root = (uint8_t)value; break;
    case S_PATTERN: p->scale = (uint8_t)(value == 0 ? MK_PATTERN_SCALE_OFF : value == 1 ? MK_PATTERN_SCALE_GLOBAL : value - 1); break;
    default: p->root = (uint8_t)value; break;
    }
    if (which == S_GLOBAL || which == S_GLOBAL_ROOT) app.settings_dirty = 1;
    else app.dirty = 1;
}

static const menu arp_menu = { arp_entries, ARP_ENTRIES, arp_get, arp_set };
static const menu chord_menu = { chord_entries, CHORD_ENTRIES, chord_get, chord_set };
static const menu scale_menu = { scale_entries, SCALE_ENTRIES, scale_get, scale_set };
static const menu step_menu = { step_fields, FIELDS, step_get, step_set };
static const menu track_menu = { track_entries, TRACK_ENTRIES, track_get, track_set };
static const menu global_menu = { global_entries, GLOBAL_ENTRIES, global_get, global_set };

static const menu *current_menu(void) {
    switch (kind) {
    case PAGE_STEPS: return level == INSIDE ? &step_menu : 0;
    case PAGE_TRACK: return &track_menu;
    case PAGE_GLOBAL: return &global_menu;
    case PAGE_ARP: return &arp_menu;
    case PAGE_CHORD: return &chord_menu;
    case PAGE_SCALE: return &scale_menu;
    default: return 0;
    }
}

/* ---- the chain of the bank in use ---- */

static void chain_add(int pattern) {
    if (app.chain.count >= MK_CHAIN_MAX) return;      /* the page shows 32 of 32 */
    app.chain.pattern[app.chain.count++] = (uint8_t)pattern;
    app.chain_dirty = 1;
}

static void chain_switch(int on) {
    if (app.chain.on == on) return;
    app.chain.on = (uint8_t)on;
    app.chain_dirty = 1;
}

int pages_hold(void) {
    if (kind != PAGE_CHAIN) return 0;
    if (app.chain.count) {
        app.chain.count--;
        app.chain_dirty = 1;
    }
    app.redraw = 1;
    return 1;
}

int pages_clear(void) {
    if (kind != PAGE_CHAIN) return 0;
    if (app.chain.count) app.chain_dirty = 1;
    app.chain.count = 0;
    app.redraw = 1;
    return 1;
}

/* ---- opening, closing and input ---- */

void pages_open(int which, uint32_t buttons) {
    if (kind != which) {
        pages_close();
        kind = (uint8_t)which;
        level = LIST;
        cursor = shown = editing = 0;
        snapped = NO_TRACK;
    }
    holding = buttons & (KS_BTN_SEQ | KS_BTN_SHIFT | KS_BTN_ARP | KS_BTN_CHORD);
    app.redraw = 1;
}

/* A chain is saved as its page closes, so a power cut afterwards keeps it. */
void pages_close(void) {
    if (kind == PAGE_CHAIN && app.chain_dirty) app_chain_save();
    kind = PAGE_NONE;
    holding = 0;
    editing = 0;
    app.redraw = 1;
}

int pages_active(void) { return kind; }

void pages_buttons(uint32_t buttons) { holding &= buttons; }

int pages_holding(void) { return kind != PAGE_NONE && holding; }

/* The list uses the numbered keys for the steps of the page of 16 and the bank keys for the page;
 * a key past the end of the track is ignored. */
static void jump(int to) {
    int length = track()->length;
    if (to / 16 * 16 >= length) return;
    cursor = (uint8_t)(to < length ? to : length - 1);
}

int pages_key(int key, uint8_t note, uint8_t velocity, int shift) {
    int numbered = key >= FIRST_NUMBERED && key < FIRST_EDIT;
    app.redraw = 1;
    switch (kind) {
    case PAGE_CHAIN:
        if (shift && key == KEY_CLEAR) return pages_clear(), PAGE_KEY_USED;
        if (!numbered) return PAGE_KEY_PLAYS;
        chain_add(key - FIRST_NUMBERED);
        return PAGE_KEY_USED;
    case PAGE_STEPS:
        if (level == INSIDE) {
            enter_note(key, note, velocity);
            return PAGE_KEY_TRACK;
        }
        if (key < MK_BANKS) jump(key * 16 + cursor % 16);
        else if (numbered) jump(cursor / 16 * 16 + key - FIRST_NUMBERED);
        else return PAGE_KEY_PLAYS;
        return PAGE_KEY_USED;
    default:
        return PAGE_KEY_PLAYS;
    }
}

void pages_key_up(int key) { chord &= ~(1u << key); }

static int clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }

void pages_turn(int by) {
    const menu *m = current_menu();
    app.redraw = 1;
    if (kind == PAGE_CHAIN) {
        chain_switch(by > 0);
    } else if (!m) {
        step();                                         /* brings the cursor inside the track */
        cursor = (uint8_t)clamp(cursor + by, 0, track()->length - 1);
    } else if (!editing) {
        shown = (uint8_t)clamp(shown + by, 0, m->count - 1);
    } else {
        const entry *e = &m->entries[shown];
        int value = m->get(shown), next = clamp(value + by * e->step, e->low, e->high);
        if (next != value) m->set(shown, next);
    }
}

void pages_click(void) {
    app.redraw = 1;
    if (kind == PAGE_CHAIN) pages_close();
    else if (kind == PAGE_STEPS && level == LIST) level = INSIDE;
    else {
        if (editing && kind == PAGE_TRACK && shown == T_QUANTIZE) ui_quantize(quantize_strength);
        if (editing && kind == PAGE_TRACK && shown == T_CLEAR_AUTOM) clear_autom(autom_choice);
        editing = !editing;
    }
}

void pages_back(void) {
    app.redraw = 1;
    if (editing) editing = 0;
    else if (kind == PAGE_STEPS && level == INSIDE) level = LIST;
    else pages_close();
}

/* ---- drawing ---- */

static char *value_text(char *at, const entry *e, int value) {
    if (e->words) return put(at, e->words[value - e->low]);
    if (!value && e->zero) return put(at, e->zero);
    if (e->low < 0 && value > 0) at = put(at, "+");
    at = put_number(at, value);
    return e->unit ? put(at, e->unit) : at;
}

/* The step's notes, as many as fit in `width`; "Rest" when it has none. */
static void notes_text(char *line, const ks_font *font, int width) {
    const mk_step *s = step();
    char *at = line, *fits = line;
    *at = 0;
    for (int i = 0; i < MK_POLY && s->note[i]; i++) {
        at = note_name(at == line ? at : put(at, " "), s->note[i]);
        if (ks_font_width(font, line) > width) break;
        fits = at;
    }
    *fits = 0;
    if (!line[0]) put(line, "Rest");
}

static char *step_title(char *at) {
    at = put_number(put(at, "T"), app.track + 1);
    return put_number(put(at, " Step "), step() - track()->step + 1);
}

static void step_list_screen(void) {
    const mk_track *t = track();
    char line[48];
    step_title(line);
    text(SMALL, 0, TOP, line);
    right(SMALL, TOP, "click: edit");
    notes_text(line, LARGE, KS_OLED_W);
    text(LARGE, 0, MID, line);
    int page = cursor / 16, used = 0;
    for (int i = 0; i < 16 && page * 16 + i < t->length; i++) used |= mk_step_used(&t->step[page * 16 + i]) << i;
    cells(used, cursor % 16);
}

/* Below the field: the values of the other half of the fields, so most of the step is in view.
 * Closer together, then with shorter names, until it fits. */
static void step_summary(char *line) {
    static const char *const names[2][3] = { { "Vel ", "Gate ", "Prob " }, { "V ", "G ", "P " } };
    const mk_step *s = step();
    for (int tier = 0; tier < 3; tier++) {
        const char *space = tier ? " " : "  ";
        const char *const *name = names[tier == 2];
        char *at = line;
        if (shown >= F_RATCHET) {
            at = put_number(put(at, name[0]), step_get(F_VELOCITY));
            at = put(put(at, space), name[1]);
            at = s->gate ? put_number(at, s->gate) : put(at, "Track");
            put_number(put(put(at, space), name[2]), s->probability);
        } else {
            at = put_number(put(at, "Ratchet "), s->ratchet);
            at = put(put(at, space), conditions[s->condition < MK_COND_COUNT ? s->condition : 0]);
            if (s->flags & MK_STEP_TIE) put(put(at, space), "Tie");
        }
        if (ks_font_width(SMALL, line) <= KS_OLED_W) return;
    }
}

static void step_screen(void) {
    char line[48];
    step_title(line);
    int x = text(SMALL, 0, TOP, line);
    notes_text(line, SMALL, KS_OLED_W - x - 8);
    right(SMALL, TOP, line);
    const entry *e = &step_fields[shown];
    text(LARGE, 0, MID, e->name);
    value_text(line, e, step_get(shown));
    if (editing) boxed(LARGE, KS_OLED_W - ks_font_width(LARGE, line) - 1, MID, line);
    else right(LARGE, MID, line);
    step_summary(line);
    text(SMALL, 0, LOW, line);
}

/* Moving: the menu's name above, the entry in the large font, its value below. Editing: the
 * entry's name above and its value in the large font, boxed. */
static void menu_screen(const char *title, const menu *m) {
    const entry *e = &m->entries[shown];
    char line[32];
    value_text(line, e, m->get(shown));
    if (editing) {
        text(SMALL, 0, TOP, e->name);
        boxed(LARGE, 1, MID, line);
        right(SMALL, LOW, "click: done");
        return;
    }
    text(SMALL, 0, TOP, title);
    char where[12];
    put_number(put(put_number(where, shown + 1), " of "), m->count);
    right(SMALL, TOP, where);
    text(LARGE, 0, MID, e->name);
    text(SMALL, 0, LOW, line);
}

static void about_screen(void) {
    char line[32];
    text(SMALL, 0, TOP, "About");
    right(SMALL, TOP, ks_version);
    text(LARGE, (KS_OLED_W - ks_font_width(LARGE, "KeyStep MK2000")) / 2, MID, "KeyStep MK2000");
    put(put_number(put(line, "Free: "), mk_store_free_sectors()), " of 31 sectors");
    text(SMALL, 0, LOW, line);
}

/* The chain in the small font, whose 1 reads as a 1. When it is too long, the end is shown. */
static void chain_screen(void) {
    char line[MK_CHAIN_MAX * 3 + 4], *at;
    text(SMALL, 0, TOP, "Pattern Chaining");
    put_number(put(line, "Bank "), app.chain_bank + 1);
    right(SMALL, TOP, line);
    for (int first = 0; first < app.chain.count; first++) {
        at = put(line, first ? ".. " : "");
        for (int i = first; i < app.chain.count; i++) at = put(put_number(at, app.chain.pattern[i] + 1), " ");
        if (ks_font_width(SMALL, line) <= KS_OLED_W) break;
    }
    text(SMALL, 0, MID + 3, app.chain.count ? line : "Press pattern keys");
    put(put_number(line, app.chain.count), " of 32");
    text(SMALL, 0, LOW, line);
    right(SMALL, LOW, app.chain.on ? "Chain On" : "Chain Off");
}

int pages_draw(void) {
    switch (kind) {
    case PAGE_STEPS:
        if (level == LIST) step_list_screen();
        else step_screen();
        return 1;
    case PAGE_TRACK: {
        char title[16];
        put(put_number(put(title, "T"), app.track + 1), " Settings");
        menu_screen(title, &track_menu);
        return 1;
    }
    case PAGE_GLOBAL:
        if (editing && shown == G_ABOUT) about_screen();
        else menu_screen("Global Settings", &global_menu);
        return 1;
    case PAGE_CHAIN:
        chain_screen();
        return 1;
    case PAGE_ARP:
        menu_screen("Arp Settings", &arp_menu);
        return 1;
    case PAGE_CHORD:
        menu_screen("Chord", &chord_menu);
        return 1;
    case PAGE_SCALE:
        menu_screen("Scale", &scale_menu);
        return 1;
    default:
        return 0;
    }
}
