/* Host tests for engine.c: every callback goes into a log with the tick it came in, and the
 * tests compare the log with the ticks the specification gives. */
#include <stdio.h>
#include "mk.h"

static int failures;

static void check(int ok, const char *what, const char *file, int line) {
    if (ok) return;
    failures++;
    printf("%s:%d: failed: %s\n", file, line, what);
}

static void check_eq(long got, long want, const char *expr, const char *file, int line) {
    if (got == want) return;
    failures++;
    printf("%s:%d: %s is %ld, expected %ld\n", file, line, expr, got, want);
}

#define CHECK(cond) check((cond) != 0, #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) check_eq((long)(got), (long)(want), #got, __FILE__, __LINE__)

/* ---- the log ---- */

enum { ON, OFF, CTL, RT, SWITCHED };

typedef struct {
    uint32_t tick;
    int kind, track;
    uint8_t channel, a, b;
} event;

#define LOG_MAX 4096
static event log_[LOG_MAX];
static int logged;
static uint32_t now;                    /* the tick being played, or the next one between ticks */

/* Every note on and off, checked as it happens: never the same note on twice, never an off
 * without its on. */
static uint8_t active[16][128];
static int doubled, unmatched;
static long notes_on;

static void add(int kind, int track, uint8_t channel, uint8_t a, uint8_t b) {
    if (logged < LOG_MAX) log_[logged++] = (event){ now, kind, track, channel, a, b };
}

static void (*on_note)(void);            /* a test hook run inside the note callback */

static void io_note(int track, uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity) {
        if (active[channel][note]) doubled++;
        active[channel][note] = 1;
        notes_on++;
    } else {
        if (!active[channel][note]) unmatched++;
        active[channel][note] = 0;
    }
    add(velocity ? ON : OFF, track, channel, note, velocity);
    if (on_note) on_note();
}

static void io_controller(int track, uint8_t channel, int which, uint8_t value) {
    add(CTL, track, channel, (uint8_t)which, value);
}

static void io_realtime(uint8_t byte) { add(RT, -1, 0, byte, 0); }
static void io_switched(void) { add(SWITCHED, -1, 0, 0, 0); }

/* Scripted random numbers first, then a fixed generator. */
static uint32_t script[64];
static int script_len, script_pos, draws;
static uint32_t seed = 12345;

static uint32_t xorshift(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static uint32_t io_random(void) {
    draws++;
    if (script_pos < script_len) return script[script_pos++];
    return xorshift();
}

static const mk_engine_io io = { io_note, io_controller, io_realtime, io_switched, io_random };
static mk_pattern pa, pb;

static void setup(void) {
    mk_pattern_init(&pa);
    mk_pattern_init(&pb);
    logged = 0;
    now = 0;
    script_len = script_pos = draws = 0;
    doubled = unmatched = 0;
    on_note = NULL;
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < 128; n++) active[c][n] = 0;
    mk_engine_init(&pa, &io);
}

static void run(int ticks) {
    for (int i = 0; i < ticks; i++) {
        mk_engine_tick();
        now++;
    }
}

static void set_note(mk_pattern *p, int t, int s, uint8_t note) {
    mk_step *st = &p->track[t].step[s];
    for (int i = 0; i < MK_POLY; i++) {
        if (st->note[i]) continue;
        st->note[i] = note;
        st->velocity[i] = 100;
        return;
    }
}

/* The ticks of the events of a kind for a track (-1: any) and pitch (-1: any), in order. */
static int collect(int kind, int track, int pitch, uint32_t *out, int max) {
    int n = 0;
    for (int i = 0; i < logged; i++) {
        const event *e = &log_[i];
        if (e->kind != kind || (track >= 0 && e->track != track)) continue;
        if (pitch >= 0 && e->a != pitch) continue;
        if (n < max) out[n] = e->tick;
        n++;
    }
    return n;
}

static int count(int kind, int track) {
    uint32_t ignored[1];
    return collect(kind, track, -1, ignored, 0);
}

/* The pitches a track was started with, in order. */
static int pitches(int track, uint8_t *out, int max) {
    int n = 0;
    for (int i = 0; i < logged; i++)
        if (log_[i].kind == ON && log_[i].track == track && n < max) out[n++] = log_[i].a;
    return n;
}

static int find(int kind, int track, int pitch, uint32_t tick) {
    for (int i = 0; i < logged; i++)
        if (log_[i].kind == kind && log_[i].track == track && log_[i].a == pitch && log_[i].tick == tick) return i;
    return -1;
}

static void check_ticks(const uint32_t *got, int n, const uint32_t *want, int m, const char *what, int line) {
    if (n != m) {
        failures++;
        printf("%s:%d: %s: %d events, expected %d\n", __FILE__, line, what, n, m);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (got[i] == want[i]) continue;
        failures++;
        printf("%s:%d: %s: event %d at tick %u, expected %u\n", __FILE__, line, what, i, got[i], want[i]);
        return;
    }
}

#define EXPECT_TICKS(kind, track, pitch, ...) do { \
        const uint32_t want_[] = { __VA_ARGS__ }; \
        uint32_t got_[64]; \
        int n_ = collect(kind, track, pitch, got_, 64); \
        check_ticks(got_, n_ > 64 ? 64 : n_, want_, (int)(sizeof want_ / sizeof want_[0]), #kind " " #track " " #pitch, __LINE__); \
    } while (0)

static void ok(const char *name, int before) {
    if (failures == before) printf("ok %s\n", name);
    else printf("FAILED %s\n", name);
}

static int all_quiet(void) {
    for (int c = 0; c < 16; c++)
        for (int n = 0; n < 128; n++)
            if (active[c][n]) return 0;
    return 1;
}

/* ---- tests ---- */

static void test_clock(void) {
    int before = failures;
    setup();
    run(10);
    CHECK_EQ(logged, 0);                                /* stopped: nothing */
    now = 0;
    mk_engine_start();
    CHECK_EQ(logged, 1);
    CHECK_EQ(log_[0].kind, RT);
    CHECK_EQ(log_[0].a, 0xFA);
    CHECK(mk_engine_playing());
    run(96);
    uint32_t got[32];
    int n = 0;
    for (int i = 0; i < logged; i++)
        if (log_[i].kind == RT && log_[i].a == 0xF8 && n < 32) got[n++] = log_[i].tick;
    CHECK_EQ(n, 24);
    for (int i = 0; i < n; i++) CHECK_EQ(got[i], 4 * i);
    mk_engine_stop();
    CHECK_EQ(log_[logged - 1].a, 0xFC);
    CHECK(!mk_engine_playing());
    int was = logged;
    run(50);
    CHECK_EQ(logged, was);
    mk_engine_start();                                  /* the clock starts again with the first tick */
    run(1);
    CHECK_EQ(log_[logged - 1].a, 0xF8);
    CHECK_EQ(log_[logged - 1].tick, 146);
    ok("clock", before);
}

