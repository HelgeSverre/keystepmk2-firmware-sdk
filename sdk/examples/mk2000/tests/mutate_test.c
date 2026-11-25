/* Host tests for mutate.c: what must hold for every variation, over many of them. */
#include <stdio.h>
#include <string.h>
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
    else printf("FAILED %s\n", what);
}

static uint32_t seed;
static uint32_t random_(void) { return seed = seed * 1103515245u + 12345u, seed >> 8; }

static mk_pattern pattern, copy;

/* Eight steps like the ones measured on the stock firmware: C D E F G A B C with a rest at 3 and
 * 6, a chord on step 5, velocities that tell the steps apart, and a recorded controller. */
static void fill(mk_track *t) {
    static const uint8_t notes[8] = { 60, 62, 0, 65, 67, 69, 0, 72 };
    t->length = 8;
    for (int s = 0; s < 8; s++) {
        t->step[s].note[0] = notes[s];
        t->step[s].velocity[0] = (uint8_t)(notes[s] ? 90 + s : 100);
    }
    t->step[4].note[1] = 71;
    t->step[4].velocity[1] = 80;
    t->step[1].controller[MK_CTL_MOD] = 99;
    t->step[3].ratchet = 3;
    t->step[9].note[0] = 40;            /* past the length: not part of it */
}

static int used(const mk_track *t) {
    int n = 0;
    for (int s = 0; s < t->length; s++) n += mk_step_used(&t->step[s]);
    return n;
}

/* How many of the eight steps differ from the original's. */
static int changed(const mk_track *track, const mk_track *original) {
    int n = 0;
    for (int s = 0; s < 8; s++) n += memcmp(track->step[s].note, original->step[s].note, MK_POLY) != 0;
    return n;
}

static void test_variations(void) {
    int before = failures;
    mk_pattern_init(&pattern);
    mk_track *original = &pattern.track[0], *track = &copy.track[0];
    fill(original);
    long differ_from_last = 0, octaves = 0, moved = 0, harmonies = 0;
    long changes[3] = { 0 };            /* at amounts 10, 30 and 100 */
    static mk_step last[8];
    for (int round = 0; round < 3000; round++) {
        int amount = round % 3 == 0 ? 10 : round % 3 == 1 ? 30 : 100;
        seed = (uint32_t)round * 2654435761u + 1;
        memcpy(&copy, &pattern, sizeof copy);
        if (round & 1) memset(track->step, 0x55, 8 * sizeof track->step[0]);    /* built from the original only */
        mk_mutate(track, original, amount, random_);
        CHECK_EQ(track->length, 8);
        CHECK_EQ(used(track), 6);       /* a rest stays a rest unless a note moves into it */
        CHECK_EQ(memcmp(&track->step[8], &original->step[8], (MK_STEPS - 8) * sizeof track->step[0]), 0);
        int velocities = 0, ratchets = 0;
        for (int s = 0; s < 8; s++) {
            const mk_step *st = &track->step[s], *was = &original->step[s];
            CHECK_EQ(st->controller[MK_CTL_MOD], was->controller[MK_CTL_MOD]);  /* automation stays in time */
            ratchets += st->ratchet == 3;
            if (!mk_step_used(st)) continue;
            velocities += st->velocity[0];
            moved += st->velocity[0] != was->velocity[0];
            harmonies += st->note[1] && st->velocity[0] != 94;
            for (int i = 0; i < MK_POLY && st->note[i]; i++) {
                int pitch = st->note[i] % 12, exact = 0;
                CHECK_EQ(pitch == 0 || pitch == 2 || pitch == 4 || pitch == 5 || pitch == 7 || pitch == 9 || pitch == 11, 1);
                CHECK_EQ(st->note[i] >= 60 - 12 && st->note[i] <= 72 + 12, 1);  /* within an octave of the range */
                CHECK_EQ(st->velocity[i] >= 1 && st->velocity[i] <= 127, 1);
                for (int o = 0; o < 8; o++)
                    for (int j = 0; j < 2; j++) exact |= original->step[o].note[j] == st->note[i];
                octaves += !exact;
            }
        }
        CHECK_EQ(velocities, 90 + 91 + 93 + 94 + 95 + 97);      /* the same hits, wherever they went */
        CHECK_EQ(ratchets, 1);
        CHECK_EQ(changed(track, original) > 0, 1);              /* never the original again */
        changes[round % 3] += changed(track, original);
        differ_from_last += memcmp(last, track->step, sizeof last) != 0;
        memcpy(last, track->step, sizeof last);
    }
    CHECK_EQ(differ_from_last > 2950, 1);
    CHECK_EQ(changes[0] < changes[1] && changes[1] < changes[2], 1);    /* more amount, more change */
    CHECK_EQ(changes[0] > 1000 && changes[2] < 7900, 1);        /* of 8000 steps each: a little, and not all */
    CHECK_EQ(moved > 0 && octaves > 0 && harmonies > 0, 1);     /* C major has all seven: the track has a chord */
    printf("   steps changed of 8000 at amounts 10, 30, 100: %ld, %ld, %ld; %ld notes outside the original's\n",
           changes[0], changes[1], changes[2], octaves);
    seed = 99;                          /* the same numbers, the same variation */
    memcpy(&copy, &pattern, sizeof copy);
    mk_mutate(track, original, 30, random_);
    memcpy(last, track->step, sizeof last);
    seed = 99;
    mk_mutate(track, original, 30, random_);
    CHECK_EQ(memcmp(last, track->step, sizeof last), 0);
    ok("every variation keeps the hits, the velocities and the scale, differs, and goes as far as the amount", before);
}

