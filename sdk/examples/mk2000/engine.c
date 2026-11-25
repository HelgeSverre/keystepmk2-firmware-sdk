/* The sequencer engine: four independent tracks played from a 96-per-quarter tick.
 *
 * Interrupt safety. mk_engine_tick runs in the timer interrupt; everything else runs in the main
 * loop. Every main-loop function that changes state holds a lock (`busy`) while it works. A tick
 * that arrives while the lock is held is only counted, and the main-loop function plays the
 * ticks it owes before it lets go of the lock. So the tick body never sees a half-made change,
 * main-loop functions act at once (a mute cuts its notes in the call itself), and a tick is at
 * most a few microseconds late. Both contexts call the io callbacks. The callbacks must not call
 * the engine's functions that change state; the ones that only read (position, pass, ...) are
 * fine.
 *
 * Every note the engine has switched on is kept in `notes` with the channel and pitch it was sent
 * with, and every note off comes from that table, so a note off always matches its note on.
 *
 * Timing. Each track walks a grid of windows, one per step, `length` ticks each (swing makes the
 * two of a pair unequal). A step begins - decides its probability and condition, sends its
 * controllers, plays its first ratchet part - at its grid time plus its offset. A late step begins
 * inside its own window, at most (length - 1) / 2 ticks in; an early one inside the window before,
 * at most length / 2 of that window early, so the steps of a track always begin in order, never
 * two at once, and every step begins exactly once (the first after Play or a pattern change,
 * having no window before it, on the grid). Ratchet parts and gates are counted from the
 * moment the step began; a step that begins while the one before still has ratchet parts to play
 * drops them, and a note of its own track still sounding at the same pitch is ended and struck
 * again. Only the steps around one begin are ever sounding, so `notes` has room for two steps of
 * eight per track. */
#include "mk.h"

#define HOLD 0xFFFF             /* a tied note: held until the next step of its track that plays */
#define FREE 0xFF
#define NOTES 64

#define barrier() __asm__ volatile("" ::: "memory")

typedef struct {
    uint8_t track;              /* FREE when the entry is unused */
    uint8_t channel, pitch;
    uint16_t left;              /* ticks until the note off, or HOLD */
} sounding;

typedef struct {
    uint8_t pos;                /* the step whose window the track is in */
    uint8_t index;              /* where in the pass: 0 to cycle - 1 */
    uint8_t fresh;              /* the window at index 0 has not begun yet */
    uint8_t drawn, next;        /* random direction: the next step, once drawn */
    uint8_t begun;              /* the step of this window has begun */
    uint8_t ahead;              /* the step of the next window has begun, early */
    uint8_t cur;                /* the step begun last: the one whose ratchet parts play */
    uint8_t hits, hit;          /* ratchet: parts in step `cur` and the next one to play */
    uint16_t length;            /* ticks this window lasts, swing included */
    uint16_t late;              /* ticks into the window its step begins */
    uint16_t span;              /* ticks gates are measured against: the window of `cur`, never lengthened */
    uint16_t elapsed;           /* ticks since the window began */
    uint16_t since;             /* ticks since step `cur` began */
    uint32_t pass, count;       /* passes, and windows begun, since Play */
    uint32_t cur_count;         /* the window of step `cur` */
    uint8_t skip_on, skip_step; /* live-recorded notes not to sound on step `count` == skip_count */
    uint32_t skip_count;
    uint8_t skip[MK_POLY];
    uint8_t sent[MK_CONTROLLERS];       /* the last value sent of each controller, or MK_CTL_NONE */
    int16_t transposition;
} track_state;

static mk_pattern *pattern, *pending;
static const mk_engine_io *io;
static track_state tracks[MK_TRACKS];
static sounding notes[NOTES];
static uint8_t playing, enabled = 1, solo, clocks;
static volatile uint8_t busy;
static volatile uint32_t requested, done;

static void tick_body(void);

static void lock(void) {
    busy = (uint8_t)(busy + 1);
    barrier();
}

/* Play the ticks that came in while the lock was held. One that arrives between the last check
 * and `busy = 0` finds the count unequal, so the loop takes the lock back and plays it. */
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

static uint32_t draw(void) { return io->random ? io->random() : 0; }

static int track_length(const mk_track *tr) {
    if (tr->length < 1) return 1;
    return tr->length > MK_STEPS ? MK_STEPS : tr->length;
}

