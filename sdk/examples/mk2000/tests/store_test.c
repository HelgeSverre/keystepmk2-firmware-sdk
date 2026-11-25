/* Host tests for store.c over a fake NOR flash. Run: make -C sdk/examples/mk2000/tests store
 *
 * The fake: 512 KiB, writes AND into what is there, an erase sets a 4 KiB sector to FF. It counts
 * erases and writes per sector, fails the test on any erase or write outside the SDK's area, and can
 * cut the power: after `budget` more bytes written or sectors erased the operation in progress
 * stops (a write keeps the bytes written so far, an erase leaves garbage) and every call fails
 * until power_on(). */
#include <stdio.h>
#include <string.h>
#include "keystep.h"
#include "mk.h"

#define AREA_SECTORS 31
#define AREA_END (KS_FLASH_USER + KS_FLASH_USER_SIZE)
#define CHIP_SECTORS ((int)(KS_FLASH_SIZE / KS_FLASH_SECTOR))

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                             \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        long long a_ = (long long)(a), b_ = (long long)(b);                         \
        if (a_ != b_) {                                                             \
            printf("%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b, a_, b_); \
            failures++;                                                             \
        }                                                                           \
    } while (0)

/* ---- the fake flash ---- */

static uint8_t chip[KS_FLASH_SIZE];
static uint32_t erases[CHIP_SECTORS], writes[CHIP_SECTORS];
static int outside;                     /* erases or writes attempted outside the area */
static long budget = -1;                /* bytes or erases until the power goes; -1: never */
static int power_off;
static int cut_style;                   /* what a cut erase leaves */
static int weak_sector = -1;            /* reads blank, but takes no writes until it is erased */
static uint32_t rng = 12345;

static uint32_t next_random(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int in_area(uint32_t addr, uint32_t len) {
    return addr >= KS_FLASH_USER && len <= KS_FLASH_USER_SIZE && addr + len <= AREA_END;
}

int ks_flash_read(uint32_t addr, void *data, uint32_t len) {
    if (power_off) return KS_FLASH_TIMEOUT;
    if (len > KS_FLASH_SIZE || addr > KS_FLASH_SIZE - len) return KS_FLASH_RANGE;
    memcpy(data, chip + addr, len);
    return 0;
}

int ks_flash_erase(uint32_t addr) {
    if (power_off) return KS_FLASH_TIMEOUT;
    addr &= ~(KS_FLASH_SECTOR - 1);
    if (!in_area(addr, KS_FLASH_SECTOR)) {
        printf("erase outside the area at %06x\n", (unsigned)addr);
        outside++;
        return KS_FLASH_RANGE;
    }
    uint8_t *sector = chip + addr;
    erases[addr / KS_FLASH_SECTOR]++;
    if (budget == 0) {
        int style = cut_style % 3;
        for (unsigned i = 0; i < KS_FLASH_SECTOR; i++) {
            if (style == 0) sector[i] |= (uint8_t)next_random();        /* partly erased */
            if (style == 1) sector[i] = (uint8_t)next_random();         /* anything */
        }                                                               /* style 2: not started */
        power_off = 1;
        return KS_FLASH_TIMEOUT;
    }
    if (budget > 0) budget--;
    memset(sector, 0xFF, KS_FLASH_SECTOR);
    if ((int)(addr / KS_FLASH_SECTOR) == weak_sector) weak_sector = -1;
    return 0;
}

int ks_flash_write(uint32_t addr, const void *data, uint32_t len) {
    if (power_off) return KS_FLASH_TIMEOUT;
    if (!in_area(addr, len)) {
        printf("write outside the area at %06x, %u bytes\n", (unsigned)addr, (unsigned)len);
        outside++;
        return KS_FLASH_RANGE;
    }
    const uint8_t *p = data;
    writes[addr / KS_FLASH_SECTOR]++;
    for (uint32_t i = 0; i < len; i++) {
        if (budget == 0) {
            power_off = 1;
            return KS_FLASH_TIMEOUT;
        }
        if (budget > 0) budget--;
        if ((int)((addr + i) / KS_FLASH_SECTOR) != weak_sector) chip[addr + i] &= p[i];
    }
    return 0;
}

/* The first two cut runs (where a save erases) are each tried with the three kinds of erase
 * garbage; the others once. */
static long cut_at(long i) {
    cut_style = (int)(i % 3);
    return i < 6 ? i / 3 : i - 4;
}

static void power_on(void) {
    power_off = 0;
    budget = -1;
}

/* The rest of the chip holds a pattern the store must never change. */
static uint8_t outside_byte(uint32_t addr) { return (uint8_t)(addr * 7 + (addr >> 12) + 0x5A); }

static void reset_flash(void) {
    memset(chip + KS_FLASH_USER, 0xFF, KS_FLASH_USER_SIZE);     /* the outside is set once, in main */
    memset(erases, 0, sizeof erases);
    memset(writes, 0, sizeof writes);
    power_on();
}

static int outside_intact(void) {
    for (uint32_t a = 0; a < KS_FLASH_SIZE; a++)
        if (!in_area(a, 1) && chip[a] != outside_byte(a)) return 0;
    return 1;
}

static uint8_t snapshot[KS_FLASH_USER_SIZE];
static void save_snapshot(void) { memcpy(snapshot, chip + KS_FLASH_USER, sizeof snapshot); }
static void load_snapshot(void) { memcpy(chip + KS_FLASH_USER, snapshot, sizeof snapshot); }

/* ---- patterns ---- */

static void make_typical(mk_pattern *p, int seed) {
    mk_pattern_init(p);
    p->tempo = (uint16_t)(1000 + seed);
    for (int t = 0; t < MK_TRACKS; t++)
        for (int s = 0; s < 16; s += 2) {
            mk_step *step = &p->track[t].step[s];
            int n = (s + seed) % 3 == 0 ? 2 : 1;
            for (int i = 0; i < n; i++) {
                step->note[i] = (uint8_t)(36 + (seed + t * 5 + s + i * 7) % 60);
                step->velocity[i] = (uint8_t)(64 + (seed + s) % 60);
            }
        }
}

/* Every field away from its default. */
static void make_worst(mk_pattern *p, int seed) {
    mk_pattern_init(p);
    p->tempo = (uint16_t)(1755 + seed);
    p->scale = 3;
    p->root = 7;
    for (int t = 0; t < MK_TRACKS; t++) {
        mk_track *track = &p->track[t];
        track->channel = (uint8_t)(15 - t);
        track->length = 64;
        track->division = MK_DIV_32T;
        track->swing = 61;
        track->gate = 77;
        track->direction = MK_DIR_PENDULUM;
        track->muted = 1;
        track->follows_transpose = 1;
        track->shift = (int8_t)(-12 - t);
        for (int s = 0; s < MK_STEPS; s++) {
            mk_step *step = &track->step[s];
            for (int i = 0; i < MK_POLY; i++) {
                step->note[i] = (uint8_t)(1 + (seed + t + s * 3 + i * 11) % 127);
                step->velocity[i] = (uint8_t)(1 + (seed + s + i) % 127);
            }
            step->gate = (uint8_t)(1 + (s + seed) % 100);
            step->probability = (uint8_t)((s + seed) % 100);
            step->ratchet = (uint8_t)(2 + s % 3);
            step->condition = (uint8_t)(1 + s % (MK_COND_COUNT - 1));
            step->flags = MK_STEP_TIE;
            for (int c = 0; c < MK_CONTROLLERS; c++) step->controller[c] = (uint8_t)((s + c + seed) % 128);
            step->offset = (int8_t)((s * 37 + seed) % 255 - 127);
            if (!step->offset) step->offset = -128;
        }
    }
}

static int quiet;                       /* same_pattern does not say where they differ */

#define SAY(...) (quiet ? 0 : printf(__VA_ARGS__))

/* Field by field; prints the first difference. */
static int same_pattern(const mk_pattern *a, const mk_pattern *b) {
    if (a->tempo != b->tempo || a->scale != b->scale || a->root != b->root) {
        SAY("  pattern fields differ\n");
        return 0;
    }
    for (int t = 0; t < MK_TRACKS; t++) {
        const mk_track *x = &a->track[t], *y = &b->track[t];
        if (x->channel != y->channel || x->length != y->length || x->division != y->division || x->swing != y->swing ||
            x->gate != y->gate || x->direction != y->direction || x->muted != y->muted ||
            x->follows_transpose != y->follows_transpose || x->shift != y->shift || x->spice != y->spice ||
            x->dice != y->dice || x->reserved != y->reserved) {
            SAY("  track %d fields differ\n", t);
            return 0;
        }
        for (int s = 0; s < MK_STEPS; s++) {
            const mk_step *u = &x->step[s], *v = &y->step[s];
            int same = u->gate == v->gate && u->probability == v->probability && u->ratchet == v->ratchet &&
                       u->condition == v->condition && u->flags == v->flags && u->offset == v->offset;
            for (int i = 0; i < MK_POLY; i++) same = same && u->note[i] == v->note[i] && u->velocity[i] == v->velocity[i];
            for (int c = 0; c < MK_CONTROLLERS; c++) same = same && u->controller[c] == v->controller[c];
            if (!same) {
                SAY("  track %d step %d differs\n", t, s);
                return 0;
            }
        }
    }
    return 1;
}

static int reserved_zero(const mk_pattern *p) {
    for (int t = 0; t < MK_TRACKS; t++)
        if (p->track[t].spice || p->track[t].dice || p->track[t].reserved) return 0;
    return 1;
}

static mk_pattern a, b, c, loaded;      /* too big for the stack under the sanitizers' taste */

static void fill_garbage(void *p, size_t n) { memset(p, 0xAB, n); }

static int untouched(const void *p, size_t n) {
    const uint8_t *q = p;
    for (size_t i = 0; i < n; i++)
        if (q[i] != 0xAB) return 0;
    return 1;
}

static void check_round_trip(int slot, const mk_pattern *p) {
    fill_garbage(&loaded, sizeof loaded);
    CHECK_EQ(mk_store_load(slot, &loaded), MK_STORE_OK);
    CHECK(same_pattern(&loaded, p));
    CHECK(reserved_zero(&loaded));
}

static uint32_t crc32(const uint8_t *p, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? crc >> 1 ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* Sectors holding a committed, live part 0 of pattern `slot`: the format, read independently. */
static int live_versions(int slot) {
    int n = 0;
    for (int s = 0; s < AREA_SECTORS; s++) {
        const uint8_t *h = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        uint32_t crc = h[20] | h[21] << 8 | h[22] << 16 | (uint32_t)h[23] << 24;
        int live = h[24] == 0xFF && h[25] == 0xFF && h[26] == 0xFF && h[27] == 0xFF;
        if (memcmp(h, "MK2S", 4) == 0 && crc == crc32(h, 20) && h[5] == 1 && h[6] == slot && h[7] == 0 && live) n++;
    }
    return n;
}

/* Payload bytes on the flash for pattern `slot`, all versions. */
static int stored_bytes(int slot) {
    int n = 0;
    for (int s = 0; s < AREA_SECTORS; s++) {
        const uint8_t *h = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        uint32_t crc = h[20] | h[21] << 8 | h[22] << 16 | (uint32_t)h[23] << 24;
        if (memcmp(h, "MK2S", 4) == 0 && crc == crc32(h, 20) && h[5] == 1 && h[6] == slot) n += h[10] | h[11] << 8;
    }
    return n;
}

static void fresh(void) {
    reset_flash();
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
}

/* ---- tests ---- */

static void test_blank(void) {
    fresh();
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS);
    for (int s = 0; s < MK_SLOTS; s++) CHECK(!mk_store_has(s));
    fill_garbage(&loaded, sizeof loaded);
    CHECK_EQ(mk_store_load(0, &loaded), MK_STORE_EMPTY);
    CHECK(untouched(&loaded, sizeof loaded));
    mk_settings settings;
    mk_chain chain;
    fill_garbage(&settings, sizeof settings);
    fill_garbage(&chain, sizeof chain);
    CHECK_EQ(mk_store_load_settings(&settings), MK_STORE_EMPTY);
    CHECK_EQ(mk_store_load_chain(1, &chain), MK_STORE_EMPTY);
    CHECK(untouched(&settings, sizeof settings));
    CHECK(untouched(&chain, sizeof chain));
    CHECK_EQ(mk_store_load(64, &loaded), MK_STORE_ERROR);
    CHECK_EQ(mk_store_load(-1, &loaded), MK_STORE_ERROR);
    CHECK(!mk_store_has(64));
    for (int s = 0; s < CHIP_SECTORS; s++) CHECK_EQ(erases[s] + writes[s], 0);
    printf("ok blank\n");
}

static int sectors_for(const mk_pattern *p) {
    fresh();
    int before = mk_store_free_sectors();
    CHECK_EQ(mk_store_save(7, p), MK_STORE_OK);
    CHECK(mk_store_has(7));
    check_round_trip(7, p);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);       /* and after a reboot */
    check_round_trip(7, p);
    return before - mk_store_free_sectors();
}

static void test_round_trips(void) {
    mk_pattern_init(&a);
    CHECK_EQ(sectors_for(&a), 1);
    printf("  empty pattern: 1 sector, %d bytes\n", stored_bytes(7));
    make_typical(&a, 3);
    CHECK_EQ(sectors_for(&a), 1);
    printf("  typical pattern: 1 sector, %d bytes\n", stored_bytes(7));
    make_worst(&a, 1);
    int worst = sectors_for(&a);
    CHECK_EQ(worst, 2);
    printf("  worst-case pattern: %d sectors, %d bytes (%d in RAM)\n", worst, stored_bytes(7), (int)sizeof a);

    /* steps beyond the length and odd poly slots are kept as they are */
    mk_pattern_init(&a);
    a.track[2].length = 4;
    a.track[2].step[40].note[3] = 99;
    a.track[2].step[40].velocity[5] = 7;
    a.track[1].step[63].controller[2] = 0;
    a.track[0].step[0].probability = 0;
    CHECK_EQ(sectors_for(&a), 1);
    printf("ok round_trips\n");
}

/* The header byte of a pattern's live part 0: its format version, or -1. */
static int format_of(int slot) {
    for (int s = 0; s < AREA_SECTORS; s++) {
        const uint8_t *h = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        if (memcmp(h, "MK2S", 4) == 0 && h[5] == 1 && h[6] == slot && h[7] == 0 && h[24] == 0xFF) return h[4];
    }
    return -1;
}

static void test_offsets(void) {
    make_typical(&a, 3);
    CHECK_EQ(sectors_for(&a), 1);
    int plain = stored_bytes(7);
    CHECK_EQ(format_of(7), 1);                          /* no offsets: what older firmware reads */
    a.track[0].step[0].offset = -40;                    /* a step with notes: one byte more */
    a.track[1].step[5].offset = 127;                    /* an empty step: its flags and the offset */
    a.track[3].step[63].offset = -128;
    CHECK_EQ(sectors_for(&a), 1);
    CHECK_EQ(format_of(7), 2);
    CHECK_EQ(stored_bytes(7), plain + 1 + 2 + 2);
    a.track[1].step[5].offset = 0;                      /* and back: format 1 again */
    a.track[0].step[0].offset = 0;
    a.track[3].step[63].offset = 0;
    CHECK_EQ(sectors_for(&a), 1);
    CHECK_EQ(format_of(7), 1);
    CHECK_EQ(stored_bytes(7), plain);

    mk_pattern_init(&a);                                /* every field: written whole, offset included */
    mk_step *st = &a.track[2].step[9];
    st->note[0] = 60;
    st->velocity[0] = 90;
    st->gate = 30;
    st->probability = 50;
    st->ratchet = 2;
    st->condition = 1;
    st->flags = MK_STEP_TIE;
    st->controller[1] = 5;
    st->offset = 12;
    CHECK_EQ(sectors_for(&a), 1);
    make_worst(&a, 5);                                  /* raw steps with offsets */
    CHECK_EQ(sectors_for(&a), 2);
    printf("ok offsets\n");
}

/* A pattern record as the firmware before the offsets wrote it, built by hand: step 0 whole (FF
 * and 25 bytes), step 1 with notes and ratchet. `extra` adds an offset field to step 1, which
 * format 1 does not have. */
static void write_format1(int slot, int extra) {
    uint8_t payload[200], *p = payload;
    *p++ = 0xB0;                                        /* 1200 */
    *p++ = 0x04;
    *p++ = 0;
    *p++ = 0;
    for (int t = 0; t < MK_TRACKS; t++) {
        const uint8_t settings[12] = { (uint8_t)t, 16, MK_DIV_16, 50, 50, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(p, settings, 12);
        p += 12;
        for (int i = 0; i < 8; i++) *p++ = t == 0 && i == 0 ? 3 : 0;
        if (t) continue;
        *p++ = 0xFF;
        for (int i = 0; i < MK_POLY; i++) *p++ = (uint8_t)(i < 3 ? 48 + i : 0);
        for (int i = 0; i < MK_POLY; i++) *p++ = (uint8_t)(i < 3 ? 100 + i : 0);
        *p++ = 40;                                      /* gate, probability, ratchet, condition, flags */
        *p++ = 100;
        *p++ = 1;
        *p++ = 0;
        *p++ = MK_STEP_TIE;
        for (int c = 0; c < MK_CONTROLLERS; c++) *p++ = c == 0 ? 64 : MK_CTL_NONE;
        *p++ = (uint8_t)(0x09 | (extra ? 0x80 : 0));
        *p++ = 1;
        *p++ = 60;
        *p++ = 99;
        *p++ = 3;
        if (extra) *p++ = 10;
    }
    uint32_t len = (uint32_t)(p - payload);
    uint8_t *sector = chip + KS_FLASH_USER + 5 * KS_FLASH_SECTOR;
    memset(sector, 0xFF, KS_FLASH_SECTOR);
    memcpy(sector, "MK2S", 4);
    sector[4] = 1;
    sector[5] = 1;
    sector[6] = (uint8_t)slot;
    sector[7] = 0;
    sector[8] = 1;
    sector[9] = 0;
    sector[10] = (uint8_t)len;
    sector[11] = (uint8_t)(len >> 8);
    uint32_t seq = 40, crc = crc32(payload, len);
    for (int i = 0; i < 4; i++) {
        sector[12 + i] = (uint8_t)(seq >> 8 * i);
        sector[16 + i] = (uint8_t)(crc >> 8 * i);
    }
    uint32_t hcrc = crc32(sector, 20);
    for (int i = 0; i < 4; i++) sector[20 + i] = (uint8_t)(hcrc >> 8 * i);
    memcpy(sector + 32, payload, len);
}

static void test_format1(void) {
    reset_flash();
    write_format1(12, 0);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    CHECK(mk_store_has(12));
    fill_garbage(&loaded, sizeof loaded);
    CHECK_EQ(mk_store_load(12, &loaded), MK_STORE_OK);
    mk_pattern_init(&a);
    mk_step *st = &a.track[0].step[0];
    for (int i = 0; i < 3; i++) {
        st->note[i] = (uint8_t)(48 + i);
        st->velocity[i] = (uint8_t)(100 + i);
    }
    st->gate = 40;
    st->flags = MK_STEP_TIE;
    st->controller[0] = 64;
    st = &a.track[0].step[1];
    st->note[0] = 60;
    st->velocity[0] = 99;
    st->ratchet = 3;
    CHECK(same_pattern(&loaded, &a));                   /* the offsets all 0 */
    CHECK_EQ(mk_store_save(12, &loaded), MK_STORE_OK);  /* saved again: still format 1 */
    CHECK_EQ(format_of(12), 1);
    check_round_trip(12, &a);

    reset_flash();                                      /* an offset field in format 1 is damage */
    write_format1(12, 1);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    CHECK_EQ(mk_store_load(12, &loaded), MK_STORE_ERROR);
    printf("ok format1\n");
}

static void test_fill(void) {
    fresh();
    int fit = 0;
    for (int s = 0; s < MK_SLOTS; s++) {
        make_typical(&a, s);
        int result = mk_store_save(s, &a);
        if (result == MK_STORE_FULL) break;
        CHECK_EQ(result, MK_STORE_OK);
        fit++;
    }
    CHECK_EQ(fit, AREA_SECTORS);
    CHECK_EQ(mk_store_free_sectors(), 0);
    CHECK(!mk_store_has(fit));
    make_typical(&b, 99);
    CHECK_EQ(mk_store_save(0, &b), MK_STORE_FULL);   /* no room beside the old version */
    make_typical(&a, 0);
    check_round_trip(0, &a);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    for (int s = 0; s < fit; s++) {
        make_typical(&a, s);
        check_round_trip(s, &a);
    }
    CHECK_EQ(mk_store_erase(30), MK_STORE_OK);
    CHECK_EQ(mk_store_save(0, &b), MK_STORE_OK);
    check_round_trip(0, &b);
    printf("  %d typical patterns fit\n", fit);

    /* every slot number works: the rest of them in a second round */
    for (int s = 0; s < fit; s++) CHECK_EQ(mk_store_erase(s), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS);
    int from = MK_SLOTS - fit;                      /* 33 to 63, then 31 and 32 in place of 33 and 34 */
    for (int s = from; s < MK_SLOTS; s++) {
        make_typical(&a, s);
        CHECK_EQ(mk_store_save(s, &a), MK_STORE_OK);
    }
    for (int s = fit; s < from; s++) {
        CHECK_EQ(mk_store_erase(s + from - fit), MK_STORE_OK);
        make_typical(&a, s);
        CHECK_EQ(mk_store_save(s, &a), MK_STORE_OK);
    }
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    for (int s = 0; s < MK_SLOTS; s++) {
        int stored = s >= fit && (s < from || s >= from + from - fit);
        CHECK_EQ(mk_store_has(s), stored);
        make_typical(&a, s);
        if (stored) check_round_trip(s, &a);
    }

    /* worst cases: two sectors each */
    fresh();
    int worst = 0;
    for (int s = 0; s < MK_SLOTS; s++) {
        make_worst(&a, s);
        if (mk_store_save(s, &a) != MK_STORE_OK) break;
        worst++;
    }
    CHECK_EQ(worst, AREA_SECTORS / 2);
    CHECK_EQ(mk_store_free_sectors(), 1);
    make_worst(&b, 77);
    CHECK_EQ(mk_store_save(3, &b), MK_STORE_FULL);
    make_worst(&a, 3);
    check_round_trip(3, &a);
    make_typical(&b, 5);
    CHECK_EQ(mk_store_save(3, &b), MK_STORE_OK);    /* a smaller one fits beside the old */
    check_round_trip(3, &b);
    CHECK_EQ(mk_store_free_sectors(), 2);
    printf("ok fill\n");
}

static void test_overwrite(void) {
    fresh();
    make_typical(&a, 1);
    CHECK_EQ(mk_store_save(9, &a), MK_STORE_OK);
    int free_after_one = mk_store_free_sectors();
    for (int i = 0; i < 5; i++) {
        make_typical(&b, 10 + i);
        CHECK_EQ(mk_store_save(9, &b), MK_STORE_OK);
        CHECK_EQ(mk_store_free_sectors(), free_after_one);
        CHECK_EQ(live_versions(9), 1);
        check_round_trip(9, &b);
    }
    make_worst(&b, 4);
    CHECK_EQ(mk_store_save(9, &b), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - 2);
    make_typical(&b, 6);
    CHECK_EQ(mk_store_save(9, &b), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - 1);
    CHECK_EQ(live_versions(9), 1);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - 1);
    check_round_trip(9, &b);
    printf("ok overwrite\n");
}

static void test_erase(void) {
    fresh();
    make_typical(&a, 1);
    make_worst(&b, 2);
    CHECK_EQ(mk_store_save(1, &a), MK_STORE_OK);
    CHECK_EQ(mk_store_save(1, &a), MK_STORE_OK);    /* an older version on the flash too */
    CHECK_EQ(mk_store_save(2, &b), MK_STORE_OK);
    CHECK_EQ(mk_store_erase(1), MK_STORE_OK);
    CHECK_EQ(mk_store_erase(2), MK_STORE_OK);
    CHECK_EQ(mk_store_erase(2), MK_STORE_OK);       /* nothing there: fine */
    CHECK_EQ(mk_store_erase(64), MK_STORE_ERROR);
    CHECK(!mk_store_has(1));
    CHECK(!mk_store_has(2));
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS);
    fill_garbage(&loaded, sizeof loaded);
    CHECK_EQ(mk_store_load(1, &loaded), MK_STORE_EMPTY);
    CHECK(untouched(&loaded, sizeof loaded));
    CHECK_EQ(mk_store_init(), MK_STORE_OK);         /* nothing comes back */
    CHECK(!mk_store_has(1));
    CHECK(!mk_store_has(2));
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS);
    printf("ok erase\n");
}

