/* KeyStep MK2000: start-up, the sequencer's clock and what it follows, and where MIDI goes.
 * See docs/MK2000.md. */
#include "app.h"

void *memcpy(void *to, const void *from, size_t n);

app_state app;

const char ks_version[] = "0.1.0.0";         /* what the SysEx version request answers */
const char ks_usb_product[] = "KeyStep MK2000";

static mk_pattern buffers[3];           /* two the engine alternates between, one to load and save through */
#define SCRATCH (&buffers[2])
static mk_track undo;
static uint8_t undo_track = 0xFF;
static volatile uint8_t switched;
static volatile uint32_t ticks;
static uint32_t period_rest;
static uint8_t clocks, gate_holders;
static uint32_t settings_changed_at;
static uint8_t chain_running, chain_at;        /* Play started the chain; the entry playing or waiting */

uint32_t app_lock(void) {
    uint32_t state;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(state));
    return state;
}

void app_unlock(uint32_t state) {
    if (!(state & 1)) __asm__ volatile("cpsie i");
}

enum { TO_DIN = 1, TO_USB = 2 };

/* A whole message at a time: the engine sends from the timer interrupt, the keys and the thru
 * from the main loop, and their bytes must not mix. */
static void send_to(int to, const uint8_t *bytes, int n) {
    uint32_t state = app_lock();
    for (int i = 0; i < n; i++) {
        if (to & TO_DIN) ks_midi_send_byte(bytes[i]);
        if (to & TO_USB) ks_usb_midi_send_byte(bytes[i]);
    }
    app_unlock(state);
}

static void send(const uint8_t *bytes, int n) { send_to(TO_DIN | TO_USB, bytes, n); }

void app_note(uint8_t channel, uint8_t note, uint8_t velocity) {
    uint8_t message[3] = { (uint8_t)((velocity ? 0x90 : 0x80) | channel), note, velocity };
    send(message, 3);
}

uint8_t app_keys_channel(void) {
    if (app.settings.keys_on_track || app.recording) return mk_engine_pattern()->track[app.track].channel;
    return app.settings.home_channel;
}

void app_message(const char *text) {
    app.message = text;
    app.message_until = ks_millis() + 1200;
    app.redraw = 1;
}

uint32_t app_beat_phase(void) { return ticks % MK_PPQN; }

/* ---- what the engine sends ---- */

static void engine_note(int track, uint8_t channel, uint8_t note, uint8_t velocity) {
    app_note(channel, note, velocity);
    if (track != 0) return;                             /* track 1 also plays on the CV outputs */
    if (velocity) {
        int value = (note - 36) * 65536 / 120;
        ks_cv_pitch((uint16_t)(value < 0 ? 0 : value > 65535 ? 65535 : value));
        gate_holders++;
    } else if (gate_holders) {
        gate_holders--;
    }
    ks_gate(gate_holders != 0);
}

static void engine_controller(int track, uint8_t channel, int which, uint8_t value) {
    (void)track;
    uint8_t message[3] = { 0, 0, value };
    int n = 3;
    if (which == MK_CTL_BEND) {
        message[0] = (uint8_t)(0xE0 | channel);
    } else if (which == MK_CTL_PRESSURE) {
        message[0] = (uint8_t)(0xD0 | channel);
        message[1] = value;
        n = 2;
    } else {
        message[0] = (uint8_t)(0xB0 | channel);
        message[1] = which == MK_CTL_MOD ? 1 : 11;
    }
    send(message, n);
}

static void engine_realtime(uint8_t byte) {
    send(&byte, 1);
    if (byte == 0xFA) {
        clocks = 0;
        ks_sync_out(KS_SYNC_RUN, 1);
    } else if (byte == 0xFC) {
        ks_sync_out(KS_SYNC_RUN, 0);
        ks_sync_out(KS_SYNC_CLOCK, 0);
        gate_holders = 0;
        ks_gate(0);
    } else if (byte == 0xF8) {                          /* the sync output: one pulse per sixteenth */
        if (clocks == 0) ks_sync_out(KS_SYNC_CLOCK, 1);
        if (clocks == 3) ks_sync_out(KS_SYNC_CLOCK, 0);
        clocks = (uint8_t)((clocks + 1) % 6);
    }
}