static void test_four_steps(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    mk_engine_start();
    run(96);
    EXPECT_TICKS(ON, 0, -1, 0, 24, 48, 72);
    EXPECT_TICKS(OFF, 0, -1, 12, 36, 60, 84);
    uint8_t p[8];
    CHECK_EQ(pitches(0, p, 8), 4);
    for (int s = 0; s < 4; s++) CHECK_EQ(p[s], 60 + s);
    CHECK_EQ(log_[2].channel, 0);                     /* after Start and the first clock */
    CHECK_EQ(log_[2].b, 100);
    CHECK_EQ(mk_engine_position(0), 3);
    CHECK_EQ(mk_engine_step_phase(0), 23 * 256 / 24);
    CHECK_EQ(mk_engine_pass(0), 0);
    run(1);
    CHECK_EQ(mk_engine_position(0), 0);
    CHECK_EQ(mk_engine_pass(0), 1);
    mk_engine_stop();
    CHECK_EQ(mk_engine_position(0), 0);
    CHECK(all_quiet());
    ok("four steps at 1/16, gate 50", before);
}

static void test_polymeter(void) {
    int before = failures;
    setup();
    pa.track[0].length = 3;
    pa.track[1].length = 4;
    for (int s = 0; s < 3; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    for (int s = 0; s < 4; s++) set_note(&pa, 1, s, (uint8_t)(70 + s));
    mk_engine_start();
    run(12 * 24);
    uint8_t p[16];
    CHECK_EQ(pitches(0, p, 16), 12);
    for (int i = 0; i < 12; i++) CHECK_EQ(p[i], 60 + i % 3);
    CHECK_EQ(pitches(1, p, 16), 12);
    for (int i = 0; i < 12; i++) CHECK_EQ(p[i], 70 + i % 4);
    CHECK_EQ(mk_engine_pass(0), 3);
    CHECK_EQ(mk_engine_pass(1), 2);
    CHECK_EQ(log_[3].channel, 1);
    ok("polymeter 3 against 4", before);
}

/* The steps a track plays over `steps` steps, as step numbers. */
static int play_direction(int direction, int length, int steps, uint8_t *out) {
    setup();
    pa.track[0].length = (uint8_t)length;
    pa.track[0].direction = (uint8_t)direction;
    for (int s = 0; s < length; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    mk_engine_start();
    run(steps * 24);
    int n = pitches(0, out, 32);
    for (int i = 0; i < n; i++) out[i] = (uint8_t)(out[i] - 60);
    return n;
}

static void expect_steps(int direction, int length, const char *want, int line) {
    uint8_t got[32];
    int steps = 0;
    while (want[steps]) steps++;
    int n = play_direction(direction, length, steps, got);
    int same = n == steps;
    for (int i = 0; same && i < n; i++) same = got[i] == want[i] - '0';
    if (same) return;
    failures++;
    printf("%s:%d: direction %d length %d played", __FILE__, line, direction, length);
    for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf(", expected %s\n", want);
}

static void test_directions(void) {
    int before = failures;
    expect_steps(MK_DIR_FORWARD, 4, "01230123", __LINE__);
    expect_steps(MK_DIR_FORWARD, 1, "0000", __LINE__);
    expect_steps(MK_DIR_FORWARD, 2, "0101", __LINE__);
    expect_steps(MK_DIR_REVERSE, 4, "32103210", __LINE__);
    expect_steps(MK_DIR_REVERSE, 1, "000", __LINE__);
    expect_steps(MK_DIR_REVERSE, 2, "1010", __LINE__);
    expect_steps(MK_DIR_PENDULUM, 4, "012321012321", __LINE__);
    expect_steps(MK_DIR_PENDULUM, 1, "0000", __LINE__);
    expect_steps(MK_DIR_PENDULUM, 2, "010101", __LINE__);
    expect_steps(MK_DIR_PENDULUM, 3, "01210121", __LINE__);

    uint8_t got[32];
    play_direction(MK_DIR_PENDULUM, 4, 6, got);
    CHECK_EQ(mk_engine_pass(0), 0);
    run(1);
    CHECK_EQ(mk_engine_pass(0), 1);                     /* back at step 0 */
    play_direction(MK_DIR_REVERSE, 4, 4, got);
    run(1);
    CHECK_EQ(mk_engine_pass(0), 1);
    CHECK_EQ(mk_engine_position(0), 3);

    setup();                                            /* random: one draw per step */
    pa.track[0].length = 4;
    pa.track[0].direction = MK_DIR_RANDOM;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    const uint32_t r[] = { 1, 7, 8, 2, 6, 5, 3 };
    for (int i = 0; i < 7; i++) script[i] = r[i];
    script_len = 7;
    mk_engine_start();
    run(3 * 24 + 12);
    CHECK_EQ(draws, 4);
    run(12);                                            /* the next step is drawn in the second half */
    CHECK_EQ(draws, 5);
    CHECK_EQ(mk_engine_pass(0), 0);
    run(1);
    CHECK_EQ(mk_engine_pass(0), 1);                     /* every `length` steps */
    run(2 * 24);
    uint8_t p[16];
    int n = pitches(0, p, 16);
    CHECK_EQ(n, 7);
    for (int i = 0; i < n; i++) CHECK_EQ(p[i], 60 + r[i] % 4);

    setup();                                            /* random of length 1 still works */
    pa.track[0].length = 1;
    pa.track[0].direction = MK_DIR_RANDOM;
    set_note(&pa, 0, 0, 60);
    mk_engine_start();
    run(3 * 24);
    CHECK_EQ(count(ON, 0), 3);
    CHECK_EQ(mk_engine_pass(0), 2);
    ok("directions", before);
}

static void test_swing(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    pa.track[0].swing = 66;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, 60);
    mk_engine_start();
    run(2 * 96);
    /* pairs of 48 ticks; the second step at round(48 * 0.66) = 32 */
    EXPECT_TICKS(ON, 0, 60, 0, 32, 48, 80, 96, 128, 144, 176);
    EXPECT_TICKS(OFF, 0, 60, 12, 40, 60, 88, 108, 136, 156, 184);
    ok("swing 66", before);
}

static void test_conditions(void) {
    int before = failures;
    static const struct { uint8_t condition; uint8_t passes; } table[] = {
        { MK_COND_ALWAYS, 0xFF }, { MK_COND_1_OF_2, 0x55 }, { MK_COND_2_OF_2, 0xAA },
        { MK_COND_1_OF_3, 0x49 }, { MK_COND_1_OF_4, 0x11 }, { MK_COND_FIRST, 0x01 },
        { MK_COND_NOT_FIRST, 0xFE },
    };
    for (size_t c = 0; c < sizeof table / sizeof table[0]; c++) {
        setup();
        pa.track[0].length = 2;                         /* one step with notes, one without */
        set_note(&pa, 0, 0, 60);
        pa.track[0].step[0].condition = table[c].condition;
        mk_engine_start();
        run(8 * 48);
        uint32_t ticks[16];
        int n = collect(ON, 0, 60, ticks, 16);
        uint8_t played = 0;
        for (int i = 0; i < n && i < 16; i++) played |= (uint8_t)(1 << (ticks[i] / 48));
        CHECK_EQ(played, table[c].passes);
        CHECK_EQ(draws, 0);
    }
    ok("conditions over 8 passes", before);
}

static void test_probability(void) {
    int before = failures;
    setup();
    pa.track[0].length = 1;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].probability = 0;
    mk_engine_start();
    run(8 * 24);
    CHECK_EQ(count(ON, 0), 0);
    CHECK_EQ(draws, 0);

    setup();
    pa.track[0].length = 1;
    set_note(&pa, 0, 0, 60);
    mk_engine_start();
    run(8 * 24);
    CHECK_EQ(count(ON, 0), 8);
    CHECK_EQ(draws, 0);

    setup();
    pa.track[0].length = 1;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].probability = 50;
    const uint32_t r[] = { 10, 60, 49, 50, 99, 0, 149, 150 };
    for (int i = 0; i < 8; i++) script[i] = r[i];
    script_len = 8;
    mk_engine_start();
    run(8 * 24);
    CHECK_EQ(draws, 8);
    EXPECT_TICKS(ON, 0, 60, 0, 2 * 24, 5 * 24, 6 * 24);
    ok("probability 0, 100, 50", before);
}

