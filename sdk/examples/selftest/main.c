/* selftest: exercises every part of the SDK on a unit, shows OK / FAIL / SKIP per check on the
 * display, and sends each result with its measured numbers to the computer for
 * tools/selftest.py. docs/SELFTEST.md says what each check does and what the person must do.
 *
 * First the automatic part (auto.c), then, after Play, the guided part (guided.c), then a
 * summary. On both summary screens the knob lists the results, Record sends the report again
 * and Play goes on: to the guided part, or to a whole new run.
 *
 * The only flash writes are to one sector of the SDK's area (0x41000-0x5FFFF) that reads
 * blank; it is erased again and checked. With no blank sector the write checks are skipped.
 *
 * Report: one SysEx message per event, F0 7D 'K' 'S' <type> <body> F7, over USB once a host has
 * configured the device and on the DIN output until then. The body is packed seven bytes into
 * eight, the first of each eight holding the top bits, and ends with a CRC-16 (CCITT, start
 * FFFF) of the bytes before it. Numbers are little-endian.
 *   10 start    format u8 (1), version major u8, minor u8, run u8 (counts from 1 since power-on),
 *               run id u32 (differs for every run), ms u32, number of checks u8
 *   11 result   sequence u8 (from 0), check id u8, status u8 (0 OK, 1 FAIL, 2 SKIP, 3 INFO:
 *               measured, no verdict), n u8, ms u32 (ks_millis when it ended), n x i32 values,
 *               then text in ASCII up to the checksum
 *   12 end      results u8, OK u8, FAIL u8, SKIP u8, INFO u8, messages dropped u8, ms u32
 *   13 pong     the ping's number u8, ks_millis u32, ks_micros u32
 * From the host, unpacked and without checksum:
 *   20 ping     F0 7D 'K' 'S' 20 <number> F7: answered with a pong at the next poll
 *   21 replay   F0 7D 'K' 'S' 21 F7: start, every result so far, and end if the run is over,
 *               sent again with the same run id and sequence numbers
 * The check ids are in selftest.h and each check's values in tools/selftest.py. */
#include "selftest.h"
#include "keystep_fonts.h"

const char *const names[CHECKS + 1] = {
    "", "USB", "Millis, micros", "Timer", "Flash id", "Flash read", "Flash write", "Flash store",
    "Sync loop", "Aftertouch rest", "Pedal rest", "Strips rest",
    "Display", "Lights red", "Lights green", "Lights blue", "Lights white",
    "Seq button", "Arp button", "Mutate button", "Chord button", "Tempo button", "Record button",
    "Stop button", "Play button", "Shift button", "Hold button", "Oct - button", "Oct + button",
    "Back button", "Knob push", "Knob turn", "Keys", "Bend strip", "Mod strip", "Aftertouch",
    "Pedal", "MIDI sockets", "CV and gate", "USB in",
};
static const char *const words[4] = { "OK", "FAIL", "SKIP", "INFO" };

uint32_t buttons, pressed;

void poll(void) {
    uint32_t now = ks_buttons();
    pressed = now & ~buttons;
    buttons = now;
    report_poll();
}

char *cat(char *to, const char *text) {
    while (*text) *to++ = *text++;
    *to = 0;
    return to;
}

char *cat_number(char *to, int32_t value) {
    char digits[12];
    if (value < 0) *to++ = '-';
    return cat(to, ks_utoa(value < 0 ? 0u - (uint32_t)value : (uint32_t)value, digits));
}

void lights_off(void) {
    for (int led = 0; led < KS_LED_COUNT; led++) ks_led(led, 0);
    ks_leds_show();
}

/* Until no button is held, so that one press answers one question. A button stuck down must
 * not hold the test up: its own check reports it. */
void wait_release(void) {
    for (uint32_t start = ks_millis(); ks_millis() - start < 2000;) {
        poll();
        if (!buttons) break;
    }
}

/* ---- a check's screen ---- */

static char lines[2][48];
static int step_id, drawn_seconds;
static uint32_t deadline;

static void small(int x, int y, const char *text) { ks_oled_text_font(&ks_font_small, x, y, text); }