static void test_scale(void) {
    int before = failures;
    mk_pattern_init(&pattern);
    mk_track *original = &pattern.track[0], *track = &copy.track[0];
    static const uint8_t minor[5] = { 57, 60, 62, 64, 67 };     /* A C D E G: a pentatonic, and nothing else */
    original->length = 16;
    for (int s = 0; s < 16; s++) {
        original->step[s].note[0] = (uint8_t)(minor[s % 5] + (s > 9 ? 12 : 0));
        original->step[s].velocity[0] = 100;
    }
    for (int round = 0; round < 500; round++) {
        seed = (uint32_t)round * 40503u + 3;
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, 100, random_);
        for (int s = 0; s < 16; s++) {
            int pitch = track->step[s].note[0] % 12;
            CHECK_EQ(pitch == 9 || pitch == 0 || pitch == 2 || pitch == 4 || pitch == 7, 1);
        }
    }
    original->length = 4;               /* C C# D D#: no scale fits, so only these four are used */
    for (int s = 0; s < 4; s++) original->step[s].note[0] = (uint8_t)(60 + s);
    for (int round = 0; round < 500; round++) {
        seed = (uint32_t)round * 40503u + 3;
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, 100, random_);
        for (int s = 0; s < 4; s++) CHECK_EQ(track->step[s].note[0] % 12 < 4, 1);
    }
    ok("the track's own notes set the scale: a pentatonic stays one, and notes no scale has stay themselves", before);
}

static void test_amount(void) {
    int before = failures;
    CHECK_EQ(mk_mutate_amount(0), 30);
    CHECK_EQ(mk_mutate_amount(249), 30);
    CHECK_EQ(mk_mutate_amount(250), 0);
    CHECK_EQ(mk_mutate_amount(750), 50);
    CHECK_EQ(mk_mutate_amount(1249), 99);
    CHECK_EQ(mk_mutate_amount(1250), 100);
    CHECK_EQ(mk_mutate_amount(1583), 50);
    CHECK_EQ(mk_mutate_amount(1915), 0);
    CHECK_EQ(mk_mutate_amount(1916 + 500), 50);
    for (uint32_t ms = 0; ms < 10000; ms++) CHECK_EQ(mk_mutate_amount(ms) >= 0 && mk_mutate_amount(ms) <= 100, 1);
    ok("the amount from how long Mutate is held: 30 for a tap, then up over a second and down again", before);
}

static void test_edges(void) {
    int before = failures;
    mk_pattern_init(&pattern);
    mk_track *original = &pattern.track[0], *track = &copy.track[0];
    memcpy(&copy, &pattern, sizeof copy);
    seed = 7;
    mk_mutate(track, original, 30, random_);            /* empty: stays empty */
    CHECK_EQ(used(track), 0);
    original->length = 1;                               /* one step of one note: an octave is all there is */
    original->step[0].note[0] = 60;
    original->step[0].velocity[0] = 100;
    for (int round = 0; round < 50; round++) {
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, 30, random_);
        CHECK_EQ(track->step[0].note[0] == 72 || track->step[0].note[0] == 48, 1);
    }
    original->length = 64;                              /* a full track of the highest and lowest notes */
    for (int s = 0; s < 64; s++) {
        for (int i = 0; i < MK_POLY; i++) original->step[s].note[i] = (uint8_t)(s & 1 ? 1 + i : 127 - i);
        for (int i = 0; i < MK_POLY; i++) original->step[s].velocity[i] = 100;
    }
    for (int round = 0; round < 200; round++) {
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, 1 + round % 100, random_);
        CHECK_EQ(used(track), 64);
        for (int s = 0; s < 64; s++)
            for (int i = 0; i < MK_POLY; i++) CHECK_EQ(track->step[s].note[i] >= 1 && track->step[s].note[i] <= 127, 1);
    }
    memcpy(&copy, &pattern, sizeof copy);
    mk_mutate(track, original, 30, NULL);               /* no random numbers: the original */
    CHECK_EQ(memcmp(track->step, original->step, sizeof track->step), 0);
    memset(track->step, 0x55, sizeof track->step);
    mk_mutate(track, original, 0, random_);             /* no amount: the original */
    CHECK_EQ(memcmp(track->step, original->step, sizeof track->step), 0);
    ok("empty, one step, the ends of the range, a full track, no amount", before);
}

int main(void) {
    test_variations();
    test_scale();
    test_amount();
    test_edges();
    if (failures) printf("%d checks failed\n", failures);
    return failures != 0;
}