static void make_settings(mk_settings *s, int seed) {
    memset(s, 0, sizeof *s);
    s->version = MK_SETTINGS_VERSION;
    s->home_channel = (uint8_t)(seed % 16);
    s->keys_on_track = 1;
    s->slot = (uint8_t)(seed % 64);
    s->seq_on = 1;
    s->brightness = (uint8_t)(200 + seed);
    s->tempo_follows_pattern = 2;
}

static void make_chain(mk_chain *c, int seed) {
    memset(c, 0, sizeof *c);
    c->on = (uint8_t)(seed & 1);
    c->count = (uint8_t)(1 + seed % MK_CHAIN_MAX);
    for (int i = 0; i < c->count; i++) c->pattern[i] = (uint8_t)((seed + i) % 16);
}

static void test_settings_and_chains(void) {
    mk_settings s, t;
    mk_chain chain[MK_BANKS], got;
    fresh();
    make_settings(&s, 3);
    CHECK_EQ(mk_store_save_settings(&s), MK_STORE_OK);
    fill_garbage(&got, sizeof got);
    CHECK_EQ(mk_store_load_chain(0, &got), MK_STORE_EMPTY);
    CHECK(untouched(&got, sizeof got));
    for (int k = 0; k < MK_BANKS; k++) {
        make_chain(&chain[k], 5 + k * 9);
        CHECK_EQ(mk_store_save_chain(k, &chain[k]), MK_STORE_OK);
    }
    make_chain(&chain[2], 31);
    CHECK_EQ(mk_store_save_chain(2, &chain[2]), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - 1);       /* all five share one sector */
    for (int pass = 0; pass < 2; pass++) {
        CHECK_EQ(mk_store_load_settings(&t), MK_STORE_OK);
        CHECK(memcmp(&s, &t, sizeof s) == 0);
        for (int k = 0; k < MK_BANKS; k++) {
            CHECK_EQ(mk_store_load_chain(k, &got), MK_STORE_OK);
            CHECK(memcmp(&chain[k], &got, sizeof got) == 0);
        }
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
    }
    CHECK_EQ(mk_store_load_chain(4, &got), MK_STORE_ERROR);
    CHECK_EQ(mk_store_save_chain(-1, &got), MK_STORE_ERROR);
    CHECK(!mk_store_has(0));
    printf("ok settings_and_chains\n");
}