static char *heading(char *to, int id) {
    return cat(cat(cat_number(cat(cat_number(to, id), "/"), CHECKS), " "), names[id]);
}

static void step_draw(int seconds) {
    char text[48];
    ks_oled_clear();
    heading(text, step_id);
    small(0, 0, text);
    cat_number(text, seconds);
    small(KS_OLED_W - ks_font_width(&ks_font_small, text), 0, text);
    small(0, 11, lines[0]);
    small(0, 22, lines[1]);
    ks_oled_flush();
    drawn_seconds = seconds;
}

void step_more_time(void) { deadline = ks_millis() + TIMEOUT_MS; }

void step_text(const char *what, const char *hint) {
    const char *from[2] = { what, hint };
    for (int i = 0; i < 2; i++) {
        unsigned n = 0;
        for (; n < sizeof lines[i] - 1 && from[i][n]; n++) lines[i][n] = from[i][n];
        lines[i][n] = 0;
    }
    drawn_seconds = -1;
}

/* Also empties what the keys and the knob collected before the check began. */
void step_begin(int id, const char *what, const char *hint) {
    ks_key_event event;
    while (ks_key_get(&event)) { }
    ks_encoder();
    step_id = id;
    step_text(what, hint);
    step_more_time();
    step_left();
}

uint32_t step_left(void) {
    poll();
    int32_t left = (int32_t)(deadline - ks_millis());
    if (left < 0) left = 0;
    int seconds = (left + 999) / 1000;
    if (seconds != drawn_seconds) step_draw(seconds);
    return (uint32_t)left;
}

int ask(void) {
    while (step_left()) {
        if (pressed & KS_BTN_PLAY) return 1;
        if (pressed & KS_BTN_STOP) return 0;
    }
    return -1;
}

/* The word large, for a glance; a failure stays long enough to be read out. */
void verdict(int id, int status, const char *text, const int32_t *values, int count) {
    char number[16];
    report_result(id, status, text, values, count);
    ks_oled_clear();
    ks_oled_text_big(0, 0, words[status]);
    cat_number(cat(cat_number(number, id), "/"), CHECKS);
    small(72, 0, number);
    small(0, 20, text);
    ks_oled_flush();
    for (uint32_t start = ks_millis(); ks_millis() - start < (status == OK ? 350u : 1500u);) poll();
}

/* ---- the counts, and with the knob each result: name and status, text, values. Returns
 * when Play is pressed. ---- */

static void summary(const char *head, const char *hint) {
    char text[112];
    int at = 0, shown = -1;
    wait_release();
    ks_encoder();
    for (;;) {
        poll();
        if (pressed & KS_BTN_PLAY) return;
        if (pressed & KS_BTN_RECORD) report_replay();
        at += ks_encoder();
        if (at < 0) at = 0;
        if (at > results) at = results;
        if (at == shown) continue;
        shown = at;
        ks_oled_clear();
        if (!at) {
            cat_number(cat(cat_number(cat(cat(text, head), ": OK "), tally[OK]), "  FAIL "), tally[FAIL]);
            small(0, 0, text);
            cat_number(cat(cat_number(cat(text, "SKIP "), tally[SKIP]), "  INFO "), tally[INFO]);
            small(0, 11, text);
            small(0, 22, hint);
        } else {
            const result *r = report_get(at - 1);
            cat(cat(cat(cat(cat_number(text, r->id), " "), names[r->id]), ": "), words[r->status]);
            small(0, 0, text);
            small(0, 11, r->text);
            char *p = text;
            *p = 0;
            for (int i = 0; i < r->count; i++) p = cat(cat_number(p, r->values[i]), " ");
            small(0, 22, text);
        }
        ks_oled_flush();
    }
}

int main(void) {
    ks_init();
    for (;;) {
        lights_off();
        run_automatic();
        summary("Auto", "Play: go on  knob: list");
        run_guided();
        /* The host tool pings while it listens: one heard shows USB MIDI in working. */
        RESULT(C_USB_IN, host_messages ? OK : SKIP, host_messages ? "" : "no ping from the tool",
               (int32_t)host_messages);
        report_end();
        summary("Done", "Play: again  knob: list");
    }
}
