/* Mutate: a variation of a track. No hardware access.
 *
 * The rule is the stock firmware's (1.0.2, read from its code: docs/STOCK-UI.md, Mutate), fitted
 * to this pattern format:
 *
 *   - Every variation starts from the original.
 *   - The track's own notes say what scale it is in: the notes that every fitting transposition of
 *     eight scales has in common, or the notes themselves if none fits. Pitches move by members
 *     of that set ("degrees"), so a variation stays in the key the track was played in.
 *   - `amount`, 0 to 100, sets how many operations run (amount / 20 + 1 a round) and how much of
 *     the track each one covers.
 *   - An operation is one of seven: transpose a stretch by up to three degrees, mirror a stretch
 *     about a degree, swap two blocks of steps, reverse a stretch, add a harmony note (only where
 *     the track already has chords), write a run up or down the scale, move a note into a rest.
 *   - A note that ends more than an octave outside the original's range comes back by octaves
 *     until it is within one (the stock firmware brings it back one octave only).
 *
 * Steps keep their velocities, gates and other fields and travel whole; a rest stays a rest
 * unless a note moves into it, so as many steps play as before. Recorded controllers stay where
 * they are in time.
 *
 * Differences from the stock firmware: it keeps three hidden copies of the pattern that only
 * block swaps reach, and its harmony notes get a longer gate; neither is done here. It seeds
 * itself from a clock, so no variation of its could be reproduced anyway. */
#include "mk.h"

typedef struct {
    mk_track *track;
    uint32_t (*random)(void);
    uint16_t scale;             /* bit n: pitch class n is a degree */
    uint8_t degrees, root, low, high, chords, dense, length, amount;
} mutation;

static uint32_t roll(mutation *m, uint32_t n) { return m->random() % n; }

static int in_scale(const mutation *m, int note) { return m->scale >> (note % 12) & 1; }

/* The scale a set of pitch classes belongs to, as the stock firmware finds it. */
static uint16_t infer_scale(uint16_t classes) {
    static const uint16_t scales[8] = { 0x295, 0x8D1, 0x29D, 0xAB5, 0x9AD, 0x9CD, 0x55B, 0x9B3 };
    uint16_t common = 0xFFF;
    for (int s = 0; s < 8; s++) {
        uint16_t scale = scales[s];
        for (int turn = 0; turn < 12; turn++) {
            if ((classes & scale) == classes) common &= scale;
            scale = (uint16_t)(((scale << 12 | scale) & 0x1FFF) >> 1);
        }
    }
    return common == 0xFFF ? classes : common;
}

/* The commonest pitch class, unless it is a third or a fifth above the next commonest. */
static int infer_root(uint8_t *count) {
    int first = 0, second = 0;
    for (int c = 1; c < 12; c++)
        if (count[c] > count[first]) first = c;
    uint8_t most = count[first];
    count[first] = 0;
    for (int c = 1; c < 12; c++)
        if (count[c] > count[second]) second = c;
    if (!most || !count[second]) return first;
    int up = (second - first + 12) % 12, down = (first - second + 12) % 12;
    if (up != 7 && up != 3 && up != 4 && (down == 7 || down == 3 || down == 4)) return second;
    return first;
}

/* `note` moved `by` degrees; a note that is no degree moves an octave. 0 if it leaves 1 to 127. */
static int degree(const mutation *m, int note, int by) {
    if (!in_scale(m, note)) note += by > 0 ? 12 : by < 0 ? -12 : 0;
    else
        for (; by && note >= 1 && note <= 127; by += by > 0 ? -1 : 1) {
            do note += by > 0 ? 1 : -1;
            while (note >= 1 && note <= 127 && !in_scale(m, note));
        }
    return note >= 1 && note <= 127 ? note : 0;
}

