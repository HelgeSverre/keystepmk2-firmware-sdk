/* The automatic part: what the firmware can check with nobody touching the unit. */
#include "selftest.h"

int32_t rest_aftertouch, rest_aftertouch_spread, rest_pedal;

static void progress(int id) {
    ks_oled_clear();
    ks_oled_text(0, 0, "Self test");
    ks_oled_text(0, 12, "automatic part");
    ks_oled_text(0, 22, names[id]);
    ks_oled_flush();
}

/* ---- time ---- */

static void check_millis(void) {
    uint32_t ms = ks_millis();
    while (ks_millis() == ms) { }                   /* start on a tick */
    ms = ks_millis();
    uint32_t us = ks_micros();
    while (ks_millis() - ms < 500) { }
    int32_t took = (int32_t)(ks_micros() - us);
    us = ks_micros();
    ks_delay_ms(100);                               /* from somewhere inside a tick: 99 to 100 ms */
    int32_t delay = (int32_t)(ks_micros() - us);
    uint16_t low = ks_micros16();
    int32_t apart = (int16_t)((uint16_t)ks_micros() - low);     /* the two counters are one timer */
    int ok = took >= 499000 && took <= 501000 && delay >= 98500 && delay <= 100500 && apart >= 0 && apart < 500;
    RESULT(C_MILLIS, ok ? OK : FAIL, ok ? "" : "millis and micros disagree", 500, took, delay, apart);
}

static volatile uint32_t timer_ticks;

static void on_timer(void) { timer_ticks++; }

/* The timer's ticks in about `ms`, and how long that was to the microsecond. The wait watches
 * ks_millis: a loop around ks_micros keeps interrupts off for most of its length. */
static int32_t ticks_in(uint32_t ms, int32_t *us) {
    uint32_t start = ks_millis(), from = ks_micros(), first = timer_ticks;
    while (ks_millis() - start < ms) { }
    uint32_t ticks = timer_ticks - first;
    *us = (int32_t)(ks_micros() - from);
    return (int32_t)ticks;
}

static int near(int32_t ticks, int32_t period, int32_t us) {
    int32_t off = ticks * period - us;
    return off > -3 * period && off < 3 * period;
}

static void check_timer(void) {
    int32_t slow_us, fast_us, unused;
    ks_timer_start(1000, on_timer);
    int32_t slow = ticks_in(200, &slow_us);         /* 200 */
    ks_timer_period(250);
    ticks_in(2, &unused);                           /* the new period starts with the next one */
    int32_t fast = ticks_in(100, &fast_us);         /* 400 */
    ks_timer_stop();
    int32_t stopped = ticks_in(20, &unused);
    int ok = near(slow, 1000, slow_us) && near(fast, 250, fast_us) && !stopped;
    RESULT(C_TIMER, ok ? OK : FAIL, ok ? "" : "ticks do not fit the time", slow, slow_us, fast, fast_us, stopped);
}

/* ---- external flash. Writes go to one sector of the SDK's area that reads blank, and that
 * sector is erased again; nothing else is written, and the stock area is only read. ---- */

static uint8_t block[256];
static uint32_t blank_sector;                       /* 0: none found */
static int flash_known, write_ok;

/* Bytes that are not 0xFF in `sectors` sectors from `addr`; also a checksum and whether the
 * file system's name is there. */
static int32_t scan(uint32_t addr, int sectors, uint16_t *sum, int *named) {
    static const char name[] = "littlefs";
    int32_t used = 0;
    for (uint32_t end = addr + (uint32_t)sectors * KS_FLASH_SECTOR; addr < end; addr += sizeof block) {
        ks_flash_read(addr, block, sizeof block);
        for (unsigned i = 0; i < sizeof block; i++) {
            used += block[i] != 0xFF;
            if (sum) *sum = (uint16_t)(*sum * 31 + block[i]);
            if (named && i + 8 <= sizeof block) {
                unsigned k = 0;
                while (k < 8 && block[i + k] == (uint8_t)name[k]) k++;
                if (k == 8) *named = 1;
            }
        }
    }
    return used;
}

