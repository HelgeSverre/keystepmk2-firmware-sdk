/* Scales and chords: the tables and the arithmetic. The scales, the chord presets, the strum
 * orders and the way an off-scale note is moved are the stock firmware's, measured in the emulator
 * (docs/STOCK-UI.md). */
#include "mk.h"

const char *const mk_scale_name[MK_SCALE_COUNT] = {
    "Off", "Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Harmonic Minor",
    "Blues", "Pentatonic Major", "Pentatonic Minor", "Japanese", "Gypsy", "Arabic", "Freygish",
};
const char *const mk_scale_short[MK_SCALE_COUNT] = {
    "", "Maj", "Min", "Dor", "Phr", "Lyd", "Mix", "Loc", "HMi", "Blu", "PMa", "PMi", "Jap", "Gyp", "Ara", "Fry",
};
const char *const mk_root_name[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char *const mk_chord_name[MK_CHORD_COUNT] = {
    "User", "Oct", "5th", "Maj", "min", "sus2", "sus4", "Maj7", "min7", "Maj9", "min9", "Maj11", "min11",
};
const char *const mk_strum_name[MK_STRUM_COUNT] = { "Up", "Alt Up", "Down", "Alt Down", "Random" };

#define S(n) (1u << (n))

/* Bit n set: n semitones above the root is in the scale. */
static const uint16_t scales[MK_SCALE_COUNT] = {
    0,
    S(0) | S(2) | S(4) | S(5) | S(7) | S(9) | S(11),    /* Major */
    S(0) | S(2) | S(3) | S(5) | S(7) | S(8) | S(10),    /* Minor */
    S(0) | S(2) | S(3) | S(5) | S(7) | S(9) | S(10),    /* Dorian */
    S(0) | S(1) | S(3) | S(5) | S(7) | S(8) | S(10),    /* Phrygian */
    S(0) | S(2) | S(4) | S(6) | S(7) | S(9) | S(11),    /* Lydian */
    S(0) | S(2) | S(4) | S(5) | S(7) | S(9) | S(10),    /* Mixolydian */
    S(0) | S(1) | S(3) | S(5) | S(6) | S(8) | S(10),    /* Locrian */
    S(0) | S(2) | S(3) | S(5) | S(7) | S(8) | S(11),    /* Harmonic Minor */
    S(0) | S(3) | S(5) | S(6) | S(7) | S(10),           /* Blues */
    S(0) | S(2) | S(4) | S(7) | S(9),                   /* Pentatonic Major */
    S(0) | S(3) | S(5) | S(7) | S(10),                  /* Pentatonic Minor */
    S(0) | S(2) | S(3) | S(7) | S(8),                   /* Japanese */
    S(0) | S(2) | S(3) | S(6) | S(7) | S(8) | S(10),    /* Gypsy */
    S(0) | S(1) | S(4) | S(5) | S(6) | S(8) | S(11),    /* Arabic */
    S(0) | S(1) | S(4) | S(5) | S(7) | S(8) | S(10),    /* Freygish */
};

/* Bit n set: n semitones above the key. */
static const uint32_t chords[MK_CHORD_COUNT] = {
    0,
    S(0) | S(12),
    S(0) | S(7),
    S(0) | S(4) | S(7),
    S(0) | S(3) | S(7),
    S(0) | S(2) | S(7),
    S(0) | S(5) | S(7),
    S(0) | S(4) | S(7) | S(11),
    S(0) | S(3) | S(7) | S(10),
    S(0) | S(4) | S(7) | S(11) | S(14),
    S(0) | S(3) | S(7) | S(10) | S(14),
    S(0) | S(4) | S(7) | S(11) | S(14) | S(17),
    S(0) | S(3) | S(7) | S(10) | S(14) | S(17),
};

static int in_scale(int note, uint16_t scale, int root) { return scale >> ((note - root + 120) % 12) & 1; }

int mk_scale_fit(int note, int scale, int root) {
    if (scale <= MK_SCALE_OFF || scale >= MK_SCALE_COUNT || note < 0 || note > 127) return note;
    uint16_t notes = scales[scale];
    root = (root % 12 + 12) % 12;
    for (int away = 0; away < 12; away++) {            /* the lower one first */
        if (note - away >= 0 && in_scale(note - away, notes, root)) return note - away;
        if (note + away <= 127 && in_scale(note + away, notes, root)) return note + away;
    }
    return note;
}

static int add(uint8_t *out, int n, int note) {
    if (note < 0 || note > 127 || n >= MK_CHORD_NOTES) return n;
    int at = n;
    for (int i = 0; i < n; i++) {
        if (out[i] == note) return n;
        if (out[i] > note && at == n) at = i;
    }
    for (int i = n; i > at; i--) out[i] = out[i - 1];
    out[at] = (uint8_t)note;
    return n + 1;
}

int mk_chord_notes(const mk_chord_settings *chord, int note, int scale, int root, uint8_t out[MK_CHORD_NOTES]) {
    uint32_t shape = chord->preset == MK_CHORD_USER || chord->preset >= MK_CHORD_COUNT
                         ? (uint32_t)chord->user[0] | (uint32_t)chord->user[1] << 8 | (uint32_t)chord->user[2] << 16
                         : chords[chord->preset];
    int n = add(out, 0, mk_scale_fit(note, scale, root));
    if (chord->voicing >= 1 && chord->voicing <= 3) n = add(out, n, mk_scale_fit(note - 12 * chord->voicing, scale, root));
    for (int i = 1; i < 24; i++)
        if (shape >> i & 1) n = add(out, n, mk_scale_fit(note + i, scale, root));
    if (!n) out[n++] = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
    return n;
}

static void reverse(uint8_t *notes, int n) {
    for (int i = 0; i < n / 2; i++) {
        uint8_t held = notes[i];
        notes[i] = notes[n - 1 - i];
        notes[n - 1 - i] = held;
    }
}

/* Alt Up: the lowest, the highest, the second lowest, the second highest, and so on. Alt Down
 * starts from the top. */
static void alternate(uint8_t *notes, int n) {
    uint8_t sorted[MK_CHORD_NOTES];
    for (int i = 0; i < n; i++) sorted[i] = notes[i];
    for (int i = 0, low = 0, high = n - 1; i < n; i++) notes[i] = i & 1 ? sorted[high--] : sorted[low++];
}

void mk_chord_order(uint8_t *notes, int n, int strum_type, uint32_t (*random)(void)) {
    if (n < 2 || n > MK_CHORD_NOTES) return;
    switch (strum_type) {
    case MK_STRUM_ALT_UP: alternate(notes, n); break;
    case MK_STRUM_DOWN: reverse(notes, n); break;
    case MK_STRUM_ALT_DOWN:
        reverse(notes, n);
        alternate(notes, n);
        break;
    case MK_STRUM_RANDOM:
        for (int i = n - 1; i > 0 && random; i--) {
            int j = (int)(random() % (uint32_t)(i + 1));
            uint8_t held = notes[i];
            notes[i] = notes[j];
            notes[j] = held;
        }
        break;
    default: break;
    }
}

void mk_chord_learn(mk_chord_settings *chord, const uint8_t *notes, int n) {
    uint32_t shape = 1;
    int lowest = 127;
    for (int i = 0; i < n; i++)
        if (notes[i] < lowest) lowest = notes[i];
    for (int i = 0; i < n; i++)
        if (notes[i] - lowest < 24) shape |= 1u << (notes[i] - lowest);
    chord->user[0] = (uint8_t)shape;
    chord->user[1] = (uint8_t)(shape >> 8);
    chord->user[2] = (uint8_t)(shape >> 16);
}
