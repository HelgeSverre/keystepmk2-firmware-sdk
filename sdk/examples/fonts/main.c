/* fonts: the stock firmware's two display fonts next to the SDK's built-in one.
 *
 * Including "keystep_fonts.h" is all it takes: the build writes that header from your copy of
 * the stock image (firmware/, fetched with `just fetch`). The fonts are not part of the SDK.
 *
 *   encoder   change the number
 *   any key   show its note name */
#include "keystep.h"
#include "keystep_fonts.h"

static void centred(const ks_font *font, int y, const char *text) {
    ks_oled_text_font(font, (KS_OLED_W - ks_font_width(font, text)) / 2, y, text);
}

int main(void) {
    static const char names[12][3] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char number[12], line[24];
    int value = 120, note = -1, dirty = 1;
    ks_init();
    for (;;) {
        int turned = ks_encoder();
        ks_key_event event;
        while (ks_key_get(&event))
            if (event.on) {
                note = 53 + event.key;
                dirty = 1;
            }
        if (turned) {
            value += turned;
            dirty = 1;
        }
        if (!dirty) continue;
        dirty = 0;
        ks_oled_clear();
        ks_oled_text_font(&ks_font_small, 0, 0, "Tempo");
        ks_utoa((uint32_t)(value < 0 ? 0 : value), number);
        ks_oled_text_font(&ks_font_large, 0, 9, number);
        if (note >= 0) {
            char *p = line;
            for (const char *s = names[note % 12]; *s; s++) *p++ = *s;
            ks_utoa((uint32_t)(note / 12 - 1), p);
            centred(&ks_font_large, 9, line);
        } else {
            centred(&ks_font_large, 9, "KeyStep");
        }
        ks_oled_rect(0, 25, KS_OLED_W, 7, 1);
        ks_oled_text(84, 12, "8x8");                 /* the built-in font, for comparison */
        ks_oled_flush();
    }
}
