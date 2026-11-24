/* Host tests for clock.c: pulses at chosen times, a model of the tick timer, and every tick
 * counted against the pulses that have come. */
#include <stdio.h>
#include "mk.h"

static int failures;

static void check_eq(long got, long want, const char *expr, const char *file, int line) {
    if (got == want) return;
    failures++;
    printf("%s:%d: %s is %ld, expected %ld\n", file, line, expr, got, want);
}

static void check(int ok, const char *what, const char *file, int line) {
    if (ok) return;
    failures++;
    printf("%s:%d: failed: %s\n", file, line, what);
}

#define CHECK(cond) check((cond) != 0, #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) check_eq((long)(got), (long)(want), #got, __FILE__, __LINE__)

/* ---- the board: a microsecond clock and the tick timer, as ks_timer_start does it ---- */

static uint64_t now, next_fire;
static uint32_t period, per;
static long ticks, pulses;              /* since the last arm, for the invariants */
static long ahead;                      /* times a tick ran ahead of the pulses */
static uint64_t last_tick;
static uint32_t gap_min, gap_max;

static void io_tick(void) {
    ticks++;
    if (ticks > (long)per * pulses) ahead++;
    uint64_t gap = now - last_tick;
    if (last_tick) {
        if (gap < gap_min) gap_min = (uint32_t)gap;
        if (gap > gap_max) gap_max = (uint32_t)gap;
    }
    last_tick = now;
}

static void io_restart(uint32_t us) {
    period = us < 50 ? 50 : us > 65536 ? 65536 : us;
    next_fire = now + period;
}

static const mk_clock_io io = { io_tick, io_restart };

static void run_until(uint64_t t) {
    while (next_fire && next_fire <= t) {
        now = next_fire;
        next_fire += period;
        mk_clock_timer();
    }
    now = t;
}

static void pulse_at(uint64_t t) {
    run_until(t);
    pulses++;
    mk_clock_pulse((uint32_t)t);
}

static void follow(uint32_t ticks_per_pulse, uint32_t guess) {
    mk_clock_init(&io);
    mk_clock_follow((int)ticks_per_pulse, guess);
    per = ticks_per_pulse;
    now = 1000;
    next_fire = 0;
    ticks = pulses = ahead = 0;
    last_tick = 0;
}

static void reset_gaps(void) {
    gap_min = 0xFFFFFFFF;
    gap_max = 0;
}

