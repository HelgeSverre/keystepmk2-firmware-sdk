/* The guided part: one check at a time, each telling the person what to do. Play answers yes,
 * Stop answers no (or gives up); a check nobody answers ends after TIMEOUT_MS. */
#include "selftest.h"
#include "keystep_fonts.h"

/* The order the buttons are asked for, and the lights are lit in. */
#define CONTROLS 14
#define LIGHTS 13
static const struct { const char *name; uint32_t button; int8_t led; } controls[CONTROLS] = {
    { "Seq", KS_BTN_SEQ, KS_LED_SEQ }, { "Arp", KS_BTN_ARP, KS_LED_ARP },
    { "Mutate", KS_BTN_MUTATE, KS_LED_MUTATE }, { "Chord", KS_BTN_CHORD, KS_LED_CHORD },
    { "Tempo", KS_BTN_TEMPO, KS_LED_TEMPO }, { "Record", KS_BTN_RECORD, KS_LED_RECORD },
    { "Stop", KS_BTN_STOP, KS_LED_STOP }, { "Play", KS_BTN_PLAY, KS_LED_PLAY },
    { "Shift", KS_BTN_SHIFT, KS_LED_SHIFT }, { "Hold", KS_BTN_HOLD, KS_LED_HOLD },
    { "Oct -", KS_BTN_OCT_DOWN, KS_LED_OCT_DOWN }, { "Oct +", KS_BTN_OCT_UP, KS_LED_OCT_UP },
    { "Back", KS_BTN_BACK, -1 }, { "Knob", KS_BTN_ENCODER, KS_LED_ENCODER },
};

static const char *why(int gave_up) { return gave_up ? "given up" : "timeout"; }

/* ---- display: a frame on the outermost pixels, 1 2 3 4 in the corners (top left, top right,
 * bottom left, bottom right), two dotted columns with every other row lit, the SDK's font, and
 * both stock fonts, the large one dark on a lit bar ---- */

static void check_display(void) {
    static const char hint[] = "Play=ok Stop=bad", word[] = "Display";
    wait_release();
    ks_oled_clear();
    ks_oled_rect(0, 0, KS_OLED_W, KS_OLED_H, 1);
    ks_oled_rect(1, 1, KS_OLED_W - 2, KS_OLED_H - 2, 0);
    ks_oled_text(2, 2, "1");
    ks_oled_text(KS_OLED_W - 10, 2, "2");
    ks_oled_text(2, KS_OLED_H - 10, "3");
    ks_oled_text(KS_OLED_W - 10, KS_OLED_H - 10, "4");
    for (int y = 2; y < KS_OLED_H - 2; y += 2) {
        ks_oled_pixel(11, y, 1);
        ks_oled_pixel(KS_OLED_W - 12, y, 1);
    }
    ks_oled_text_font(&ks_font_small, (KS_OLED_W - ks_font_width(&ks_font_small, hint)) / 2, 2, hint);
    ks_oled_rect(14, 12, KS_OLED_W - 28, 17, 1);
    ks_oled_ink(0);
    ks_oled_text_font(&ks_font_large, (KS_OLED_W - ks_font_width(&ks_font_large, word)) / 2, 13, word);
    ks_oled_ink(1);
    ks_oled_flush();

    int answer = -1;
    for (uint32_t start = ks_millis(); answer < 0 && ks_millis() - start < TIMEOUT_MS;) {
        poll();
        if (pressed & KS_BTN_PLAY) answer = 1;
        else if (pressed & KS_BTN_STOP) answer = 0;
    }
    VERDICT(C_DISPLAY, answer == 1 ? OK : FAIL, answer == 1 ? "" : answer ? "timeout" : "said wrong", answer);
}

/* ---- lights: each button's light alone, named on the display, then all thirteen. The answer
 * counts once every light has had its turn. ---- */

