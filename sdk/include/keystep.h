/* KeyStep mk2 SDK: the board, as far as it is mapped (docs/HARDWARE.md).
 *
 * Written against tools/emu.py and checked on one 32-key unit with sdk/examples/selftest. What
 * that showed to work, and what has not been tried there (MIDI DIN, CV, gate, sync), is in
 * sdk/README.md. */
#ifndef KEYSTEP_H
#define KEYSTEP_H
#include <stdint.h>

/* Clock (64 MHz), pins, display, keybed scan, LEDs, MIDI, USB, flash, CV and the touch controller. */
void ks_init(void);

/* ---- time ---- */
uint32_t ks_millis(void);
void ks_delay_ms(uint32_t ms);
uint16_t ks_micros16(void);             /* a free-running microsecond counter, wraps every 65 ms */
uint32_t ks_micros(void);               /* microseconds since start, wraps every 71 minutes */
/* One periodic interrupt for your own use, 50 to 65536 us: a sequencer clock, for example. The
 * callback runs in the interrupt, so keep it short. A new period applies from the next one;
 * ks_timer_start again (an interrupt may call it) starts the count over from now. */
void ks_timer_start(uint32_t period_us, void (*callback)(void));
void ks_timer_period(uint32_t period_us);
void ks_timer_stop(void);

/* ---- display: 128 x 32, one bit per pixel. Draw, then ks_oled_flush(). ---- */
#define KS_OLED_W 128
#define KS_OLED_H 32
void ks_oled_clear(void);
void ks_oled_pixel(int x, int y, int on);
void ks_oled_rect(int x, int y, int w, int h, int on);             /* filled */
void ks_oled_text(int x, int y, const char *s);                    /* 8 x 8 font: 16 x 4 characters */
void ks_oled_text_big(int x, int y, const char *s);                /* the same font at 16 x 16 */
void ks_oled_flush(void);

/* Proportional fonts. The stock firmware's two fonts can be used by including "keystep_fonts.h",
 * which the build generates from your copy of the stock image (ks_font_small: 9 pixels tall,
 * ks_font_large: 15). Only set pixels are drawn, so text can go over a filled area. */
typedef struct {
    uint8_t height, ascent, first, last;        /* pixels; the first and last character codes */
    const uint16_t *offsets;                    /* per character: where its glyph starts in bits */
    const uint8_t *widths;                      /* per character: pixels */
    const uint8_t *bits;
} ks_font;
int ks_oled_text_font(const ks_font *font, int x, int y, const char *s);   /* returns the x after the text */
int ks_font_width(const ks_font *font, const char *s);
void ks_oled_ink(int on);                /* 0: the next texts clear pixels, for dark text on a filled area */
char *ks_utoa(uint32_t value, char *out);                          /* decimal; returns out */

/* ---- buttons: one bit each in ks_buttons(), set while held ---- */
enum {
    KS_BTN_MUTATE = 1 << 0, KS_BTN_ARP = 1 << 1, KS_BTN_SEQ = 1 << 2, KS_BTN_SHIFT = 1 << 3,
    KS_BTN_HOLD = 1 << 4, KS_BTN_OCT_UP = 1 << 5, KS_BTN_OCT_DOWN = 1 << 6,
    KS_BTN_ENCODER = 1 << 9, KS_BTN_BACK = 1 << 10, KS_BTN_PLAY = 1 << 11, KS_BTN_STOP = 1 << 12,
    KS_BTN_RECORD = 1 << 13, KS_BTN_CHORD = 1 << 14, KS_BTN_TEMPO = 1 << 16,
};
uint32_t ks_buttons(void);              /* debounced: read every 2 ms, changes after two equal readings */
int ks_encoder(void);                   /* detents turned since the last call; clockwise is positive */

/* ---- keys: 32 keys, 0 = the low F. Events come from the scan interrupt. ---- */
#define KS_KEYS 32
typedef struct { uint8_t key, velocity, on; } ks_key_event;
int ks_key_get(ks_key_event *event);    /* 1 if an event was waiting */
uint32_t ks_keys_down(void);            /* bit n set while key n is held */

/* ---- touch strips: 0 (bottom) to 255 (top), or -1 with no finger on the strip ---- */
#define KS_STRIP_MOD 0
#define KS_STRIP_BEND 1
int ks_touch(int strip);
/* 1 if the touch controller answers on its bus. ks_touch gives -1 both for no finger and for a
 * controller that is not there; this tells them apart. Takes under a millisecond. */
int ks_touch_present(void);

/* ---- LEDs: set colours as 0xRRGGBB, then ks_leds_show(). The stock firmware never drives a
 * channel above 0x8D. The 32-key model has the thirteen button lights only. ---- */
#define KS_LED_KEY(n) (n)               /* 0 to 38: lights above the keys, on the 37-key model only */
enum {
    KS_LED_OCT_DOWN = 39, KS_LED_OCT_UP, KS_LED_HOLD, KS_LED_SHIFT, KS_LED_SEQ, KS_LED_ARP,
    KS_LED_MUTATE, KS_LED_CHORD, KS_LED_TEMPO, KS_LED_RECORD, KS_LED_STOP, KS_LED_PLAY,
    KS_LED_ENCODER, KS_LED_COUNT = 54,
};
void ks_led(int led, uint32_t rgb);
void ks_leds_show(void);                /* sends both chains; takes about 3 ms */

/* ---- CV, gate and sync. The values are what the processor puts out: the circuits between the
 * pins and the sockets are not mapped, so volts per count and the gate levels are unknown, and
 * none of these has been tried on a unit. The calibration the stock firmware applies is kept in
 * the unit's flash (docs/HARDWARE.md); examples/mk2000/calib.c reads it. ---- */