static void test_reboot(void) {
    mk_settings s, t;
    mk_chain chain, got;
    fresh();
    for (int slot = 0; slot < 20; slot += 2) {
        make_typical(&a, slot);
        CHECK_EQ(mk_store_save(slot, &a), MK_STORE_OK);
    }
    make_worst(&b, 9);
    CHECK_EQ(mk_store_save(63, &b), MK_STORE_OK);
    make_settings(&s, 8);
    make_chain(&chain, 12);
    CHECK_EQ(mk_store_save_settings(&s), MK_STORE_OK);
    CHECK_EQ(mk_store_save_chain(3, &chain), MK_STORE_OK);
    int free_before = mk_store_free_sectors();

    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), free_before);
    for (int slot = 0; slot < MK_SLOTS; slot++) {
        int stored = (slot < 20 && slot % 2 == 0) || slot == 63;
        CHECK_EQ(mk_store_has(slot), stored);
        if (slot == 63) check_round_trip(63, &b);
        else if (stored) {
            make_typical(&a, slot);
            check_round_trip(slot, &a);
        }
    }
    CHECK_EQ(mk_store_load_settings(&t), MK_STORE_OK);
    CHECK(memcmp(&s, &t, sizeof s) == 0);
    CHECK_EQ(mk_store_load_chain(3, &got), MK_STORE_OK);
    CHECK(memcmp(&chain, &got, sizeof got) == 0);
    CHECK_EQ(mk_store_load_chain(2, &got), MK_STORE_EMPTY);
    printf("ok reboot\n");
}

