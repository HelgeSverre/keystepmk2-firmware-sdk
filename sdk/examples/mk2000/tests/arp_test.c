/* Host tests for arp.c: every note goes into a log with the tick it came in, and the tests
 * compare the log with the notes and ticks the specification gives. */
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

enum { ON, OFF };

typedef struct {
    uint32_t tick;
    int kind;
    uint8_t pitch, velocity;
} event;

#define LOG_MAX 4096
static event log_[LOG_MAX];
static int logged;
static uint32_t now;                    /* the tick being played, or the next one between ticks */

/* Every note on and off, checked as it happens: never the same note on twice, never an off
 * without its on. */
static uint8_t active[128];
static int doubled, unmatched;
static long notes_on;

static void (*on_note)(void);           /* a test hook run inside the note callback */

static void io_note(uint8_t note, uint8_t velocity) {
    if (note > 127) {
        unmatched++;
        return;
    }
    if (velocity) {
        if (active[note]) doubled++;
        active[note] = 1;
        notes_on++;
    } else {
        if (!active[note]) unmatched++;
        active[note] = 0;
    }
    if (logged < LOG_MAX) log_[logged++] = (event){ now, velocity ? ON : OFF, note, velocity };
    if (on_note) on_note();
}

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

static const mk_arp_io io = { io_note, io_random };
static mk_arp_settings set;

static void setup(void) {
    set = (mk_arp_settings){ MK_ARP_UP, 1, MK_DIV_16, 50, 50, 0, 1, 0 };    /* free running: no Arp Length */
    logged = 0;
    now = 0;
    script_len = script_pos = draws = 0;
    doubled = unmatched = 0;
    on_note = NULL;
    for (int n = 0; n < 128; n++) active[n] = 0;
    mk_arp_init(&set, &io);
}

static void run(int ticks) {
    for (int i = 0; i < ticks; i++) {
        mk_arp_tick();
        now++;
    }
}

static void scripted(const uint32_t *values, int n) {
    for (int i = 0; i < n && i < 64; i++) script[i] = values[i];
    script_len = n;
    script_pos = 0;
}

/* The ticks of the events of a kind for a pitch (-1: any), in order. */
static int collect(int kind, int pitch, uint32_t *out, int max) {
    int n = 0;
    for (int i = 0; i < logged; i++) {
        const event *e = &log_[i];
        if (e->kind != kind || (pitch >= 0 && e->pitch != pitch)) continue;
        if (n < max) out[n] = e->tick;
        n++;
    }
    return n;
}

static int count(int kind) {
    uint32_t ignored[1];
    return collect(kind, -1, ignored, 0);
}

/* The pitches switched on, in order. */
static int pitches(uint8_t *out, int max) {
    int n = 0;
    for (int i = 0; i < logged; i++)
        if (log_[i].kind == ON && n < max) out[n++] = log_[i].pitch;
    return n;
}

static void check_list(const uint32_t *got, int n, const uint32_t *want, int m, const char *what, int line) {
    if (n < m) {
        failures++;
        printf("%s:%d: %s: %d events, expected %d\n", __FILE__, line, what, n, m);
        return;
    }
    for (int i = 0; i < m; i++) {
        if (got[i] == want[i]) continue;
        failures++;
        printf("%s:%d: %s: event %d is %u, expected %u\n", __FILE__, line, what, i, got[i], want[i]);
        return;
    }
}

