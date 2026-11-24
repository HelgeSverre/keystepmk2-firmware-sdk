/* Mutate: a variation of a track. No hardware access.
 *
 * What the stock firmware's Mutate does, as far as the emulator showed (docs/STOCK-UI.md): each
 * press makes a new variation of the original, not of the last variation; as many steps play as
 * before, with the velocities they had; some have moved a step, some play a note from elsewhere in
 * the track, a few are an octave up or down. The rule below gives that; the proportions are
 * estimates from a handful of presses. */
#include "mk.h"

static int roll(uint32_t (*random)(void), int n) { return (int)(random() % (uint32_t)n); }

static int same_notes(const mk_step *a, const mk_step *b) {
    for (int i = 0; i < MK_POLY; i++)
        if (a->note[i] != b->note[i]) return 0;
    return 1;
}

/* Everything of two steps changes places except the recorded controllers, which stay where
 * they are in time. */
static void swap(mk_step *a, mk_step *b) {
    mk_step held = *a;
    *a = *b;
    *b = held;
    for (int c = 0; c < MK_CONTROLLERS; c++) {
        uint8_t value = a->controller[c];
        a->controller[c] = b->controller[c];
        b->controller[c] = value;
    }
}

/* The notes of `from`, with the velocities `to` has; a slot `to` never used takes its first. */
static void take_notes(mk_step *to, const mk_step *from) {
    for (int i = 0; i < MK_POLY; i++) {
        to->note[i] = from->note[i];
        if (!to->velocity[i]) to->velocity[i] = to->velocity[0];
    }
}

static int octave(mk_step *step, int by) {
    for (int i = 0; i < MK_POLY && step->note[i]; i++)
        if (step->note[i] + by < 1 || step->note[i] + by > 127) return 0;
    for (int i = 0; i < MK_POLY && step->note[i]; i++) step->note[i] = (uint8_t)(step->note[i] + by);
    return 1;
}

void mk_mutate(mk_track *track, const mk_track *original, uint32_t (*random)(void)) {
    int n = track->length < 1 ? 1 : track->length > MK_STEPS ? MK_STEPS : track->length;
    uint8_t used[MK_STEPS];
    int count = 0;
    for (int s = 0; s < n; s++) {
        track->step[s] = original->step[s];
        if (mk_step_used(&original->step[s])) used[count++] = (uint8_t)s;
    }
    if (!count || !random) return;
    for (int s = 0; s < n && n > 1; s++) {              /* one in six changes places with the next step */
        int next = (s + 1) % n;
        if ((mk_step_used(&track->step[s]) || mk_step_used(&track->step[next])) && roll(random, 6) == 0)
            swap(&track->step[s], &track->step[next]);
    }
    for (int s = 0; s < n; s++) {                       /* three in eight play another step's notes, one in eight moves an octave */
        mk_step *step = &track->step[s];
        if (!mk_step_used(step)) continue;
        int chance = roll(random, 8);
        if (chance < 3) take_notes(step, &original->step[used[roll(random, count)]]);
        else if (chance == 3 && !octave(step, roll(random, 3) ? 12 : -12)) octave(step, -12);
    }
    for (int s = 0; s < n; s++)                         /* a press always changes something */
        if (!same_notes(&track->step[s], &original->step[s])) return;
    mk_step *step = &track->step[used[roll(random, count)]];
    if (!octave(step, 12)) octave(step, -12);
}
