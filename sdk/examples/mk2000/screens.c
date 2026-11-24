/* KeyStep MK2000: the display and the button lights. The rows are in app.h. */
#include "app.h"
#include "keystep_fonts.h"

#define SMALL (&ks_font_small)
#define LARGE (&ks_font_large)

const ks_font *const small_font = SMALL, *const large_font = LARGE;   /* the fonts live here only */

/* ---- text building, since there is no printf ---- */

char *put(char *at, const char *text) {
    while (*text) *at++ = *text++;
    *at = 0;
    return at;
}

char *put_number(char *at, int value) {
    char digits[12];
    if (value < 0) {
        *at++ = '-';
        value = -value;
    }
    return put(at, ks_utoa((uint32_t)value, digits));
}

int text(const ks_font *font, int x, int y, const char *s) { return ks_oled_text_font(font, x, y, s); }

void right(const ks_font *font, int y, const char *s) { text(font, KS_OLED_W - ks_font_width(font, s), y, s); }

int boxed(const ks_font *font, int x, int y, const char *s) {
    int w = ks_font_width(font, s), top = font == LARGE ? y + 2 : y - 1, h = font == LARGE ? 12 : 9;
    ks_oled_rect(x - 1, top, w + 1, h, 1);
    ks_oled_ink(0);
    text(font, x, y, s);
    ks_oled_ink(1);
    return x + w;
}

void cells(int used, int cursor) {
    for (int i = 0; i < 16; i++) {
        int x = i * 8;
        ks_oled_rect(x + 1, 24, 6, 5, 1);
        if (i != cursor && !(used & (1 << i))) ks_oled_rect(x + 2, 25, 4, 3, 0);
        if (i == cursor) ks_oled_rect(x + 1, 30, 6, 1, 1);
    }
}

char *note_name(char *at, int note) {
    static const char names[] = "C C#D D#E F F#G G#A A#B ";
    *at++ = names[note % 12 * 2];
    if (names[note % 12 * 2 + 1] == '#') *at++ = '#';
    return put_number(at, note / 12 - 1);
}

/* ---- pieces ---- */

static void keys_label(char *at) {
    at = put(at, app.recording ? "REC " : "KBD ");
    if (app.settings.keys_on_track || app.recording) put_number(put(at, "T"), app.track + 1);
    else put_number(at, app.settings.home_channel + 1);
}

/* The four tracks: the selected one in a box, muted struck through, empty as dashes. */
static void track_line(const char *at_right) {
    const mk_pattern *p = mk_engine_pattern();
    int x = 1;
    for (int t = 0; t < MK_TRACKS; t++) {
        char name[3] = { 'T', (char)('1' + t), 0 };
        const char *shown = mk_track_used(&p->track[t]) || t == app.track ? name : "--";
        int end = t == app.track ? boxed(SMALL, x, LOW, shown) : text(SMALL, x, LOW, shown);
        int silent = p->track[t].muted || (app.solo && !(app.solo & (1 << t)));
        if (silent) ks_oled_rect(x - 1, LOW + 3, end - x + 1, 1, t != app.track);
        x = end + 5;
    }
    right(SMALL, LOW, at_right);
}

static void position_bar(void) {
    const mk_track *t = &mk_engine_pattern()->track[app.track];
    int at = mk_engine_position(app.track) * 256 + mk_engine_step_phase(app.track);
    for (int x = 0; x < KS_OLED_W; x += 2) ks_oled_pixel(x, 31, 1);
    ks_oled_rect(0, 30, KS_OLED_W * at / (t->length * 256), 2, 1);
}