/* Every sector holds a record of the old ks_store_* format. */
static void test_kss1(void) {
    reset_flash();
    for (int s = 0; s < AREA_SECTORS; s++) {
        uint8_t *p = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        memcpy(p, "KSS1", 4);
        p[4] = 100;
        p[5] = 0;
        for (int i = 8; i < 108; i++) p[i] = (uint8_t)(i * s);
    }
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS);
    for (int slot = 0; slot < AREA_SECTORS; slot++) {
        make_typical(&a, slot);
        CHECK_EQ(mk_store_save(slot, &a), MK_STORE_OK);
    }
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    for (int slot = 0; slot < AREA_SECTORS; slot++) {
        make_typical(&a, slot);
        check_round_trip(slot, &a);
    }
    for (int s = 0; s < AREA_SECTORS; s++) CHECK_EQ(erases[KS_FLASH_USER / KS_FLASH_SECTOR + s], 1);
    printf("ok kss1\n");
}

/* Power lost between the commit and the retire leaves two live versions: the newer one wins and
 * start-up retires the other, whichever sector it is in. */
static void test_two_versions(void) {
    for (int swap = 0; swap < 2; swap++) {
        fresh();
        make_typical(&a, 1);
        make_typical(&b, 2);
        CHECK_EQ(mk_store_save(6, &a), MK_STORE_OK);    /* sector 0 */
        CHECK_EQ(mk_store_save(6, &b), MK_STORE_OK);    /* sector 1 */
        uint8_t *first = chip + KS_FLASH_USER, *second = first + KS_FLASH_SECTOR;
        memset(first + 24, 0xFF, 4);                    /* as if the retire never happened */
        if (swap) {
            static uint8_t tmp[KS_FLASH_SECTOR];
            memcpy(tmp, first, sizeof tmp);
            memcpy(first, second, sizeof tmp);
            memcpy(second, tmp, sizeof tmp);
        }
        CHECK_EQ(live_versions(6), 2);
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        CHECK_EQ(live_versions(6), 1);
        CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - 1);
        check_round_trip(6, &b);
        CHECK_EQ(mk_store_erase(6), MK_STORE_OK);
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        CHECK(!mk_store_has(6));                        /* the older one does not come back */
    }
    /* and if the newer one is damaged, the older one is used */
    fresh();
    CHECK_EQ(mk_store_save(6, &a), MK_STORE_OK);
    CHECK_EQ(mk_store_save(6, &b), MK_STORE_OK);
    memset(chip + KS_FLASH_USER + 24, 0xFF, 4);
    chip[KS_FLASH_USER + KS_FLASH_SECTOR + 40] ^= 4;
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    check_round_trip(6, &a);
    CHECK_EQ(live_versions(6), 1);
    printf("ok two_versions\n");
}