/* Steps in one pass: a pendulum does not repeat its end steps. */
static int cycle(const mk_track *tr) {
    int n = track_length(tr);
    return tr->direction == MK_DIR_PENDULUM && n > 1 ? 2 * (n - 1) : n;
}

static int step_at(const mk_track *tr, int index) {
    int n = track_length(tr);
    switch (tr->direction) {
    case MK_DIR_REVERSE: return n - 1 - index;
    case MK_DIR_PENDULUM: return index < n ? index : 2 * (n - 1) - index;
    default: return index < n ? index : 0;
    }
}

/* The step the track moves to when its next step begins. A random track draws it once and keeps
 * it, so live recording can find out the next step before it comes. */
static int upcoming(track_state *s, const mk_track *tr) {
    int n = track_length(tr);
    if (tr->direction == MK_DIR_RANDOM) {
        if (!s->drawn) {
            s->next = (uint8_t)(draw() % (uint32_t)n);
            s->drawn = 1;
        }
        return s->next % n;
    }
    int index = s->fresh ? 0 : s->index + 1;
    return step_at(tr, index >= cycle(tr) ? 0 : index);
}

static int audible(int t) {
    return enabled && !pattern->track[t].muted && (!solo || (solo >> t & 1));
}

static void note_off(sounding *e) {
    if (io->note) io->note(e->track, e->channel, e->pitch, 0);
    e->track = FREE;
}

/* t < 0 ends the notes of every track. */
static void end_notes(int t) {
    for (int i = 0; i < NOTES; i++)
        if (notes[i].track != FREE && (t < 0 || notes[i].track == t)) note_off(&notes[i]);
}

static sounding *find(uint8_t channel, uint8_t pitch) {
    for (int i = 0; i < NOTES; i++)
        if (notes[i].track != FREE && notes[i].channel == channel && notes[i].pitch == pitch) return &notes[i];
    return NULL;
}

static void note_on(int t, uint8_t channel, uint8_t pitch, uint8_t velocity, uint16_t gate) {
    for (int i = 0; i < NOTES; i++) {
        if (notes[i].track != FREE) continue;
        notes[i] = (sounding){ (uint8_t)t, channel, pitch, gate };
        if (io->note) io->note(t, channel, pitch, velocity ? (velocity > 127 ? 127 : velocity) : 1);
        return;
    }
}

/* The gates run down at the start of a tick, before any step begins, so a gate that reaches the
 * next step ends just before that step's notes start. */
static void count_down(void) {
    for (int i = 0; i < NOTES; i++)
        if (notes[i].track != FREE && notes[i].left != HOLD && --notes[i].left == 0) note_off(&notes[i]);
}

static int pitch_of(int t, uint8_t note) {
    if (!note || note > 127) return -1;
    const mk_track *tr = &pattern->track[t];
    int pitch = note + tr->shift + tracks[t].transposition;
    if (pitch < 0 || pitch > 127) return -1;
    if (tr->spice) pitch += mk_spice_octave(tr, tracks[t].cur);        /* Spice: the step an octave away, as far as there is room */
    return pitch < 0 ? pitch + 12 : pitch > 127 ? pitch - 12 : pitch;
}

static int skipped(const track_state *s, uint8_t note) {
    if (!s->skip_on || s->skip_count != s->cur_count || s->skip_step != s->cur) return 0;
    for (int i = 0; i < MK_POLY; i++)
        if (s->skip[i] == note) return 1;
    return 0;
}

/* Would the step begun last on track t sound this pitch on this channel? */
static int sounds(int t, uint8_t channel, uint8_t pitch) {
    const track_state *s = &tracks[t];
    const mk_step *st = &pattern->track[t].step[s->cur];
    if ((pattern->track[t].channel & 15) != channel) return 0;
    for (int i = 0; i < MK_POLY; i++)
        if (pitch_of(t, st->note[i]) == pitch && !skipped(s, st->note[i])) return 1;
    return 0;
}

static int condition_holds(uint8_t condition, uint32_t pass) {
    switch (condition) {
    case MK_COND_1_OF_2: return pass % 2 == 0;
    case MK_COND_2_OF_2: return pass % 2 == 1;
    case MK_COND_1_OF_3: return pass % 3 == 0;
    case MK_COND_1_OF_4: return pass % 4 == 0;
    case MK_COND_FIRST: return pass == 0;
    case MK_COND_NOT_FIRST: return pass != 0;
    default: return 1;
    }
}