/* Following a clock: its tempo as measured, or dashes until it is heard. */
static void top_line(void) {
    const mk_pattern *p = mk_engine_pattern();
    int tempo = app_external_tempo();
    char line[24], *at = put(line, tempo < 0 ? "Tempo: " : "Ext Tempo: ");
    if (tempo < 0) tempo = p->tempo;
    if (tempo) {
        at = put_number(at, tempo / 10);
        at = put(at, ".");
        put_number(at, tempo % 10);
    } else {
        put(at, "--");
    }
    int x = text(SMALL, 0, TOP, line);
    const char *division = mk_div_name[p->track[app.track].division];
    right(SMALL, TOP, division);
    int root, scale = app_scale(&root);                 /* the scale the keys are held to, where there is room */
    if (!scale) return;
    put(put(put(line, mk_root_name[root]), " "), mk_scale_short[scale]);
    int room = KS_OLED_W - ks_font_width(SMALL, division) - x, width = ks_font_width(SMALL, line);
    if (width + 8 <= room) text(SMALL, x + (room - width) / 2, TOP, line);
}

static void preset_text(char *at, int slot) {
    at = put_number(put(at, "Bank: "), slot / 16 + 1);
    put_number(put(at, " | Preset: "), slot % 16 + 1);
}

/* ---- screens ---- */

static void home(void) {
    char line[32], label[12];
    top_line();
    if (app.message) {
        text(LARGE, 0, MID, app.message);
    } else {
        preset_text(line, app.pending_slot != NO_SLOT ? app.pending_slot : app.slot);
        char *end = line;
        while (*end) end++;
        if (app.pending_slot != NO_SLOT) put(end, " ...");
        else if (app.dirty) put(end, "*");
        text(LARGE, 0, MID, line);
    }
    keys_label(label);
    track_line(label);
    if (mk_engine_playing()) position_bar();
}

static void channel_screen(uint32_t buttons) {
    const mk_pattern *p = mk_engine_pattern();
    int target = app.channel_target >= 0 ? app.channel_target : buttons & KS_BTN_SEQ ? app.track : -1;
    int shown = target >= 0 ? target : app.track;
    char kbd[12], trk[16], line[40], *at;
    text(SMALL, 0, TOP, "MIDI Channel");
    right(SMALL, TOP, "hold + key");
    at = put(kbd, app.settings.keys_on_track ? "Kbd T" : "Kbd ");
    put_number(at, app.settings.keys_on_track ? app.track + 1 : app.settings.home_channel + 1);
    at = put_number(put(trk, "T"), shown + 1);
    put_number(put(at, " > "), p->track[shown].channel + 1);
    if (target < 0) {
        boxed(LARGE, 1, MID, kbd);
        right(LARGE, MID, trk);
    } else {
        text(LARGE, 1, MID, kbd);
        boxed(LARGE, KS_OLED_W - ks_font_width(LARGE, trk) - 1, MID, trk);
    }
    at = line;
    for (int t = 0; t < MK_TRACKS; t++) {
        at = put_number(put(at, "T"), t + 1);
        at = put_number(put(at, ":"), p->track[t].channel + 1);
        at = put(at, "  ");
    }
    text(SMALL, 0, LOW, line);
}

static void tracks_screen(void) {
    const mk_track *t = &mk_engine_pattern()->track[app.track];
    char big[16], small[12];
    text(SMALL, 0, TOP, "Tracks");
    right(SMALL, TOP, "hold + key");
    put_number(put(big, "Track "), app.track + 1);
    text(LARGE, 0, MID, big);
    put_number(put(big, "Ch "), t->channel + 1);
    right(LARGE, MID, big);
    put(put_number(small, t->length), " st");
    track_line(small);
}

static void length_screen(void) {
    const mk_track *t = &mk_engine_pattern()->track[app.track];
    char line[16];
    put(put_number(put(line, "T"), app.track + 1), " Length");
    text(SMALL, 0, TOP, line);
    right(SMALL, TOP, "hold + key");
    put(put_number(line, t->length), " steps");
    text(LARGE, 0, MID, line);
    int in_page = t->length - app.length_page * 16;     /* how much of this page of 16 the track covers */
    cells(in_page >= 16 ? 0xFFFF : in_page > 0 ? (1 << in_page) - 1 : 0, in_page >= 1 && in_page <= 16 ? in_page - 1 : -1);
}

