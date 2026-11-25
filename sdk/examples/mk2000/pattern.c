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

/* A number that depends on the dice, the step and which of the two amounts it is for, well mixed. */
static uint32_t spice_number(const mk_track *track, int step, uint32_t which) {
    uint32_t x = (track->dice + 1u) * 0x9E3779B1u ^ (uint32_t)(step + 1) * 0x85EBCA6Bu ^ which * 0xC2B2AE35u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    return x ^ x >> 15;
}

static int spice_of(const mk_track *track) { return track->spice > 100 ? 100 : track->spice; }

int mk_spice_velocity(const mk_track *track, int step, int velocity) {
    int amount = (int)(spice_number(track, step, 1) % 201) - 100;      /* -100 to 100 */
    velocity += spice_of(track) * amount * 24 / 10000;
    return velocity < 1 ? 1 : velocity > 127 ? 127 : velocity;
}

int mk_spice_gate(const mk_track *track, int step, int ticks, int most) {
    int amount = (int)(spice_number(track, step, 2) % 306) - 85;       /* -85 to 220 per cent */
    ticks = ticks * (10000 + spice_of(track) * amount) / 10000;
    return ticks < 1 ? 1 : ticks > most ? most : ticks;
}

int mk_track_used(const mk_track *track) {
    for (int s = 0; s < track->length; s++)
        if (mk_step_used(&track->step[s])) return 1;
    return 0;
}
