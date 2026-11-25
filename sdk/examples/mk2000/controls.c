/* KeyStep MK2000: the strips, aftertouch and the pedal. The bend, the Mod strip and pressure also
 * go to the CV outputs (cv.c). They play on the channel the keys go to,
 * and while recording live they are written into the steps of the selected track (Rec Autom).
 *
 * The Bend strip is the stock one: where the finger is, 14 bits, and back to the centre over a
 * tenth of a second when it is let go. The Mod strip sends CC 1 and stays where it was left.
 *
 * On a unit (the self-test, docs/SELFTEST.md) the aftertouch strip reads 0 to 5 with no key
 * pressed and about 3300 pressed hard; the pedal socket reads about 4090 with nothing plugged in
 * or a switch pedal up, and about 40 with it down. Other pedals may differ, so both are read
 * against what they show at rest: pressure counts from the level with no key down, the pedal
 * from its level at start-up, whichever way either moves. MK2000's own handling of the two has
 * not been confirmed on a unit.
 *
 * With Shift held the strips are the stock firmware's Gate and Spice: the Bend strip sets the
 * selected track's gate, the Mod strip its Spice, where the finger is, and nothing is sent. Three
 * taps on the Spice strip within half a second of each other roll the dice. */
#include "app.h"

#define CENTRE 8192
#define SPRING_STEP 410         /* per 5 ms: the stock firmware comes back in twenty messages */

static int bend = CENTRE;       /* the last values sent */
static int mod = -1, pressure, pedal;
static uint8_t touching;
static uint32_t sprung_at;
static int16_t rest[2] = { -1, -1 };    /* aftertouch, pedal: the level with nothing pressed */
static uint8_t snapped;         /* this recording has its Undo snapshot */
static int8_t written[MK_CONTROLLERS] = { -1, -1, -1, -1 };
static char shown[16];          /* "Gate 45 %" or "Spice 30 %", while a strip is used with Shift */
static uint32_t shown_until;
static uint8_t shift_touch[2];  /* a finger is on the strip with Shift held */
static uint8_t taps;
static uint32_t tapped_at;

static void show(const char *name, int value, uint32_t now) {
    put(put_number(put(put(shown, name), " "), value), " %");
    shown_until = now + 1200;
    app.redraw = 1;
}

const char *controls_text(void) { return shown[0] && (int32_t)(ks_millis() - shown_until) < 0 ? shown : 0; }

/* Shift + a strip. Returns 1 while Shift owns the strip, so it sends nothing. */
static int shifted(int strip, uint32_t now) {
    int at = ks_touch(strip), index = strip == KS_STRIP_BEND;
    if (!(ks_buttons() & KS_BTN_SHIFT)) {
        if (!shift_touch[index]) return 0;
        if (at < 0) shift_touch[index] = 0;             /* Shift went first: the finger still does nothing */
        return 1;
    }
    mk_track *track = &mk_engine_pattern()->track[app.track];
    if (at < 0) {
        shift_touch[index] = 0;
        return 1;
    }
    if (strip == KS_STRIP_BEND) {
        int gate = 10 + (at * 80 + 127) / 255;
        if (gate != track->gate) {
            track->gate = (uint8_t)gate;
            app.dirty = 1;
        }
        show("Gate", gate, now);
    } else {
        if (!shift_touch[index]) {                      /* a finger came down: the third of three quick taps rolls the dice */
            taps = (uint8_t)(now - tapped_at <= 500 ? taps + 1 : 1);
            tapped_at = now;
            if (taps >= 3) {
                taps = 0;
                track->dice = (uint8_t)(track->dice + 1 + app_random() % 255);  /* never the same again */
                app.dirty = 1;
                app_message("Dice Rolled");
            }
        }
        int spice = (at * 100 + 127) / 255;
        if (spice != track->spice) {
            track->spice = (uint8_t)spice;
            app.dirty = 1;
        }
        show("Spice", spice, now);
    }
    shift_touch[index] = 1;
    return 1;
}

/* Into the step being played, once per value. */
static void record(int which, int value) {
    if (app.recording != REC_LIVE || !mk_engine_playing() || app.settings.options & MK_OPT_NO_AUTOM) return;
    if (written[which] == value) return;
    if (!snapped) app_snapshot();
    snapped = 1;
    if (mk_engine_record_controller(app.track, which, (uint8_t)value) < 0) return;
    written[which] = (int8_t)value;
    app.dirty = 1;
}

