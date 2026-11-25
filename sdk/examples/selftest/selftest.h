/* Shared between the self-test's files. The report format is described at the top of main.c. */
#ifndef SELFTEST_H
#define SELFTEST_H
#include "keystep.h"

#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define TIMEOUT_MS 15000u               /* how long a guided check waits for the person */

enum { OK, FAIL, SKIP, INFO };          /* INFO: measured, but the check cannot tell right from wrong */

/* Check ids: part of the report format, so never renumber. tools/selftest.py has the same list. */
enum {
    C_USB = 1, C_MILLIS, C_TIMER, C_FLASH_ID, C_FLASH_READ, C_FLASH_WRITE, C_FLASH_STORE, C_SYNC,
    C_AFTERTOUCH_REST, C_PEDAL_REST, C_TOUCH_REST,
    C_DISPLAY, C_LEDS_RED, C_LEDS_GREEN, C_LEDS_BLUE, C_LEDS_WHITE,
    C_BUTTON,                           /* fourteen ids, in the order of `controls` in guided.c */
    C_ENCODER = C_BUTTON + 14, C_KEYS, C_STRIP_BEND, C_STRIP_MOD, C_AFTERTOUCH,
    C_PEDAL, C_DIN, C_CV, C_USB_IN,
    CHECKS = C_USB_IN,
};

/* ---- report.c: the log of results and its SysEx messages ---- */
#define VALUES_MAX 8
#define TEXT_MAX 31
typedef struct {
    uint8_t id, status, count;
    uint32_t ms;
    int32_t values[VALUES_MAX];
    char text[TEXT_MAX + 1];
} result;
extern int results;                     /* in the log, this run */
extern uint8_t tally[4];                /* per status */
extern uint32_t host_messages;          /* pings and replay requests from the host, this run */
const result *report_get(int index);
void report_start(void);                /* a new run: clears the log, sends the start message */
void report_result(int id, int status, const char *text, const int32_t *values, int count);
void report_end(void);
void report_replay(void);               /* everything of this run again */
void report_poll(void);                 /* reads what the host sent; answers pings and replay requests */

/* The values of a result, written in place: RESULT(C_SYNC, OK, "", sent, counted). */
#define VALUES(...) (const int32_t[]){ __VA_ARGS__ }, (int)(sizeof((const int32_t[]){ __VA_ARGS__ }) / sizeof(int32_t))
#define RESULT(id, status, text, ...) report_result(id, status, text, VALUES(__VA_ARGS__))

/* ---- main.c: the screen and the buttons ---- */
extern const char *const names[CHECKS + 1];
extern uint32_t buttons, pressed;       /* held now; newly down at the last poll() */
void poll(void);                        /* once per turn of every waiting loop */
char *cat(char *to, const char *text);  /* both return the end, for the next piece */
char *cat_number(char *to, int32_t value);
void lights_off(void);
void wait_release(void);
/* A check's screen: its number and name, a countdown, and two lines of text. */
void step_begin(int id, const char *what, const char *hint);
void step_text(const char *what, const char *hint);
void step_more_time(void);
uint32_t step_left(void);               /* polls and redraws; the milliseconds left, 0 when time is up */
int ask(void);                          /* 1 Play, 0 Stop, -1 when time ran out */
/* Logs a guided check's result and shows it for a moment. */
void verdict(int id, int status, const char *text, const int32_t *values, int count);
#define VERDICT(id, status, text, ...) verdict(id, status, text, VALUES(__VA_ARGS__))

/* ---- auto.c, guided.c ---- */
extern int32_t rest_aftertouch, rest_aftertouch_spread, rest_pedal;    /* measured by the automatic part */
void run_automatic(void);
void run_guided(void);

#endif
