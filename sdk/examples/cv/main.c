/* cv: the keyboard as a CV and gate source, to exercise the analog side of the SDK.
 *
 *   keys        pitch CV and gate (last note; the gate stays up while any key is held)
 *   Mod strip   Mod 1
 *   aftertouch  Mod 2
 *   Play/Stop   a clock on the sync output, one pulse per sixteenth at 120 BPM, with the run line
 *   Hold        cycles the gate format
 *
 * The display shows what goes out and what comes in: pitch, the mod values, the pedal, and the
 * pulses counted on the sync clock input.
 *
 * Pitch uses the scale the stock firmware puts out with a blank calibration: 65536 counts for
 * ten octaves, zero at MIDI note 36. Whether that is one volt per octave at the socket depends on
 * the circuit and on the unit's calibration, which this example does not read. */
#include "keystep.h"

static uint16_t pitch_of(int note) {
    int value = (note - 36) * 65536 / 120;
    return (uint16_t)(value < 0 ? 0 : value > 65535 ? 65535 : value);
}

static void field(int x, int y, const char *label, uint32_t value) {
    char text[12];
    ks_oled_text(x, y, label);
    ks_oled_text(x + 8 * 3, y, ks_utoa(value, text));
}

int main(void) {
    ks_init();
    uint32_t last_buttons = 0, next_draw = 0, next_pulse = 0, pulse_off = 0, pulses_in = 0;
    int note = 60, format = 1, playing = 0;

    for (;;) {
        uint32_t now = ks_millis(), buttons = ks_buttons(), pressed = buttons & ~last_buttons;
        last_buttons = buttons;

        ks_key_event event;
        while (ks_key_get(&event)) {
            if (event.on) {
                note = 53 + event.key;
                ks_cv_pitch(pitch_of(note));
            }
            ks_gate(ks_keys_down() != 0);
        }

        int mod = ks_touch(KS_STRIP_MOD);
        if (mod >= 0) ks_cv_mod(0, (uint16_t)(mod * 4095 / 255));
        ks_cv_mod(1, (uint16_t)ks_aftertouch());

        if (pressed & KS_BTN_HOLD) ks_gate_format(format = (format + 1) % 3);
        if (pressed & KS_BTN_PLAY) {
            playing = 1;
            next_pulse = now;
            ks_sync_out(KS_SYNC_RUN, 1);
        }
        if (pressed & KS_BTN_STOP) {
            playing = 0;
            ks_sync_out(KS_SYNC_RUN, 0);
            ks_sync_out(KS_SYNC_CLOCK, 0);
        }
        if (playing && (int32_t)(now - next_pulse) >= 0) {
            ks_sync_out(KS_SYNC_CLOCK, 1);
            pulse_off = now + 15;
            next_pulse += 125;
        }
        if (pulse_off && (int32_t)(now - pulse_off) >= 0) {
            ks_sync_out(KS_SYNC_CLOCK, 0);
            pulse_off = 0;
        }
        pulses_in += (uint32_t)ks_sync_pulses();

        if ((int32_t)(now - next_draw) >= 0) {
            next_draw = now + 50;
            char text[12];
            ks_oled_clear();
            field(0, 0, "N", (uint32_t)note);
            field(40, 0, "P", pitch_of(note));
            ks_oled_text(112, 0, ks_keys_down() ? "G" : "-");
            ks_oled_text(120, 0, ks_utoa((uint32_t)format, text));
            field(0, 8, "M1", mod < 0 ? 0 : (uint32_t)(mod * 4095 / 255));
            field(64, 8, "M2", (uint32_t)ks_aftertouch());
            field(0, 16, "PD", (uint32_t)ks_pedal());
            field(64, 16, "CK", pulses_in);
            ks_oled_text(0, 24, playing ? "clock out" : "stopped");
            ks_oled_text(96, 24, ks_sync_in(KS_SYNC_RUN) ? "RUN" : "");
            ks_oled_flush();
            ks_led(KS_LED_PLAY, playing ? 0x008D00 : 0);
            ks_leds_show();
        }
    }
}