static uint32_t seed = 2024;
static uint32_t rnd(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

/* ---- tests ---- */

static void test_steady(void) {
    follow(4, 20833);
    reset_gaps();
    int caught = 1;
    for (int k = 1; k <= 2000; k++) {
        pulse_at(1000 + (uint64_t)k * 20833);
        caught &= ticks == 4L * k - 3;
        if (k == 3) reset_gaps();
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    CHECK(gap_min >= 5200 && gap_max <= 5215);
    CHECK_EQ(mk_clock_interval(), 20833);
    printf("ok steady: 4 ticks per clock, spaced %u to %u us\n", gap_min, gap_max);
}

/* 60 to 240 BPM and back, a little faster every clock. */
static void test_ramp(void) {
    follow(4, 41667);
    uint64_t t = 1000;
    int caught = 1;
    long bursts = 0;
    for (int k = 1; k <= 6000; k++) {
        int x = k <= 3000 ? k : 6000 - k;
        uint32_t interval = (uint32_t)(41667 - (41667 - 10417) * (long)x / 3000);
        t += interval;
        run_until(t);
        long before = ticks;
        pulse_at(t);
        caught &= ticks == 4L * k - 3;
        if (ticks - before > 2) bursts++;               /* more than the clock's own tick and one owed */
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    CHECK(bursts < 10);
    printf("ok ramp 60 to 240 BPM and back: nothing lost or doubled, %ld catch-ups of more than one tick\n", bursts);
}

static void test_jitter(void) {
    follow(4, 20833);
    uint64_t t = 1000;
    int caught = 1;
    reset_gaps();
    for (int k = 1; k <= 5000; k++) {
        t += 20833 - 2000 + rnd() % 4001;
        pulse_at(t);
        caught &= ticks == 4L * k - 3;
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    uint32_t interval = mk_clock_interval();
    CHECK(interval > 19800 && interval < 21900);
    printf("ok jitter of 2 ms: every clock worth 4 ticks, measured %u us\n", interval);
}

static void test_jumps(void) {
    static const uint32_t intervals[] = { 20833, 8333, 62500, 20833, 10417, 41667 };
    follow(4, 20833);
    uint64_t t = 1000;
    int caught = 1;
    long k = 0;
    for (int j = 0; j < 6; j++) {
        for (int i = 0; i < 50; i++) {
            t += intervals[j];
            pulse_at(t);
            caught &= ticks == 4 * ++k - 3;
        }
        CHECK_EQ(mk_clock_interval(), intervals[j]);
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    printf("ok tempo jumps 120, 300, 40, 120, 240, 60 BPM taken at once\n");
}

/* Play arms: the ticks owed are dropped at the next clock, which starts the count again.
 * Hold: clocks are counted, nothing plays, and letting go plays what is due by then. */
static void test_arm_and_hold(void) {
    follow(4, 20833);
    uint64_t t = 1000;
    for (int k = 0; k < 10; k++) pulse_at(t += 20833);
    run_until(t + 12000);                               /* three of the last four played */
    CHECK_EQ(ticks, 40 - 4 + 3);
    mk_clock_arm();
    mk_clock_hold(1);
    run_until(t + 20000);
    CHECK_EQ(ticks, 39);
    ticks = pulses = 0;
    pulse_at(t += 20833);                               /* the start: nothing yet, held */
    CHECK_EQ(ticks, 0);
    run_until(t + 6000);
    mk_clock_hold(0);                                   /* the first two are due */
    CHECK_EQ(ticks, 2);
    pulse_at(t += 20833);
    CHECK_EQ(ticks, 5);
    mk_clock_hold(1);
    pulse_at(t += 20833);
    pulse_at(t += 20833);
    run_until(t + 11000);
    CHECK_EQ(ticks, 5);
    mk_clock_hold(0);
    CHECK_EQ(ticks, 4 * 4 - 4 + 3);
    pulse_at(t += 20833);
    CHECK_EQ(ticks, 4 * 5 - 3);
    CHECK_EQ(ahead, 0);
    mk_clock_follow(0, 0);
    pulse_at(t += 20833);
    run_until(t + 20000);
    CHECK_EQ(ticks, 17);
    printf("ok arm drops the ticks owed, hold counts and lets go of what is due\n");
}

/* The sync jack: a pulse per sixteenth, 24 ticks each; at 30 BPM a pulse every half second. */
static void test_jack(void) {
    follow(24, 125000);
    reset_gaps();
    uint64_t t = 1000;
    int caught = 1;
    for (int k = 1; k <= 200; k++) {
        pulse_at(t += 500000);
        caught &= ticks == 24L * k - 23;
        if (k == 2) reset_gaps();
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    CHECK(gap_min >= 20800 && gap_max <= 20850);
    printf("ok sync jack at 30 BPM: 24 ticks per pulse, %u to %u us apart\n", gap_min, gap_max);
    /* slower than the timer can space out: counted, never measured */
    for (int k = 0; k < 3; k++) pulse_at(t += 2000000);
    CHECK_EQ(ticks, 24 * 203 - 23);
    CHECK_EQ(mk_clock_interval(), 500000);
    printf("ok pulses too slow to measure still count 24 ticks each\n");
}

/* A long run of random tempos, jumps and jitter: the count never runs ahead and is always made
 * up at each clock. After clock k the first tick of it has played: 4k - 3. */
static void test_long(void) {
    follow(4, 20833);
    uint64_t t = 1000;
    uint32_t interval = 20833;
    int caught = 1;
    for (long k = 1; k <= 1000000; k++) {
        if (rnd() % 500 == 0) interval = 2083 + rnd() % 81250;      /* 30 to 300 BPM */
        t += interval - interval / 20 + rnd() % (interval / 10 + 1);
        pulse_at(t);
        if (ticks != 4 * k - 3) caught = 0;
    }
    CHECK(caught);
    CHECK_EQ(ahead, 0);
    printf("ok %ld ticks over a million clocks at random tempos, none ahead, none lost\n", ticks);
}

int main(void) {
    test_steady();
    test_ramp();
    test_jitter();
    test_jumps();
    test_arm_and_hold();
    test_jack();
    test_long();
    if (failures) printf("%d failed\n", failures);
    return failures != 0;
}