void ks_cv_pitch(uint16_t value);       /* 0 to 65535 */
void ks_cv_mod(int output, uint16_t value);     /* output 0 or 1 (Mod 1, Mod 2), 0 to 4095 */
void ks_gate(int on);
void ks_gate_format(int format);        /* 0 to 2 as the stock firmware numbers them; 1 at power-on */
#define KS_SYNC_CLOCK 0
#define KS_SYNC_RUN 1
void ks_sync_out(int line, int on);
int ks_sync_in(int line);               /* 1 while the line is active */
int ks_sync_pulses(void);               /* pulses that arrived on the clock input since the last call */
/* Define this to hear each pulse on the clock input as it arrives: it runs in the 65 us
 * interrupt, so keep it short. The pulse is counted for ks_sync_pulses() as well. */
void ks_sync_clock_in(void);

/* ---- analog inputs, 0 to 4095, refreshed every 4 ms ---- */
int ks_aftertouch(void);                /* pressure on the keybed's strip: near 0 at rest, about 3300 pressed hard */
int ks_pedal(void);                     /* the pedal socket: near 4095 with nothing plugged in, near 0 shorted */
uint32_t ks_analog_count(void);         /* conversions finished since start: it stands still if the converter does */

/* ---- MIDI on the DIN sockets ---- */
void ks_midi_send(uint8_t status, uint8_t d1, uint8_t d2);         /* a three-byte message */
void ks_midi_send_byte(uint8_t byte);
int ks_midi_get(void);                  /* the next received byte, or -1 */
int ks_midi_tx_free(void);              /* bytes that can be sent now without being dropped */

/* ---- USB: a MIDI device with the stock firmware's identity and descriptors. It also takes the
 * host's update-mode request and calls ks_enter_update_mode() 500 ms later. To change the names,
 * define ks_usb_product or ks_usb_serial in your firmware. ---- */
int ks_usb_ready(void);                 /* 1 once a host has configured the device */
void ks_usb_midi_send(uint8_t status, uint8_t d1, uint8_t d2);
void ks_usb_midi_send_byte(uint8_t byte);       /* any MIDI stream, SysEx included */
int ks_usb_midi_get(void);              /* the next byte from the host, or -1 */
int ks_usb_midi_tx_free(void);          /* bytes that can be sent now without being dropped */
void ks_usb_sysex_send(const uint8_t *message, uint32_t len);      /* F0 ... F7, kept together */
extern const char ks_usb_product[];     /* "KeyStep mk2" */
extern const char ks_usb_serial[17];    /* 16 characters */

/* ---- MIDI real-time bytes (0xF8 to 0xFF) as they arrive. Define ks_midi_realtime to follow a
 * clock without waiting for the main loop: it runs with interrupts off, mostly in the interrupt
 * that received the byte (the 65 us timer for DIN, USB's own for USB), so keep it short. The bytes still reach ks_midi_get()
 * and ks_usb_midi_get(), wherever they fell in the stream. ---- */
#define KS_MIDI_DIN 0
#define KS_MIDI_USB 1
void ks_midi_realtime(int port, uint8_t byte);

/* ---- Arturia SysEx on the USB port (docs/SYSEX.md). The SDK answers the version request
 * (EA F9) with ks_version. To answer other operations, define ks_sysex_request: it runs in the
 * USB interrupt, gets the operation as the firmware reads it (wire bytes swapped) and the CBOR
 * arguments, and returns the length of the CBOR reply it wrote, or -1 for no reply. ---- */
#define KS_SYSEX_REPLY_MAX 40
extern const char ks_version[];         /* "0.0.1.0" unless your firmware defines it; 23 characters at most */
int ks_sysex_request(uint16_t op, const uint8_t *args, int len, uint8_t *reply, int max);

/* ---- external flash: 512 KiB, erased in 4 KiB sectors. The stock firmware keeps its patterns,
 * settings and the factory calibration there; it never addresses KS_FLASH_USER and the 124 KiB
 * after it. Erase and write are refused outside that area unless ks_flash_allow_stock_area(1)
 * was called. Results: 0, or a negative KS_FLASH_ code. ---- */
#define KS_FLASH_SIZE 0x80000u
#define KS_FLASH_SECTOR 0x1000u
#define KS_FLASH_USER 0x41000u
#define KS_FLASH_USER_SIZE 0x1F000u
enum { KS_FLASH_RANGE = -1, KS_FLASH_TIMEOUT = -2, KS_FLASH_VERIFY = -3, KS_FLASH_EMPTY = -4 };
uint32_t ks_flash_id(void);                                        /* 0x1F8401 or 0xC84013 */
int ks_flash_read(uint32_t addr, void *data, uint32_t len);
int ks_flash_erase(uint32_t addr);                                 /* the sector holding addr; up to 0.5 s */
int ks_flash_write(uint32_t addr, const void *data, uint32_t len); /* can only clear bits: erase first */
void ks_flash_allow_stock_area(int allow);

/* ---- store: numbered records in the SDK's flash area, one sector each, with a checksum.
 * Saving erases first, so power lost during a save loses that slot. ---- */
#define KS_STORE_SLOTS 31
#define KS_STORE_MAX (KS_FLASH_SECTOR - 8)
int ks_store_save(int slot, const void *data, uint32_t len);       /* 0 or a KS_FLASH_ code */
int ks_store_load(int slot, void *data, uint32_t max);             /* the length, or KS_FLASH_EMPTY */

/* ---- system ---- */
void ks_reset(void);
void ks_enter_update_mode(void);        /* leave the marker the bootloader looks for, then reset */

#endif
