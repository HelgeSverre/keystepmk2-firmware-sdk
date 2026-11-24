/* KeyStep MK2000: the state shared by main.c, ui.c and screens.c. */
#ifndef APP_H
#define APP_H
#include "keystep.h"
#include "mk.h"

enum { REC_OFF, REC_STEP, REC_LIVE };
enum { LAYER_HOME, LAYER_SHIFT, LAYER_CHANNEL, LAYER_TRACKS, LAYER_LENGTH, LAYER_SAVE, LAYER_ARP };
enum { PAGE_NONE, PAGE_STEPS, PAGE_TRACK, PAGE_GLOBAL, PAGE_CHAIN };
enum { PAGE_KEY_PLAYS, PAGE_KEY_USED, PAGE_KEY_TRACK };     /* what pages_key did with a key */
#define NO_SLOT 0xFF
#define FIRST_NUMBERED 4        /* keys 4-19 carry the numbers 1-16 */
#define FIRST_EDIT 20
#define KEY_CLEAR 30

typedef struct {
    mk_settings settings;
    uint8_t track;              /* the selected track */
    uint8_t slot;               /* the pattern in use */
    uint8_t pending_slot;       /* the one waiting to take over, or NO_SLOT */
    uint8_t dirty;              /* edits not saved */
    uint8_t settings_dirty;
    uint8_t layer;
    uint8_t recording;
    uint8_t rec_cursor;         /* step recording: the step being written */
    uint8_t transpose_hold;
    uint8_t latch;              /* Hold: the arp's notes, or the played ones, stay after release */
    uint8_t solo;               /* tracks soloed right now, one bit each */
    int8_t channel_target;      /* channel layer: a track picked with a bank key, or -1 */
    uint8_t length_page;        /* length layer: 0-3, sixteen steps each */
    uint8_t save_slot;          /* save page: where to */
    int8_t octave;
    uint8_t redraw;
    const char *message;        /* shown in the big line until message_until */
    uint32_t message_until;
    mk_chain chain;             /* the chain of bank chain_bank, the bank in use */
    uint8_t chain_bank;
    uint8_t chain_dirty;        /* changed since it was last saved */
} app_state;

extern app_state app;

/* main.c */
uint32_t app_lock(void);                        /* interrupts off; returns what to pass to app_unlock */
void app_unlock(uint32_t state);
void app_note(uint8_t channel, uint8_t note, uint8_t velocity);     /* velocity 0 = off */
uint8_t app_keys_channel(void);                 /* where the keys go right now */
void app_message(const char *text);
void app_select_slot(int slot);                 /* by hand: load it, hand it to the engine, chain off */
void app_start(void);                           /* Play: from the top, on the chain's first pattern if it is on */
void app_stop(void);
void app_count_in(void);                        /* a bar, then start */
int app_counting(void);                         /* beats of the count-in left, 4 to 1; 0 when not counting */
void app_chain_save(void);
void app_save(int slot);
void app_reload(void);
void app_snapshot(void);                        /* remember the selected track for Undo */
void app_undo(void);
uint32_t app_beat_phase(void);                  /* ticks into the current quarter note, 0 to 95 */
int app_external_tempo(void);                   /* tenths of a BPM of the clock followed; 0 not heard, -1 own clock */

/* ui.c */
void ui_poll(void);
void ui_quantize(int percent);                  /* the selected track: 100, or 50 for half way */

/* pages.c: the pages that stay open until Back. While the buttons that opened one are still
 * held it has every control; after that a held layer takes over while it is held. */
void pages_open(int kind, uint32_t buttons);
void pages_close(void);
int pages_active(void);                         /* PAGE_ */
void pages_buttons(uint32_t buttons);           /* every poll: notes which opening buttons were let go */
int pages_holding(void);                        /* the buttons that opened the page are still down */
int pages_key(int key, uint8_t note, uint8_t velocity, int shift);  /* PAGE_KEY_ */
void pages_key_up(int key);
int pages_hold(void);                           /* the Hold button; 1 if the page used it */
int pages_clear(void);                          /* Shift + Clear; 1 if the page used it */
void pages_turn(int by);
void pages_click(void);
void pages_back(void);
int pages_draw(void);                           /* 1 if a page was drawn */

/* screens.c */
void screens_draw(void);
void lights_update(uint32_t buttons);

/* screens.c: the pieces the screens are drawn from, shared with pages.c. Rows: small font at 0,
 * large font at 7 (its capitals start three rows down), small font at 22, a bar at 30. */
#define TOP 0
#define MID 7
#define LOW 22
extern const ks_font *const small_font, *const large_font;
char *put(char *at, const char *text);          /* no printf: text is built with these */
char *put_number(char *at, int value);
char *note_name(char *at, int note);
int text(const ks_font *font, int x, int y, const char *s);        /* returns the x after it */
void right(const ks_font *font, int y, const char *s);
int boxed(const ks_font *font, int x, int y, const char *s);       /* dark on a filled box */
void cells(int used, int cursor);               /* sixteen cells along the bottom, one bit each */

#endif
