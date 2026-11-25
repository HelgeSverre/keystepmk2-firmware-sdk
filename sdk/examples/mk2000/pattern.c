/* Pattern helpers and the tables everything shares. */
#include "mk.h"

const uint16_t mk_div_ticks[MK_DIV_COUNT] = { 288, 192, 128, 144, 96, 64, 72, 48, 32, 36, 24, 16, 18, 12, 8 };
const char *const mk_div_name[MK_DIV_COUNT] = {
    "1/2D", "1/2", "1/2T", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T",
    "1/16D", "1/16", "1/16T", "1/32D", "1/32", "1/32T",
};

void mk_pattern_init(mk_pattern *pattern) {
    uint8_t *bytes = (uint8_t *)pattern;
    for (size_t i = 0; i < sizeof *pattern; i++) bytes[i] = 0;
    pattern->tempo = 1200;
    for (int t = 0; t < MK_TRACKS; t++) {
        mk_track *track = &pattern->track[t];
        track->channel = (uint8_t)t;
        track->length = 16;
        track->division = MK_DIV_16;
        track->swing = 50;
        track->gate = 50;
        for (int s = 0; s < MK_STEPS; s++) {
            mk_step *step = &track->step[s];
            step->probability = 100;
            step->ratchet = 1;
            for (int c = 0; c < MK_CONTROLLERS; c++) step->controller[c] = MK_CTL_NONE;
        }
    }
}

int mk_step_used(const mk_step *step) { return step->note[0] != 0; }

/* Spice, by the stock firmware's rule (1.0.2, read from its code: docs/STOCK-UI.md, Gate, Spice
 * and Dice). The dice are a seed; from it come two numbers from 0 to 128 for each of 64 steps,
 * four steps to a turn of the generator, and the three amounts below are made from those two and
 * the Spice setting. So a step's amounts are the same on every pass and in proportion to Spice.
 * The stock firmware keeps a 16-bit seed in the pattern; here the track's dice byte is spread
 * over 16 bits. */
static void spice_numbers(const mk_track *track, int step, int *one, int *two) {
    static const uint8_t first[4] = { 8, 16, 4, 20 }, second[4] = { 22, 18, 10, 6 };
    uint32_t state = (uint32_t)(int32_t)(int16_t)((track->dice + 1u) * 0x9E37u);
    step &= 63;
    for (int turn = 0; turn <= step / 4; turn++) state = state * 0x0BB38435u + 0x3619636Bu;
    int a = 255 * (int8_t)(state >> first[step % 4]) >> 8, b = 255 * (int8_t)(state >> second[step % 4]) >> 8;
    *one = a < 0 ? -a : a;
    *two = b < 0 ? -b : b;
}

static int spice_of(const mk_track *track) { return (track->spice > 100 ? 100 : track->spice) * 255 / 100; }

int mk_spice_octave(const mk_track *track, int step) {
    int one, two, spice = spice_of(track);
    if (!spice) return 0;
    spice_numbers(track, step, &one, &two);
    return two * 6 < one * spice >> 8 ? 12 : one * 7 < spice * two >> 8 ? -12 : 0;
}

int mk_spice_velocity(const mk_track *track, int step, int velocity) {
    int one, two;
    spice_numbers(track, step, &one, &two);
    int twice = 2 * velocity + ((two - 64) * spice_of(track) >> 8);
    twice = twice < 0 ? -twice : twice > 255 ? 510 - twice : twice;
    return twice < 2 ? 1 : twice >> 1;
}

int mk_spice_gate(const mk_track *track, int step, int percent) {
    int one, two, spice = spice_of(track);
    if (!spice) return percent;
    spice_numbers(track, step, &one, &two);
    int level = 46 + (percent - 1) * 162 / 98;          /* the stock firmware's scale: 46 is no gate, 208 all of the step */
    if (level > 207) level = 207;
    level = ((256 - spice) * level >> 8) + (spice * one >> 7);
    if (level > 255) level = 510 - level;
    if (level <= 46) return 0;
    return level >= 208 ? 50 : (level - 46) * 98 / 162 + 1;
}

int mk_track_used(const mk_track *track) {
    for (int s = 0; s < track->length; s++)
        if (mk_step_used(&track->step[s])) return 1;
    return 0;
}