/* The condition first, so a step that cannot play does not use up a random number. */
static int step_plays(const mk_step *st, uint32_t pass) {
    if (!condition_holds(st->condition, pass)) return 0;
    if (st->probability >= 100) return 1;
    return st->probability && draw() % 100 < st->probability;
}

static int part_start(const track_state *s, int part) { return part * s->span / s->hits; }

static int gate_percent(const mk_track *tr, const mk_step *st) {
    int percent = st->gate ? st->gate : tr->gate;
    return percent < 1 ? 1 : percent > 100 ? 100 : percent;
}

/* One ratchet part of step `cur`. A tied note of this track with a pitch the step plays carries
 * on into the first part instead of being struck again. */
static void play_hit(int t) {
    track_state *s = &tracks[t];
    const mk_track *tr = &pattern->track[t];
    const mk_step *st = &tr->step[s->cur];
    int part = s->hit++;
    if (!audible(t)) return;
    int percent = gate_percent(tr, st);
    if (tr->spice) percent = mk_spice_gate(tr, s->cur, percent);
    int whole = part_start(s, part + 1) - part_start(s, part);
    int gate = whole * percent / 100;
    if (gate < 1) gate = 1;
    if (part == s->hits - 1 && (st->flags & MK_STEP_TIE)) gate = HOLD;
    uint8_t channel = tr->channel & 15;
    for (int i = 0; i < MK_POLY; i++) {
        int pitch = pitch_of(t, st->note[i]);
        if (pitch < 0 || skipped(s, st->note[i])) continue;
        sounding *e = find(channel, (uint8_t)pitch);
        if (e && part == 0 && e->track == t && e->left == HOLD) {
            e->left = (uint16_t)gate;
            continue;
        }
        if (e && e->track != t) continue;               /* never the same note on twice */
        if (e) note_off(e);
        int velocity = tr->spice ? mk_spice_velocity(tr, s->cur, st->velocity[i]) : st->velocity[i];
        note_on(t, channel, (uint8_t)pitch, (uint8_t)velocity, (uint16_t)gate);
    }
}

static void send_controllers(int t, const mk_step *st) {
    track_state *s = &tracks[t];
    for (int c = 0; c < MK_CONTROLLERS; c++) {
        uint8_t value = st->controller[c];
        if (value > 127 || value == s->sent[c]) continue;
        s->sent[c] = value;
        if (io->controller) io->controller(t, pattern->track[t].channel & 15, c, value);
    }
}

static int step_ticks(const mk_track *tr) { return mk_div_ticks[tr->division < MK_DIV_COUNT ? tr->division : MK_DIV_16]; }

/* Swing: windows go in pairs of two step lengths, and the second of a pair starts swing per cent
 * into the pair. `count` is the window's number since Play. */
static int window(const mk_track *tr, uint32_t count) {
    int ticks = step_ticks(tr);
    int swing = tr->swing < 50 ? 50 : tr->swing > 75 ? 75 : tr->swing;
    int first = (2 * ticks * swing + 50) / 100;
    return count & 1 ? 2 * ticks - first : first;
}

/* A step's offset in ticks, rounded to the nearest. */
static int offset_ticks(const mk_track *tr, int step) {
    int x = tr->step[step].offset * step_ticks(tr);
    return x < 0 ? -((-x + 128) / 256) : (x + 128) / 256;
}

static void advance(track_state *s, const mk_track *tr) {
    int pos = upcoming(s, tr);
    s->drawn = 0;
    if (!s->fresh) {
        if (++s->index >= cycle(tr)) {
            s->index = 0;
            s->pass++;
        }
        s->count++;
    }
    s->fresh = 0;
    s->pos = (uint8_t)pos;
}

