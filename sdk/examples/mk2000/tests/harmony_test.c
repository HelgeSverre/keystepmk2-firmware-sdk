/* Host tests for harmony.c. The expected notes are what the stock firmware 1.0.2 played in the
 * emulator: each of the twelve keys from C4 with the global scale set and root C, and C4 with each
 * chord preset. */
#include <stdio.h>
#include "mk.h"

static int failures;

static void check_eq(long got, long want, const char *expr, const char *file, int line) {
    if (got == want) return;
    failures++;
    printf("%s:%d: %s is %ld, expected %ld\n", file, line, expr, got, want);
}

#define CHECK_EQ(got, want) check_eq((long)(got), (long)(want), #got, __FILE__, __LINE__)

static void ok(const char *what, int before) {
    if (failures == before) printf("ok %s\n", what);
}

static uint32_t rolls;
static uint32_t random_(void) { return rolls = rolls * 1103515245u + 12345u, rolls >> 8; }

static void test_scales(void) {
    int before = failures;
    static const int8_t stock[MK_SCALE_COUNT][12] = {
        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },       /* Off */
        { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 9, 11 },        /* Major */
        { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8, 10, 10 },       /* Minor */
        { 0, 0, 2, 3, 3, 5, 5, 7, 7, 9, 10, 10 },       /* Dorian */
        { 0, 1, 1, 3, 3, 5, 5, 7, 8, 8, 10, 10 },       /* Phrygian */
        { 0, 0, 2, 2, 4, 4, 6, 7, 7, 9, 9, 11 },        /* Lydian */
        { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 10, 10 },       /* Mixolydian */
        { 0, 1, 1, 3, 3, 5, 6, 6, 8, 8, 10, 10 },       /* Locrian */
        { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8, 11, 11 },       /* Harmonic Minor */
        { 0, 0, 3, 3, 3, 5, 6, 7, 7, 10, 10, 10 },      /* Blues */
        { 0, 0, 2, 2, 4, 4, 7, 7, 7, 9, 9, 12 },        /* Pentatonic Major */
        { 0, 0, 3, 3, 3, 5, 5, 7, 7, 10, 10, 10 },      /* Pentatonic Minor */
        { 0, 0, 2, 3, 3, 3, 7, 7, 8, 8, 8, 12 },        /* Japanese */
        { 0, 0, 2, 3, 3, 6, 6, 7, 8, 8, 10, 10 },       /* Gypsy */
        { 0, 1, 1, 4, 4, 5, 6, 6, 8, 8, 11, 11 },       /* Arabic */
        { 0, 1, 1, 4, 4, 5, 5, 7, 8, 8, 10, 10 },       /* Freygish */
    };
    for (int scale = 0; scale < MK_SCALE_COUNT; scale++)
        for (int key = 0; key < 12; key++) CHECK_EQ(mk_scale_fit(60 + key, scale, 0), 60 + stock[scale][key]);
    /* another root moves the whole scale; D major has F# and C# */
    CHECK_EQ(mk_scale_fit(65, MK_SCALE_MAJOR, 2), 64);
    CHECK_EQ(mk_scale_fit(66, MK_SCALE_MAJOR, 2), 66);
    CHECK_EQ(mk_scale_fit(60, MK_SCALE_MAJOR, 2), 59);
    CHECK_EQ(mk_scale_fit(61, MK_SCALE_MAJOR, 14), 61);         /* a root outside 0-11 wraps */
    /* every note, every root: the nearest note of the scale that exists, the lower of two; a
     * key is in the scale when the stock firmware left it alone */
    for (int scale = 1; scale < MK_SCALE_COUNT; scale++)
        for (int root = 0; root < 12; root++)
            for (int note = 0; note < 128; note++) {
                int best = -1;
                for (int n = 0; n < 128; n++) {
                    int degree = (n - root + 120) % 12, far = n > note ? n - note : note - n;
                    if (stock[scale][degree] != degree) continue;
                    if (best < 0 || far < (best > note ? best - note : note - best)) best = n;
                }
                CHECK_EQ(mk_scale_fit(note, scale, root), best);
            }
    CHECK_EQ(mk_scale_fit(61, MK_SCALE_COUNT, 0), 61);
    CHECK_EQ(mk_scale_fit(61, -1, 0), 61);
    CHECK_EQ(mk_scale_fit(0, MK_SCALE_MAJOR, 2), 1);            /* nothing below the bottom: up */
    ok("the fifteen scales move each key as the stock firmware does", before);
}

static void expect(const mk_chord_settings *chord, int note, int scale, const int *want, int n, int line) {
    uint8_t got[MK_CHORD_NOTES];
    int count = mk_chord_notes(chord, note, scale, 0, got);
    check_eq(count, n, "notes in the chord", __FILE__, line);
    for (int i = 0; i < n && i < count; i++) check_eq(got[i], want[i], "chord note", __FILE__, line);
}

#define EXPECT(chord, note, scale, ...) \
    expect(chord, note, scale, (const int[]){ __VA_ARGS__ }, (int)(sizeof((const int[]){ __VA_ARGS__ }) / sizeof(int)), __LINE__)