/* Exactly these ticks. */
#define EXPECT_TICKS(kind, pitch, ...) do { \
        const uint32_t want_[] = { __VA_ARGS__ }; \
        int m_ = (int)(sizeof want_ / sizeof want_[0]); \
        uint32_t got_[64]; \
        int n_ = collect(kind, pitch, got_, 64); \
        if (n_ != m_) { failures++; printf("%s:%d: %s: %d events, expected %d\n", __FILE__, __LINE__, #kind " " #pitch, n_, m_); } \
        else check_list(got_, n_, want_, m_, #kind " " #pitch, __LINE__); \
    } while (0)

/* The notes switched on start with these pitches. */
#define EXPECT_PITCHES(...) do { \
        const uint32_t want_[] = { __VA_ARGS__ }; \
        uint8_t p_[64]; \
        uint32_t got_[64]; \
        int n_ = pitches(p_, 64); \
        for (int i_ = 0; i_ < n_; i_++) got_[i_] = p_[i_]; \
        check_list(got_, n_, want_, (int)(sizeof want_ / sizeof want_[0]), "pitches", __LINE__); \
    } while (0)

static void ok(const char *name, int before) {
    if (failures == before) printf("ok %s\n", name);
    else printf("FAILED %s\n", name);
}

static int all_quiet(void) {
    for (int n = 0; n < 128; n++)
        if (active[n]) return 0;
    return 1;
}

/* ---- tests ---- */

/* Keys pressed 64, 60, 67 (velocities 100, 90, 80); the first `keys` of them are held. */
typedef struct {
    uint8_t mode, keys, octaves, len;
    uint8_t notes[12];
    uint8_t draws;
    uint32_t script[6];
} order_case;

static const order_case orders[] = {
    { MK_ARP_UP, 1, 1, 4, { 64, 64, 64, 64 }, 0, { 0 } },
    { MK_ARP_UP, 1, 2, 4, { 64, 76, 64, 76 }, 0, { 0 } },
    { MK_ARP_UP, 2, 1, 4, { 60, 64, 60, 64 }, 0, { 0 } },
    { MK_ARP_UP, 2, 2, 8, { 60, 64, 72, 76, 60, 64, 72, 76 }, 0, { 0 } },
    { MK_ARP_UP, 3, 1, 6, { 60, 64, 67, 60, 64, 67 }, 0, { 0 } },
    { MK_ARP_UP, 3, 2, 8, { 60, 64, 67, 72, 76, 79, 60, 64 }, 0, { 0 } },

    { MK_ARP_DOWN, 1, 1, 4, { 64, 64, 64, 64 }, 0, { 0 } },
    { MK_ARP_DOWN, 1, 2, 4, { 76, 64, 76, 64 }, 0, { 0 } },
    { MK_ARP_DOWN, 2, 1, 4, { 64, 60, 64, 60 }, 0, { 0 } },
    { MK_ARP_DOWN, 2, 2, 6, { 76, 72, 64, 60, 76, 72 }, 0, { 0 } },
    { MK_ARP_DOWN, 3, 1, 6, { 67, 64, 60, 67, 64, 60 }, 0, { 0 } },
    { MK_ARP_DOWN, 3, 2, 8, { 79, 76, 72, 67, 64, 60, 79, 76 }, 0, { 0 } },

    { MK_ARP_UP_DOWN, 1, 1, 4, { 64, 64, 64, 64 }, 0, { 0 } },
    { MK_ARP_UP_DOWN, 1, 2, 4, { 64, 76, 64, 76 }, 0, { 0 } },
    { MK_ARP_UP_DOWN, 2, 1, 4, { 60, 64, 60, 64 }, 0, { 0 } },
    { MK_ARP_UP_DOWN, 2, 2, 9, { 60, 64, 72, 76, 72, 64, 60, 64, 72 }, 0, { 0 } },
    { MK_ARP_UP_DOWN, 3, 1, 8, { 60, 64, 67, 64, 60, 64, 67, 64 }, 0, { 0 } },
    { MK_ARP_UP_DOWN, 3, 2, 12, { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 64 }, 0, { 0 } },

    { MK_ARP_RANDOM, 1, 1, 3, { 64, 64, 64 }, 3, { 0, 7, 2 } },
    { MK_ARP_RANDOM, 1, 2, 4, { 76, 64, 76, 76 }, 4, { 1, 0, 1, 3 } },
    { MK_ARP_RANDOM, 2, 1, 3, { 64, 60, 64 }, 3, { 1, 0, 3 } },
    { MK_ARP_RANDOM, 2, 2, 4, { 76, 72, 60, 64 }, 4, { 3, 2, 0, 1 } },
    { MK_ARP_RANDOM, 3, 1, 4, { 67, 60, 64, 67 }, 4, { 2, 0, 1, 5 } },
    { MK_ARP_RANDOM, 3, 2, 4, { 79, 60, 72, 76 }, 4, { 5, 0, 3, 4 } },

    { MK_ARP_ORDER, 1, 1, 4, { 64, 64, 64, 64 }, 0, { 0 } },
    { MK_ARP_ORDER, 1, 2, 4, { 64, 76, 64, 76 }, 0, { 0 } },
    { MK_ARP_ORDER, 2, 1, 4, { 64, 60, 64, 60 }, 0, { 0 } },
    { MK_ARP_ORDER, 2, 2, 6, { 64, 60, 76, 72, 64, 60 }, 0, { 0 } },
    { MK_ARP_ORDER, 3, 1, 6, { 64, 60, 67, 64, 60, 67 }, 0, { 0 } },
    { MK_ARP_ORDER, 3, 2, 8, { 64, 60, 67, 76, 72, 79, 64, 60 }, 0, { 0 } },

    /* chords low to high, one per step */
    { MK_ARP_POLY, 1, 1, 3, { 64, 64, 64 }, 0, { 0 } },
    { MK_ARP_POLY, 1, 2, 3, { 64, 76, 64 }, 0, { 0 } },
    { MK_ARP_POLY, 2, 1, 4, { 60, 64, 60, 64 }, 0, { 0 } },
    { MK_ARP_POLY, 2, 2, 6, { 60, 64, 72, 76, 60, 64 }, 0, { 0 } },
    { MK_ARP_POLY, 3, 1, 6, { 60, 64, 67, 60, 64, 67 }, 0, { 0 } },
    { MK_ARP_POLY, 3, 2, 9, { 60, 64, 67, 72, 76, 79, 60, 64, 67 }, 0, { 0 } },
};

static uint8_t velocity_of(uint8_t pitch) {
    switch (pitch % 12) {
    case 4: return 100;                 /* 64 */
    case 0: return 90;                  /* 60 */
    default: return 80;                 /* 67 */
    }
}

static void test_orders(void) {
    int before = failures;
    static const uint8_t press[3] = { 64, 60, 67 };
    for (size_t c = 0; c < sizeof orders / sizeof orders[0]; c++) {
        const order_case *oc = &orders[c];
        setup();
        set.mode = oc->mode;
        set.octaves = oc->octaves;
        scripted(oc->script, oc->draws);
        for (int k = 0; k < oc->keys; k++) mk_arp_key(press[k], velocity_of(press[k]));
        CHECK_EQ(mk_arp_held(), oc->keys);
        run(12 * 24);
        uint8_t got[64];
        int n = pitches(got, 64);
        int bad = n < oc->len;
        for (int i = 0; !bad && i < oc->len; i++) bad = got[i] != oc->notes[i];
        if (bad) {
            failures++;
            printf("%s:%d: %s, %d keys, %d octaves: got", __FILE__, __LINE__, mk_arp_mode_name[oc->mode], oc->keys, oc->octaves);
            for (int i = 0; i < n && i < 12; i++) printf(" %d", got[i]);
            printf("\n");
        }
        for (int i = 0; i < logged; i++)
            if (log_[i].kind == ON) CHECK_EQ(log_[i].velocity, velocity_of(log_[i].pitch));
        /* one step every 24 ticks: chords at the same tick, single notes one per step */
        int per_step = oc->mode == MK_ARP_POLY ? oc->keys : 1;
        uint32_t ticks[64];
        int m = collect(ON, -1, ticks, 64);
        for (int i = 0; i < m && i < 64; i++) CHECK_EQ(ticks[i], (uint32_t)(i / per_step) * 24);
        if (oc->mode == MK_ARP_RANDOM) CHECK_EQ(draws >= oc->draws, 1);
        else CHECK_EQ(draws, 0);
        CHECK_EQ(doubled, 0);
        CHECK_EQ(unmatched, 0);
    }
    ok("every mode with 1, 2 and 3 keys over 1 and 2 octaves", before);
}

/* Arp Length: the pattern starts again after that many steps; the stock firmware with three keys
 * and a length of 4 plays 60 64 67 60, 60 64 67 60. With Arp Retrigger off it runs on. */
static void test_length(void) {
    int before = failures;
    static const uint8_t four[10] = { 60, 64, 67, 60, 60, 64, 67, 60, 60, 64 };
    static const uint8_t one[4] = { 60, 60, 60, 60 };
    static const uint8_t down[6] = { 67, 64, 60, 67, 67, 64 };
    static const uint8_t free_[8] = { 60, 64, 67, 60, 64, 67, 60, 64 };
    static const uint8_t sixteen[18] = { 60, 64, 67, 60, 64, 67, 60, 64, 67, 60, 64, 67, 60, 64, 67, 60, 60, 64 };
    static const struct {
        uint8_t mode, length, free_run;
        const uint8_t *want;
        int n;
    } cases[] = {
        { MK_ARP_UP, 4, 0, four, 10 },   { MK_ARP_UP, 1, 0, one, 4 },        { MK_ARP_DOWN, 4, 0, down, 6 },
        { MK_ARP_UP, 4, 1, free_, 8 },   { MK_ARP_UP, 0, 0, sixteen, 18 },   { MK_ARP_UP, 16, 0, sixteen, 18 },
        { MK_ARP_ORDER, 4, 0, four, 10 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        setup();
        set.mode = cases[c].mode;
        set.length = cases[c].length;
        set.free_run = cases[c].free_run;
        mk_arp_key(60, 100);
        mk_arp_key(64, 100);
        mk_arp_key(67, 100);
        run(20 * 24);
        uint8_t got[64];
        int n = pitches(got, 64);
        CHECK(n >= cases[c].n);
        for (int i = 0; i < cases[c].n && i < n; i++) CHECK_EQ(got[i], cases[c].want[i]);
        uint32_t ticks[64];
        int m = collect(ON, -1, ticks, 64);
        for (int i = 0; i < m; i++) CHECK_EQ(ticks[i], (uint32_t)i * 24);     /* the grid is not touched */
        CHECK_EQ(doubled, 0);
        CHECK_EQ(unmatched, 0);
    }
    setup();                            /* releasing every key starts the count again */
    set.length = 4;
    set.free_run = 0;
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    run(3 * 24);                        /* 60 64 60 */
    mk_arp_key(60, 0);
    mk_arp_key(64, 0);
    run(24);
    logged = 0;
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    run(5 * 24);
    uint8_t got[16];
    CHECK(pitches(got, 16) >= 5);
    for (int i = 0; i < 5; i++) CHECK_EQ(got[i], four[i]);
    ok("Arp Length restarts the pattern, Retrigger off lets it run", before);
}

static void test_names(void) {
    int before = failures;
    static const char *const want[MK_ARP_MODES] = { "Up", "Down", "UpDown", "Random", "Order", "Poly" };
    for (int m = 0; m < MK_ARP_MODES; m++) {
        const char *a = mk_arp_mode_name[m], *b = want[m];
        while (*a && *a == *b) a++, b++;
        CHECK(*a == *b);
    }
    ok("mode names", before);
}

static void test_gate(void) {
    int before = failures;
    setup();
    mk_arp_key(60, 100);
    run(4 * 24);
    EXPECT_TICKS(ON, 60, 0, 24, 48, 72);
    EXPECT_TICKS(OFF, 60, 12, 36, 60, 84);
    /* gate 10 at 1/32T: 10% of 8 ticks rounds to 0, so one tick */
    setup();
    set.division = MK_DIV_32T;
    set.gate = 10;
    mk_arp_key(60, 100);
    run(24);
    EXPECT_TICKS(ON, 60, 0, 8, 16);
    EXPECT_TICKS(OFF, 60, 1, 9, 17);
    ok("1/16 gate 50: on every 24 ticks, off 12 ticks later", before);
}

static void test_swing(void) {
    int before = failures;
    setup();
    set.swing = 66;
    mk_arp_key(60, 100);
    run(2 * 96);
    /* pairs of 48 ticks; the second step at round(48 * 0.66) = 32, its gate half of 16 */
    EXPECT_TICKS(ON, 60, 0, 32, 48, 80, 96, 128, 144, 176);
    EXPECT_TICKS(OFF, 60, 12, 40, 60, 88, 108, 136, 156, 184);
    ok("swing 66", before);
}

static void test_first_key_waits(void) {
    int before = failures;
    setup();
    run(10);                                            /* the grid began a step at tick 0 */
    CHECK_EQ(logged, 0);
    mk_arp_key(60, 100);
    run(60);
    EXPECT_TICKS(ON, 60, 24, 48);
    /* with swing, idle: the key comes in the second step of a pair (32-47) */
    setup();
    set.swing = 66;
    run(40);
    mk_arp_key(60, 100);
    run(60);
    EXPECT_TICKS(ON, 60, 48, 80, 96);
    /* a key on a boundary plays on that tick */
    setup();
    run(24);
    mk_arp_key(60, 100);
    run(1);
    EXPECT_TICKS(ON, 60, 24);
    ok("the first key waits for the next step of the running grid", before);
}

static void test_sync(void) {
    int before = failures;
    setup();
    mk_arp_key(60, 100);
    run(10);
    mk_arp_sync();                                      /* the note from tick 0 is still on */
    run(40);
    EXPECT_TICKS(ON, 60, 0, 10, 34);
    EXPECT_TICKS(OFF, 60, 10, 22, 46);
    CHECK_EQ(log_[1].kind, OFF);                        /* at tick 10: off first, then on */
    CHECK_EQ(log_[2].kind, ON);
    CHECK_EQ(log_[2].tick, 10);
    /* the swing pair starts again too */
    setup();
    set.swing = 66;
    mk_arp_key(60, 100);
    run(40);                                            /* second step of the pair began at 32 */
    mk_arp_sync();
    run(60);
    EXPECT_TICKS(ON, 60, 0, 32, 40, 72, 88);
    CHECK_EQ(doubled + unmatched, 0);
    ok("mk_arp_sync restarts the grid and the swing pair", before);
}

static void test_release(void) {
    int before = failures;
    setup();
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    run(30);                                            /* 60 at 0, 64 at 24 still on */
    mk_arp_key(64, 0);
    CHECK_EQ(mk_arp_held(), 2);
    run(5 * 24);
    EXPECT_PITCHES(60, 64, 67, 60, 67, 60);
    EXPECT_TICKS(OFF, 64, 36);                          /* the released note keeps its gate */
    /* Order goes on with the key after the one it played */
    setup();
    set.mode = MK_ARP_ORDER;
    mk_arp_key(64, 100);
    mk_arp_key(60, 100);
    mk_arp_key(67, 100);
    run(30);                                            /* 64, 60 */
    mk_arp_key(60, 0);
    run(4 * 24);
    EXPECT_PITCHES(64, 60, 67, 64, 67, 64);
    /* Order: the key played before the released one */
    setup();
    set.mode = MK_ARP_ORDER;
    mk_arp_key(64, 100);
    mk_arp_key(60, 100);
    mk_arp_key(67, 100);
    run(30);
    mk_arp_key(64, 0);
    run(3 * 24);
    EXPECT_PITCHES(64, 60, 67, 60, 67);
    /* UpDown keeps its direction */
    setup();
    set.mode = MK_ARP_UP_DOWN;
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    mk_arp_key(72, 100);
    run(4 * 24 + 6);                                    /* 60 64 67 72, then on the way down */
    mk_arp_key(67, 0);
    run(4 * 24);
    EXPECT_PITCHES(60, 64, 67, 72, 67, 64, 60, 64, 72);
    CHECK_EQ(doubled + unmatched, 0);
    ok("a key released mid-pattern leaves it without restarting", before);
}

static void test_last_released(void) {
    int before = failures;
    setup();
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    run(30);                                            /* 60, 64 */
    mk_arp_key(60, 0);
    mk_arp_key(64, 0);
    mk_arp_key(67, 0);
    CHECK_EQ(mk_arp_held(), 0);
    run(100);
    EXPECT_TICKS(ON, -1, 0, 24);
    EXPECT_TICKS(OFF, 64, 36);                          /* at its normal gate end */
    CHECK(all_quiet());
    mk_arp_key(67, 100);                                /* now 130: the pattern starts again */
    mk_arp_key(60, 100);
    run(3 * 24);
    EXPECT_PITCHES(60, 64, 60, 67, 60);
    uint32_t ticks[8];
    CHECK_EQ(collect(ON, -1, ticks, 8), 5);
    CHECK_EQ(ticks[2], 144);
    ok("the last key released: the note ends at its gate, the arp goes idle", before);
}

static void test_latch(void) {
    int before = failures;
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    run(10);
    mk_arp_key(60, 0);
    mk_arp_key(64, 0);
    CHECK_EQ(mk_arp_held(), 2);
    run(3 * 24);                                        /* 24, 48, 72 */
    EXPECT_PITCHES(60, 64, 60, 64);
    mk_arp_key(67, 90);                                 /* replaces the latched chord */
    CHECK_EQ(mk_arp_held(), 1);
    mk_arp_key(72, 90);                                 /* 67 is still down: added */
    CHECK_EQ(mk_arp_held(), 2);
    run(3 * 24);
    EXPECT_PITCHES(60, 64, 60, 64, 67, 72, 67);         /* from the beginning of the new set */
    mk_arp_key(67, 0);
    mk_arp_key(72, 0);
    CHECK_EQ(mk_arp_held(), 2);
    mk_arp_key(72, 100);                                /* a latched pitch again: a new set of one */
    CHECK_EQ(mk_arp_held(), 1);
    /* latch add: while one key is down, others join */
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(64, 0);
    mk_arp_key(67, 100);                                /* 60 still down */
    CHECK_EQ(mk_arp_held(), 3);
    mk_arp_key(60, 0);
    mk_arp_key(67, 0);
    CHECK_EQ(mk_arp_held(), 3);
    run(3 * 24);
    EXPECT_PITCHES(60, 64, 67);
    CHECK_EQ(doubled + unmatched, 0);
    ok("latch: a new chord replaces, keys added while one is down join", before);
}

static void test_hold_off(void) {
    int before = failures;
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    mk_arp_key(60, 0);
    mk_arp_key(67, 0);
    run(30);
    CHECK_EQ(mk_arp_held(), 3);
    mk_arp_hold(0);                                     /* only 64 is down */
    CHECK_EQ(mk_arp_held(), 1);
    run(2 * 24);
    EXPECT_PITCHES(60, 64, 64, 64);
    mk_arp_key(64, 0);
    CHECK_EQ(mk_arp_held(), 0);
    /* hold off with nothing down: idle */
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    mk_arp_key(60, 0);
    run(10);
    mk_arp_hold(0);
    CHECK_EQ(mk_arp_held(), 0);
    run(100);
    EXPECT_TICKS(ON, 60, 0);
    CHECK(all_quiet());
    ok("hold off drops the latched notes", before);
}

static void test_settings_change(void) {
    int before = failures;
    setup();
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    mk_arp_key(67, 100);
    run(30);                                            /* 60 at 0, 64 at 24 */
    set.division = MK_DIV_4;                            /* from the next step, at 48 */
    run(20);
    set.gate = 90;                                      /* 67 is on since 48 with gate 48 */
    run(200);
    EXPECT_TICKS(ON, -1, 0, 24, 48, 144, 240);
    EXPECT_TICKS(OFF, 67, 96);
    uint32_t ticks[8];
    CHECK_EQ(collect(OFF, 60, ticks, 8), 2);
    CHECK_EQ(ticks[1], 144 + 86);                       /* 90% of 96 */
    /* the mode and the octaves */
    setup();
    set.octaves = 2;
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    run(2 * 24 + 6);                                    /* 60 64 72 */
    set.mode = MK_ARP_DOWN;
    run(2 * 24);                                        /* from 72 down: 64, 60 */
    set.octaves = 1;
    set.mode = MK_ARP_UP;
    run(3 * 24);
    EXPECT_PITCHES(60, 64, 72, 64, 60, 64, 60, 64);
    set.octaves = 4;
    set.mode = MK_ARP_ORDER;                            /* goes on after 64 */
    run(3 * 24);
    EXPECT_PITCHES(60, 64, 72, 64, 60, 64, 60, 64, 72, 76, 84);
    set.mode = MK_ARP_POLY;                             /* octave 3 next */
    run(24);
    set.octaves = 1;                                    /* the octave it was in is gone */
    run(2 * 24);
    EXPECT_PITCHES(60, 64, 72, 64, 60, 64, 60, 64, 72, 76, 84, 96, 100, 60, 64, 60, 64);
    CHECK_EQ(doubled + unmatched, 0);
    /* out of range: mode Up, 4 octaves, 1/16, gate 90, swing 75 */
    setup();
    set = (mk_arp_settings){ 200, 9, 99, 255, 255, 0, 1, 0 };
    mk_arp_key(60, 100);
    run(4 * 48);
    EXPECT_PITCHES(60, 72, 84, 96, 60, 72, 84, 96);
    EXPECT_TICKS(ON, 60, 0, 96);
    EXPECT_TICKS(ON, 72, 36, 132);                      /* swing 75 */
    EXPECT_TICKS(OFF, 60, 21, 117);                     /* 90% of 24 */
    set = (mk_arp_settings){ 0, 0, 0, 0, 0, 0, 1, 0 };      /* 1 octave, 1/2D, gate 10, swing 50 */
    run(300);                                           /* the next step, at 192, lasts 288 */
    EXPECT_TICKS(ON, 60, 0, 96, 192, 480);
    EXPECT_TICKS(OFF, 60, 21, 117, 192 + 28);
    CHECK_EQ(doubled + unmatched, 0);
    ok("settings changed while running, and out of range", before);
}

static void test_above_127(void) {
    int before = failures;
    setup();
    set.octaves = 2;
    mk_arp_key(120, 100);
    run(4 * 24);
    EXPECT_TICKS(ON, 120, 0, 48);                       /* 132 is a rest */
    CHECK_EQ(count(ON), 2);
    setup();
    set.mode = MK_ARP_POLY;
    set.octaves = 2;
    mk_arp_key(120, 100);
    mk_arp_key(100, 100);
    run(2 * 24);
    EXPECT_PITCHES(100, 120, 112);
    setup();
    set.octaves = 3;
    mk_arp_key(110, 100);
    mk_arp_key(100, 100);
    run(7 * 24);                                        /* 134 is a rest */
    EXPECT_PITCHES(100, 110, 112, 122, 124, 100);
    EXPECT_TICKS(ON, -1, 0, 24, 48, 72, 96, 144);
    CHECK_EQ(doubled + unmatched, 0);
    ok("notes above 127 are left out", before);
}

static void test_clear(void) {
    int before = failures;
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    mk_arp_key(64, 100);
    run(28);                                            /* 64 on since 24 */
    mk_arp_clear();
    CHECK_EQ(mk_arp_held(), 0);
    CHECK(all_quiet());
    EXPECT_TICKS(OFF, 64, 28);
    run(100);
    CHECK_EQ(count(ON), 2);
    mk_arp_key(64, 0);                                  /* keys forgotten by the clear go up */
    mk_arp_key(67, 100);                                /* the latch is still on */
    mk_arp_key(67, 0);
    CHECK_EQ(mk_arp_held(), 1);
    run(24);
    EXPECT_PITCHES(60, 64, 67);
    mk_arp_clear();
    mk_arp_clear();
    CHECK(all_quiet());
    CHECK_EQ(doubled + unmatched, 0);
    ok("mk_arp_clear ends the notes and forgets the keys", before);
}

/* The same pitch on consecutive steps, with a grid restart in the middle of its gate. */
static void test_repeated_pitch(void) {
    int before = failures;
    setup();
    set.gate = 90;
    set.division = MK_DIV_32T;
    mk_arp_key(60, 100);
    for (int i = 0; i < 50; i++) {
        run(3);
        if (i % 7 == 0) mk_arp_sync();
    }
    CHECK(count(ON) > 20);
    for (int i = 1; i < logged; i++) CHECK(log_[i].kind != log_[i - 1].kind);
    CHECK_EQ(doubled + unmatched, 0);
    ok("one pitch on every step: always off before the next on", before);
}

/* A tick that "interrupts" a main-loop call is played when the call lets go. */
static int ticked_inside;
static void tick_inside(void) {
    if (ticked_inside) return;
    ticked_inside = 1;
    mk_arp_tick();
}

static void test_deferred_tick(void) {
    int before = failures;
    setup();
    mk_arp_hold(1);
    mk_arp_key(60, 100);
    run(4);
    ticked_inside = 0;
    on_note = tick_inside;
    mk_arp_clear();                                     /* its note off "interrupts" with a tick */
    on_note = NULL;
    CHECK_EQ(ticked_inside, 1);
    CHECK_EQ(count(OFF), 1);
    mk_arp_key(64, 100);
    run(19);                                            /* tick 4 was played inside: 20 so far */
    CHECK_EQ(count(ON), 1);
    run(1);
    EXPECT_TICKS(ON, 64, 23);                           /* the 24th tick, the next step */
    ok("a tick during a main-loop call waits for it", before);
}

static uint32_t roll(uint32_t n) { return xorshift() % n; }

static void test_soak(void) {
    int before = failures;
    setup();
    notes_on = 0;
    uint8_t down[128] = { 0 };
    for (int i = 0; i < 300000; i++) {
        logged = 0;                                     /* the checks run in the callback */
        mk_arp_tick();
        now++;
        if (roll(8)) continue;
        uint8_t note = (uint8_t)(roll(4) == 0 ? 90 + roll(38) : 36 + roll(36));
        switch (roll(20)) {
        case 0: case 1: case 2: case 3: case 4:
            if (!down[note]) {
                down[note] = 1;
                mk_arp_key(note, (uint8_t)(1 + roll(127)));
            }
            break;
        case 5: case 6: case 7: case 8: case 9: case 10:
            for (int k = 0; k < 128; k++) {
                int n = (note + k) & 127;
                if (!down[n]) continue;
                down[n] = 0;
                mk_arp_key((uint8_t)n, 0);
                break;
            }
            break;
        case 11: mk_arp_hold((int)roll(2)); break;
        case 12: set.mode = (uint8_t)roll(MK_ARP_MODES + 1); break;
        case 13: set.octaves = (uint8_t)roll(6); break;
        case 14: set.division = (uint8_t)roll(MK_DIV_COUNT + 1); break;
        case 15: set.gate = (uint8_t)(roll(8) ? 10 + roll(81) : roll(256)); break;
        case 16: set.swing = (uint8_t)(roll(8) ? 50 + roll(26) : roll(256)); break;
        case 17:
            if (roll(20) == 0) mk_arp_clear();
            else if (roll(4) == 0) set.length = (uint8_t)roll(70), set.free_run = (uint8_t)roll(2);
            break;
        case 18: if (roll(10) == 0) mk_arp_sync(); break;
        default: mk_arp_key(note, 0); down[note] = 0; break;     /* up for a key never down too */
        }
        CHECK(mk_arp_held() >= 0 && mk_arp_held() <= 16);
    }
    mk_arp_clear();
    CHECK_EQ(doubled, 0);
    CHECK_EQ(unmatched, 0);
    CHECK(all_quiet());
    CHECK(notes_on > 10000);
    printf("   %ld notes over 300000 ticks\n", notes_on);
    ok("long random run: every note on ends, none twice", before);
}

/* mk_arp_render: the pattern as steps, without a sound and without moving the arp. */
static void test_render(void) {
    int before = failures;
    static mk_step out[MK_STEPS];
    setup();
    for (int i = 0; i < MK_STEPS; i++) out[i] = (mk_step){ 0 };
    CHECK_EQ(mk_arp_render(out, MK_STEPS), 0);          /* no keys: nothing */
    mk_arp_key(64, 90);                                 /* E C G, pressed in that order */
    mk_arp_key(60, 100);
    mk_arp_key(67, 80);
    run(30);                                            /* the arp is playing: two steps in */
    int logged_before = logged;
    CHECK_EQ(mk_arp_render(out, MK_STEPS), 3);          /* Up, free running: once round the set */
    CHECK_EQ(out[0].note[0], 60);
    CHECK_EQ(out[0].velocity[0], 100);
    CHECK_EQ(out[1].note[0], 64);
    CHECK_EQ(out[2].note[0], 67);
    CHECK_EQ(out[2].velocity[0], 80);
    CHECK_EQ(out[0].note[1], 0);
    CHECK_EQ(out[3].note[0], 0);
    CHECK_EQ(logged, logged_before);                    /* nothing sounded */
    run(24 * 4);                                        /* and the arp goes on from where it was */
    EXPECT_PITCHES(60, 64, 67, 60, 64, 67);
    set.octaves = 2;
    set.mode = MK_ARP_UP_DOWN;
    for (int i = 0; i < MK_STEPS; i++) out[i] = (mk_step){ 0 };
    CHECK_EQ(mk_arp_render(out, MK_STEPS), 10);         /* six places up and down, the ends once */
    static const uint8_t up_down[10] = { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64 };
    for (int i = 0; i < 10; i++) CHECK_EQ(out[i].note[0], up_down[i]);
    set.mode = MK_ARP_POLY;
    for (int i = 0; i < MK_STEPS; i++) out[i] = (mk_step){ 0 };
    CHECK_EQ(mk_arp_render(out, MK_STEPS), 2);          /* the chord in each octave */
    CHECK_EQ(out[0].note[0], 60);
    CHECK_EQ(out[0].note[2], 67);
    CHECK_EQ(out[1].note[1], 76);
    set.mode = MK_ARP_ORDER;
    set.octaves = 1;
    set.free_run = 0;                                   /* Arp Length 8: eight steps, the set round and round */
    set.length = 8;
    for (int i = 0; i < MK_STEPS; i++) out[i] = (mk_step){ 0 };
    CHECK_EQ(mk_arp_render(out, MK_STEPS), 8);
    static const uint8_t order[8] = { 64, 60, 67, 64, 60, 67, 64, 60 };
    for (int i = 0; i < 8; i++) CHECK_EQ(out[i].note[0], order[i]);
    CHECK_EQ(mk_arp_render(out, 5), 5);                 /* no more than there is room for */
    CHECK_EQ(doubled + unmatched, 0);
    ok("mk_arp_render writes the pattern as steps without sounding it or moving the arp", before);
}

int main(void) {
    test_names();
    test_orders();
    test_length();
    test_gate();
    test_swing();
    test_first_key_waits();
    test_sync();
    test_release();
    test_last_released();
    test_latch();
    test_hold_off();
    test_settings_change();
    test_above_127();
    test_clear();
    test_repeated_pitch();
    test_deferred_tick();
    test_soak();
    test_render();
    if (failures) printf("%d checks failed\n", failures);
    return failures != 0;
}