/* Step `pos` of window `count` in pass `pass` begins. */
static void begin(int t, int pos, uint32_t count, uint32_t pass) {
    track_state *s = &tracks[t];
    const mk_track *tr = &pattern->track[t];
    int ticks = step_ticks(tr), length = window(tr, count);
    s->cur = (uint8_t)pos;
    s->cur_count = count;
    s->since = 0;
    s->span = (uint16_t)(length < ticks ? length : ticks);
    s->hits = s->hit = 0;
    const mk_step *st = &tr->step[pos];
    if (!audible(t)) {
        end_notes(t);
        return;
    }
    send_controllers(t, st);
    if (!step_plays(st, pass)) return;
    if (tr->spice && !mk_spice_gate(tr, pos, gate_percent(tr, st))) return;     /* Spice leaves this step out */
    for (int i = 0; i < NOTES; i++)
        if (notes[i].track == t && notes[i].left == HOLD && !sounds(t, notes[i].channel, notes[i].pitch))
            note_off(&notes[i]);
    int hits = st->ratchet < 1 ? 1 : st->ratchet > 4 ? 4 : st->ratchet;
    s->hits = (uint8_t)(hits < s->span ? hits : s->span);
    play_hit(t);
}

/* Ticks from now until a waiting pattern takes over, when track 0 begins its next pass. Each pair
 * of windows is two step lengths whatever the swing. */
static uint32_t until_switch(void) {
    const track_state *s = &tracks[0];
    const mk_track *tr = &pattern->track[0];
    int steps = cycle(tr) - 1 - s->index;
    uint32_t left = steps < 0 ? 0 : (uint32_t)steps;
    return (uint32_t)(s->length - s->elapsed) + left / 2 * 2 * (uint32_t)step_ticks(tr) +
           (left & 1 ? (uint32_t)window(tr, s->count + 1) : 0);
}

/* Begin the next window's step now if it is early and its time has come. Its window may not come:
 * a step whose window would begin when or after a waiting pattern takes over belongs to the pass
 * the switch replaces, so it does not begin early (and the new pattern's first steps begin on
 * the grid, there being no step before them). A random track draws its next step here, in the
 * second half of the window, where an early step can begin. */
static int begin_early(int t) {
    track_state *s = &tracks[t];
    const mk_track *tr = &pattern->track[t];
    if (2 * s->elapsed < s->length) return 0;
    int next = upcoming(s, tr);
    int early = -offset_ticks(tr, next), most = s->length / 2;
    if (early <= 0) return 0;
    if (s->elapsed + (early < most ? early : most) < s->length) return 0;
    if (pending && (uint32_t)(s->length - s->elapsed) >= until_switch()) return 0;
    s->ahead = 1;
    begin(t, next, s->count + 1, s->pass + (s->index + 1 >= cycle(tr)));
    return 1;
}

static void track_tick(int t) {
    track_state *s = &tracks[t];
    const mk_track *tr = &pattern->track[t];
    if (s->hit < s->hits) s->since++;
    if (s->fresh || s->elapsed + 1 >= s->length) {
        advance(s, tr);
        s->length = (uint16_t)window(tr, s->count);
        s->elapsed = 0;
        s->begun = s->ahead;
        s->ahead = 0;
        int late = offset_ticks(tr, s->pos), most = (s->length - 1) / 2;
        s->late = (uint16_t)(late < 0 ? 0 : late < most ? late : most);
    } else {
        s->elapsed++;
    }
    if (!s->begun && s->elapsed >= s->late) {
        s->begun = 1;
        begin(t, s->pos, s->count, s->pass);
        return;
    }
    if (s->begun && !s->ahead && begin_early(t)) return;
    if (s->hit < s->hits && s->since >= part_start(s, s->hit)) play_hit(t);
}

static void reset_tracks(void) {
    for (int t = 0; t < MK_TRACKS; t++) {
        track_state *s = &tracks[t];
        const mk_track *tr = &pattern->track[t];
        s->index = 0;
        s->pos = (uint8_t)(tr->direction == MK_DIR_RANDOM ? 0 : step_at(tr, 0));
        s->fresh = 1;
        s->drawn = 0;
        s->begun = s->ahead = 0;
        s->cur = s->pos;
        s->hits = s->hit = 0;
        s->elapsed = s->since = s->late = 0;
        s->pass = s->count = s->cur_count = 0;
        s->skip_on = 0;
    }
}

static void take_over(mk_pattern *next) {
    end_notes(-1);
    pattern = next;
    pending = NULL;
    reset_tracks();
    if (io->switched) io->switched();
}