static void send_bend(uint8_t channel, int value) {
    uint8_t message[3] = { (uint8_t)(0xE0 | channel), (uint8_t)(value & 0x7F), (uint8_t)(value >> 7) };
    bend = value;
    app_send(message, 3);
    cv_bend(value);
    record(MK_CTL_BEND, value >> 7);
}

static void bend_poll(uint8_t channel, uint32_t now) {
    int at = shifted(KS_STRIP_BEND, now) ? -1 : ks_touch(KS_STRIP_BEND);
    if (at >= 0) {
        int want = at >= 126 && at <= 129 ? CENTRE : at * 16383 / 255;
        touching = 1;
        if (want != bend) send_bend(channel, want);
    } else if (touching) {
        touching = 0;
        sprung_at = now;
    } else if (bend != CENTRE && now - sprung_at >= 5) {
        int away = bend - CENTRE;
        sprung_at = now;
        send_bend(channel, away > SPRING_STEP ? bend - SPRING_STEP : away < -SPRING_STEP ? bend + SPRING_STEP : CENTRE);
    }
}

static void mod_poll(uint8_t channel, uint32_t now) {
    int at = shifted(KS_STRIP_MOD, now) ? -1 : ks_touch(KS_STRIP_MOD);
    if (at < 0 || at >> 1 == mod) return;
    mod = at >> 1;
    uint8_t message[3] = { (uint8_t)(0xB0 | channel), 1, (uint8_t)mod };
    app_send(message, 3);
    cv_source(MK_MOD_WHEEL, mod);
    record(MK_CTL_MOD, mod);
}

/* Pressure counts only while a key is down. The level without one is followed slowly, so a
 * strip that drifts does not start to sound; a twentieth of the range is ignored on top. */
static void pressure_poll(uint8_t channel) {
    static const int16_t span[3] = { 2000, 3000, 1000 };       /* medium, low, high sensitivity */
    int setting = (app.settings.options & MK_OPT_AFTERTOUCH) >> MK_OPT_AFTERTOUCH_SHIFT;
    int raw = ks_aftertouch(), want = 0;
    if (rest[0] < 0) rest[0] = (int16_t)raw;
    if (!ks_keys_down()) rest[0] = (int16_t)(rest[0] + (raw - rest[0]) / 8);
    else if (setting < 3) {
        int pressed = raw > rest[0] ? raw - rest[0] : rest[0] - raw;
        want = (pressed - 200) * 127 / span[setting];
        want = want < 0 ? 0 : want > 127 ? 127 : want;
        if (want && want != 127 && (want - pressure == 1 || pressure - want == 1)) return;     /* the last bit flickers */
    }
    if (want == pressure) return;
    pressure = want;
    uint8_t message[2] = { (uint8_t)(0xD0 | channel), (uint8_t)want };
    app_send(message, 2);
    cv_source(MK_MOD_AFTERTOUCH, want);
    record(MK_CTL_PRESSURE, want);
}

/* Sustain. Down is a third of the range away from the level at start-up, up is back within a
 * quarter, so either kind of pedal works when it is not held down while switching on. */
static void pedal_poll(uint8_t channel) {
    int raw = ks_pedal();
    if (rest[1] < 0) rest[1] = (int16_t)raw;
    int away = raw > rest[1] ? raw - rest[1] : rest[1] - raw;
    int down = pedal ? away > 1000 : away > 1365;
    if (down == pedal) return;
    pedal = down;
    uint8_t message[3] = { (uint8_t)(0xB0 | channel), 64, (uint8_t)(down ? 127 : 0) };
    app_send(message, 3);
    record(MK_CTL_PEDAL, down ? 127 : 0);
}

void controls_poll(uint32_t now) {
    static uint32_t ready_at;
    uint8_t channel = app_keys_channel();
    if (app.recording != REC_LIVE) {                    /* the next recording starts afresh */
        snapped = 0;
        for (int c = 0; c < MK_CONTROLLERS; c++) written[c] = -1;
    }
    bend_poll(channel, now);
    mod_poll(channel, now);
    if (!ready_at) ready_at = now + 50;                 /* the analog inputs are first read a few ms after start */
    if ((int32_t)(now - ready_at) < 0) return;
    pressure_poll(channel);
    pedal_poll(channel);
}