/* A sector that looks blank but does not program is found by the read-back, erased and written again. */
static void test_weak_sector(void) {
    fresh();
    weak_sector = KS_FLASH_USER / KS_FLASH_SECTOR;  /* the first one a fresh store takes */
    make_typical(&a, 8);
    CHECK_EQ(mk_store_save(2, &a), MK_STORE_OK);
    CHECK_EQ(weak_sector, -1);
    CHECK_EQ(erases[KS_FLASH_USER / KS_FLASH_SECTOR], 1);
    check_round_trip(2, &a);
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    check_round_trip(2, &a);
    printf("ok weak_sector\n");
}

/* A flipped bit gives an error or the previous version, never wrong data. */
static void test_bit_flips(void) {
    int errors = 0, empties = 0, flips = 0;
    fresh();
    make_typical(&a, 4);
    make_worst(&b, 6);
    CHECK_EQ(mk_store_save(4, &a), MK_STORE_OK);
    CHECK_EQ(mk_store_save(5, &b), MK_STORE_OK);
    save_snapshot();
    for (int s = 0; s < AREA_SECTORS; s++) {
        uint8_t *sector = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        if (memcmp(sector, "MK2S", 4) != 0) continue;
        int slot = sector[6];
        uint32_t len = sector[10] | sector[11] << 8;
        for (uint32_t off = 0; off < 32 + len; off += off < 40 ? 1 : 37) {
            load_snapshot();
            chip[KS_FLASH_USER + s * KS_FLASH_SECTOR + off] ^= (uint8_t)(1u << (off % 8));
            flips++;
            CHECK_EQ(mk_store_init(), MK_STORE_OK);
            fill_garbage(&loaded, sizeof loaded);
            int result = mk_store_load(slot, &loaded);
            if (result == MK_STORE_OK) CHECK(same_pattern(&loaded, slot == 4 ? &a : &b));
            else if (result == MK_STORE_ERROR) errors++;
            else {
                CHECK_EQ(result, MK_STORE_EMPTY);
                CHECK(untouched(&loaded, sizeof loaded));
                empties++;
            }
            /* the other pattern is unaffected */
            int other = slot == 4 ? 5 : 4;
            check_round_trip(other, other == 4 ? &a : &b);
        }
    }
    /* damage after start-up is found by the load itself */
    load_snapshot();
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    for (int s = 0; s < AREA_SECTORS; s++) {
        uint8_t *sector = chip + KS_FLASH_USER + s * KS_FLASH_SECTOR;
        if (memcmp(sector, "MK2S", 4) == 0 && sector[6] == 5 && sector[7] == 1) sector[500] ^= 0x10;
    }
    CHECK_EQ(mk_store_load(5, &loaded), MK_STORE_ERROR);
    printf("  %d flips: %d errors, %d lost (header bit), rest loaded intact\n", flips, errors, empties);
    printf("ok bit_flips\n");
}