static void check_flash_id(void) {
    uint32_t id = ks_flash_id();
    flash_known = id == 0x1F8401 || id == 0xC84013;
    RESULT(C_FLASH_ID, flash_known ? OK : FAIL, flash_known ? "" : "not a chip stock accepts", (int32_t)id);
}

/* A read proves itself only on data known to be there: the stock firmware's file system starts
 * with its name. On a chip without it (a blank one in the emulator) this reports the numbers. */
static void check_flash_read(void) {
    int32_t blank = 0;
    blank_sector = 0;
    for (uint32_t s = 0; s < KS_FLASH_USER_SIZE / KS_FLASH_SECTOR; s++) {
        uint32_t addr = KS_FLASH_USER + s * KS_FLASH_SECTOR;
        if (!scan(addr, 1, 0, 0)) {
            blank++;
            blank_sector = addr;                    /* the highest: the store fills from the bottom */
        }
    }
    uint16_t first = 0, second = 0;
    int named = 0;
    int32_t settings = scan(0x40000, 1, &first, 0), files = scan(0x60000, 2, &first, &named);
    scan(0x40000, 1, &second, 0);
    scan(0x60000, 2, &second, 0);
    int status = first != second ? FAIL : named ? OK : INFO;
    RESULT(C_FLASH_READ, status, first != second ? "two reads differ" : named ? "" : "no littlefs name found",
           blank, settings, files, named, first != second);
}

static void check_flash_write(void) {
    static uint8_t out[300];
    write_ok = 0;
    if (!flash_known || !blank_sector) {
        RESULT(C_FLASH_WRITE, SKIP, flash_known ? "no blank sector" : "unknown chip", 0);
        return;
    }
    uint32_t addr = blank_sector + 200, seed = ks_micros();     /* 300 bytes from 200: over two page ends */
    for (unsigned i = 0; i < sizeof out; i++) out[i] = (uint8_t)(i * 7 + seed);
    uint32_t start = ks_millis();
    int32_t wrote = ks_flash_write(addr, out, sizeof out), bad = 0;
    int32_t write_ms = (int32_t)(ks_millis() - start);
    for (unsigned at = 0; at < sizeof out; at += 100) {
        ks_flash_read(addr + at, block, 100);
        for (unsigned i = 0; i < 100; i++) bad += block[i] != out[at + i];
    }
    start = ks_millis();
    int32_t erased = ks_flash_erase(blank_sector);
    int32_t erase_ms = (int32_t)(ks_millis() - start), left = scan(blank_sector, 1, 0, 0);
    write_ok = !wrote && !bad && !erased && !left;
    RESULT(C_FLASH_WRITE, write_ok ? OK : FAIL,
           write_ok ? "" : left ? "sector NOT blank after" : bad || wrote ? "read back differs" : "erase failed",
           (int32_t)blank_sector, wrote, bad, erased, left, write_ms, erase_ms);
}

static void check_flash_store(void) {
    uint8_t data[64], back[64];
    if (!write_ok) {
        RESULT(C_FLASH_STORE, SKIP, "needs the write check", 0);
        return;
    }
    int slot = (int)((blank_sector - KS_FLASH_USER) / KS_FLASH_SECTOR);
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13 + 5);
    int32_t saved = ks_store_save(slot, data, sizeof data), bad = 0;
    int32_t loaded = ks_store_load(slot, back, sizeof back);
    for (unsigned i = 0; i < sizeof data; i++) bad += loaded != (int32_t)sizeof data || back[i] != data[i];
    int32_t erased = ks_flash_erase(blank_sector), left = scan(blank_sector, 1, 0, 0);
    int32_t after = ks_store_load(slot, back, sizeof back);         /* now empty */
    int ok = !saved && !bad && !erased && !left && after == KS_FLASH_EMPTY;
    RESULT(C_FLASH_STORE, ok ? OK : FAIL, ok ? "" : left ? "sector NOT blank after" : "record differs",
           slot, saved, loaded, bad, erased, left, after);
}

/* ---- sync: with nothing in the sync socket the inputs are expected to read the unit's own
 * outputs. That is known from the emulator's model only, so no loop at all is a SKIP, and the
 * run line, which nothing counts, is reported and not judged. ---- */