static void slots_screen(void) {
    int bank = app.slot / 16, used = 0;
    char line[32];
    for (int i = 0; i < 16; i++) used |= mk_store_has(bank * 16 + i) << i;
    put_number(put(line, "Bank "), bank + 1);
    text(SMALL, 0, TOP, line);
    right(SMALL, TOP, "hold + key");
    if (app.message) {
        text(LARGE, 0, MID, app.message);
    } else {
        char *at = put_number(put(line, "Preset "), app.slot % 16 + 1);
        if (app.pending_slot != NO_SLOT) put_number(put(at, " > "), app.pending_slot % 16 + 1);
        else if (app.dirty) put(at, "*");
        text(LARGE, 0, MID, line);
    }
    cells(used, app.slot % 16);
}

static void save_screen(void) {
    char line[32];
    text(SMALL, 0, TOP, "Save Pattern");
    right(SMALL, TOP, "click: save");
    preset_text(line, app.save_slot);
    text(LARGE, 0, MID, line);
    text(SMALL, 0, LOW, mk_store_has(app.save_slot) ? "Replaces a pattern" : "Empty slot");
    right(SMALL, LOW, "Back");
}

static void step_record_screen(void) {
    const mk_track *t = &mk_engine_pattern()->track[app.track];
    const mk_step *step = &t->step[app.rec_cursor];
    char line[40], *at;
    int page = app.rec_cursor / 16, used = 0;
    put(put_number(put(line, "T"), app.track + 1), app.settings.overdub ? " Overdub" : " Rec");
    text(SMALL, 0, TOP, line);
    put_number(put(line, "Step "), app.rec_cursor + 1);
    right(SMALL, TOP, line);
    at = line;
    *at = 0;
    for (int i = 0; i < MK_POLY && step->note[i] && at - line < 28; i++) at = put(note_name(at, step->note[i]), " ");
    text(LARGE, 0, MID, line[0] ? line : app.rec_cursor ? "" : "Keys: add notes");
    for (int i = 0; i < 16 && page * 16 + i < t->length; i++) used |= mk_step_used(&t->step[page * 16 + i]) << i;
    cells(used, app.rec_cursor % 16);
}

static void splash(void) {
    text(SMALL, 0, TOP, "Arturia");
    right(SMALL, TOP, ks_version);
    text(LARGE, (KS_OLED_W - ks_font_width(LARGE, "KeyStep MK2000")) / 2, MID + 2, "KeyStep MK2000");
}

static void arp_screen(void) {
    const mk_arp_settings *arp = &app.settings.arp;
    char line[24];
    text(SMALL, 0, TOP, "Arp");
    right(SMALL, TOP, "hold + key");
    text(LARGE, 0, MID, mk_arp_mode_name[arp->mode < MK_ARP_MODES ? arp->mode : 0]);
    put(put_number(line, arp->octaves), " oct");
    right(LARGE, MID, line);
    text(SMALL, 0, LOW, "Keys 1-6: mode");
    right(SMALL, LOW, mk_div_name[arp->division < MK_DIV_COUNT ? arp->division : MK_DIV_16]);
}

/* Chord held: the preset, or the keys played so far into the user chord. */
static void chord_screen(void) {
    const mk_chord_settings *chord = &app.settings.chord;
    char line[24];
    text(SMALL, 0, TOP, app.learning ? "User Chord" : "Chord");
    right(SMALL, TOP, "hold + key");
    if (app.learning) {
        put(put_number(line, app.learning), app.learning == 1 ? " note" : " notes");
        text(LARGE, 0, MID, line);
    } else {
        text(LARGE, 0, MID, mk_chord_name[chord->preset < MK_CHORD_COUNT ? chord->preset : 0]);
        right(LARGE, MID, chord->on ? "On" : "Off");
    }
    text(SMALL, 0, LOW, "Keys: user chord");
    right(SMALL, LOW, "turn: preset");
}