/* Background records, and churn so free sectors need erasing before reuse. */
static void populate(void) {
    mk_settings s;
    reset_flash();
    CHECK_EQ(mk_store_init(), MK_STORE_OK);
    for (int i = 0; i < 42; i++) {                 /* past the end, so the next sectors are used ones */
        make_typical(&c, 100 + i);
        CHECK_EQ(mk_store_save(20 + i % 3, &c), MK_STORE_OK);
    }
    make_settings(&s, 1);
    CHECK_EQ(mk_store_save_settings(&s), MK_STORE_OK);
}

static void check_background(void) {
    mk_settings s, t;
    for (int i = 39; i < 42; i++) {
        make_typical(&c, 100 + i);
        check_round_trip(20 + i % 3, &c);
    }
    make_settings(&s, 1);
    CHECK_EQ(mk_store_load_settings(&t), MK_STORE_OK);
    CHECK(memcmp(&s, &t, sizeof s) == 0);
}

static int sectors_of(const mk_pattern *p) {
    int n = 0;
    for (int t = 0; t < MK_TRACKS; t++) n += p->track[t].step[1].note[7] != 0;     /* only make_worst fills it */
    return n ? 2 : 1;
}

/* Power off after every possible number of bytes or erases into overwriting slot 0 with `next`. */
static void cut_save(const char *name, const mk_pattern *old, const mk_pattern *next) {
    int cuts = 0, olds = 0, news = 0;
    populate();
    if (old) CHECK_EQ(mk_store_save(0, old), MK_STORE_OK);
    save_snapshot();
    int background = AREA_SECTORS - mk_store_free_sectors() - (old ? sectors_of(old) : 0);
    for (long i = 0;; i++) {
        long cut = cut_at(i);
        load_snapshot();
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        budget = cut;
        int result = mk_store_save(0, next);
        int finished = !power_off;
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        fill_garbage(&loaded, sizeof loaded);
        int got = mk_store_load(0, &loaded);
        quiet = 1;
        int is_new = got == MK_STORE_OK && same_pattern(&loaded, next);
        int is_old = old ? got == MK_STORE_OK && same_pattern(&loaded, old) : got == MK_STORE_EMPTY;
        quiet = 0;
        CHECK(is_new || is_old);
        if (result == MK_STORE_OK) CHECK(is_new);
        if (!is_new && !is_old) printf("  cut at %ld: load %d, save %d\n", cut, got, result);
        news += is_new;
        olds += is_old;
        /* half-written sectors are reclaimed */
        int held = is_new ? sectors_of(next) : old ? sectors_of(old) : 0;
        CHECK_EQ(mk_store_free_sectors(), AREA_SECTORS - background - held);
        check_background();
        /* and the store goes on working */
        make_typical(&c, 55);
        CHECK_EQ(mk_store_save(0, &c), MK_STORE_OK);
        check_round_trip(0, &c);
        CHECK_EQ(live_versions(0), 1);
        if (finished) break;
        cuts++;
    }
    printf("  %s: %d cut runs, %d old, %d new\n", name, cuts, olds, news);
}