static volatile uint32_t sync_heard;

void ks_sync_clock_in(void) { sync_heard++; }

static void check_sync(void) {
    enum { PULSES = 8 };
    int32_t idle = ks_sync_in(KS_SYNC_CLOCK) | (ks_sync_in(KS_SYNC_RUN) << 1), followed = 0, run = 0;
    ks_sync_pulses();
    sync_heard = 0;
    for (int i = 0; i < PULSES; i++) {
        ks_sync_out(KS_SYNC_CLOCK, 1);
        ks_delay_ms(5);
        int high = ks_sync_in(KS_SYNC_CLOCK);
        ks_sync_out(KS_SYNC_CLOCK, 0);
        ks_delay_ms(5);
        followed += high && !ks_sync_in(KS_SYNC_CLOCK);
    }
    ks_sync_out(KS_SYNC_RUN, 1);
    ks_delay_ms(5);
    run = ks_sync_in(KS_SYNC_RUN);
    ks_sync_out(KS_SYNC_RUN, 0);
    ks_delay_ms(5);
    run = run && !ks_sync_in(KS_SYNC_RUN);
    int32_t counted = ks_sync_pulses(), heard = (int32_t)sync_heard;
    int ok = counted == PULSES && heard == PULSES && followed == PULSES;
    int none = !counted && !heard && !followed;
    RESULT(C_SYNC, ok ? OK : none ? SKIP : FAIL, ok ? "" : none ? "no pulse came back" : "counts differ",
           PULSES, counted, heard, followed, run, idle);
}

/* ---- analog inputs with nothing pressed: the levels of a real unit are what we want to know,
 * so this measures and judges nothing but a value the converter cannot give ---- */

static void check_rest(int id, int (*read)(void), int32_t *mean, int32_t *spread) {
    enum { SAMPLES = 250 };
    int32_t low = 0x7FFFFFFF, high = -1, sum = 0;
    for (int i = 0; i < SAMPLES; i++) {
        ks_delay_ms(4);                             /* the driver's refresh time */
        int32_t value = read();
        if (value < low) low = value;
        if (value > high) high = value;
        sum += value;
    }
    *mean = sum / SAMPLES;
    if (spread) *spread = high - low;
    int possible = low >= 0 && high <= 4095;
    RESULT(id, possible ? INFO : FAIL, possible ? "" : "outside 0 to 4095", low, high, *mean, SAMPLES);
}

/* ---- touch strips with no finger. This catches a phantom touch; that the controller answers
 * at all is only shown by the guided strip checks. ---- */

static void check_touch_rest(void) {
    int32_t mod = -1, bend = -1, touched = 0;
    for (uint32_t start = ks_millis(); ks_millis() - start < 500;) {
        mod = ks_touch(KS_STRIP_MOD);
        bend = ks_touch(KS_STRIP_BEND);
        if (mod >= 0 || bend >= 0) touched = 1;
    }
    RESULT(C_TOUCH_REST, touched ? FAIL : OK, touched ? "a strip reads a finger" : "", mod, bend);
}

void run_automatic(void) {
    progress(C_USB);
    uint32_t start = ks_millis();
    while (!ks_usb_ready() && ks_millis() - start < 3000) { }       /* give the host time to find us */
    int32_t waited = (int32_t)(ks_millis() - start), ready = ks_usb_ready();
    report_start();
    RESULT(C_USB, ready ? OK : FAIL, ready ? "" : "no host, report on DIN", ready, waited);

    static void (*const checks[])(void) = {
        check_millis, check_timer, check_flash_id, check_flash_read, check_flash_write, check_flash_store, check_sync,
    };
    for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        progress(C_MILLIS + (int)i);
        checks[i]();
        report_poll();
    }
    progress(C_AFTERTOUCH_REST);
    check_rest(C_AFTERTOUCH_REST, ks_aftertouch, &rest_aftertouch, &rest_aftertouch_spread);
    report_poll();
    progress(C_PEDAL_REST);
    check_rest(C_PEDAL_REST, ks_pedal, &rest_pedal, 0);
    progress(C_TOUCH_REST);
    check_touch_rest();
    report_poll();
}
