/* screens: the planned KeyStep MK2000 screens drawn with the stock fonts, to see whether they
 * fit 128 x 32 pixels. Nothing here does anything; turn the encoder to step through them.
 *
 * Layout used throughout: small font at row 0, large font at row 7 (its capitals start three
 * rows down), small font at row 22, and a two-row bar at the bottom. */
#include "keystep.h"
#include "keystep_fonts.h"

#define SMALL (&ks_font_small)
#define LARGE (&ks_font_large)
#define TOP 0
#define MID 7
#define LOW 22

static int text(const ks_font *font, int x, int y, const char *s) { return ks_oled_text_font(font, x, y, s); }

static void right(const ks_font *font, int y, const char *s) { text(font, KS_OLED_W - ks_font_width(font, s), y, s); }

/* Text on a filled box: the selected track, the side being set. */
static int boxed(const ks_font *font, int x, int y, const char *s) {
    int w = ks_font_width(font, s), top = font == LARGE ? y + 2 : y - 1, h = font == LARGE ? 12 : 9;
    ks_oled_rect(x - 1, top, w + 1, h, 1);
    ks_oled_ink(0);
    text(font, x, y, s);
    ks_oled_ink(1);
    return x + w;
}

/* The four tracks: selected in a box, muted struck through, empty as dashes. */
static void track_line(int selected, int muted, int used, const char *keys) {
    static const char *names[4] = { "T1", "T2", "T3", "T4" };
    int x = 1;
    for (int t = 0; t < 4; t++) {
        const char *name = used & (1 << t) ? names[t] : "--";
        int end = t == selected ? boxed(SMALL, x, LOW, name) : text(SMALL, x, LOW, name);
        if (muted & (1 << t)) ks_oled_rect(x - 1, LOW + 3, end - x + 1, 1, t != selected);
        x = end + 5;
    }
    right(SMALL, LOW, keys);
}

static void bar(int position, int length) {
    for (int x = 0; x < KS_OLED_W; x += 2) ks_oled_pixel(x, 31, 1);
    ks_oled_rect(0, 30, KS_OLED_W * position / length, 2, 1);
}

static void cells(int y, int used, int cursor) {
    for (int i = 0; i < 16; i++) {
        int x = i * 8;
        ks_oled_rect(x + 1, y, 6, 5, 1);
        if (i != cursor && !(used & (1 << i))) ks_oled_rect(x + 2, y + 1, 4, 3, 0);
        if (i == cursor) ks_oled_rect(x + 1, y + 6, 6, 1, 1);
    }
}

static void home(const char *preset, int selected, int muted, int used, const char *keys, int position) {
    text(SMALL, 0, TOP, "Tempo: 120.0");
    right(SMALL, TOP, "1/16");
    text(LARGE, 0, MID, preset);
    track_line(selected, muted, used, keys);
    if (position >= 0) bar(position, 16);
}

static void draw(int screen) {
    ks_oled_clear();
    switch (screen) {
    case 0:                                             /* home, stopped, new unit */
        home("Bank: 1 | Preset: 1", 0, 0, 1, "KBD T1", -1);
        break;
    case 1:                                             /* home, playing, track 1 muted, unsaved */
        home("Bank: 1 | Preset: 1*", 2, 1, 7, "KBD T3", 5);
        break;
    case 2:                                             /* home, keys on the home channel */
        home("Bank: 2 | Preset: 16*", 1, 4, 15, "KBD 16", 11);
        break;
    case 3:                                             /* Shift + Stop held */
        text(SMALL, 0, TOP, "MIDI Channel");
        right(SMALL, TOP, "hold + key");
        boxed(LARGE, 1, MID, "Kbd 9");
        right(LARGE, MID, "T2 > 5");
        text(SMALL, 0, LOW, "T1:1  T2:5  T3:10  T4:4");
        break;
    case 4:                                             /* Shift + Stop + Seq held */
        text(SMALL, 0, TOP, "MIDI Channel");
        right(SMALL, TOP, "hold + key");
        text(LARGE, 1, MID, "Kbd 9");
        boxed(LARGE, KS_OLED_W - ks_font_width(LARGE, "T2 > 10") - 1, MID, "T2 > 10");
        text(SMALL, 0, LOW, "T1:1  T2:10  T3:10  T4:4");
        break;
    case 5:                                             /* Seq held */
        text(SMALL, 0, TOP, "Tracks");
        right(SMALL, TOP, "click: settings");
        text(LARGE, 0, MID, "Track 2");
        right(LARGE, MID, "Ch 5");
        track_line(1, 1, 7, "12 st");
        break;
    case 6:                                             /* step recording */
        text(SMALL, 0, TOP, "T1 Rec");
        right(SMALL, TOP, "Step 5");
        text(LARGE, 0, MID, "F2 C3");
        cells(24, 0x000D, 4);
        break;
    case 7:                                             /* step page */
        text(SMALL, 0, TOP, "T2 Step 7");
        right(SMALL, TOP, "C4 E4");
        text(LARGE, 0, MID, "Ratchet");
        right(LARGE, MID, "3");
        text(SMALL, 0, LOW, "Vel 96  Gate 50  Prob 75");
        break;
    case 8:                                             /* Shift + Tempo held */
        text(SMALL, 0, TOP, "T2 Length");
        right(SMALL, TOP, "hold + key");
        text(LARGE, 0, MID, "12 steps");
        cells(24, 0x0FFF, 11);
        break;
    case 9:                                             /* Shift held: pattern slots */
        text(SMALL, 0, TOP, "Bank 1");
        right(SMALL, TOP, "hold + key");
        text(LARGE, 0, MID, "Preset 1 > 2");
        cells(24, 0x0025, 0);
        break;
    case 10:                                            /* Shift + Play held */
        text(SMALL, 0, TOP, "Chain");
        right(SMALL, TOP, "hold + key");
        text(LARGE, 0, MID, "1 1 2 1");
        text(SMALL, 0, LOW, "4 of 32");
        right(SMALL, LOW, "Back: clear");
        break;
    default:                                            /* start-up */
        text(SMALL, 0, TOP, "Arturia");
        right(SMALL, TOP, "0.1.0");
        text(LARGE, (KS_OLED_W - ks_font_width(LARGE, "KeyStep MK2000")) / 2, MID + 2, "KeyStep MK2000");
        break;
    }
    ks_oled_flush();
}

#define SCREENS 12

int main(void) {
    int screen = 0;
    ks_init();
    draw(screen);
    for (;;) {
        int turned = ks_encoder();
        if (!turned) continue;
        screen = ((screen + turned) % SCREENS + SCREENS) % SCREENS;
        draw(screen);
    }
}