static void check_lights(int id, const char *colour, uint32_t rgb) {
    enum { SLOT_MS = 250, ALL_SLOTS = 8 };
    char what[40];
    int shown = -1, swept = 0, answer = -1;
    wait_release();
    step_begin(id, "", "");
    for (uint32_t start = ks_millis();;) {
        uint32_t left = step_left();
        int slot = (int)((ks_millis() - start) / SLOT_MS % (LIGHTS + ALL_SLOTS));
        if (slot > LIGHTS) slot = LIGHTS;
        if (slot == LIGHTS && !swept) {
            swept = 1;
            step_more_time();
        }
        if (slot != shown) {
            const char *lit = "all thirteen";
            for (int i = 0, n = 0; i < CONTROLS; i++) {
                if (controls[i].led < 0) continue;
                ks_led(controls[i].led, slot == LIGHTS || slot == n ? rgb : 0);
                if (slot == n++) lit = controls[i].name;
            }
            ks_leds_show();
            cat(cat(cat(what, colour), ": "), lit);
            step_text(what, swept ? "Play: right  Stop: wrong" : "watch each light");
            shown = slot;
        }
        if (!swept) continue;
        if (!left) break;
        if (pressed & (KS_BTN_PLAY | KS_BTN_STOP)) {
            answer = pressed & KS_BTN_PLAY ? 1 : 0;
            break;
        }
    }
    lights_off();
    VERDICT(id, answer == 1 ? OK : FAIL, answer == 1 ? "" : answer ? "timeout" : "said wrong", (int32_t)rgb);
}

/* ---- buttons: the first one to go down after the instruction is the answer, so a button
 * wired to another bit fails by name ---- */

static void describe(uint32_t mask, char *to) {
    char *p = cat(to, "saw ");
    for (int bit = 0; bit < 32; bit++) {
        if (!(mask & (1u << bit))) continue;
        const char *name = 0;
        for (int i = 0; i < CONTROLS; i++)
            if (controls[i].button == 1u << bit) name = controls[i].name;
        if (p[-1] != ' ') p = cat(p, "+");
        p = name ? cat(p, name) : cat_number(cat(p, "bit"), bit);
    }
}

static void check_button(int index) {
    char text[240];
    int id = C_BUTTON + index;
    uint32_t want = controls[index].button, seen = 0;
    wait_release();
    int32_t held = (int32_t)buttons;                /* a button stuck down shows here */
    cat(cat(text, "Press "), index == CONTROLS - 1 ? "the knob" : controls[index].name);
    step_begin(id, text, "");
    if (controls[index].led >= 0) {
        ks_led(controls[index].led, 0x404040);
        ks_leds_show();
    }
    while (!seen && step_left()) seen = pressed;
    if (seen)                                       /* anything else that goes down with it */
        for (uint32_t start = ks_millis(); ks_millis() - start < 30;) {
            poll();
            seen |= pressed;
        }
    lights_off();
    if (seen == want) {
        VERDICT(id, OK, "", (int32_t)want, (int32_t)seen, held);
    } else {
        if (seen) describe(seen, text);
        VERDICT(id, FAIL, seen ? text : "nothing pressed", (int32_t)want, (int32_t)seen, held);
    }
}

/* ---- encoder: four detents each way; the sign says which way it counts ---- */

static int32_t turn(const char *what, int sign, int *gave_up) {
    char hint[40];
    int32_t turned = 0, shown = 1;                  /* counted in the direction asked for */
    uint32_t reached = 0;
    step_text(what, "");
    step_more_time();
    ks_encoder();
    while (step_left()) {
        if (pressed & KS_BTN_STOP) {
            *gave_up = 1;
            break;
        }
        turned += sign * ks_encoder();
        if (turned != shown) {
            char *p = cat_number(cat(hint, "clicks: "), turned < 0 ? -turned : turned);
            cat(p, turned < 0 ? " the other way" : " of 4");
            step_text(what, hint);
            shown = turned;
        }
        if ((turned >= 4 || turned <= -4) && !reached) reached = ks_millis();
        if (reached && ks_millis() - reached > 400) break;      /* a click too many still belongs here */
    }
    return turned;
}