static void test_chords(void) {
    int before = failures;
    mk_chord_settings c = { 0 };
    c.preset = MK_CHORD_USER;
    EXPECT(&c, 60, 0, 60);                               /* nothing learned yet: the key alone */
    c.preset = MK_CHORD_OCT;
    EXPECT(&c, 60, 0, 60, 72);
    c.preset = MK_CHORD_5TH;
    EXPECT(&c, 60, 0, 60, 67);
    c.preset = MK_CHORD_MAJ;
    EXPECT(&c, 60, 0, 60, 64, 67);
    c.preset = MK_CHORD_MIN;
    EXPECT(&c, 60, 0, 60, 63, 67);
    c.preset = MK_CHORD_SUS2;
    EXPECT(&c, 60, 0, 60, 62, 67);
    c.preset = MK_CHORD_SUS4;
    EXPECT(&c, 60, 0, 60, 65, 67);
    c.preset = MK_CHORD_MAJ7;
    EXPECT(&c, 60, 0, 60, 64, 67, 71);
    c.preset = MK_CHORD_MIN7;
    EXPECT(&c, 60, 0, 60, 63, 67, 70);
    c.preset = MK_CHORD_MAJ9;
    EXPECT(&c, 60, 0, 60, 64, 67, 71, 74);
    c.preset = MK_CHORD_MIN9;
    EXPECT(&c, 60, 0, 60, 63, 67, 70, 74);
    c.preset = MK_CHORD_MAJ11;
    EXPECT(&c, 60, 0, 60, 64, 67, 71, 74, 77);
    c.preset = MK_CHORD_MIN11;
    EXPECT(&c, 60, 0, 60, 63, 67, 70, 74, 77);
    /* the bass voicings, stock on sus4: the root one and three octaves down as well */
    c.preset = MK_CHORD_SUS4;
    c.voicing = 1;
    EXPECT(&c, 60, 0, 48, 60, 65, 67);
    c.voicing = 3;
    EXPECT(&c, 60, 0, 24, 60, 65, 67);
    EXPECT(&c, 30, 0, 30, 35, 37);                       /* no room below: no bass note */
    c.voicing = 0;
    /* in a scale every note is fitted: stock plays min on C in C major as 60 62 67, on D as 62 65 69 */
    c.preset = MK_CHORD_MIN;
    EXPECT(&c, 60, MK_SCALE_MAJOR, 60, 62, 67);
    EXPECT(&c, 62, MK_SCALE_MAJOR, 62, 65, 69);
    c.preset = MK_CHORD_SUS2;                            /* two notes fitted onto one sound once */
    EXPECT(&c, 60, MK_SCALE_PENTATONIC_MINOR, 60, 63, 67);
    c.preset = MK_CHORD_MAJ11;                           /* the top of the range: what does not fit is left out */
    EXPECT(&c, 120, 0, 120, 124, 127);
    /* the user chord: stock, Chord held and C Eb G played, then C and D */
    static const uint8_t played[3] = { 67, 60, 63 };
    mk_chord_learn(&c, played, 3);
    c.preset = MK_CHORD_USER;
    EXPECT(&c, 60, 0, 60, 63, 67);
    EXPECT(&c, 62, 0, 62, 65, 69);
    static const uint8_t wide[8] = { 40, 41, 45, 50, 52, 59, 62, 63 };
    mk_chord_learn(&c, wide, 8);
    EXPECT(&c, 40, 0, 40, 41, 45, 50, 52, 59, 62, 63);
    c.voicing = 2;                                       /* never more than eight */
    EXPECT(&c, 40, 0, 16, 40, 41, 45, 50, 52, 59, 62);
    ok("chord presets, voicings, scale and the user chord", before);
}

static void test_strum(void) {
    int before = failures;
    static const uint8_t want[MK_STRUM_COUNT - 1][5] = {
        { 1, 2, 3, 4, 5 }, { 1, 5, 2, 4, 3 }, { 5, 4, 3, 2, 1 }, { 5, 1, 4, 2, 3 },
    };
    for (int type = 0; type < MK_STRUM_COUNT - 1; type++) {
        uint8_t notes[5] = { 1, 2, 3, 4, 5 };
        mk_chord_order(notes, 5, type, random_);
        for (int i = 0; i < 5; i++) CHECK_EQ(notes[i], want[type][i]);
    }
    uint8_t three[3] = { 60, 63, 67 };                  /* stock, Alt Up on min: 60 67 63 */
    mk_chord_order(three, 3, MK_STRUM_ALT_UP, random_);
    CHECK_EQ(three[1], 67);
    CHECK_EQ(three[2], 63);
    int moved = 0;
    for (int round = 0; round < 50; round++) {          /* random: the same notes, in some order */
        uint8_t notes[6] = { 1, 2, 3, 4, 5, 6 };
        mk_chord_order(notes, 6, MK_STRUM_RANDOM, random_);
        int sum = 0, mask = 0;
        for (int i = 0; i < 6; i++) sum += notes[i], mask |= 1 << notes[i], moved += notes[i] != i + 1;
        CHECK_EQ(sum, 21);
        CHECK_EQ(mask, 0x7E);
    }
    CHECK_EQ(moved > 100, 1);
    uint8_t single[1] = { 9 };
    mk_chord_order(single, 1, MK_STRUM_DOWN, random_);
    CHECK_EQ(single[0], 9);
    ok("strum orders", before);
}

int main(void) {
    test_scales();
    test_chords();
    test_strum();
    if (failures) printf("%d checks failed\n", failures);
    return failures != 0;
}
