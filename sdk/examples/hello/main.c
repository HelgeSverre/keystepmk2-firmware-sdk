/* The starting point for a new firmware (`make new APP=name` copies this folder). It shows
 * every control on the display, lights the button under your finger, and plays the keys as
 * MIDI notes on the DIN output and over USB, with the strips as pitch bend and mod wheel. It
 * also passes MIDI between USB and the DIN sockets. */
#include "keystep.h"

static const struct { uint32_t button; int led; } lights[] = {
    { KS_BTN_SEQ, KS_LED_SEQ }, { KS_BTN_ARP, KS_LED_ARP }, { KS_BTN_MUTATE, KS_LED_MUTATE },
    { KS_BTN_CHORD, KS_LED_CHORD }, { KS_BTN_TEMPO, KS_LED_TEMPO }, { KS_BTN_RECORD, KS_LED_RECORD },
    { KS_BTN_STOP, KS_LED_STOP }, { KS_BTN_PLAY, KS_LED_PLAY }, { KS_BTN_SHIFT, KS_LED_SHIFT },
    { KS_BTN_HOLD, KS_LED_HOLD }, { KS_BTN_OCT_DOWN, KS_LED_OCT_DOWN }, { KS_BTN_OCT_UP, KS_LED_OCT_UP },
    { KS_BTN_ENCODER, KS_LED_ENCODER },
};

static void hex4(uint32_t v, char *out) {
    for (int i = 0; i < 5; i++) out[i] = "0123456789ABCDEF"[(v >> (16 - 4 * i)) & 15];
    out[5] = 0;
}

int main(void) {
    char text[12];
    int knob = 0, note = -1, bend = -2, mod = -2;
    uint32_t shown = ~0u, drawn = 0;
    ks_init();
    for (;;) {
        ks_key_event e;
        while (ks_key_get(&e)) {
            note = 53 + e.key;                          /* key 0 is F3 */
            ks_midi_send(e.on ? 0x90 : 0x80, (uint8_t)note, e.velocity);
            ks_usb_midi_send(e.on ? 0x90 : 0x80, (uint8_t)note, e.velocity);
            ks_led(KS_LED_KEY(e.key), e.on ? 0x00306080 : 0);
            shown = ~0u;
        }
        /* a MIDI interface: USB to the DIN output, the DIN input to USB */
        int byte;
        while (ks_midi_tx_free() > 0 && (byte = ks_usb_midi_get()) >= 0) ks_midi_send_byte((uint8_t)byte);
        while ((byte = ks_midi_get()) >= 0) ks_usb_midi_send_byte((uint8_t)byte);

        int b = ks_touch(KS_STRIP_BEND), m = ks_touch(KS_STRIP_MOD);
        if (b != bend) {
            int value = b < 0 ? 8192 : b * 16383 / 255;
            ks_midi_send(0xE0, value & 0x7F, (uint8_t)(value >> 7));
            bend = b;
        }
        if (m != mod && m >= 0) ks_midi_send(0xB0, 1, (uint8_t)(m / 2));
        mod = m;
        knob += ks_encoder();

        uint32_t buttons = ks_buttons();
        if (buttons != shown) {
            for (unsigned i = 0; i < sizeof lights / sizeof lights[0]; i++)
                ks_led(lights[i].led, buttons & lights[i].button ? 0x008D4000 : 0);
            ks_leds_show();
            shown = buttons;
        }
        if (ks_millis() - drawn >= 50) {
            drawn = ks_millis();
            ks_oled_clear();
            ks_oled_text(0, 0, "KeyStep SDK");
            ks_oled_text(0, 12, "btn");
            hex4(buttons, text);
            ks_oled_text(32, 12, text);
            ks_oled_text(84, 12, "knob");
            ks_oled_text(84, 22, knob < 0 ? "-" : "+");
            ks_oled_text(92, 22, ks_utoa((uint32_t)(knob < 0 ? -knob : knob), text));
            ks_oled_text(0, 22, "note");
            ks_oled_text(40, 22, note < 0 ? "--" : ks_utoa((uint32_t)note, text));
            ks_oled_rect(120, 0, 3, 32, 0);
            if (b >= 0) ks_oled_rect(120, 31 - b * 31 / 255, 3, 1, 1);
            if (m >= 0) ks_oled_rect(124, 31 - m * 31 / 255, 3, 1, 1);
            ks_oled_flush();
        }
    }
}
