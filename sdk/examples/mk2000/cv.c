/* KeyStep MK2000: the CV outputs - pitch, gate and the two mod outputs - with the stock firmware's
 * settings. Every number here is what the stock firmware 1.0.2 put into the same registers in the
 * emulator with a blank calibration (docs/STOCK-UI.md). The outputs have not been tried on a unit,
 * so what that is in volts at the sockets is not known.
 *
 *   pitch, volts per octave   (note - reference) x 65536 / 120: ten octaves over the range
 *   pitch, hertz per volt     6553.6 at the reference note, doubling every octave
 *   bend                      range x 546 at either end of the strip
 *   mod output                value / 127 x 309 x Max Volt
 *   vibrato                   a sine on the pitch output only, as deep as Wheel Vibrato says with
 *                             the Mod strip at the top and in proportion below it; nothing in MIDI
 *
 * cv_note is called from the tick interrupt (the tracks, the arp) and from the main loop (the
 * keys); the rest from the main loop. */
#include "app.h"

#define PULSE_MS 10             /* stock: on at 10 ms after the key, off at 20 */
#define RETRIG_MS 2             /* stock, Gate Retrig Mono: the gate is down for 2 ms */

static uint8_t holders;         /* notes being held on the outputs */
static uint8_t last = 60;       /* the note the pitch output is on */
static int bend = 8192;
static int32_t pitch_now, pitch_target;
static uint8_t value[MK_MOD_SOURCES];           /* 0 to 127 from each source */
static volatile uint8_t pulse, retrig;          /* a pulse is on; the gate is down between two notes */
static volatile uint32_t pulse_until, retrig_until;
static uint32_t slid_at;
static app_calibration calibration = { { 0, 0, 0 }, { 65535, 65535, 65535 } };
static uint8_t calibrated, calibration_read;
static uint32_t vib_phase;      /* the whole 32 bits are one turn */
static int32_t vib;             /* what the vibrato adds now, in 256ths of a semitone */

/* A quarter of a sine in 17 steps, 0 to 32767. */
static const uint16_t quarter[17] = { 0,     3212,  6393,  9512,  12539, 15446, 18204, 20787, 23170,
                                      25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767 };

static int32_t sine(uint32_t phase) {                  /* -32767 to 32767 */
    uint32_t at = phase >> 14 & 0xFFFF, index = at >> 12, part = at & 0xFFF;       /* a quarter turn in 16 steps */
    int quadrant = (int)(phase >> 30);
    if (quadrant & 1) index = 15 - index, part = 0x1000 - part;
    int32_t low = quarter[quadrant & 1 ? index + 1 : index], high = quarter[quadrant & 1 ? index : index + 1];
    int32_t value = quadrant & 1 ? high + (low - high) * (int32_t)part / 0x1000 : low + (high - low) * (int32_t)part / 0x1000;
    return quadrant & 2 ? -value : value;
}

static const mk_cv_settings *set(void) { return &app.settings.cv; }

/* 6553.6 x 2^(n/12), a semitone apart. */
static const uint16_t hertz[13] = { 6554, 6943, 7356, 7794, 8257, 8748, 9268, 9819, 10403, 11022, 11677, 12372, 13107 };

static int32_t pitch_of(int note) {
    const mk_cv_settings *s = set();
    int range = s->bend_range < 1 ? 1 : s->bend_range > 24 ? 24 : s->bend_range;
    int reference = 12 * (s->pitch_ref > 5 ? 5 : s->pitch_ref);
    int32_t out;
    if (s->pitch_format == 0) {
        out = (note - reference) * 65536 / 120 + (bend - 8192) * range / 15 + vib * 32 / 15;
    } else {
        int32_t at = (note - reference) * 256 + (bend - 8192) * range / 32 + vib;       /* 256ths of a semitone */
        int octave = 0;
        while (at < 0) at += 12 * 256, octave--;
        while (at >= 12 * 256) at -= 12 * 256, octave++;
        int low = hertz[at / 256], high = hertz[at / 256 + 1];
        out = low + (high - low) * (at % 256) / 256;
        out = octave >= 4 ? 65535 : octave >= 0 ? out << octave : octave > -16 ? out >> -octave : 0;
    }
    return out < 0 ? 0 : out > 65535 ? 65535 : out;
}