static void check_encoder(void) {
    int gave_up = 0;
    wait_release();
    step_begin(C_ENCODER, "", "");
    int32_t right = turn("Knob: 4 clicks right", 1, &gave_up), left = 0;
    if (right >= 4) left = turn("Knob: 4 clicks left", -1, &gave_up);
    int ok = right >= 4 && left >= 4;
    VERDICT(C_ENCODER, ok ? OK : FAIL,
            ok ? "" : right <= -4 || left <= -4 ? "counts the other way" : why(gave_up), right, -left);
}

/* ---- keys: all of them once, lowest first. A key that fires out of turn is counted and the
 * check carries on from it, so one slip does not end the run. ---- */

static void check_keys(void) {
    char what[40], hint[40];
    uint32_t fired = 0, released = 0;
    int32_t misordered = 0, repeats = 0, low = 128, high = 0;
    int next = 0, last = -1, gave_up = 0, changed = 1;
    wait_release();
    cat(hint, "low to high, one by one");
    step_begin(C_KEYS, "", "");
    while (next < KS_KEYS && step_left()) {
        if (pressed & KS_BTN_STOP) {
            gave_up = 1;
            break;
        }
        ks_key_event event;
        while (ks_key_get(&event)) {
            if (!event.on) {
                released |= 1u << event.key;
                continue;
            }
            step_more_time();
            if (event.velocity < low) low = event.velocity;
            if (event.velocity > high) high = event.velocity;
            cat_number(cat(cat_number(cat(hint, "got key "), event.key + 1), "  velocity "), event.velocity);
            changed = 1;
            if (event.key == last) {
                repeats++;
                continue;
            }
            last = event.key;
            fired |= 1u << event.key;
            if (event.key != next) misordered++;
            if (event.key >= next) next = event.key + 1;
        }
        if (changed && next < KS_KEYS) {
            cat(cat_number(cat(what, "Play key "), next + 1), " of 32");
            step_text(what, hint);
            changed = 0;
        }
    }
    for (uint32_t start = ks_millis(); ks_keys_down() && ks_millis() - start < 1000;) { }  /* the last one up */
    ks_key_event event;
    while (ks_key_get(&event))
        if (!event.on) released |= 1u << event.key;
    int32_t missing = 0, stuck = 0;
    for (int key = 0; key < KS_KEYS; key++) {
        missing += !(fired & (1u << key));
        stuck += (fired & ~released & (1u << key)) != 0;
    }
    if (low > high) low = high;
    if (!missing && !stuck && !misordered) {
        VERDICT(C_KEYS, OK, "", (int32_t)fired, (int32_t)released, misordered, low, high, repeats);
    } else {
        char *p = cat_number(cat(what, "missing "), missing);
        if (misordered) p = cat_number(cat(p, " misordered "), misordered);
        if (stuck) p = cat_number(cat(p, " stuck "), stuck);
        if (missing && !misordered && !stuck) cat(cat(p, ", "), why(gave_up));
        VERDICT(C_KEYS, FAIL, what, (int32_t)fired, (int32_t)released, misordered, low, high, repeats);
    }
}

/* ---- touch strips: one slide from the bottom to the top, then the finger off ---- */