/* Track 0 is about to begin the first step of a new pass. */
static int track0_wraps(void) {
    const track_state *s = &tracks[0];
    return !s->fresh && s->elapsed + 1 >= s->length && s->index + 1 >= cycle(&pattern->track[0]);
}

static void tick_body(void) {
    if (!playing) return;
    if (clocks == 0 && io->realtime) io->realtime(0xF8);
    clocks = (uint8_t)((clocks + 1) & 3);
    count_down();
    if (pending && track0_wraps()) take_over(pending);
    for (int t = 0; t < MK_TRACKS; t++) track_tick(t);
}

void mk_engine_tick(void) {
    requested++;
    if (busy) return;
    while (done != requested) {
        done++;
        tick_body();
    }
}

void mk_engine_init(mk_pattern *first, const mk_engine_io *callbacks) {
    lock();
    pattern = first;
    pending = NULL;
    io = callbacks;
    playing = 0;
    enabled = 1;
    solo = 0;
    for (int i = 0; i < NOTES; i++) notes[i].track = FREE;
    for (int t = 0; t < MK_TRACKS; t++) {
        tracks[t].transposition = 0;
        for (int c = 0; c < MK_CONTROLLERS; c++) tracks[t].sent[c] = MK_CTL_NONE;
    }
    reset_tracks();
    unlock();
}

/* Play also sends the controllers afresh, in case the receiver was changed while stopped. */
void mk_engine_start(void) {
    lock();
    end_notes(-1);
    reset_tracks();
    for (int t = 0; t < MK_TRACKS; t++)
        for (int c = 0; c < MK_CONTROLLERS; c++) tracks[t].sent[c] = MK_CTL_NONE;
    clocks = 0;
    playing = 1;
    if (io->realtime) io->realtime(0xFA);
    unlock();
}

/* A pattern change still waiting takes over now: there is no wrap to wait for any more. */
void mk_engine_stop(void) {
    lock();
    end_notes(-1);
    playing = 0;
    if (io->realtime) io->realtime(0xFC);
    if (pending) take_over(pending);
    unlock();
}

int mk_engine_playing(void) { return playing; }

void mk_engine_enable(int on) {
    lock();
    enabled = on != 0;
    if (!enabled) end_notes(-1);
    unlock();
}

int mk_engine_enabled(void) { return enabled; }

static void cut_silent(void) {
    for (int t = 0; t < MK_TRACKS; t++)
        if (!audible(t)) end_notes(t);
}

void mk_engine_solo(int mask) {
    lock();
    solo = (uint8_t)(mask & ((1 << MK_TRACKS) - 1));
    cut_silent();
    unlock();
}

void mk_engine_mute_changed(int track) {
    if (track < 0 || track >= MK_TRACKS) return;
    lock();
    if (!audible(track)) end_notes(track);
    unlock();
}

void mk_engine_transpose(int track, int semitones) {
    if (track < 0 || track >= MK_TRACKS) return;
    if (semitones < -127) semitones = -127;
    if (semitones > 127) semitones = 127;
    lock();
    for (int t = 0; t < MK_TRACKS; t++)
        if (t == track || pattern->track[t].follows_transpose) tracks[t].transposition = (int16_t)semitones;
    unlock();
}

int mk_engine_transposition(int track) {
    return track >= 0 && track < MK_TRACKS ? tracks[track].transposition : 0;
}

int mk_engine_position(int track) {
    return playing && track >= 0 && track < MK_TRACKS ? tracks[track].pos : 0;
}

uint32_t mk_engine_pass(int track) {
    return track >= 0 && track < MK_TRACKS ? tracks[track].pass : 0;
}

int mk_engine_step_phase(int track) {
    if (!playing || track < 0 || track >= MK_TRACKS) return 0;
    const track_state *s = &tracks[track];
    if (s->fresh || !s->length) return 0;
    int phase = s->elapsed * 256 / s->length;
    return phase > 255 ? 255 : phase;
}

/* A NULL `next` cancels a change that is waiting. */
void mk_engine_switch(mk_pattern *next, int now) {
    lock();
    if (!next) pending = NULL;
    else if (!playing || now) take_over(next);
    else pending = next;
    unlock();
}

mk_pattern *mk_engine_pattern(void) { return pattern; }
mk_pattern *mk_engine_pending(void) { return pending; }