static void engine_switched(void) { switched = 1; }

static uint32_t engine_random(void) {
    static uint32_t x = 0x2545F491;
    x ^= ks_micros16();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* The arp plays where the keys were pointing when each note started. */
static uint8_t arp_channel[128];

static void arp_note(uint8_t note, uint8_t velocity) {
    if (velocity) arp_channel[note] = app_keys_channel();
    app_note(arp_channel[note], note, velocity);
}

static const mk_arp_io arp_io = { arp_note, engine_random };

static const mk_engine_io io = { engine_note, engine_controller, engine_realtime, engine_switched, engine_random };

/* ---- the clock: its own, or one it follows ---- */

static volatile uint8_t source = MK_SYNC_INTERNAL;     /* where the ticks come from now */
static uint8_t source_setting = 0xFF;                  /* the setting `source` was last set from */
static volatile uint32_t heard_at;                     /* ks_micros() of the last pulse from `source` */
static volatile uint8_t transport;                     /* Start or Stop from `source`, for the main loop */
#define SILENT_US 2000000u      /* this long without a pulse: Auto goes back to its own clock */

static volatile uint16_t count_in;                     /* ticks of the count-in still to go */

/* The count-in ends in the interrupt, so the first step falls exactly on the bar. */
static void play_tick(void) {
    ticks++;
    if (count_in && !--count_in) {
        ticks = 0;
        mk_engine_start();
        mk_arp_sync();
    }
    mk_engine_tick();
    mk_arp_tick();
}

static uint32_t tempo_tenths(void) {
    uint32_t tempo = mk_engine_pattern()->tempo;
    return tempo < 300 ? 300 : tempo;
}

/* 96 ticks per quarter note. The period in microseconds is 6,250,000 / tempo (tempo in tenths of
 * a BPM); the remainder is carried so the tempo does not drift. */
static void tick(void) {
    if (source != MK_SYNC_INTERNAL) {
        mk_clock_timer();
        return;
    }
    uint32_t tempo = tempo_tenths(), total = 6250000u + period_rest;
    ks_timer_period(total / tempo);
    period_rest = total % tempo;
    play_tick();
}

static void restart_timer(uint32_t period_us) { ks_timer_start(period_us, tick); }

static const mk_clock_io clock_io = { play_tick, restart_timer };

static int per_pulse(uint8_t from) { return from == MK_SYNC_JACK ? MK_PPQN / 4 : MK_PPQN / 24; }

/* Interrupts off. */
static void follow(uint8_t from) {
    if (source == from) return;
    source = from;
    heard_at = ks_micros();
    if (from == MK_SYNC_INTERNAL) {
        mk_clock_follow(0, 0);
        period_rest = 0;
        ks_timer_start(6250000u / tempo_tenths(), tick);
    } else {
        mk_clock_follow(per_pulse(from), 6250000u / tempo_tenths() * (uint32_t)per_pulse(from));
    }
}

/* Auto takes the first port that sends a clock or Start while it runs on its own clock. An
 * unplugged sync jack reads the sync output, so the jack only counts while that is quiet. */
static int takes(uint8_t from) {
    if (source == from) return 1;
    return app.settings.sync_source == MK_SYNC_AUTO && source == MK_SYNC_INTERNAL &&
           (from != MK_SYNC_JACK || !mk_engine_playing());
}

/* For reading: 16 times the mean interval over the last half second or more, which the jitter of
 * single pulses hardly moves. */
static volatile uint32_t mean_interval;
static uint32_t mean_since, mean_pulses;

static void pulse(uint8_t from) {
    if (!takes(from)) return;
    if (source != from) mean_interval = mean_pulses = 0;
    follow(from);
    heard_at = ks_micros();
    if (!mean_pulses++) {
        mean_since = heard_at;
    } else if (heard_at - mean_since >= 500000u) {
        mean_interval = (heard_at - mean_since) / (mean_pulses - 1) * 16 + (heard_at - mean_since) % (mean_pulses - 1) * 16 / (mean_pulses - 1);
        mean_since = heard_at;
        mean_pulses = 1;
    }
    mk_clock_pulse(heard_at);
}

void ks_sync_clock_in(void) { pulse(MK_SYNC_JACK); }

static void thru_realtime(int port, uint8_t byte);

/* Start and Stop play nothing until the main loop has acted on them; the clocks in between are
 * counted. Continue starts from the top as well: there is no song position. */
void ks_midi_realtime(int port, uint8_t byte) {
    uint8_t from = port == KS_MIDI_USB ? MK_SYNC_USB : MK_SYNC_DIN;
    if (byte == 0xF8) {
        pulse(from);
    } else if (byte == 0xFA || byte == 0xFB) {
        if (!takes(from)) return;
        follow(from);
        heard_at = ks_micros();
        mk_clock_arm();
        mk_clock_hold(1);
        transport = 0xFA;
    } else if (byte == 0xFC) {
        if (source != from) return;
        mk_clock_hold(1);
        transport = 0xFC;
    } else {
        thru_realtime(port, byte);
    }
}

int app_external_tempo(void) {
    uint8_t from = source;
    if (from == MK_SYNC_INTERNAL) return -1;
    uint32_t interval = mk_clock_interval();
    if (!interval || ks_micros() - heard_at > SILENT_US) return 0;
    uint32_t shown = mean_interval ? mean_interval : interval * 16;
    return (int)((100000000u * (uint32_t)per_pulse(from) + shown / 2) / shown);
}

static void start(void);

static void sync_poll(void) {
    uint32_t state = app_lock();
    if (app.settings.sync_source != source_setting) {
        source_setting = app.settings.sync_source;
        follow(source_setting == MK_SYNC_AUTO || source_setting >= MK_SYNC_COUNT ? MK_SYNC_INTERNAL : source_setting);
    }
    if (source_setting == MK_SYNC_AUTO && source != MK_SYNC_INTERNAL && ks_micros() - heard_at > SILENT_US)
        follow(MK_SYNC_INTERNAL);       /* still playing, if it was, at the pattern's tempo */
    uint8_t request = transport;
    transport = 0;
    app_unlock(state);
    static uint8_t drawn_source;
    static uint32_t drawn_at;
    uint32_t now = ks_millis();
    if (source != drawn_source || (source != MK_SYNC_INTERNAL && now - drawn_at >= 250)) {
        drawn_source = source;                          /* the tempo shown follows the clock while stopped */
        drawn_at = now;
        app.redraw = 1;
    }
    if (request == 0xFA) start();
    else if (request == 0xFC) app_stop();
    if (!request) return;
    state = app_lock();
    mk_clock_hold(0);
    app_unlock(state);
    app.redraw = 1;
}

/* ---- thru and merge: whole messages from the inputs, in between the sequencer's own ---- */

#define THRU_MAX 128            /* the longest SysEx passed on; longer ones are dropped */
#define THRU_WAIT_MS 100        /* how long a message waits for room at the outputs */
#define THRU_SPARE 32           /* room left in the output buffers for the sequencer */

typedef struct {
    uint8_t running;            /* the channel status for running status, or 0 */
    uint8_t need;               /* bytes of the message being gathered, status included */
    uint8_t len;                /* gathered so far; 0 = none */
    uint8_t sysex, dropping;    /* inside F0 ... F7; it is too long to pass on */
    uint8_t ready;              /* msg is whole and waits for room */
    uint32_t since;
    uint8_t msg[THRU_MAX];
} thru_port;

static thru_port din_in, usb_in;

static int thru_to(int port) {
    if (port == KS_MIDI_DIN) return app.settings.din_thru ? TO_DIN | TO_USB : 0;
    return app.settings.usb_thru ? TO_DIN : 0;
}

/* Clock, Start, Continue and Stop are never passed on: MK2000 sends its own, from the ticks. */
static void thru_realtime(int port, uint8_t byte) {
    int to = thru_to(port);
    if (to && byte != 0xF9 && byte != 0xFD) send_to(to, &byte, 1);
}

static int data_bytes(uint8_t status) {
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 1 : 2;
    return status == 0xF2 ? 2 : status == 0xF1 || status == 0xF3 ? 1 : 0;
}

/* One byte from an input; 1 when it completes a message. */
static int gather(thru_port *p, uint8_t b) {
    if (b >= 0xF8) return 0;                            /* real time: passed on as it came in */
    if (b == 0xF0) {
        p->running = 0;
        p->sysex = 1;
        p->dropping = 0;
        p->msg[0] = b;
        p->len = 1;
        return 0;
    }
    if (p->sysex && b == 0xF7) {
        p->sysex = 0;
        if (!p->dropping) {
            p->msg[p->len++] = b;
            return 1;
        }
        p->len = 0;
        return 0;
    }
    if (b & 0x80) {                                     /* any other status ends a SysEx left open */
        p->sysex = 0;
        p->running = b < 0xF0 ? b : 0;
        if (b == 0xF4 || b == 0xF5 || b == 0xF7) {      /* undefined, or an end with no start */
            p->len = 0;
            return 0;
        }
        p->need = (uint8_t)(1 + data_bytes(b));
        p->msg[0] = b;
        p->len = 1;
        return p->need == 1;
    }
    if (p->sysex) {
        if (p->len < THRU_MAX - 1) p->msg[p->len++] = b;
        else p->dropping = 1;
        return 0;
    }
    if (!p->len) {
        if (!p->running) return 0;                      /* data with no status to go with it */
        p->msg[0] = p->running;
        p->need = (uint8_t)(1 + data_bytes(p->running));
        p->len = 1;
    }
    p->msg[p->len++] = b;
    return p->len >= p->need;
}

static int room(int to, int n) {
    if ((to & TO_DIN) && ks_midi_tx_free() < n + THRU_SPARE) return 0;
    if ((to & TO_USB) && ks_usb_ready() && ks_usb_midi_tx_free() < n + THRU_SPARE) return 0;
    return 1;
}

/* Arturia's requests from the host are for this unit (sdk/src/sysex.c answers them). */
static int for_us(const thru_port *p) {
    return p->msg[0] == 0xF0 && p->len > 4 && p->msg[1] == 0x00 && p->msg[2] == 0x20 && p->msg[3] == 0x6B;
}

static void thru_poll(thru_port *p, int port) {
    int to = thru_to(port);
    uint32_t now = ks_millis();
    for (int n = 0; n < 64; n++) {
        if (p->ready) {
            if (!to || (!room(to, p->len) && now - p->since >= THRU_WAIT_MS)) {
                p->ready = p->len = 0;          /* dropped */
            } else if (room(to, p->len)) {
                send_to(to, p->msg, p->len);
                p->ready = p->len = 0;
            } else {
                return;                         /* the input waits in the SDK's buffer meanwhile */
            }
        }
        int b = port == KS_MIDI_DIN ? ks_midi_get() : ks_usb_midi_get();
        if (b < 0) return;
        if (!to) {
            p->running = p->len = p->sysex = 0;
            continue;
        }
        if (!gather(p, (uint8_t)b)) continue;
        if (port == KS_MIDI_USB && for_us(p)) {
            p->len = 0;
            continue;
        }
        p->ready = 1;
        p->since = now;
    }
}

/* ---- patterns ---- */

static mk_pattern *spare(void) { return mk_engine_pattern() == &buffers[0] ? &buffers[1] : &buffers[0]; }

static void load_or_new(int slot, mk_pattern *into, const mk_pattern *like) {
    if (mk_store_load(slot, into) == MK_STORE_OK) return;
    mk_pattern_init(into);                              /* an empty slot keeps the channels and tempo you had */
    if (!like) return;
    into->tempo = like->tempo;
    for (int t = 0; t < MK_TRACKS; t++) into->track[t].channel = like->track[t].channel;
}

static void change_to(int slot, int now) {
    mk_pattern *playing = mk_engine_pattern(), *next = spare();
    load_or_new(slot, SCRATCH, playing);
    if (mk_engine_playing() && app.settings.tempo_follows_pattern != 1) SCRATCH->tempo = playing->tempo;
    if (app.settings.tempo_follows_pattern == 2) SCRATCH->tempo = playing->tempo;
    uint32_t state = app_lock();                        /* the engine may be about to take `next` over */
    memcpy(next, SCRATCH, sizeof *next);
    app.pending_slot = (uint8_t)slot;
    app_unlock(state);
    mk_engine_switch(next, now);
    app.redraw = 1;
}

void app_select_slot(int slot) {
    if (app.chain.on) {
        app.chain.on = 0;
        app.chain_dirty = 1;
    }
    chain_running = 0;
    change_to(slot, !mk_engine_playing() || app.settings.pattern_change == 1);
}

/* ---- chains ---- */

static void chain_load(int bank) {
    if (app.chain_dirty) app_chain_save();
    app.chain_bank = (uint8_t)bank;
    int valid = mk_store_load_chain(bank, &app.chain) == MK_STORE_OK && app.chain.count <= MK_CHAIN_MAX;
    for (int i = 0; valid && i < app.chain.count; i++) valid = app.chain.pattern[i] < 16;
    if (!valid) app.chain = (mk_chain){ 0 };
}

void app_chain_save(void) {
    int result = mk_store_save_chain(app.chain_bank, &app.chain);
    app.chain_dirty = 0;
    if (result != MK_STORE_OK) app_message(result == MK_STORE_FULL ? "Memory Full" : "Save Failed");
}

/* The entry that was waiting has taken over: queue the one after it for the next wrap. The same
 * pattern twice in a row is queued all the same, so it plays again. Never at once, whatever the
 * Pattern change setting says, or the chain would run through in a moment. */
static void chain_next(void) {
    if (!chain_running || !app.chain.on || !app.chain.count || !mk_engine_playing()) {
        chain_running = 0;
        return;
    }
    chain_at = (uint8_t)((chain_at + 1) % app.chain.count);
    change_to(app.chain_bank * 16 + app.chain.pattern[chain_at], 0);
}

static void start(void) {
    chain_running = app.chain.on && app.chain.count;
    chain_at = 0;
    if (chain_running) change_to(app.chain_bank * 16 + app.chain.pattern[0], 1);
    mk_engine_start();
    mk_arp_sync();                                      /* arp steps fall on the tracks' steps */
}

/* Following a clock, Play waits for its next pulse, which is the first tick. */
void app_start(void) {
    uint32_t state = app_lock();
    int following = source != MK_SYNC_INTERNAL;
    if (following) {
        mk_clock_arm();
        mk_clock_hold(1);
    }
    app_unlock(state);
    start();
    state = app_lock();
    if (following) mk_clock_hold(0);
    app_unlock(state);
}

/* Record then Play: a bar of four beats on the Play light and the display, then the pattern
 * starts. Following a clock there is no bar to count: it starts as Play does. */
void app_count_in(void) {
    if (source != MK_SYNC_INTERNAL) {
        app_start();
        return;
    }
    chain_running = 0;
    uint32_t state = app_lock();
    ticks = 0;
    count_in = 4 * MK_PPQN;
    app_unlock(state);
}

int app_counting(void) { return (count_in + MK_PPQN - 1) / MK_PPQN; }

/* Stop lets a waiting pattern take over; the chain's next entry is not wanted then. */
void app_stop(void) {
    uint32_t state = app_lock();
    count_in = 0;
    if (chain_running && mk_engine_pending()) {
        mk_engine_switch(NULL, 0);
        app.pending_slot = NO_SLOT;
    }
    app_unlock(state);
    chain_running = 0;
    mk_engine_stop();
}

void app_save(int slot) {
    uint32_t state = app_lock();
    memcpy(SCRATCH, mk_engine_pattern(), sizeof *SCRATCH);
    app_unlock(state);
    int result = mk_store_save(slot, SCRATCH);
    if (result == MK_STORE_OK) {
        app.slot = (uint8_t)slot;
        app.settings.slot = app.slot;
        app.settings_dirty = 1;
        app.dirty = 0;
    }
    app_message(result == MK_STORE_OK ? "Pattern Saved" : result == MK_STORE_FULL ? "Memory Full" : "Save Failed");
}

void app_reload(void) {
    load_or_new(app.slot, SCRATCH, mk_engine_pattern());
    uint32_t state = app_lock();
    memcpy(mk_engine_pattern(), SCRATCH, sizeof *SCRATCH);
    app_unlock(state);
    for (int t = 0; t < MK_TRACKS; t++) mk_engine_edited(t);
    app.dirty = 0;
    undo_track = 0xFF;
    app_message("Pattern Reloaded");
}

void app_snapshot(void) {
    uint32_t state = app_lock();
    memcpy(&undo, &mk_engine_pattern()->track[app.track], sizeof undo);
    undo_track = app.track;
    app_unlock(state);
}

void app_undo(void) {
    static mk_track swap;
    if (undo_track >= MK_TRACKS) return;
    mk_track *track = &mk_engine_pattern()->track[undo_track];
    uint32_t state = app_lock();
    memcpy(&swap, track, sizeof swap);
    memcpy(track, &undo, sizeof undo);
    memcpy(&undo, &swap, sizeof swap);
    app_unlock(state);
    mk_engine_edited(undo_track);
    app.dirty = 1;
    app_message("Undo");
}

/* ---- start ---- */

static void splash(void) {
    app.layer = 0xFF;                                   /* screens.c draws the name for this */
    screens_draw();
    for (int led = KS_LED_OCT_DOWN; led <= KS_LED_ENCODER; led++) {
        ks_led(led, 0x00303030);
        ks_leds_show();
        ks_delay_ms(60);
        ks_led(led, 0);
    }
    ks_leds_show();
    ks_delay_ms(250);
    app.layer = LAYER_HOME;
}

int main(void) {
    ks_init();
    mk_store_init();
    app.settings = (mk_settings){ .version = MK_SETTINGS_VERSION, .keys_on_track = 1, .seq_on = 1, .brightness = 0x8D,
                                   .arp = { .mode = MK_ARP_UP, .octaves = 1, .division = MK_DIV_16, .gate = 50, .swing = 50 } };
    mk_settings stored;
    if (mk_store_load_settings(&stored) == MK_STORE_OK && stored.version == MK_SETTINGS_VERSION) app.settings = stored;
    app.slot = app.settings.slot < MK_SLOTS ? app.settings.slot : 0;
    app.pending_slot = NO_SLOT;
    app.channel_target = -1;
    load_or_new(app.slot, &buffers[0], NULL);
    chain_load(app.slot / 16);
    mk_engine_init(&buffers[0], &io);
    mk_engine_enable(app.settings.seq_on);
    mk_arp_init(&app.settings.arp, &arp_io);
    mk_clock_init(&clock_io);
    splash();
    ks_timer_start(5208, tick);
    app.redraw = 1;

    uint32_t last_draw = 0;
    for (;;) {
        ui_poll();
        sync_poll();
        thru_poll(&din_in, KS_MIDI_DIN);
        thru_poll(&usb_in, KS_MIDI_USB);
        if (switched) {                                 /* the pattern chosen earlier has taken over */
            switched = 0;
            app.slot = app.pending_slot < MK_SLOTS ? app.pending_slot : app.slot;
            app.pending_slot = NO_SLOT;
            app.settings.slot = app.slot;
            app.settings_dirty = 1;
            app.dirty = 0;
            undo_track = 0xFF;
            app.redraw = 1;
            chain_next();
        }
        if (app.pending_slot != NO_SLOT && !mk_engine_pending() && !switched) {     /* it took over at once */
            switched = 1;
            continue;
        }
        uint32_t now = ks_millis();
        if (app.message && (int32_t)(now - app.message_until) >= 0) {
            app.message = 0;
            app.redraw = 1;
        }
        if (app.settings_dirty == 1) {                  /* written a moment later, and only while stopped */
            app.settings_dirty = 2;
            settings_changed_at = now;
        }
        if (app.settings_dirty == 2 && !mk_engine_playing() && now - settings_changed_at > 2000) {
            mk_store_save_settings(&app.settings);
            app.settings_dirty = 0;
        }
        if (app.slot / 16 != app.chain_bank) chain_load(app.slot / 16);
        if (app.chain_dirty && !mk_engine_playing() && pages_active() != PAGE_CHAIN) app_chain_save();
        if (app.redraw || ((mk_engine_playing() || app_counting()) && now - last_draw >= 40)) {
            app.redraw = 0;
            last_draw = now;
            lights_update(ks_buttons());
            screens_draw();
        }
    }
}
