/* KeyStep MK2000: the strips, aftertouch and the pedal. They play on the channel the keys go to,
 * and while recording live they are written into the steps of the selected track (Rec Autom).
 *
 * The Bend strip is the stock one: where the finger is, 14 bits, and back to the centre over a
 * tenth of a second when it is let go. The Mod strip sends CC 1 and stays where it was left.
 *
 * Aftertouch and the pedal have only been seen in the emulator, where the stock firmware sent
 * nothing for either, so their levels on a device are not known. Both are therefore read against
 * what they show at rest: pressure counts from the level with no key down, the pedal from its
 * level at start-up, whichever way either moves. */
#include "app.h"

#define CENTRE 8192
#define SPRING_STEP 410         /* per 5 ms: the stock firmware comes back in twenty messages */
#define MOD_CV_TOP 1545         /* what the stock firmware puts on Mod 2 with the strip at the top */

static int bend = CENTRE;       /* the last values sent */
static int mod = -1, pressure, pedal;
static uint8_t touching;
static uint32_t sprung_at;
static int16_t rest[2] = { -1, -1 };    /* aftertouch, pedal: the level with nothing pressed */
static uint8_t snapped;         /* this recording has its Undo snapshot */
static int8_t written[MK_CONTROLLERS] = { -1, -1, -1, -1 };

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
    record(MK_CTL_BEND, value >> 7);
}

static void bend_poll(uint8_t channel, uint32_t now) {
    int at = ks_touch(KS_STRIP_BEND);
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

static void mod_poll(uint8_t channel) {
    int at = ks_touch(KS_STRIP_MOD);
    if (at < 0 || at >> 1 == mod) return;
    mod = at >> 1;
    uint8_t message[3] = { (uint8_t)(0xB0 | channel), 1, (uint8_t)mod };
    app_send(message, 3);
    ks_cv_mod(1, (uint16_t)(mod * MOD_CV_TOP / 127));
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
    mod_poll(channel);
    if (!ready_at) ready_at = now + 50;                 /* the analog inputs are first read a few ms after start */
    if ((int32_t)(now - ready_at) < 0) return;
    pressure_poll(channel);
    pedal_poll(channel);
}