static void count_in_screen(void) {
    char line[16];
    put(put_number(put(line, "T"), app.track + 1), " Record");
    text(SMALL, 0, TOP, line);
    put_number(put(line, "Count-in: "), app_counting());
    text(LARGE, 0, MID, line);
    cells((1 << (5 - app_counting())) - 1, 4 - app_counting());
}

void screens_draw(void) {
    uint32_t buttons = ks_buttons();
    ks_oled_clear();
    switch (app.layer) {
    case LAYER_CHANNEL: channel_screen(buttons); break;
    case LAYER_TRACKS: tracks_screen(); break;
    case LAYER_LENGTH: length_screen(); break;
    case LAYER_SHIFT: slots_screen(); break;
    case LAYER_SAVE: save_screen(); break;
    case LAYER_ARP: arp_screen(); break;
    case LAYER_CHORD: chord_screen(); break;
    case LAYER_HOME:
        if (pages_draw()) break;
        if (app_counting()) count_in_screen();
        else if (app.recording == REC_STEP && !app.message) step_record_screen();
        else home();
        break;
    default: splash(); break;
    }
    ks_oled_flush();
}

/* ---- lights ---- */

static uint32_t scale(uint32_t rgb, uint32_t level) {  /* level 0-255 */
    uint32_t r = (rgb >> 16 & 0xFF) * level / 255, g = (rgb >> 8 & 0xFF) * level / 255, b = (rgb & 0xFF) * level / 255;
    return r << 16 | g << 8 | b;
}

void lights_update(uint32_t buttons) {
    static const uint32_t track_colour[MK_TRACKS] = { 0xFF5000, 0x00A0C0, 0xC020A0, 0x30C010 };
    static uint32_t shown[KS_LED_COUNT];
    uint32_t want[KS_LED_COUNT] = { 0 }, bright = app.settings.brightness, dim = bright / 6, white = 0xFFFFFF;
    int counting = app_counting(), playing = mk_engine_playing() || counting, beat = app_beat_phase() < 12;
    int blink = (ks_millis() / 250) & 1;

    want[KS_LED_SEQ] = scale(track_colour[app.track], app.settings.seq_on ? bright : dim);
    want[KS_LED_ARP] = app.settings.arp_on ? scale(white, bright) : 0;
    want[KS_LED_CHORD] = app.settings.chord.on ? scale(white, bright) : 0;
    want[KS_LED_PLAY] = playing && beat ? scale(0x00FF20, bright) : playing ? scale(0x00FF20, dim) : 0;
    want[KS_LED_STOP] = playing ? 0 : scale(white, dim);
    want[KS_LED_RECORD] = counting ? (beat ? scale(0xFF0000, bright) : 0) : app.recording ? scale(0xFF0000, bright) : app.settings.overdub ? scale(0xFF0000, dim) : 0;
    want[KS_LED_HOLD] = app.transpose_hold ? (blink ? scale(white, bright) : 0) : app.latch ? scale(white, bright) : 0;
    want[KS_LED_SHIFT] = buttons & KS_BTN_SHIFT ? scale(white, bright) : 0;
    want[KS_LED_TEMPO] = buttons & KS_BTN_TEMPO ? scale(white, bright) : 0;
    want[KS_LED_OCT_UP] = app.octave > 0 ? scale(white, bright * (uint32_t)app.octave / 3) : 0;
    want[KS_LED_OCT_DOWN] = app.octave < 0 ? scale(white, bright * (uint32_t)-app.octave / 3) : 0;
    want[KS_LED_ENCODER] = app.layer == LAYER_SAVE || pages_active() ? scale(white, bright) : 0;

    int changed = 0;
    for (int led = KS_LED_OCT_DOWN; led <= KS_LED_ENCODER; led++) {
        if (want[led] == shown[led]) continue;
        shown[led] = want[led];
        ks_led(led, want[led]);
        changed = 1;
    }
    if (changed) ks_leds_show();                        /* takes 3 ms, so only when something differs */
}
