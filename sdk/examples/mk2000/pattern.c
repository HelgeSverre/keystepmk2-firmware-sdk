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

int mk_track_used(const mk_track *track) {
    for (int s = 0; s < track->length; s++)
        if (mk_step_used(&track->step[s])) return 1;
    return 0;
}