static int add_note(mk_step *st, uint8_t note, uint8_t velocity) {
    for (int i = 0; i < MK_POLY; i++) {
        if (st->note[i] == note) return 1;
        if (!st->note[i]) {
            st->velocity[i] = velocity;
            st->note[i] = note;
            return 1;
        }
    }
    return 0;
}

/* The player is holding the note already, so the engine leaves it out of step `count` once. */
static void skip_once(track_state *s, int step, uint32_t count, uint8_t note) {
    if (!s->skip_on || s->skip_count != count || s->skip_step != step) {
        for (int i = 0; i < MK_POLY; i++) s->skip[i] = 0;
        s->skip_on = 1;
        s->skip_step = (uint8_t)step;
        s->skip_count = count;
    }
    for (int i = 0; i < MK_POLY; i++) {
        if (s->skip[i] == note) return;
        if (!s->skip[i]) {
            s->skip[i] = note;
            return;
        }
    }
}

/* Ticks from a step's grid time as an offset, rounded to the nearest. */
static int8_t to_offset(const mk_track *tr, int ticks) {
    int ticks256 = ticks * 256, per = step_ticks(tr);
    int x = ticks256 < 0 ? -((-ticks256 + per / 2) / per) : (ticks256 + per / 2) / per;
    return (int8_t)(x < -128 ? -128 : x > 127 ? 127 : x);
}

int mk_engine_record(int track, uint8_t note, uint8_t velocity, int quantize) {
    if (track < 0 || track >= MK_TRACKS || !note || note > 127) return -1;
    lock();
    int step = -1;
    if (playing) {
        track_state *s = &tracks[track];
        mk_track *tr = &pattern->track[track];
        uint32_t count = s->count;
        int at = 0;                                     /* ticks from the step's grid time */
        if (s->fresh) step = upcoming(s, tr);
        else if (2 * s->elapsed < s->length) {
            step = s->pos;
            at = s->elapsed;
        } else {
            step = upcoming(s, tr);
            count++;
            at = s->elapsed - s->length;
        }
        mk_step *st = &tr->step[step];
        int empty = !mk_step_used(st);
        if (add_note(st, note, velocity ? (velocity > 127 ? 127 : velocity) : 1)) {
            if (empty) st->offset = quantize ? 0 : to_offset(tr, at);
            skip_once(s, step, count, note);
        }
    }
    unlock();
    return step;
}

int mk_engine_record_controller(int track, int which, uint8_t value) {
    if (track < 0 || track >= MK_TRACKS || which < 0 || which >= MK_CONTROLLERS || value > 127) return -1;
    lock();
    int step = -1;
    if (playing) {
        track_state *s = &tracks[track];
        mk_track *tr = &pattern->track[track];
        step = s->fresh ? upcoming(s, tr) : s->pos;
        tr->step[step].controller[which] = value;
        s->sent[which] = value;                         /* the control itself has just sent it */
    }
    unlock();
    return step;
}

/* Bring the position inside the track again, keeping the step it is on when that still exists
 * and the way a pendulum is going, so the next step follows on from it. */
static void place(track_state *s, const mk_track *tr) {
    int n = track_length(tr);
    int down = tr->direction == MK_DIR_PENDULUM && s->index >= n;
    if (s->pos >= n) s->pos = (uint8_t)(s->pos % n);
    switch (tr->direction) {
    case MK_DIR_REVERSE: s->index = (uint8_t)(n - 1 - s->pos); break;
    case MK_DIR_PENDULUM: s->index = (uint8_t)(down && s->pos > 0 && s->pos < n - 1 ? 2 * (n - 1) - s->pos : s->pos); break;
    case MK_DIR_RANDOM: s->index = (uint8_t)(s->index % n); break;
    default: s->index = s->pos;
    }
}

void mk_engine_edited(int track) {
    if (track < 0 || track >= MK_TRACKS) return;
    lock();
    track_state *s = &tracks[track];
    const mk_track *tr = &pattern->track[track];
    int n = track_length(tr);
    end_notes(track);
    s->hits = 0;
    s->next = (uint8_t)(s->next % n);
    if (s->skip_step >= n) s->skip_on = 0;
    if (s->fresh) s->pos = (uint8_t)(tr->direction == MK_DIR_RANDOM ? 0 : step_at(tr, 0));
    else place(s, tr);
    unlock();
}