static void test_ratchet(void) {
    int before = failures;
    setup();
    pa.track[0].length = 2;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].ratchet = 3;
    set_note(&pa, 0, 1, 62);
    mk_engine_start();
    run(48);
    EXPECT_TICKS(ON, 0, 60, 0, 8, 16);
    EXPECT_TICKS(OFF, 0, 60, 4, 12, 20);
    EXPECT_TICKS(ON, 0, 62, 24);
    ok("ratchet 3", before);
}

static void test_tie(void) {
    int before = failures;
    setup();
    pa.track[0].length = 3;
    set_note(&pa, 0, 0, 60);
    set_note(&pa, 0, 0, 64);
    pa.track[0].step[0].flags = MK_STEP_TIE;
    set_note(&pa, 0, 1, 60);
    set_note(&pa, 0, 1, 67);
    mk_engine_start();
    run(72);
    EXPECT_TICKS(ON, 0, 60, 0);                         /* carried on, not struck again */
    EXPECT_TICKS(OFF, 0, 60, 36);                       /* ended by step 1's gate */
    EXPECT_TICKS(ON, 0, 64, 0);
    EXPECT_TICKS(OFF, 0, 64, 24);
    EXPECT_TICKS(ON, 0, 67, 24);
    EXPECT_TICKS(OFF, 0, 67, 36);
    CHECK(find(OFF, 0, 64, 24) < find(ON, 0, 67, 24));

    setup();                                            /* held through a step that does not play */
    pa.track[0].length = 3;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].flags = MK_STEP_TIE;
    set_note(&pa, 0, 1, 62);
    pa.track[0].step[1].probability = 0;
    set_note(&pa, 0, 2, 64);
    mk_engine_start();
    run(72);
    EXPECT_TICKS(OFF, 0, 60, 48);
    EXPECT_TICKS(ON, 0, 64, 48);
    CHECK_EQ(count(ON, 0), 2);
    mk_engine_stop();
    CHECK(all_quiet());
    ok("tie", before);
}

static void test_gate_100(void) {
    int before = failures;
    setup();
    pa.track[0].length = 2;
    pa.track[0].gate = 90;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].gate = 100;
    set_note(&pa, 0, 1, 60);
    pa.track[0].step[1].gate = 100;
    mk_engine_start();
    run(49);
    EXPECT_TICKS(ON, 0, 60, 0, 24, 48);
    EXPECT_TICKS(OFF, 0, 60, 24, 48);
    CHECK(find(OFF, 0, 60, 24) < find(ON, 0, 60, 24));
    CHECK(find(OFF, 0, 60, 48) < find(ON, 0, 60, 48));
    CHECK_EQ(doubled, 0);
    ok("gate 100 ends before the next note", before);
}

static void test_cuts(void) {
    int before = failures;
    setup();
    for (int t = 0; t < 2; t++) {
        pa.track[t].length = 1;
        pa.track[t].division = MK_DIV_4;                /* 96 ticks, gate 50: 48 */
        set_note(&pa, t, 0, (uint8_t)(60 + t));
    }
    mk_engine_start();
    run(10);
    pa.track[0].muted = 1;
    mk_engine_mute_changed(0);
    EXPECT_TICKS(OFF, 0, 60, 10);
    CHECK_EQ(count(OFF, 1), 0);
    run(96);
    CHECK_EQ(count(ON, 0), 1);                          /* muted: the next step is silent */
    CHECK_EQ(count(ON, 1), 2);
    pa.track[0].muted = 0;
    mk_engine_mute_changed(0);
    run(96);
    EXPECT_TICKS(ON, 0, 60, 0, 192);

    mk_engine_solo(1 << 1);                             /* track 0 cut, track 1 sounding on */
    EXPECT_TICKS(OFF, 0, 60, 10, 202);
    CHECK_EQ(count(OFF, 1), 2);
    run(96);
    CHECK_EQ(count(ON, 0), 2);
    CHECK_EQ(count(ON, 1), 4);
    mk_engine_solo(0);
    run(96);
    CHECK_EQ(count(ON, 0), 3);

    run(10);
    mk_engine_enable(0);
    CHECK(!mk_engine_enabled());
    CHECK(all_quiet());
    int clocks = 0;
    int was = logged;
    run(96);
    for (int i = was; i < logged; i++) {
        CHECK(log_[i].kind == RT);
        clocks += log_[i].a == 0xF8;
    }
    CHECK_EQ(clocks, 24);                               /* the clock runs on */
    mk_engine_enable(1);
    run(96);
    CHECK_EQ(count(ON, 0), 4);
    CHECK_EQ(doubled + unmatched, 0);
    ok("mute, solo and disable cut notes", before);
}

static void test_channel_change(void) {
    int before = failures;
    setup();
    pa.track[0].length = 1;
    set_note(&pa, 0, 0, 60);
    mk_engine_start();
    run(5);
    pa.track[0].channel = 5;
    run(20);
    CHECK_EQ(log_[2].kind, ON);
    CHECK_EQ(log_[2].channel, 0);
    int off = find(OFF, 0, 60, 12);
    CHECK(off >= 0);
    if (off >= 0) CHECK_EQ(log_[off].channel, 0);
    int on = find(ON, 0, 60, 24);
    CHECK(on >= 0);
    if (on >= 0) CHECK_EQ(log_[on].channel, 5);
    pa.track[0].channel = 9;                            /* and when the pattern itself changes */
    mk_engine_switch(&pb, 1);
    CHECK_EQ(log_[logged - 2].kind, OFF);
    CHECK_EQ(log_[logged - 2].channel, 5);
    CHECK_EQ(unmatched, 0);
    ok("channel changed while a note sounds", before);
}

