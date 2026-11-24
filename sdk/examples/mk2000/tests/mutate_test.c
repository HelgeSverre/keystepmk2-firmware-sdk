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

static void test_variations(void) {
    int before = failures;
    mk_pattern_init(&pattern);
    mk_track *original = &pattern.track[0], *track = &copy.track[0];
    fill(original);
    long moved = 0, borrowed = 0, octaves = 0, differ_from_last = 0;
    static mk_step last[8];
    for (int round = 0; round < 2000; round++) {
        seed = (uint32_t)round * 2654435761u + 1;
        memcpy(&copy, &pattern, sizeof copy);
        if (round & 1) memset(track->step, 0x55, 8 * sizeof track->step[0]);    /* built from the original only */
        mk_mutate(track, original, random_);
        CHECK_EQ(track->length, 8);
        CHECK_EQ(used(track), 6);
        CHECK_EQ(memcmp(&track->step[8], &original->step[8], (MK_STEPS - 8) * sizeof track->step[0]), 0);
        int velocities = 0, same = 0, ratchets = 0;
        for (int s = 0; s < 8; s++) {
            const mk_step *st = &track->step[s], *was = &original->step[s];
            CHECK_EQ(st->controller[MK_CTL_MOD], was->controller[MK_CTL_MOD]);  /* automation stays in time */
            ratchets += st->ratchet == 3;
            if (!mk_step_used(st)) continue;
            velocities += st->velocity[0];
            moved += st->velocity[0] != was->velocity[0];
            int pitch = st->note[0] % 12, known = 0, exact = 0;
            for (int o = 0; o < 8; o++) {
                known |= original->step[o].note[0] && original->step[o].note[0] % 12 == pitch;
                exact |= original->step[o].note[0] == st->note[0];
            }
            CHECK_EQ(known, 1);         /* every note is one of the track's, in some octave */
            octaves += !exact;
            borrowed += exact && st->velocity[0] >= 90 && st->note[0] != original->step[st->velocity[0] - 90].note[0];
            for (int i = 0; i < MK_POLY && st->note[i]; i++) CHECK_EQ(st->velocity[i] >= 1 && st->velocity[i] <= 127, 1);
            same += st->note[0] == was->note[0] && st->note[1] == was->note[1];
        }
        CHECK_EQ(velocities, 90 + 91 + 93 + 94 + 95 + 97);      /* the same hits, wherever they went */
        CHECK_EQ(ratchets, 1);
        CHECK_EQ(same < 6, 1);          /* never the original again */
        differ_from_last += memcmp(last, track->step, sizeof last) != 0;
        memcpy(last, track->step, sizeof last);
    }
    CHECK_EQ(differ_from_last > 1990, 1);
    CHECK_EQ(moved > 2000 && moved < 8000, 1);          /* of 12000 steps: some move, most stay */
    CHECK_EQ(borrowed > 1500 && borrowed < 6000, 1);
    CHECK_EQ(octaves > 500 && octaves < 3000, 1);
    printf("   of 12000 steps over 2000 variations: %ld moved, %ld took another step's note, %ld an octave away\n", moved,
           borrowed, octaves);
    ok("every variation keeps the hits, the velocities and the track's notes, and differs", before);
}

static void test_edges(void) {
    int before = failures;
    mk_pattern_init(&pattern);
    mk_track *original = &pattern.track[0], *track = &copy.track[0];
    memcpy(&copy, &pattern, sizeof copy);
    seed = 7;
    mk_mutate(track, original, random_);                /* empty: stays empty */
    CHECK_EQ(used(track), 0);
    original->length = 1;                               /* one step: an octave is all there is */
    original->step[0].note[0] = 60;
    original->step[0].velocity[0] = 100;
    for (int round = 0; round < 50; round++) {
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, random_);
        CHECK_EQ(track->step[0].note[0] == 72 || track->step[0].note[0] == 48, 1);
    }
    original->step[0].note[0] = 127;                    /* nowhere to go but down */
    original->step[0].note[1] = 120;
    original->step[0].velocity[1] = 100;
    for (int round = 0; round < 50; round++) {
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, random_);
        CHECK_EQ(track->step[0].note[0], 115);
        CHECK_EQ(track->step[0].note[1], 108);
    }
    original->length = 64;                              /* a full track of the highest and lowest notes */
    for (int s = 0; s < 64; s++) {
        for (int i = 0; i < MK_POLY; i++) original->step[s].note[i] = (uint8_t)(s & 1 ? 1 + i : 127 - i);
        for (int i = 0; i < MK_POLY; i++) original->step[s].velocity[i] = 100;
    }
    for (int round = 0; round < 200; round++) {
        memcpy(&copy, &pattern, sizeof copy);
        mk_mutate(track, original, random_);
        CHECK_EQ(used(track), 64);
        for (int s = 0; s < 64; s++)
            for (int i = 0; i < MK_POLY; i++) CHECK_EQ(track->step[s].note[i] >= 1 && track->step[s].note[i] <= 127, 1);
    }
    memcpy(&copy, &pattern, sizeof copy);
    mk_mutate(track, original, NULL);                   /* no random numbers: the original */
    CHECK_EQ(memcmp(track->step, original->step, sizeof track->step), 0);
    ok("empty, one step, the ends of the range, a full track", before);
}

int main(void) {
    test_variations();
    test_edges();
    if (failures) printf("%d checks failed\n", failures);
    return failures != 0;
}