static int move(const mutation *m, uint8_t *note, int by) {
    int to = degree(m, *note, by);
    if (!to || to == *note) return 0;
    *note = (uint8_t)to;
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

static void transpose(mutation *m, int from, int to) {
    int by = (int)roll(m, 7) - 3;
    for (int s = from; s < to; s++)
        for (int i = 0; i < MK_POLY && m->track->step[s].note[i]; i++) move(m, &m->track->step[s].note[i], by);
}

/* Mirror about a degree: a note as far above it comes out as far below. */
static void invert(mutation *m, int from, int to) {
    int pivot = (int)roll(m, m->degrees);
    for (int s = from; s < to; s++)
        for (int i = 0; i < MK_POLY && m->track->step[s].note[i]; i++) {
            uint8_t *note = &m->track->step[s].note[i];
            int index = 0;
            for (int c = m->root; c != *note % 12; c = (c + 1) % 12) index += m->scale >> ((c + 1) % 12) & 1;
            int by = (pivot - index) * 2;
            while (by > m->degrees) by -= m->degrees;
            while (by < -m->degrees) by += m->degrees;
            move(m, note, by);
        }
}

/* Two blocks of steps change places; the more amount, the longer the blocks. */
static void block_swap(mutation *m) {
    int n = m->length / 2 * m->amount / 100;
    if (n < 1) n = 1;
    if (m->length < 2 * n) return;
    int a = (int)roll(m, (uint32_t)(m->length - 2 * n + 1)), b = a + n + (int)roll(m, (uint32_t)(m->length - 2 * n - a + 1));
    for (int i = 0; i < n; i++) swap(&m->track->step[a + i], &m->track->step[b + i]);
}

static void reverse(mutation *m, int from, int to) {
    for (to--; from < to; from++, to--) swap(&m->track->step[from], &m->track->step[to]);
}

/* A second note a third, a fourth or a fifth of the scale above the first. */
static void harmony(mutation *m, int from, int to) {
    for (int s = from; s < to; s++) {
        mk_step *step = &m->track->step[s];
        if (!step->note[0] || step->note[1]) continue;
        int note = degree(m, step->note[0], (int)roll(m, 3) + 2);
        if (!note) continue;
        step->note[1] = (uint8_t)note;
        step->velocity[1] = step->velocity[0];
    }
}

/* A run along the scale over the steps that play, starting near the note before it and ending
 * when it is an octave from there. */
static void run(mutation *m, int from, int to) {
    int base = 0, by = roll(m, 2) ? -2 : 1;
    for (int s = from; s >= 0 && !base; s--) base = m->track->step[s].note[0];
    for (int s = m->length - 1; s > from && !base; s--) base = m->track->step[s].note[0];
    int note = degree(m, base, (int)(roll(m, 12) % m->degrees));
    for (int s = from; s < to && note; s++) {
        if (note - base > 12 || base - note > 12) return;
        if (!m->track->step[s].note[0]) continue;
        m->track->step[s].note[0] = (uint8_t)note;
        note = degree(m, note, by);
    }
}

/* The first rest of the stretch, from either end, takes the next step that plays. */
static void shift(mutation *m, int from, int to) {
    int by = roll(m, 2) ? 1 : -1, s = by > 0 ? from : to - 1, rest = -1;
    for (; s >= from && s < to; s += by) {
        if (rest < 0 && !mk_step_used(&m->track->step[s])) rest = s;
        else if (rest >= 0 && mk_step_used(&m->track->step[s])) {
            swap(&m->track->step[rest], &m->track->step[s]);
            return;
        }
    }
}

/* One operation, as the stock firmware picks it; 0 if this draw is turned down. */
static int operate(mutation *m) {
    uint32_t r = roll(m, 256);
    int kind = (int)(r % 7), rare = r % (uint32_t)(11 - m->amount * 3 / 4 / 10) != 0;
    if ((kind == 2 || kind == 3) && rare && m->dense < 50) return 0;    /* sparse tracks are seldom reordered */
    if (kind == 4 && m->chords < 2) return 0;
    if (kind == 5 && rare) return 0;
    int width = (int)roll(m, 21) + m->amount - 10;
    width = width < 0 ? 0 : width > 100 ? 100 : width;
    int n = m->length * width / 100;
    if (n < 1) n = 1;
    int from = (int)roll(m, (uint32_t)(m->length - n + 1)), to = from + n;
    switch (kind) {
    case 0: transpose(m, from, to); break;
    case 1: invert(m, from, to); break;
    case 2: block_swap(m); break;
    case 3: reverse(m, from, to); break;
    case 4: harmony(m, from, to); break;
    case 5: run(m, from, to); break;
    default: shift(m, from, to); break;
    }
    return 1;
}

static void round_of(mutation *m) {
    for (int done = 0, tries = 0; done <= m->amount / 20 && tries < 200; tries++) done += operate(m);
}

static int same_notes(const mk_track *a, const mk_track *b, int length) {
    for (int s = 0; s < length; s++)
        for (int i = 0; i < MK_POLY; i++)
            if (a->step[s].note[i] != b->step[s].note[i]) return 0;
    return 1;
}

int mk_mutate_amount(uint32_t held_ms) {
    if (held_ms < 250) return 30;
    uint32_t t = (held_ms - 250) % 1666;
    return (int)(t < 1000 ? t / 10 : (166600 - 100 * t) / 666);
}

void mk_mutate(mk_track *track, const mk_track *original, int amount, uint32_t (*random)(void)) {
    mutation m = { .track = track, .random = random, .low = 127 };
    m.length = track->length < 1 ? 1 : track->length > MK_STEPS ? MK_STEPS : track->length;
    m.amount = (uint8_t)(amount < 0 ? 0 : amount > 100 ? 100 : amount);
    uint8_t count[12] = { 0 };
    uint16_t classes = 0;
    int active = 0;
    for (int s = 0; s < m.length; s++) {
        const mk_step *step = &original->step[s];
        track->step[s] = *step;
        active += mk_step_used(step);
        for (int i = 0; i < MK_POLY && step->note[i]; i++) {
            classes |= (uint16_t)(1 << step->note[i] % 12);
            count[step->note[i] % 12]++;
            if (step->note[i] < m.low) m.low = step->note[i];
            if (step->note[i] > m.high) m.high = step->note[i];
            if (i + 1 > m.chords) m.chords = (uint8_t)(i + 1);
        }
    }
    if (!active || !random || !m.amount) return;
    m.scale = infer_scale(classes);
    for (int c = 0; c < 12; c++) m.degrees += m.scale >> c & 1;
    m.root = (uint8_t)infer_root(count);
    int dense = 200 * active * active / (m.length * m.length);
    m.dense = (uint8_t)(dense > 100 ? 100 : dense);

    for (int again = 0; again < 5; again++) {           /* the first round, until it has changed a note */
        round_of(&m);
        if (!same_notes(track, original, m.length)) break;
    }
    for (int copy = 1; copy < 4 && copy * m.length < 64; copy++) round_of(&m);  /* short tracks get more */
    for (int s = 0; s < m.length; s++)
        for (int i = 0; i < MK_POLY && track->step[s].note[i]; i++) {
            uint8_t *note = &track->step[s].note[i];
            while (*note > m.high + 12) *note -= 12;
            while (*note + 12 < m.low) *note += 12;
        }
    for (int s = 0; s < m.length && same_notes(track, original, m.length); s++) {  /* a variation always differs */
        uint8_t *note = &track->step[s].note[0];
        if (*note && !move(&m, note, 1) && !move(&m, note, -1)) *note = (uint8_t)(*note > 12 ? *note - 12 : *note + 12);
    }
}