static void test_transpose(void) {
    int before = failures;
    setup();
    for (int t = 0; t < 3; t++) {
        pa.track[t].length = 1;
        set_note(&pa, t, 0, 60);
    }
    pa.track[2].follows_transpose = 1;
    pa.track[1].shift = 12;
    mk_engine_start();
    run(5);
    mk_engine_transpose(0, 5);
    CHECK_EQ(mk_engine_transposition(0), 5);
    CHECK_EQ(mk_engine_transposition(1), 0);
    CHECK_EQ(mk_engine_transposition(2), 5);
    run(20);
    EXPECT_TICKS(OFF, 0, 60, 12);                       /* the note that was already on */
    EXPECT_TICKS(ON, 0, 65, 24);
    EXPECT_TICKS(ON, 1, 72, 0, 24);
    EXPECT_TICKS(ON, 2, 65, 24);
    mk_engine_transpose(1, -2);                         /* track 2 follows any track */
    CHECK_EQ(mk_engine_transposition(0), 5);
    CHECK_EQ(mk_engine_transposition(2), -2);
    run(24);
    EXPECT_TICKS(ON, 1, 70, 48);
    EXPECT_TICKS(ON, 2, 58, 48);
    ok("transposition and follows_transpose", before);
}

static void test_range(void) {
    int before = failures;
    setup();
    pa.track[0].length = 1;
    pa.track[0].shift = 12;
    set_note(&pa, 0, 0, 120);                           /* 132: left out */
    set_note(&pa, 0, 0, 100);
    pa.track[1].length = 1;
    pa.track[1].shift = -12;
    set_note(&pa, 1, 0, 5);                             /* -7: left out */
    set_note(&pa, 1, 0, 12);                            /* 0: plays */
    mk_engine_start();
    run(24);
    uint8_t p[4];
    CHECK_EQ(pitches(0, p, 4), 1);
    CHECK_EQ(p[0], 112);
    CHECK_EQ(pitches(1, p, 4), 1);
    CHECK_EQ(p[0], 0);
    mk_engine_transpose(0, 20);                         /* 100 + 12 + 20 = 132 */
    run(24);
    CHECK_EQ(pitches(0, p, 4), 1);
    ok("out-of-range pitches skipped", before);
}

static void test_controllers(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    pa.track[0].channel = 3;
    const uint8_t values[4] = { 10, 10, 20, MK_CTL_NONE };
    for (int s = 0; s < 4; s++) pa.track[0].step[s].controller[MK_CTL_MOD] = values[s];
    pa.track[0].step[0].controller[MK_CTL_BEND] = 64;
    pa.track[0].step[2].probability = 0;                /* still sent when the notes do not play */
    mk_engine_start();
    run(2 * 96);
    uint32_t ticks[8];
    int n = collect(CTL, 0, MK_CTL_MOD, ticks, 8);
    CHECK_EQ(n, 4);
    const uint32_t want[4] = { 0, 48, 96, 144 };
    for (int i = 0; i < n && i < 4; i++) CHECK_EQ(ticks[i], want[i]);
    CHECK_EQ(collect(CTL, 0, MK_CTL_BEND, ticks, 8), 1);
    for (int i = 0; i < logged; i++)
        if (log_[i].kind == CTL) CHECK_EQ(log_[i].channel, 3);
    ok("controllers sent once per change", before);
}

/* A controller moved while recording goes into the step the track is on and plays back from
 * there; the step's old value is not sent again in between. */
static void test_record_controllers(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_MOD, 50), -1);      /* stopped */
    mk_engine_start();
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_MOD, 10), 0);       /* before the first tick: step 1 */
    run(30);                                            /* six ticks into step 2 */
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_MOD, 70), 1);
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_MOD, 90), 1);       /* the last value of a step stays */
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_BEND, 64), 1);
    CHECK_EQ(mk_engine_record_controller(0, MK_CTL_MOD, 128), -1);
    CHECK_EQ(mk_engine_record_controller(0, MK_CONTROLLERS, 1), -1);
    CHECK_EQ(mk_engine_record_controller(MK_TRACKS, 0, 1), -1);
    CHECK_EQ(pa.track[0].step[0].controller[MK_CTL_MOD], 10);
    CHECK_EQ(pa.track[0].step[1].controller[MK_CTL_MOD], 90);
    CHECK_EQ(pa.track[0].step[1].controller[MK_CTL_BEND], 64);
    CHECK_EQ(pa.track[0].step[2].controller[MK_CTL_MOD], MK_CTL_NONE);
    run(96 - 30);
    uint32_t ticks[8];
    CHECK_EQ(collect(CTL, 0, MK_CTL_MOD, ticks, 8), 0); /* the engine sent nothing: the control did */
    logged = 0;
    run(96);                                            /* the second pass plays it back */
    CHECK_EQ(collect(CTL, 0, MK_CTL_MOD, ticks, 8), 2);
    CHECK_EQ(ticks[0], 96);
    CHECK_EQ(ticks[1], 120);
    CHECK_EQ(collect(CTL, 0, MK_CTL_BEND, ticks, 8), 0);        /* 64 was the last value sent */
    ok("controllers recorded into the step being played", before);
}

static void test_switch(void) {
    int before = failures;
    setup();
    pa.track[0].length = 2;
    set_note(&pa, 0, 0, 60);
    set_note(&pa, 0, 1, 61);
    pa.track[1].length = 3;
    pa.track[1].division = MK_DIV_4;
    pa.track[1].gate = 90;
    set_note(&pa, 1, 0, 50);
    set_note(&pb, 0, 0, 72);
    pb.track[0].length = 4;
    mk_engine_start();
    run(10);
    mk_engine_switch(&pb, 0);
    CHECK(mk_engine_pattern() == &pa);
    CHECK(mk_engine_pending() == &pb);
    run(38);                                            /* up to tick 47 */
    CHECK_EQ(count(SWITCHED, -1), 0);
    run(1);                                             /* tick 48: track 0 wraps */
    CHECK_EQ(count(SWITCHED, -1), 1);
    CHECK(mk_engine_pattern() == &pb);
    CHECK(mk_engine_pending() == NULL);
    EXPECT_TICKS(OFF, 1, 50, 48);                       /* the old pattern's notes end */
    EXPECT_TICKS(ON, 0, 72, 48);
    CHECK_EQ(count(ON, 0), 3);
    CHECK_EQ(mk_engine_position(0), 0);
    CHECK_EQ(mk_engine_pass(0), 0);
    int s = find(SWITCHED, -1, 0, 48), on = find(ON, 0, 72, 48);
    CHECK(s >= 0 && s < on);

    run(30);
    mk_engine_switch(&pa, 1);                           /* at once while playing */
    CHECK(mk_engine_pattern() == &pa);
    EXPECT_TICKS(OFF, 0, 72, 60);
    run(1);
    EXPECT_TICKS(ON, 0, 60, 0, 79);                     /* first steps on the next tick */

    mk_engine_stop();
    mk_engine_switch(&pb, 0);                           /* stopped: at once */
    CHECK(mk_engine_pattern() == &pb);
    CHECK(mk_engine_pending() == NULL);
    CHECK_EQ(count(SWITCHED, -1), 3);

    mk_engine_start();                                  /* a change waiting at Stop takes over */
    run(5);
    mk_engine_switch(&pa, 0);
    mk_engine_stop();
    CHECK(mk_engine_pattern() == &pa);
    CHECK(all_quiet());
    ok("pattern switch", before);
}