static void test_power_cut_save(void) {
    mk_pattern old, worst_old;
    make_typical(&old, 1);
    make_worst(&worst_old, 2);
    make_typical(&a, 2);
    make_worst(&b, 3);
    cut_save("typical over typical", &old, &a);
    cut_save("worst over typical", &old, &b);
    cut_save("typical over worst", &worst_old, &a);
    cut_save("worst over worst", &worst_old, &b);
    cut_save("typical into an empty slot", NULL, &a);
    cut_save("worst into an empty slot", NULL, &b);
    printf("ok power_cut_save\n");
}

static void test_power_cut_settings(void) {
    mk_settings s, t, old;
    int cuts = 0;
    make_settings(&old, 1);
    make_settings(&s, 2);
    populate();
    save_snapshot();
    for (long i = 0;; i++) {
        long cut = cut_at(i);
        load_snapshot();
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        budget = cut;
        int result = mk_store_save_settings(&s);
        int finished = !power_off;
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        CHECK_EQ(mk_store_load_settings(&t), MK_STORE_OK);
        CHECK(memcmp(&t, &s, sizeof t) == 0 || memcmp(&t, &old, sizeof t) == 0);
        if (result == MK_STORE_OK) CHECK(memcmp(&t, &s, sizeof t) == 0);
        CHECK_EQ(mk_store_save_settings(&s), MK_STORE_OK);
        CHECK_EQ(mk_store_load_settings(&t), MK_STORE_OK);
        CHECK(memcmp(&t, &s, sizeof t) == 0);
        if (finished) break;
        cuts++;
    }
    printf("  settings: %d cut runs\n", cuts);
    printf("ok power_cut_settings\n");
}

