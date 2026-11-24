/* The arpeggiator: the keys held (or latched) played one after another on a step grid of its own.
 *
 * Interrupt safety, as in engine.c. mk_arp_tick runs in the timer interrupt; everything else runs
 * in the main loop. Every main-loop function that changes state holds a lock (`busy`) while it
 * works. A tick that arrives while the lock is held is only counted, and the main-loop function
 * plays the ticks it owes before it lets go of the lock. Both contexts call the io callbacks, which
 * must not call back into the arp's functions that change state.
 *
 * The grid. It runs all the time, keys or not: a step begins every mk_div_ticks[division] ticks,
 * with every second step of a pair moved later by swing, as in the engine. Only mk_arp_sync (and
 * mk_arp_init) restarts it. When the first key comes in with no other in the set, the pattern
 * starts from its beginning but the grid is left alone: that first note waits for the next step
 * boundary of the running grid, at most one step, so the arp stays in time with the tracks and
 * the clock instead of starting off the beat wherever the key happened to land.
 *
 * Every note switched on is kept in `on`, and every note off comes from that list. All the notes
 * of a step share one gate, and a step begins by ending whatever is still sounding (normally
 * nothing: a gate is shorter than its step), so a pitch is never on twice and every on gets one
 * off, whatever happens to the settings, the keys or the grid in between.
 *
 * Arp Length: with Arp Retrigger on (free_run 0), the pattern starts from its beginning again
 * after that many steps, as the stock firmware's does; the grid is not touched.
 *
 * Releasing the last key (hold off) lets the sounding note run to its normal gate end; the arp is
 * then idle. mk_arp_clear ends the sounding notes at once. */
#include "mk.h"

#define KEYS 16                 /* notes in the set */

#define barrier() __asm__ volatile("" ::: "memory")

typedef struct {
    uint8_t pitch, velocity;
    uint8_t down;               /* the key is physically held */
} key;

const char *const mk_arp_mode_name[MK_ARP_MODES] = { "Up", "Down", "UpDown", "Random", "Order", "Poly" };

static mk_arp_settings *settings;
static const mk_arp_io *io;

static key keys[KEYS];          /* the set, in the order the keys were pressed */
static uint8_t held;            /* entries used in keys */
static uint8_t hold;            /* latch */

/* Where the pattern is. `last` (a pitch of the set, before transposition) and `oct` say where the
 * sorted modes are, `pos` (index into keys, -1 = before the first) where Order is; every step
 * updates all of them, so the pattern goes on from the same place when the mode changes. */
static uint8_t restart;         /* the next step starts the pattern from its beginning */
static uint8_t oct;             /* the octave being played, 0 to octaves - 1 */
static uint8_t last;
static int8_t pos;
static uint8_t falling;         /* UpDown is on its way down */
static uint8_t steps;           /* steps played since the pattern last started from its beginning */

/* The grid. */
static uint8_t fresh;           /* the next tick begins a step, the first of a swing pair */
static uint8_t second;          /* the current step is the second of a swing pair */
static uint16_t length;         /* ticks the current step lasts, swing included */
static uint16_t elapsed;        /* ticks since it began */

/* What is sounding. */
static uint8_t on[KEYS];
static uint8_t sounding;
static uint16_t left;           /* ticks until their note off */

static volatile uint8_t busy;
static volatile uint32_t requested, done;

static void tick_body(void);

static void lock(void) {
    busy = (uint8_t)(busy + 1);
    barrier();
}

/* Play the ticks that came in while the lock was held (see engine.c). */
static void unlock(void) {
    barrier();
    if (busy > 1) {
        busy = (uint8_t)(busy - 1);
        return;
    }
    for (;;) {
        while (done != requested) {
            done++;
            tick_body();
        }
        barrier();
        busy = 0;
        barrier();
        if (done == requested) return;
        busy = 1;
    }
}

/* ---- settings, always read through these ---- */