static void test_record(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    CHECK_EQ(mk_engine_record(0, 60, 100, 1), -1);         /* stopped */
    CHECK_EQ(pa.track[0].step[0].note[0], 0);
    mk_engine_start();
    run(12);                                            /* step 0 began 11 ticks ago */
    CHECK_EQ(mk_engine_record(0, 60, 90, 1), 0);
    CHECK_EQ(pa.track[0].step[0].note[0], 60);
    CHECK_EQ(pa.track[0].step[0].velocity[0], 90);
    CHECK_EQ(mk_engine_record(0, 60, 90, 1), 0);           /* already there */
    CHECK_EQ(pa.track[0].step[0].note[1], 0);
    run(13);                                            /* tick 25: 1 into step 1 */
    run(12);                                            /* step 1 began 12 ticks ago: half */
    CHECK_EQ(mk_engine_record(0, 64, 100, 1), 2);
    CHECK_EQ(pa.track[0].step[2].note[0], 64);
    run(24 * 2 + 12);                                   /* to tick 96: step 0 of pass 1 */
    CHECK_EQ(count(ON, 0), 1);                          /* not step 2 on pass 0, step 0 on pass 1 */
    EXPECT_TICKS(ON, 0, 60, 96);
    run(48);
    EXPECT_TICKS(ON, 0, 64, 144);                       /* step 2 on the following pass */

    run(37);                                            /* tick 181: 13 into step 3 */
    CHECK_EQ(mk_engine_position(0), 3);
    CHECK_EQ(mk_engine_record(0, 67, 100, 1), 0);          /* the next step is step 0 of the next pass */
    CHECK_EQ(pa.track[0].step[0].note[1], 67);
    run(24);
    uint32_t ticks[4];
    CHECK_EQ(collect(ON, 0, 67, ticks, 4), 0);
    EXPECT_TICKS(ON, 0, 60, 96, 192);
    run(96);
    EXPECT_TICKS(ON, 0, 67, 288);

    for (int i = 2; i < MK_POLY; i++) set_note(&pa, 0, 1, (uint8_t)(80 + i));
    for (int i = 0; i < 2; i++) set_note(&pa, 0, 1, (uint8_t)(90 + i));
    run(15);                                            /* tick 316: 4 into step 1 */
    CHECK_EQ(mk_engine_position(0), 1);
    CHECK_EQ(mk_engine_record(0, 99, 100, 1), 1);          /* full: ignored, still the step */
    for (int i = 0; i < MK_POLY; i++) CHECK(pa.track[0].step[1].note[i] != 99);

    setup();                                            /* a ratchet goes on without the new note */
    pa.track[0].length = 1;
    pa.track[0].step[0].ratchet = 2;
    set_note(&pa, 0, 0, 50);
    mk_engine_start();
    run(3);
    CHECK_EQ(mk_engine_record(0, 60, 100, 1), 0);
    run(24);
    EXPECT_TICKS(ON, 0, 50, 0, 12, 24);
    EXPECT_TICKS(ON, 0, 60, 24);
    ok("live recording", before);
}

static void test_edited(void) {
    int before = failures;
    setup();
    pa.track[0].length = 8;
    pa.track[0].gate = 90;
    for (int s = 0; s < 8; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    mk_engine_start();
    run(6 * 24 + 3);
    CHECK_EQ(mk_engine_position(0), 6);
    pa.track[0].length = 4;
    mk_engine_edited(0);
    EXPECT_TICKS(OFF, 0, 66, 147);                      /* at once */
    CHECK(all_quiet());
    CHECK_EQ(mk_engine_position(0), 2);
    run(22);
    EXPECT_TICKS(ON, 0, 63, 3 * 24, 168);               /* on from step 2 */
    run(24);
    EXPECT_TICKS(ON, 0, 60, 0, 192);
    CHECK_EQ(mk_engine_pass(0), 1);
    ok("edited after shortening a track", before);
}

/* A tick arriving while a main-loop function holds the engine waits until it is done. */
static int ticked_inside;
static void tick_inside(void) {
    if (ticked_inside++) return;
    mk_engine_tick();
}

static void test_deferred_tick(void) {
    int before = failures;
    setup();
    pa.track[0].length = 1;
    pa.track[0].division = MK_DIV_4;
    set_note(&pa, 0, 0, 60);
    pa.track[0].muted = 0;
    mk_engine_start();
    run(4);
    pa.track[0].muted = 1;
    ticked_inside = 0;
    on_note = tick_inside;
    mk_engine_mute_changed(0);                          /* its note off "interrupts" with a tick */
    on_note = NULL;
    now++;
    CHECK_EQ(ticked_inside, 1);
    CHECK_EQ(log_[logged - 2].kind, OFF);
    CHECK_EQ(log_[logged - 1].kind, RT);                /* tick 4's clock, after the note off */
    CHECK_EQ(log_[logged - 1].a, 0xF8);
    run(4);                                             /* the clock went on from where it was */
    CHECK_EQ(log_[logged - 1].a, 0xF8);
    CHECK_EQ(log_[logged - 1].tick, 8);
    ok("a tick during a main-loop call waits for it", before);
}

static uint32_t roll(uint32_t n) { return xorshift() % n; }

/* ---- timing offsets ---- */

static void set_offset(int t, int s, int offset) { pa.track[t].step[s].offset = (int8_t)offset; }

/* The grid tick of a step plus its offset, as the engine rounds it. */
static long shifted(long grid, int offset, int ticks) {
    long x = (long)offset * ticks;
    return grid + (x < 0 ? -((-x + 128) / 256) : (x + 128) / 256);
}

static void test_offsets(void) {
    int before = failures;
    setup();                                            /* 1/16: 24 ticks a step, a 256th is 3/32 of a tick */
    pa.track[0].length = 4;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    set_offset(0, 0, -32);                              /* 3 ticks early: not on the first pass */
    set_offset(0, 1, 64);                               /* 6 late */
    set_offset(0, 2, -64);                              /* 6 early */
    mk_engine_start();
    run(2 * 96);
    EXPECT_TICKS(ON, 0, 60, 0, 93, 189);                /* early across the wrap, at the end of step 3 */
    EXPECT_TICKS(ON, 0, 61, 30, 126);
    EXPECT_TICKS(ON, 0, 62, 42, 138);
    EXPECT_TICKS(ON, 0, 63, 72, 168);
    EXPECT_TICKS(OFF, 0, 60, 12, 105);                  /* gate 50 %: 12 ticks from where it began */
    EXPECT_TICKS(OFF, 0, 61, 42, 138);
    EXPECT_TICKS(OFF, 0, 62, 54, 150);
    CHECK(find(OFF, 0, 61, 42) < find(ON, 0, 62, 42));
    CHECK_EQ(mk_engine_pass(0), 1);
    mk_engine_stop();
    CHECK(all_quiet());

    static const uint8_t divisions[] = { MK_DIV_2D, MK_DIV_4, MK_DIV_8, MK_DIV_16T, MK_DIV_32, MK_DIV_32T };
    static const int offsets[] = { 127, 50, 1, -1, -77, -128 };
    for (size_t d = 0; d < sizeof divisions; d++) {
        for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
            setup();
            int ticks = mk_div_ticks[divisions[d]];
            pa.track[0].length = 2;
            pa.track[0].division = divisions[d];
            set_note(&pa, 0, 0, 60);
            set_note(&pa, 0, 1, 61);
            set_offset(0, 1, offsets[o]);
            mk_engine_start();
            run(4 * ticks);
            long want = shifted(ticks, offsets[o], ticks), last = shifted(3 * ticks, offsets[o], ticks);
            if (want > ticks + (ticks - 1) / 2) want = ticks + (ticks - 1) / 2;
            if (last > 3 * ticks + (ticks - 1) / 2) last = 3 * ticks + (ticks - 1) / 2;
            uint32_t got[4];
            int n = collect(ON, 0, 61, got, 4);
            CHECK_EQ(n, 2);
            if (n == 2 && (got[0] != want || got[1] != last))
                CHECK_EQ(got[0] * 1000 + got[1], want * 1000 + last);
            CHECK_EQ(count(ON, 0), 4);
        }
    }

    setup();                                            /* swing 66: windows of 32 and 16 */
    pa.track[0].length = 4;
    pa.track[0].swing = 66;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    set_offset(0, 0, -128);                             /* 12 early: at most half of step 3's 16 */
    set_offset(0, 1, 64);                               /* 6 late, in a window of 16 */
    set_offset(0, 2, -64);                              /* 6 early, into step 1's window */
    set_offset(0, 3, 127);                              /* 12 late: at most 7 in a window of 16 */
    mk_engine_start();
    run(2 * 96);
    EXPECT_TICKS(ON, 0, 60, 0, 88, 184);
    EXPECT_TICKS(ON, 0, 61, 38, 134);
    EXPECT_TICKS(ON, 0, 62, 42, 138);
    EXPECT_TICKS(ON, 0, 63, 87, 183);
    EXPECT_TICKS(OFF, 0, 61, 46, 142);                  /* gate 50 % of the 16 of its window */
    EXPECT_TICKS(OFF, 0, 62, 54, 150);                  /* of the 24 its window would have, not more */
    ok("offsets: late, early, across the wrap, every division, with swing", before);
}