static void cut_erase(const char *name, const mk_pattern *old) {
    int cuts = 0;
    populate();
    CHECK_EQ(mk_store_save(0, old), MK_STORE_OK);
    save_snapshot();
    for (long i = 0;; i++) {
        long cut = cut_at(i);
        load_snapshot();
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        budget = cut;
        int result = mk_store_erase(0);
        int finished = !power_off;
        power_on();
        CHECK_EQ(mk_store_init(), MK_STORE_OK);
        fill_garbage(&loaded, sizeof loaded);
        int got = mk_store_load(0, &loaded);
        if (got == MK_STORE_OK) CHECK(same_pattern(&loaded, old));
        else CHECK_EQ(got, MK_STORE_EMPTY);
        if (result == MK_STORE_OK) CHECK_EQ(got, MK_STORE_EMPTY);
        CHECK_EQ(mk_store_has(0), got == MK_STORE_OK);
        check_background();
        make_typical(&c, 56);
        CHECK_EQ(mk_store_save(0, &c), MK_STORE_OK);
        check_round_trip(0, &c);
        if (finished) break;
        cuts++;
    }
    printf("  %s: %d cut runs\n", name, cuts);
}

static void test_power_cut_erase(void) {
    make_typical(&a, 1);
    make_worst(&b, 2);
    cut_erase("erase typical", &a);
    cut_erase("erase worst", &b);
    printf("ok power_cut_erase\n");
}

static void wear_report(const char *name, int rounds, uint32_t *max_out, uint32_t *min_out) {
    uint32_t max = 0, min = 0xFFFFFFFFu, total = 0;
    for (int s = 0; s < AREA_SECTORS; s++) {
        uint32_t n = erases[KS_FLASH_USER / KS_FLASH_SECTOR + s];
        if (n > max) max = n;
        if (n < min) min = n;
        total += n;
    }
    printf("  %s: %d saves, %u erases, per sector max %u min %u\n", name, rounds, (unsigned)total, (unsigned)max,
           (unsigned)min);
    *max_out = max;
    *min_out = min;
}

static void test_wear(void) {
    uint32_t max, min;
    mk_settings s;
    int rounds = 5000;
    fresh();
    for (int i = 0; i < rounds; i++) {
        make_typical(&a, i);
        if (i % 10 == 9) {
            make_settings(&s, i);
            CHECK_EQ(mk_store_save_settings(&s), MK_STORE_OK);
        } else CHECK_EQ(mk_store_save(i % 4, &a), MK_STORE_OK);
        if (i % 1000 == 999) CHECK_EQ(mk_store_init(), MK_STORE_OK);      /* reboots keep the rotation */
    }
    wear_report("4 patterns and the settings", rounds, &max, &min);
    CHECK(max <= min + 2);
    CHECK(min > 0);

    /* 20 patterns that never change: the others rotate through what is left */
    fresh();
    for (int slot = 30; slot < 50; slot++) {
        make_typical(&a, slot);
        CHECK_EQ(mk_store_save(slot, &a), MK_STORE_OK);
    }
    memset(erases, 0, sizeof erases);
    for (int i = 0; i < rounds; i++) {
        if (i % 2) make_worst(&a, i);
        else make_typical(&a, i);
        CHECK_EQ(mk_store_save(i % 3 == 0 ? 0 : 1, &a), MK_STORE_OK);
    }
    wear_report("beside 20 fixed patterns", rounds, &max, &min);
    uint32_t rotating_min = 0xFFFFFFFFu;
    for (int s = 0; s < AREA_SECTORS; s++) {
        uint32_t n = erases[KS_FLASH_USER / KS_FLASH_SECTOR + s];
        if (n && n < rotating_min) rotating_min = n;
    }
    printf("  rotating sectors: max %u min %u\n", (unsigned)max, (unsigned)rotating_min);
    CHECK(max <= rotating_min + 2);
    printf("ok wear\n");
}

int main(void) {
    for (uint32_t addr = 0; addr < KS_FLASH_SIZE; addr++) chip[addr] = outside_byte(addr);
    test_blank();
    test_round_trips();
    test_offsets();
    test_format1();
    test_fill();
    test_overwrite();
    test_erase();
    test_settings_and_chains();
    test_reboot();
    test_kss1();
    test_two_versions();
    test_weak_sector();
    test_bit_flips();
    test_power_cut_save();
    test_power_cut_settings();
    test_power_cut_erase();
    test_wear();
    CHECK_EQ(outside, 0);
    CHECK(outside_intact());
    printf("ok outside_untouched\n");
    if (failures) printf("%d failures\n", failures);
    else printf("all store tests passed\n");
    return failures != 0;
}