static int mode_of(void) { return settings->mode < MK_ARP_MODES ? settings->mode : MK_ARP_UP; }
static int octaves_of(void) { return settings->octaves < 1 ? 1 : settings->octaves > 4 ? 4 : settings->octaves; }
static int ticks_of(void) { return mk_div_ticks[settings->division < MK_DIV_COUNT ? settings->division : MK_DIV_16]; }
static int gate_of(void) { return settings->gate < 10 ? 10 : settings->gate > 90 ? 90 : settings->gate; }
static int length_of(void) { return settings->length < 1 ? 16 : settings->length > 64 ? 64 : settings->length; }
static int swing_of(void) { return settings->swing < 50 ? 50 : settings->swing > 75 ? 75 : settings->swing; }

/* ---- sounding notes ---- */

static void end_notes(void) {
    for (int i = 0; i < sounding; i++)
        if (io->note) io->note(on[i], 0);
    sounding = 0;
}

static void note_on(int pitch, uint8_t velocity) {
    if (pitch < 0 || pitch > 127 || sounding >= KEYS) return;
    for (int i = 0; i < sounding; i++)
        if (on[i] == pitch) return;     /* never the same note on twice */
    on[sounding++] = (uint8_t)pitch;
    if (io->note) io->note((uint8_t)pitch, velocity);
}

/* ---- the pattern ---- */

static int find_key(uint8_t pitch) {
    for (int i = 0; i < held; i++)
        if (keys[i].pitch == pitch) return i;
    return -1;
}

/* The set from low to high, as indexes into keys. */
static void sort_keys(uint8_t *sorted) {
    for (int i = 0; i < held; i++) {
        int j = i;
        for (; j > 0 && keys[sorted[j - 1]].pitch > keys[i].pitch; j--) sorted[j] = sorted[j - 1];
        sorted[j] = (uint8_t)i;
    }
}

/* Notes of the set below `last`, and up to and including it. */
static int below(const uint8_t *sorted) {
    int c = 0;
    while (c < held && keys[sorted[c]].pitch < last) c++;
    return c;
}

static int upto(const uint8_t *sorted) {
    int c = 0;
    while (c < held && keys[sorted[c]].pitch <= last) c++;
    return c;
}

/* The sorted modes walk one line: the set low to high in octave 0, then in octave 1, and so on,
 * `m` places in all. Where to go next on it from (oct, last); `last` may have left the set. */
static int next_place(int mode, const uint8_t *sorted, int m) {
    int n = held, base = oct * n;
    int k;
    switch (mode) {
    case MK_ARP_DOWN:
        if (restart) return m - 1;
        k = base + below(sorted) - 1;
        return k < 0 ? m - 1 : k >= m ? m - 1 : k;
    case MK_ARP_UP_DOWN:
        if (restart) {
            falling = 0;
            return 0;
        }
        if (!falling) {
            k = base + upto(sorted);
            if (k < m) return k;
            falling = 1;                /* the top: turn without playing it again */
            k = base + below(sorted) - 1;
            return k < 0 ? 0 : k >= m ? m - 1 : k;
        }
        k = base + below(sorted) - 1;
        if (k >= 0) return k >= m ? m - 1 : k;
        falling = 0;                    /* the bottom: turn */
        k = base + upto(sorted);
        return k >= m ? 0 : k;
    case MK_ARP_RANDOM:
        return (int)((io->random ? io->random() : 0) % (uint32_t)m);
    default:                            /* MK_ARP_UP */
        if (restart) return 0;
        k = base + upto(sorted);
        return k >= m ? 0 : k;
    }
}

static void play_key(int i, int octave) {
    note_on(keys[i].pitch + 12 * octave, keys[i].velocity);
    last = keys[i].pitch;
    pos = (int8_t)i;
}