static void mod_out(int output) {
    const mk_cv_settings *s = set();
    int volts = s->mod_volts[output] < 1 ? 1 : s->mod_volts[output] > 10 ? 10 : s->mod_volts[output];
    int source = s->mod_source[output] < MK_MOD_SOURCES ? s->mod_source[output] : MK_MOD_VELOCITY;
    int counts = value[source] * 309 * volts / 127;
    if (counts > 4095) counts = 4095;
    ks_cv_mod(output, (uint16_t)(calib_apply(&calibration, 1 + output, counts << 4) >> 4));     /* the DAC's 12 bits, left-aligned */
}

static void mods(void) {
    mod_out(0);
    mod_out(1);
}

static void pitch_set(int slide) {
    pitch_target = pitch_of(last);
    if (slide) return;                                  /* cv_poll walks there */
    pitch_now = pitch_target;
    ks_cv_pitch((uint16_t)calib_apply(&calibration, 0, pitch_now));
}

void cv_note(uint8_t note, uint8_t velocity) {
    uint32_t state = app_lock(), now = ks_millis();
    if (velocity) {
        int legato = holders != 0;
        last = note;
        pitch_set(legato && set()->gate_retrig == MK_RETRIG_SLIDE);
        value[MK_MOD_VELOCITY] = velocity;
        value[MK_MOD_RANDOM] = (uint8_t)(app_random() & 127);
        value[MK_MOD_PULSE] = 127;
        pulse_until = now + PULSE_MS;
        pulse = 1;
        mods();
        if (holders < 255) holders++;
        if (legato && set()->gate_retrig == MK_RETRIG_MONO) {
            ks_gate(0);
            retrig_until = now + RETRIG_MS;
            retrig = 1;
        } else if (!retrig) {
            ks_gate(1);
        }
    } else if (holders && !--holders) {
        retrig = 0;
        ks_gate(0);
    }
    app_unlock(state);
}

void cv_stop(void) {
    uint32_t state = app_lock();
    holders = 0;
    retrig = 0;
    ks_gate(0);
    app_unlock(state);
}

void cv_source(int source, int amount) {
    if (source < 0 || source >= MK_MOD_SOURCES) return;
    uint32_t state = app_lock();
    value[source] = (uint8_t)(amount < 0 ? 0 : amount > 127 ? 127 : amount);
    mods();
    app_unlock(state);
}

void cv_bend(int bend14) {
    uint32_t state = app_lock();
    bend = bend14;
    pitch_set(pitch_now != pitch_target);               /* a slide under way goes on to the new place */
    app_unlock(state);
}

/* The settings were changed: the outputs show them at once. */
int cv_calibrated(void) { return calibrated; }

void cv_apply(void) {
    if (!calibration_read) {                            /* once, at start-up: a few reads of the flash */
        calibration_read = 1;
        calibrated = (uint8_t)calib_read(&calibration);
    }
    uint32_t state = app_lock();
    ks_gate_format(set()->gate_format > 2 ? 1 : set()->gate_format);
    pitch_set(0);
    mods();
    app_unlock(state);
}

void cv_poll(uint32_t now) {
    uint32_t state = app_lock();
    if (pulse && (int32_t)(now - pulse_until) >= 0) {
        pulse = 0;
        value[MK_MOD_PULSE] = 0;
        mods();
    }
    if (retrig && (int32_t)(now - retrig_until) >= 0) {
        retrig = 0;
        ks_gate(holders != 0);
    }
    int depth = set()->vibrato > 120 ? 120 : set()->vibrato, wheel = value[MK_MOD_WHEEL];
    if (depth && wheel) {                               /* a millisecond of the sine, and the pitch with it */
        int rate = set()->vibrato_rate < 3 ? 10 : set()->vibrato_rate > 100 ? 100 : set()->vibrato_rate;
        vib_phase += (uint32_t)rate * 429497u * (now - slid_at > 50 ? 1 : now - slid_at);       /* 2^32 / 10000 a ms */
        vib = sine(vib_phase) / 128 * depth * wheel / 1270;     /* depth: tenths of a semitone; wheel: of 127 */
        pitch_set(pitch_now != pitch_target);
    } else if (vib) {
        vib = 0;
        vib_phase = 0;
        pitch_set(pitch_now != pitch_target);
    }
    for (; pitch_now != pitch_target && slid_at != now; slid_at++) {    /* Legato Slide: a twelfth of the way every ms */
        int32_t left = pitch_target - pitch_now;
        pitch_now = left > -12 && left < 12 ? pitch_target : pitch_now + left / 12;
        if ((uint32_t)(now - slid_at) > 50) slid_at = now - 1;
        ks_cv_pitch((uint16_t)calib_apply(&calibration, 0, pitch_now));
    }
    slid_at = now;
    app_unlock(state);
}
