/* Following an external clock (mk.h). `owed` counts the ticks of the pulses so far that have not
 * played; `slots` the timer periods left before the next pulse is due. Everything is played
 * through `due()`: what is owed beyond the slots still to come. */
#include "mk.h"

#define LONGEST_TICK 65536u     /* the tick timer's longest period: slower pulses are not measured */

static const mk_clock_io *io;
static uint32_t per_pulse, owed, slots;
static uint32_t interval, last;
static uint8_t measured, have_last, held;

void mk_clock_init(const mk_clock_io *callbacks) {
    io = callbacks;
    mk_clock_follow(0, 0);
}

void mk_clock_follow(int ticks, uint32_t guess_us) {
    per_pulse = ticks > 0 ? (uint32_t)ticks : 0;
    owed = slots = 0;
    interval = guess_us;
    measured = have_last = held = 0;
}

static void due(void) {
    while (!held && owed > slots) {
        owed--;
        io->tick();
    }
}

/* Small changes are averaged out over a few pulses, so jitter does not shake the spacing; a jump
 * of more than an eighth is taken at once. */
static void measure(uint32_t gap) {
    if (gap / per_pulse > LONGEST_TICK) return;
    uint32_t change = gap > interval ? gap - interval : interval - gap;
    interval = !measured || change > interval / 8 ? gap : (3 * interval + gap + 2) / 4;
    measured = 1;
}

void mk_clock_pulse(uint32_t now) {
    if (!per_pulse) return;
    if (have_last) measure(now - last);
    last = now;
    have_last = 1;
    owed += per_pulse;
    slots = interval ? per_pulse - 1 : 0;               /* no idea of the spacing: all at once */
    due();
    if (interval) io->restart(interval / per_pulse);
}

void mk_clock_timer(void) {
    if (!per_pulse || !slots) return;
    slots--;
    due();
}

void mk_clock_arm(void) { owed = 0; }

void mk_clock_hold(int on) {
    held = on != 0;
    due();
}

uint32_t mk_clock_interval(void) { return measured ? interval : 0; }