static void check_strip(int id, int strip, const char *what) {
    int32_t low = -1, high = -1, first = -1, last = -1, other = 0;
    int touching = 0, done = 0, gave_up = 0;
    uint32_t lifted = 0;
    wait_release();
    step_begin(id, what, "bottom to top, then lift");
    while (!done && step_left()) {
        if (pressed & KS_BTN_STOP) {
            gave_up = 1;
            break;
        }
        int32_t value = ks_touch(strip);
        if (ks_touch(!strip) >= 0) other = 1;
        if (value >= 0) {
            if (!touching) first = low = high = value;
            if (value < low) low = value;
            if (value > high) high = value;
            last = value;
            touching = 1;
            lifted = 0;
        } else if (touching && !lifted) {
            lifted = ks_millis();
        } else if (touching && ks_millis() - lifted > 150) {
            touching = 0;
            done = low <= 51 && high >= 204 && last - first >= 100 && !other;     /* a fifth from each end */
            if (!done) step_text(what, "again: bottom to top");
        }
    }
    int32_t released = ks_touch(strip) < 0;
    const char *text = "";
    if (!done) {
        if (other) text = "the other strip moved";
        else if (first < 0) text = why(gave_up);
        else text = last < first ? "reads top to bottom" : "range too small";
    }
    VERDICT(id, done ? OK : FAIL, text, low, high, first, last, released, other);
}

/* ---- aftertouch: the level moves away from its rest level while a key is pressed hard.
 * Which way and how far is not known for a real unit, so both are reported. ---- */

static void check_aftertouch(void) {
    int32_t rest = rest_aftertouch, low = 4096, high = -1, threshold = 4 * rest_aftertouch_spread;
    int moved = 0, played = 0, gave_up = 0;
    if (threshold < 200) threshold = 200;
    wait_release();
    step_begin(C_AFTERTOUCH, "Hold a key, press hard", "then let go");
    while (step_left()) {
        if (pressed & KS_BTN_STOP) {
            gave_up = 1;
            break;
        }
        ks_key_event event;
        while (ks_key_get(&event)) { }
        if (ks_keys_down()) {
            int32_t value = ks_aftertouch();
            if (value < low) low = value;
            if (value > high) high = value;
            if (high - rest >= threshold || rest - low >= threshold) moved = 1;
            played = 1;
        } else if (moved) {
            break;
        }
    }
    if (!played) low = high = rest;
    int32_t peak = high - rest >= rest - low ? high : low;
    VERDICT(C_AFTERTOUCH, moved ? OK : FAIL, moved ? "" : played ? "level did not move" : why(gave_up),
            rest, peak, peak > rest ? 1 : peak < rest ? -1 : 0, low, high, threshold);
}

/* ---- optional checks begin with a question; Stop or no answer skips them ---- */

static int offered(int id, const char *question) {
    wait_release();
    step_begin(id, question, "Play: test  Stop: skip");
    int answer = ask();
    if (answer != 1) VERDICT(id, SKIP, answer ? "timeout" : "skipped", 0);
    wait_release();
    return answer == 1;
}

static void check_pedal(void) {
    if (!offered(C_PEDAL, "Pedal plugged in?")) return;
    int32_t start = ks_pedal(), low = start, high = start;
    int gave_up = 0;
    uint32_t moved = 0;
    step_begin(C_PEDAL, "Press the pedal, let go", "");
    while (step_left()) {
        if (pressed & KS_BTN_STOP) {
            gave_up = 1;
            break;
        }
        int32_t value = ks_pedal();
        if (value < low) low = value;
        if (value > high) high = value;
        if (high - low >= 500 && !moved) moved = ks_millis();
        if (moved && ks_millis() - moved > 500) break;      /* time for the way back */
    }
    VERDICT(C_PEDAL, moved ? OK : FAIL, moved ? "" : gave_up ? "given up" : "level did not move",
            rest_pedal, start, low, high);
}

/* ---- MIDI sockets: a cable from Out to In brings back what is sent, clock bytes included ---- */

static volatile uint32_t din_clocks;

void ks_midi_realtime(int port, uint8_t byte) {
    if (port == KS_MIDI_DIN && byte == 0xF8) din_clocks++;
}