/* Choose and sound the notes of a step that is beginning. */
static void play_step(void) {
    if (!held) return;
    if (!settings->free_run && steps >= length_of()) restart = 1;      /* Arp Length, as in the stock firmware */
    if (restart) steps = 0;
    steps++;
    int mode = mode_of(), octaves = octaves_of(), n = held;
    uint8_t sorted[KEYS];
    sort_keys(sorted);
    if (oct >= octaves) oct = (uint8_t)(octaves - 1);
    if (mode == MK_ARP_POLY) {          /* the chord, low to high */
        if (restart) oct = 0;
        else if (++oct >= octaves) oct = 0;
        for (int i = 0; i < n; i++) note_on(keys[sorted[i]].pitch + 12 * oct, keys[sorted[i]].velocity);
    } else if (mode == MK_ARP_ORDER) {
        int i = restart ? 0 : pos + 1;
        if (restart) oct = 0;
        else if (i >= n) {
            i = 0;
            if (++oct >= octaves) oct = 0;
        }
        play_key(i, oct);
    } else {
        int k = next_place(mode, sorted, n * octaves);
        oct = (uint8_t)(k / n);
        play_key(sorted[k % n], oct);
    }
    restart = 0;
}

/* ---- the grid ---- */

/* Swing as in the engine: steps go in pairs of two step lengths, and the second of a pair starts
 * swing per cent into the pair. Gates are measured against the step, never the lengthened one. */
static void begin_step(void) {
    int ticks = ticks_of();
    int first = (2 * ticks * swing_of() + 50) / 100;
    second = (uint8_t)(fresh ? 0 : !second);
    fresh = 0;
    length = (uint16_t)(second ? 2 * ticks - first : first);
    elapsed = 0;
    end_notes();
    play_step();
    if (!sounding) return;
    int span = length < ticks ? length : ticks;
    int gate = span * gate_of() / 100;
    left = (uint16_t)(gate < 1 ? 1 : gate);
}

/* The gate runs down at the start of a tick, before a step begins, so it ends just before the
 * next step's notes start. */
static void tick_body(void) {
    if (!settings || !io) return;
    if (sounding && --left == 0) end_notes();
    if (fresh || elapsed + 1 >= length) begin_step();
    else elapsed++;
}

void mk_arp_tick(void) {
    requested++;
    if (busy) return;
    while (done != requested) {
        done++;
        tick_body();
    }
}

/* ---- the keys ---- */

static void remove_key(int i) {
    for (int j = i; j + 1 < held; j++) {        /* field by field: a struct copy calls memcpy */
        keys[j].pitch = keys[j + 1].pitch;
        keys[j].velocity = keys[j + 1].velocity;
        keys[j].down = keys[j + 1].down;
    }
    held--;
    if (i <= pos) pos--;                /* Order goes on with the key after the one it played */
    if (!held) restart = 1;
}

static int any_down(void) {
    for (int i = 0; i < held; i++)
        if (keys[i].down) return 1;
    return 0;
}

void mk_arp_init(mk_arp_settings *s, const mk_arp_io *callbacks) {
    lock();
    settings = s;
    io = callbacks;
    held = 0;
    hold = 0;
    sounding = 0;
    restart = 1;
    oct = last = falling = steps = 0;
    pos = -1;
    fresh = 1;
    second = 0;
    length = 1;
    elapsed = 0;
    unlock();
}

void mk_arp_key(uint8_t note, uint8_t velocity) {
    if (note > 127) return;
    lock();
    int i = find_key(note);
    if (velocity) {
        if (velocity > 127) velocity = 127;
        if (hold && held && !any_down()) {
            held = 0;                   /* a new chord on a latched one replaces it */
            restart = 1;
            i = -1;
        }
        if (!held) restart = 1;
        if (i < 0 && held < KEYS) {
            i = held++;
            keys[i].pitch = note;
        }
        if (i >= 0) {
            keys[i].velocity = velocity;
            keys[i].down = 1;
        }
    } else if (i >= 0) {
        if (hold) keys[i].down = 0;
        else remove_key(i);
    }
    unlock();
}

void mk_arp_hold(int on_) {
    lock();
    hold = on_ != 0;
    if (!hold)
        for (int i = held - 1; i >= 0; i--)
            if (!keys[i].down) remove_key(i);
    unlock();
}

int mk_arp_held(void) { return held; }

void mk_arp_sync(void) {
    lock();
    fresh = 1;
    unlock();
}

/* The latch setting stays as it is: it belongs to the Hold button. */
void mk_arp_clear(void) {
    lock();
    if (io) end_notes();
    held = 0;
    restart = 1;
    pos = -1;
    unlock();
}