static void test_offset_directions(void) {
    int before = failures;
    setup();                                            /* reverse: step 3 begins each pass */
    pa.track[0].length = 4;
    pa.track[0].direction = MK_DIR_REVERSE;
    for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    set_offset(0, 3, -64);
    set_offset(0, 0, 64);
    mk_engine_start();
    run(3 * 96);
    EXPECT_TICKS(ON, 0, 63, 0, 90, 186, 282);
    EXPECT_TICKS(ON, 0, 60, 78, 174, 270);
    CHECK_EQ(count(ON, 0), 13);

    setup();                                            /* the condition is the pass the step is in */
    pa.track[0].length = 2;
    set_note(&pa, 0, 0, 60);
    set_note(&pa, 0, 0, 61);
    pa.track[0].step[0].condition = MK_COND_FIRST;
    set_offset(0, 0, -64);
    set_note(&pa, 0, 1, 62);
    pa.track[0].step[1].condition = MK_COND_2_OF_2;
    set_offset(0, 1, -64);
    mk_engine_start();
    run(4 * 48);
    EXPECT_TICKS(ON, 0, 60, 0);
    EXPECT_TICKS(ON, 0, 62, 66, 162);                   /* passes 1 and 3, begun in step 0's window */

    setup();                                            /* pendulum 0 1 2 1 */
    pa.track[0].length = 3;
    pa.track[0].direction = MK_DIR_PENDULUM;
    for (int s = 0; s < 3; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    set_offset(0, 0, -64);
    set_offset(0, 2, 64);
    mk_engine_start();
    run(2 * 96);
    EXPECT_TICKS(ON, 0, 60, 0, 90, 186);
    EXPECT_TICKS(ON, 0, 61, 24, 72, 120, 168);
    EXPECT_TICKS(ON, 0, 62, 54, 150);

    setup();                                            /* random: the next step is drawn before it begins */
    pa.track[0].length = 4;
    pa.track[0].direction = MK_DIR_RANDOM;
    for (int s = 0; s < 4; s++) {
        set_note(&pa, 0, s, (uint8_t)(60 + s));
        set_offset(0, s, -64);
    }
    const uint32_t r[] = { 1, 2, 3, 0, 0, 2 };
    for (int i = 0; i < 6; i++) script[i] = r[i];
    script_len = 6;
    mk_engine_start();
    run(5 * 24);
    uint8_t p[8];
    CHECK_EQ(pitches(0, p, 8), 6);
    for (int i = 0; i < 6; i++) CHECK_EQ(p[i], 60 + r[i]);
    EXPECT_TICKS(ON, 0, -1, 0, 18, 42, 66, 90, 114);
    CHECK_EQ(mk_engine_position(0), 0);                 /* the grid is where it was */
    run(1);
    CHECK_EQ(mk_engine_position(0), 2);
    CHECK_EQ(mk_engine_pass(0), 1);
    ok("offsets in every direction, conditions of the pass the step is in", before);
}

static void test_offset_ratchet_tie(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    set_note(&pa, 0, 1, 61);
    pa.track[0].step[1].ratchet = 2;
    set_offset(0, 1, 64);
    set_note(&pa, 0, 3, 63);
    pa.track[0].step[3].ratchet = 3;
    set_offset(0, 3, -64);
    mk_engine_start();
    run(96);
    EXPECT_TICKS(ON, 0, 61, 30, 42);                    /* parts of 12 from where it began */
    EXPECT_TICKS(OFF, 0, 61, 36, 48);
    EXPECT_TICKS(ON, 0, 63, 66, 74, 82);
    EXPECT_TICKS(OFF, 0, 63, 70, 78, 86);

    setup();                                            /* a step beginning early drops the parts left */
    pa.track[0].length = 4;
    set_note(&pa, 0, 1, 61);
    pa.track[0].step[1].ratchet = 4;
    set_offset(0, 1, 64);
    set_note(&pa, 0, 2, 62);
    set_offset(0, 2, -64);
    mk_engine_start();
    run(96);
    EXPECT_TICKS(ON, 0, 61, 30, 36);
    EXPECT_TICKS(ON, 0, 62, 42);

    setup();                                            /* gate 100 % running into the next step: struck again */
    pa.track[0].length = 2;
    set_note(&pa, 0, 0, 60);
    pa.track[0].step[0].gate = 100;
    set_offset(0, 0, 64);
    set_note(&pa, 0, 1, 60);
    pa.track[0].step[1].gate = 100;
    mk_engine_start();
    run(60);
    EXPECT_TICKS(ON, 0, 60, 6, 24, 54);
    EXPECT_TICKS(OFF, 0, 60, 24, 48);
    CHECK(find(OFF, 0, 60, 24) < find(ON, 0, 60, 24));
    CHECK_EQ(doubled + unmatched, 0);

    setup();                                            /* a tie holds until the next step begins, early */
    pa.track[0].length = 3;
    set_note(&pa, 0, 0, 60);
    set_note(&pa, 0, 0, 64);
    pa.track[0].step[0].flags = MK_STEP_TIE;
    set_note(&pa, 0, 1, 60);
    set_note(&pa, 0, 1, 67);
    set_offset(0, 1, -64);
    mk_engine_start();
    run(72);
    EXPECT_TICKS(ON, 0, 60, 0);                         /* carried on into step 1 */
    EXPECT_TICKS(OFF, 0, 60, 30);                       /* its gate from where step 1 began */
    EXPECT_TICKS(OFF, 0, 64, 18);
    EXPECT_TICKS(ON, 0, 67, 18);
    mk_engine_stop();
    CHECK(all_quiet());
    ok("ratchets, gates and ties counted from where the step began", before);
}

static void test_offset_switch(void) {
    int before = failures;
    setup();
    pa.track[0].length = 3;                             /* wraps at 72 */
    for (int s = 0; s < 3; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
    set_offset(0, 0, -64);
    pa.track[1].length = 1;
    pa.track[1].division = MK_DIV_4;                    /* its next window at 96, after the switch */
    set_note(&pa, 1, 0, 70);
    set_offset(1, 0, -128);                             /* would begin at 48 */
    pa.track[2].length = 1;
    pa.track[2].division = MK_DIV_8;                    /* its next window at 48, before the switch */
    set_note(&pa, 2, 0, 80);
    set_offset(2, 0, -64);                              /* begins at 36 */
    pb.track[0].length = 2;
    set_note(&pb, 0, 0, 90);
    pb.track[0].step[0].offset = -64;
    mk_engine_start();
    run(10);
    mk_engine_switch(&pb, 0);
    run(62);
    CHECK(mk_engine_pattern() == &pa);
    run(1);                                             /* tick 72 */
    CHECK(mk_engine_pattern() == &pb);
    run(48);
    EXPECT_TICKS(ON, 0, 60, 0);                         /* not at 66 */
    EXPECT_TICKS(ON, 1, 70, 0);
    EXPECT_TICKS(ON, 2, 80, 0, 36);
    EXPECT_TICKS(OFF, 2, 80, 24, 60);
    EXPECT_TICKS(ON, 0, 90, 72, 114);                   /* on the grid when it takes over, early after */
    CHECK_EQ(doubled + unmatched, 0);
    ok("an early step before a waiting pattern takes over", before);
}

static void test_record_offsets(void) {
    int before = failures;
    setup();
    pa.track[0].length = 4;
    set_note(&pa, 0, 0, 50);
    mk_engine_start();
    run(32);                                            /* step 1, 7 ticks in (tick 31 played) */
    CHECK_EQ(mk_engine_record(0, 64, 100, 0), 1);
    CHECK_EQ(pa.track[0].step[1].offset, 75);           /* 7 * 256 / 24, rounded */
    run(2);
    CHECK_EQ(mk_engine_record(0, 65, 100, 0), 1);       /* a second note keeps the step's offset */
    CHECK_EQ(pa.track[0].step[1].offset, 75);
    CHECK_EQ(pa.track[0].step[1].note[1], 65);
    run(35);                                            /* step 2, 20 in: tick 68 played */
    CHECK_EQ(mk_engine_record(0, 67, 100, 0), 3);
    CHECK_EQ(pa.track[0].step[3].offset, -43);          /* 4 ticks early */
    run(18);                                            /* step 3, 14 in */
    CHECK_EQ(mk_engine_record(0, 51, 100, 0), 0);       /* into step 0 of the next pass, which has notes */
    CHECK_EQ(pa.track[0].step[0].offset, 0);
    run(96 * 3 - 87);
    EXPECT_TICKS(ON, 0, 64, 127, 223);                  /* not on the pass it was played in */
    EXPECT_TICKS(ON, 0, 65, 127, 223);
    EXPECT_TICKS(ON, 0, 67, 164, 260);                  /* as played, 68 + 96, and the pass after */
    EXPECT_TICKS(ON, 0, 50, 0, 96, 192);
    EXPECT_TICKS(ON, 0, 51, 192);

    setup();                                            /* quantized: on the grid */
    pa.track[0].length = 4;
    pa.track[0].step[1].offset = 40;                    /* left over on an empty step */
    mk_engine_start();
    run(32);
    CHECK_EQ(mk_engine_record(0, 64, 100, 1), 1);
    CHECK_EQ(pa.track[0].step[1].offset, 0);
    run(37);
    CHECK_EQ(mk_engine_record(0, 67, 100, 1), 3);
    CHECK_EQ(pa.track[0].step[3].offset, 0);
    run(100);
    EXPECT_TICKS(ON, 0, 64, 120);
    EXPECT_TICKS(ON, 0, 67, 72 + 96);

    setup();                                            /* swing and a slow division */
    pa.track[0].length = 2;
    pa.track[0].division = MK_DIV_2D;                   /* 288 ticks: a 256th is more than a tick */
    pa.track[0].swing = 60;                             /* windows of 346 and 230 */
    mk_engine_start();
    run(346 + 100);
    CHECK_EQ(mk_engine_record(0, 64, 100, 0), 1);
    run(30);
    CHECK_EQ(mk_engine_record(0, 66, 100, 0), 0);       /* 101 ticks before the wrap, at tick 475 */
    run(576 * 3 - 476);
    uint32_t got[4];
    CHECK_EQ(collect(ON, 0, 64, got, 4), 2);
    CHECK(got[0] >= 576 + 346 + 98 && got[0] <= 576 + 346 + 100);
    CHECK_EQ(collect(ON, 0, 66, got, 4), 2);
    CHECK(got[0] >= 576 + 475 - 1 && got[0] <= 576 + 475 + 1);
    ok("recording with Rec Quantize off keeps the moment, on puts it on the grid", before);
}

/* Every step begins once, in order, at the tick the rules give: its grid tick plus its offset, at
 * most (window - 1) / 2 late and half the window before it early, never early on the first. */
static long expected_start(const mk_track *tr, int k, int step) {
    int ticks = mk_div_ticks[tr->division], first = (2 * ticks * tr->swing + 50) / 100;
    long grid = (long)k / 2 * 2 * ticks + (k & 1 ? first : 0);
    long o = shifted(0, tr->step[step].offset, ticks);
    int own = k & 1 ? 2 * ticks - first : first, before = k & 1 ? first : 2 * ticks - first;
    if (o >= 0) return grid + (o < (own - 1) / 2 ? o : (own - 1) / 2);
    if (k == 0) return grid;
    return grid - (-o < before / 2 ? -o : before / 2);
}

/* An edit (a step page change, a nudge) while the next step has already started early. */
static void test_offset_edit_mid_early(void) {
    int before = failures;
    for (int at = 40; at <= 50; at++) {
        setup();
        pa.track[0].length = 4;
        for (int s = 0; s < 4; s++) set_note(&pa, 0, s, (uint8_t)(60 + s));
        set_offset(0, 2, -64);                          /* step 3 starts at tick 42, six early */
        mk_engine_start();
        run(at);
        mk_engine_edited(0);
        run(2 * 96 - at);
        EXPECT_TICKS(ON, 0, 60, 0, 96);
        EXPECT_TICKS(ON, 0, 61, 24, 120);
        EXPECT_TICKS(ON, 0, 62, 42, 138);
        EXPECT_TICKS(ON, 0, 63, 72, 168);
        mk_engine_stop();
        CHECK(all_quiet());
    }
    ok("an edit while a step has started early loses and doubles nothing", before);
}

static void test_offsets_every_step(void) {
    int before = failures;
    for (int trial = 0; trial < 60; trial++) {
        setup();
        for (int t = 0; t < MK_TRACKS; t++) {
            mk_track *tr = &pa.track[t];
            tr->length = (uint8_t)(1 + roll(12));
            tr->division = (uint8_t)roll(MK_DIV_COUNT);
            tr->direction = (uint8_t)roll(MK_DIR_RANDOM);   /* the three that can be foreseen */
            tr->swing = (uint8_t)(50 + roll(26));
            tr->channel = (uint8_t)t;
            tr->gate = (uint8_t)(10 + roll(81));
            for (int s = 0; s < tr->length; s++) {
                set_note(&pa, t, s, (uint8_t)(40 + s));
                tr->step[s].offset = (int8_t)((int)roll(256) - 128);
                if (roll(4) == 0) tr->step[s].gate = 100;
            }
        }
        const int ticks = 1500;
        mk_engine_start();
        run(ticks);
        CHECK(logged < LOG_MAX);
        for (int t = 0; t < MK_TRACKS; t++) {
            const mk_track *tr = &pa.track[t];
            int n = tr->length, cycle = tr->direction == MK_DIR_PENDULUM && n > 1 ? 2 * (n - 1) : n;
            int k = 0, wrong = 0;
            for (int i = 0; i < logged && !wrong; i++) {
                const event *e = &log_[i];
                if (e->kind != ON || e->track != t) continue;
                int index = k % cycle;
                int step = tr->direction == MK_DIR_REVERSE ? n - 1 - index
                         : tr->direction == MK_DIR_PENDULUM && index >= n ? 2 * (n - 1) - index : index;
                if (e->a != 40 + step || e->tick != expected_start(tr, k, step)) {
                    printf("%s:%d: trial %d track %d step %d of the run: step %d at %u, expected step %d at %ld\n",
                           __FILE__, __LINE__, trial, t, k, e->a - 40, e->tick, step, expected_start(tr, k, step));
                    failures++;
                    wrong = 1;
                }
                k++;
            }
            int index = k % cycle;
            int step = tr->direction == MK_DIR_REVERSE ? n - 1 - index
                     : tr->direction == MK_DIR_PENDULUM && index >= n ? 2 * (n - 1) - index : index;
            if (!wrong && expected_start(tr, k, step) < ticks) {
                printf("%s:%d: trial %d track %d: %d steps begun, the next was due at %ld\n", __FILE__, __LINE__, trial,
                       t, k, expected_start(tr, k, step));
                failures++;
            }
        }
        mk_engine_stop();
        CHECK(all_quiet());
        CHECK_EQ(doubled + unmatched, 0);
    }
    ok("every step begins once, in order, at its tick", before);
}

static void scramble(mk_pattern *p) {
    for (int t = 0; t < MK_TRACKS; t++) {
        mk_track *tr = &p->track[t];
        tr->length = (uint8_t)(1 + roll(16));
        tr->division = (uint8_t)roll(MK_DIV_COUNT);
        tr->direction = (uint8_t)roll(MK_DIR_COUNT);
        tr->swing = (uint8_t)(50 + roll(26));
        tr->channel = (uint8_t)roll(3);                 /* shared channels on purpose */
        for (int s = 0; s < MK_STEPS; s++) {
            mk_step *st = &tr->step[s];
            int n = (int)roll(4);
            for (int i = 0; i < MK_POLY; i++) {
                st->note[i] = (uint8_t)(i < n ? 48 + roll(24) : 0);
                st->velocity[i] = (uint8_t)(1 + roll(127));
            }
            st->gate = (uint8_t)(roll(3) ? 0 : 1 + roll(100));
            st->ratchet = (uint8_t)(1 + roll(4));
            st->flags = (uint8_t)(roll(4) == 0 ? MK_STEP_TIE : 0);
            st->probability = (uint8_t)(roll(3) ? 100 : roll(101));
            st->condition = (uint8_t)roll(MK_COND_COUNT);
            st->offset = (int8_t)(roll(3) ? (int)roll(256) - 128 : 0);
        }
    }
}

static void test_soak(void) {
    int before = failures;
    setup();
    scramble(&pa);
    scramble(&pb);
    notes_on = 0;
    mk_engine_start();
    for (int i = 0; i < 200000; i++) {
        logged = 0;                                     /* the checks run in the callback */
        mk_engine_tick();
        now++;
        if (roll(20)) continue;
        int t = (int)roll(MK_TRACKS);
        mk_pattern *p = mk_engine_pattern();
        mk_track *tr = &p->track[t];
        switch (roll(17)) {
        case 0: tr->length = (uint8_t)(1 + roll(64)); mk_engine_edited(t); break;
        case 1: tr->division = (uint8_t)roll(MK_DIV_COUNT); mk_engine_edited(t); break;
        case 2: tr->muted = (uint8_t)!tr->muted; mk_engine_mute_changed(t); break;
        case 3: mk_engine_solo(roll(4) ? 0 : (int)roll(16)); break;
        case 4: mk_engine_enable(roll(4) != 0); break;
        case 5: mk_engine_transpose(t, (int)roll(49) - 24); break;
        case 6: tr->channel = (uint8_t)roll(3); break;
        case 7: tr->shift = (int8_t)((int)roll(97) - 48); break;
        case 8: mk_engine_switch(p == &pa ? &pb : &pa, (int)roll(2)); break;
        case 9: if (roll(10) == 0) mk_engine_stop(); else if (!mk_engine_playing()) mk_engine_start(); break;
        case 10: mk_engine_record(t, (uint8_t)(48 + roll(24)), 100, (int)roll(2)); break;
        case 11: tr->step[roll(64)].flags ^= MK_STEP_TIE; break;
        case 12: tr->direction = (uint8_t)roll(MK_DIR_COUNT); mk_engine_edited(t); break;
        case 13: tr->follows_transpose = (uint8_t)roll(2); break;
        case 14: tr->swing = (uint8_t)(50 + roll(26)); break;
        case 15: tr->step[roll(64)].offset = (int8_t)((int)roll(256) - 128); break;
        default: scramble(p == &pa ? &pb : &pa); break;    /* the one not playing */
        }
    }
    mk_engine_stop();
    CHECK_EQ(doubled, 0);
    CHECK_EQ(unmatched, 0);
    CHECK(all_quiet());
    CHECK(notes_on > 10000);
    printf("   %ld notes over 200000 ticks\n", notes_on);
    ok("long random run with offsets: every note on ends, none twice", before);
}

int main(void) {
    test_clock();
    test_four_steps();
    test_polymeter();
    test_directions();
    test_swing();
    test_conditions();
    test_probability();
    test_ratchet();
    test_tie();
    test_gate_100();
    test_cuts();
    test_channel_change();
    test_transpose();
    test_range();
    test_controllers();
    test_record_controllers();
    test_switch();
    test_record();
    test_edited();
    test_deferred_tick();
    test_offsets();
    test_offset_directions();
    test_offset_ratchet_tie();
    test_offset_switch();
    test_record_offsets();
    test_offset_edit_mid_early();
    test_offsets_every_step();
    test_soak();
    if (failures) printf("%d checks failed\n", failures);
    return failures != 0;
}