static void check_din(void) {
    static const uint8_t probe[16] = {
        0x90, 0x3C, 0x40, 0xF8, 0x80, 0x3C, 0x00, 0xB0, 0x01, 0x7F, 0xF8, 0xE0, 0x55, 0x2A, 0xC5, 0x11,
    };
    if (!offered(C_DIN, "MIDI cable Out > In?")) return;
    uint32_t start = ks_millis();
    while (ks_midi_tx_free() < 255 && ks_millis() - start < 300) { }       /* the report may be going out here */
    ks_delay_ms(5);
    while (ks_midi_get() >= 0) { }
    din_clocks = 0;
    for (unsigned i = 0; i < sizeof probe; i++) ks_midi_send_byte(probe[i]);
    int32_t received = 0, matching = 0;
    for (start = ks_millis(); ks_millis() - start < 60;) {
        int byte = ks_midi_get();
        if (byte < 0) continue;
        if (received < (int32_t)sizeof probe && byte == probe[received]) matching++;
        received++;
    }
    int ok = received == (int32_t)sizeof probe && matching == received && din_clocks == 2;
    VERDICT(C_DIN, ok ? OK : received ? FAIL : SKIP, ok ? "" : received ? "bytes differ" : "nothing came back",
            (int32_t)sizeof probe, received, matching, (int32_t)din_clocks);
}

/* ---- CV and gate: fixed values to hold a meter to. Only the person can say they followed. ---- */

static void check_cv(void) {
    static const struct { uint16_t pitch, mod; uint8_t gate, format; } steps[] = {
        { 0, 0, 0, 1 }, { 32768, 2048, 0, 1 }, { 65535, 4095, 0, 1 },
        { 32768, 2048, 1, 1 }, { 32768, 2048, 0, 0 }, { 32768, 2048, 1, 0 },
        { 32768, 2048, 0, 2 }, { 32768, 2048, 1, 2 },
    };
    enum { STEPS = sizeof steps / sizeof steps[0] };
    char what[40], hint[40];
    int answer = 1, step = 0;
    if (!offered(C_CV, "Meter on CV and gate?")) return;
    step_begin(C_CV, "", "");
    for (; step < STEPS && answer == 1; step++) {
        ks_cv_pitch(steps[step].pitch);
        ks_cv_mod(0, steps[step].mod);
        ks_cv_mod(1, steps[step].mod);
        ks_gate_format(steps[step].format);
        ks_gate(steps[step].gate);
        cat_number(cat(cat_number(cat(what, "Pitch "), steps[step].pitch), " Mods "), steps[step].mod);
        char *p = cat(cat_number(cat(hint, "Gate "), steps[step].format), steps[step].gate ? ": on" : ": off");
        cat(p, "  Play: next");                     /* "Gate 2: on" is gate format 2, gate on */
        step_text(what, hint);
        step_more_time();
        answer = ask();
    }
    if (answer == 1) {
        step_text("Did the outputs follow?", "Play: yes  Stop: no");
        step_more_time();
        answer = ask();
    }
    ks_cv_pitch(0);
    ks_cv_mod(0, 0);
    ks_cv_mod(1, 0);
    ks_gate_format(1);
    ks_gate(0);
    VERDICT(C_CV, answer == 1 ? OK : answer ? SKIP : FAIL,
            answer == 1 ? "confirmed by hand" : answer ? "timeout" : "said wrong", step, answer);
}

void run_guided(void) {
    check_display();
    check_lights(C_LEDS_RED, "Red", 0x8D0000);
    check_lights(C_LEDS_GREEN, "Green", 0x008D00);
    check_lights(C_LEDS_BLUE, "Blue", 0x00008D);
    check_lights(C_LEDS_WHITE, "White", 0x505050);
    for (int i = 0; i < CONTROLS; i++) check_button(i);
    check_encoder();
    check_keys();
    check_strip(C_STRIP_BEND, KS_STRIP_BEND, "Slide the Bend strip");
    check_strip(C_STRIP_MOD, KS_STRIP_MOD, "Slide the Mod strip");
    check_aftertouch();
    check_pedal();
    check_din();
    check_cv();
}
