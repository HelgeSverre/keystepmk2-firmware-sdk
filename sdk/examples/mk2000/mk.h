/* KeyStep MK2000: shared types and the interfaces between its parts.
 *
 *   engine.c    plays the four tracks from a 96-per-quarter tick; no hardware access
 *   store.c     patterns, chains and settings in the SDK's flash area
 *   ui.c        buttons, layers and gestures
 *   arp.c       the arpeggiator on the keys; no hardware access
 *   harmony.c   scales and chords: tables and arithmetic; no hardware access
 *   mutate.c    Mutate: a variation of a track; no hardware access
 *   cv.c        the CV outputs and their settings
 *   calib.c     the unit's CV calibration, read from the stock firmware's files
 *   controls.c  the strips, aftertouch and the pedal: what they send, and recording them
 *   pages.c     the menu pages: steps, track and global settings, chain
 *   screens.c   the display
 *   clock.c     following an external clock; no hardware access
 *   main.c      start-up, the tick timer, sync, MIDI routing and thru, lights
 *
 * engine.c, clock.c and store.c only use <stdint.h>, <stddef.h>, this header and (store.c) the
 * ks_flash_* functions, so they also build on the host for the tests in tests/.
 *
 * The design is in docs/MK2000.md. */
#ifndef MK_H
#define MK_H
#include <stddef.h>
#include <stdint.h>

#define MK_TRACKS 4
#define MK_STEPS 64             /* per track, at most */
#define MK_POLY 8               /* notes per step */
#define MK_PPQN 96              /* engine ticks per quarter note */
#define MK_SLOTS 64             /* pattern slots: 4 banks of 16 */
#define MK_BANKS 4
#define MK_CHAIN_MAX 32
#define MK_CONTROLLERS 4

/* ---- pattern data ---- */

enum { MK_CTL_BEND, MK_CTL_MOD, MK_CTL_PRESSURE, MK_CTL_PEDAL };      /* pitch bend (its top 7 bits), CC 1,
                                                                           channel pressure, CC 64 */
#define MK_CTL_NONE 0xFF        /* nothing recorded for this step */

enum { MK_STEP_TIE = 1 };       /* flags: the notes run into the next step without a new attack */

/* When a step plays, counted in passes of its track since Play (the first pass is pass 0). */
enum {
    MK_COND_ALWAYS,
    MK_COND_1_OF_2, MK_COND_2_OF_2,
    MK_COND_1_OF_3, MK_COND_1_OF_4,
    MK_COND_FIRST, MK_COND_NOT_FIRST,
    MK_COND_COUNT,
};

typedef struct {
    uint8_t note[MK_POLY];              /* 1 to 127, or 0 for an unused slot; used slots come first */
    uint8_t velocity[MK_POLY];          /* 1 to 127 */
    uint8_t gate;                       /* per cent of the step, 1 to 100; 0 = the track's gate */
    uint8_t probability;                /* 0 to 100 per cent */
    uint8_t ratchet;                    /* 1 to 4 evenly spaced hits */
    uint8_t condition;                  /* MK_COND_ */
    uint8_t flags;                      /* MK_STEP_ */
    uint8_t controller[MK_CONTROLLERS]; /* 0 to 127 (bend: 64 is centre), or MK_CTL_NONE */
    int8_t offset;                      /* when the notes start, in 256ths of the track's step length
                                           (its time division, before swing), from the step's grid time:
                                           -128 to 127, about half a step either way; 0 = on the grid */
} mk_step;

/* Step sizes, in the order the Time Div keys walk through them. */
enum {
    MK_DIV_2D, MK_DIV_2, MK_DIV_2T, MK_DIV_4D, MK_DIV_4, MK_DIV_4T, MK_DIV_8D, MK_DIV_8, MK_DIV_8T,
    MK_DIV_16D, MK_DIV_16, MK_DIV_16T, MK_DIV_32D, MK_DIV_32, MK_DIV_32T, MK_DIV_COUNT,
};
extern const uint16_t mk_div_ticks[MK_DIV_COUNT];       /* engine ticks per step: 288, 192, 128, 144, 96, 64, 72, 48, 32, 36, 24, 16, 18, 12, 8 */
extern const char *const mk_div_name[MK_DIV_COUNT];     /* "1/2D", "1/2", "1/2T", ... "1/32T" */

enum { MK_DIR_FORWARD, MK_DIR_REVERSE, MK_DIR_PENDULUM, MK_DIR_RANDOM, MK_DIR_COUNT };

typedef struct {
    uint8_t channel;                    /* 0 to 15 */
    uint8_t length;                     /* 1 to MK_STEPS */
    uint8_t division;                   /* MK_DIV_ */
    uint8_t swing;                      /* 50 (none) to 75 per cent: where every second step falls */
    uint8_t gate;                       /* 10 to 90 per cent */
    uint8_t direction;                  /* MK_DIR_ */
    uint8_t muted;                      /* 0 or 1 */
    uint8_t follows_transpose;          /* 0 or 1: moves when another track is transposed from the keys */
    int8_t shift;                       /* semitones added to every note, -48 to 48 (the Octave keys add 12) */
    uint8_t spice;                      /* 0 to 100: how far each step's velocity and gate are moved by its dice */
    uint8_t dice;                       /* which set of random amounts Spice uses; Dice deals another */
    uint8_t reserved;
    mk_step step[MK_STEPS];
} mk_track;

typedef struct {
    uint16_t tempo;                     /* tenths of a BPM, 300 to 2400 */
    uint8_t scale;                      /* 0 = the global scale, MK_SCALE_MAJOR and up = this scale,
                                           MK_PATTERN_SCALE_OFF = none, MK_PATTERN_SCALE_USER = user_scale */
    uint8_t root;                       /* of the pattern's own scale: 0 (C) to 11 (B) */
    uint16_t user_scale;                /* the pattern's user scale: bit n set = n semitones above the root is
                                           in it. 0 = not edited yet: all twelve. The root is always in */
    mk_track track[MK_TRACKS];
} mk_pattern;

void mk_pattern_init(mk_pattern *pattern);      /* empty: 120.0 BPM, channels 0-3, 16 steps, 1/16, gate 50, swing 50 */
int mk_step_used(const mk_step *step);          /* 1 if it holds a note */
int mk_track_used(const mk_track *track);       /* 1 if any step within its length holds a note */

/* Spice, as measured on the stock firmware: every step has two fixed random amounts, one for its
 * velocity and one for its gate, and Spice scales them. A step varies the same way on every pass.
 * At full Spice a velocity moves by up to 24 either way and a gate is anything from 15 % to 320 %
 * of what it was (never past the end of its step here). */
int mk_spice_velocity(const mk_track *track, int step, int velocity);  /* 1 to 127 */
int mk_spice_gate(const mk_track *track, int step, int ticks, int most);       /* 1 to `most` */

/* ---- engine ---- */

typedef struct {
    /* A note on (velocity 1-127) or off (velocity 0) of a track. Called from mk_engine_tick. */
    void (*note)(int track, uint8_t channel, uint8_t note, uint8_t velocity);
    /* A recorded controller value of a track as its step begins. */
    void (*controller)(int track, uint8_t channel, int which, uint8_t value);
    /* MIDI real-time bytes: 0xFA at start, 0xFC at stop, 0xF8 every fourth tick while running. */
    void (*realtime)(uint8_t byte);
    /* The pattern passed to mk_engine_switch has just taken over. May be NULL. */
    void (*switched)(void);
    /* Random numbers for probability and the random direction. */
    uint32_t (*random)(void);
} mk_engine_io;

void mk_engine_init(mk_pattern *pattern, const mk_engine_io *io);
void mk_engine_start(void);             /* from the top; sends Start; the first steps sound on the next tick */
void mk_engine_stop(void);              /* ends every sounding note, sends Stop */
int mk_engine_playing(void);
void mk_engine_tick(void);              /* one tick of 96 per quarter note */

void mk_engine_enable(int on);          /* the Seq switch: off = the clock runs but the tracks are silent */
int mk_engine_enabled(void);
void mk_engine_solo(int mask);          /* bit per track; 0 = no solo. Muted or not soloed tracks are cut at once */
void mk_engine_mute_changed(int track); /* call after changing track.muted: cuts its notes if now muted */
void mk_engine_transpose(int track, int semitones);     /* live transposition on top of track.shift; also moves
                                                           every track with follows_transpose set */
int mk_engine_transposition(int track);

int mk_engine_position(int track);      /* the step a track is on (0-based); 0 when stopped */
uint32_t mk_engine_pass(int track);     /* how many times the track has wrapped since Play */
int mk_engine_step_phase(int track);    /* 0 to 255: how far through the current step */

/* Change pattern when track 0 next wraps (or at once when stopped, or when now != 0). The engine
 * keeps using `next` afterwards; the caller owns both buffers. Notes of the old pattern are ended. */
void mk_engine_switch(mk_pattern *next, int now);
mk_pattern *mk_engine_pattern(void);    /* the one playing */
mk_pattern *mk_engine_pending(void);    /* waiting to take over, or NULL */

/* Live recording: put a note into the step nearest to now on a playing track. Returns the step.
 * The engine does not sound it again on the pass in which it was recorded. The first note into an
 * empty step sets the step's offset: 0 when `quantize` is set, else how far from the step's grid
 * time the note was played; later notes into the step keep it. */
int mk_engine_record(int track, uint8_t note, uint8_t velocity, int quantize);
/* Live recording of a controller (MK_CTL_, value 0 to 127): into the step the track is on, since a
 * value holds from its step until the next one that has a value. The engine takes it as sent.
 * Returns the step, or -1 when stopped. */
int mk_engine_record_controller(int track, int which, uint8_t value);
/* The pattern was edited from outside (step page, clear, length): re-check positions against lengths
 * and end notes that no longer belong. */
void mk_engine_edited(int track);

/* ---- arpeggiator: one, on the keys ---- */

enum { MK_ARP_UP, MK_ARP_DOWN, MK_ARP_UP_DOWN, MK_ARP_RANDOM, MK_ARP_ORDER, MK_ARP_POLY, MK_ARP_MODES };
extern const char *const mk_arp_mode_name[MK_ARP_MODES];       /* "Up", "Down", "UpDown", "Random", "Order", "Poly" */

typedef struct {
    uint8_t mode;                       /* MK_ARP_ */
    uint8_t octaves;                    /* 1 to 4 */
    uint8_t division;                   /* MK_DIV_ */
    uint8_t gate;                       /* 10 to 90 per cent */
    uint8_t swing;                      /* 50 to 75 */
    uint8_t length;                     /* the pattern starts again after this many steps, 1 to 64; 0 reads as 16 */
    uint8_t free_run;                   /* 1 = Arp Retrigger Off: the length is not used */
    uint8_t reserved;
} mk_arp_settings;

typedef struct {
    void (*note)(uint8_t note, uint8_t velocity);      /* velocity 0 = off; every on gets its off */
    uint32_t (*random)(void);
} mk_arp_io;

void mk_arp_init(mk_arp_settings *settings, const mk_arp_io *io);       /* settings stay the caller's and may change */
void mk_arp_key(uint8_t note, uint8_t velocity);       /* a key went down (velocity 1-127) or up (0) */
void mk_arp_hold(int on);                               /* latch: keys stay in after release until new ones replace them */
int mk_arp_held(void);                                  /* how many notes the arp is working with */
void mk_arp_tick(void);                                 /* one of 96 per quarter note, always running */
void mk_arp_sync(void);                                 /* the sequencer started: restart the step grid from this tick */
void mk_arp_clear(void);                                /* forget every key and end the sounding notes */

/* ---- scales and chords ---- */

enum {
    MK_SCALE_OFF, MK_SCALE_MAJOR, MK_SCALE_MINOR, MK_SCALE_DORIAN, MK_SCALE_PHRYGIAN, MK_SCALE_LYDIAN,
    MK_SCALE_MIXOLYDIAN, MK_SCALE_LOCRIAN, MK_SCALE_HARMONIC_MINOR, MK_SCALE_BLUES, MK_SCALE_PENTATONIC_MAJOR,
    MK_SCALE_PENTATONIC_MINOR, MK_SCALE_JAPANESE, MK_SCALE_GYPSY, MK_SCALE_ARABIC, MK_SCALE_FREYGISH,
    MK_SCALE_COUNT,
};
#define MK_PATTERN_SCALE_GLOBAL 0
#define MK_PATTERN_SCALE_OFF MK_SCALE_COUNT
#define MK_PATTERN_SCALE_USER (MK_SCALE_COUNT + 1)
/* Where a scale is asked for, a user scale can be given instead: its twelve bits, marked. */
#define MK_SCALE_USER(notes) (0x1000 | ((notes) & 0xFFF))
extern const char *const mk_scale_name[MK_SCALE_COUNT];        /* the stock names: "Off", "Major", ... "Freygish" */
extern const char *const mk_scale_short[MK_SCALE_COUNT];       /* three letters: "", "Maj", "Min", ... */
extern const char *const mk_root_name[12];                     /* "C", "C#", ... "B" */

/* The note of the scale nearest to `note` (0 to 127), the lower one when two are as near, as the
 * stock firmware does. `scale` is a MK_SCALE_ or MK_SCALE_USER(notes). MK_SCALE_OFF, or anything
 * that is not a scale, leaves the note alone. */
int mk_scale_fit(int note, int scale, int root);

enum {
    MK_CHORD_USER, MK_CHORD_OCT, MK_CHORD_5TH, MK_CHORD_MAJ, MK_CHORD_MIN, MK_CHORD_SUS2, MK_CHORD_SUS4,
    MK_CHORD_MAJ7, MK_CHORD_MIN7, MK_CHORD_MAJ9, MK_CHORD_MIN9, MK_CHORD_MAJ11, MK_CHORD_MIN11, MK_CHORD_COUNT,
};
extern const char *const mk_chord_name[MK_CHORD_COUNT];        /* the stock names: "User", "Oct", "5th", "Maj", ... */
enum { MK_STRUM_UP, MK_STRUM_ALT_UP, MK_STRUM_DOWN, MK_STRUM_ALT_DOWN, MK_STRUM_RANDOM, MK_STRUM_COUNT };
extern const char *const mk_strum_name[MK_STRUM_COUNT];
#define MK_CHORD_NOTES 8        /* the most a key plays */
#define MK_STRUM_MAX 80         /* milliseconds between two notes of a chord */

typedef struct {
    uint8_t on;                         /* chord mode: a key plays the chord built on it */
    uint8_t preset;                     /* MK_CHORD_ */
    uint8_t strum;                      /* milliseconds from one note to the next, 0 to MK_STRUM_MAX */
    uint8_t strum_type;                 /* MK_STRUM_ */
    uint8_t voicing;                    /* 0 = off, 1 to 3 = the root that many octaves down as well,
                                           MK_VOICING_ON = each chord kept near the one before (mk_voicing) */
    uint8_t user[3];                    /* the user chord: bit n set = n semitones above its lowest note
                                           (bit 0 is the lowest note itself and reads as set) */
} mk_chord_settings;

/* The notes a key plays in chord mode, low to high, each fitted to the scale, none twice, none
 * outside 0 to 127. Returns how many, 1 to MK_CHORD_NOTES. */
int mk_chord_notes(const mk_chord_settings *chord, int note, int scale, int root, uint8_t out[MK_CHORD_NOTES]);
#define MK_VOICING_ON 4

/* Voicing On, as the stock firmware does it [0x0805CF14]: the notes of a chord are each moved an
 * octave, or not, towards the range the chord before it sounded in, so a run of chords stays in
 * one register. `raw` is the chord as built, in the order root then its intervals; that order is
 * kept, which is also the order the strum walks. Call mk_voicing_process with on = 1 when the
 * chord starts (it changes `notes` in place) and with on = 0, on the chord as built again, when
 * its key is let go: that is when the range it sounded in is taken down for the next chord. */
typedef struct { uint8_t en, lo, hi, last, plo, phi; } mk_voicing;
void mk_voicing_set(mk_voicing *v, int on);             /* the Voicing setting was changed: start afresh */
void mk_voicing_process(mk_voicing *v, int on, uint8_t *notes, int n);
/* The chord as built for Voicing On: the key, then its preset's notes above it, none past 127,
 * none twice, nothing fitted to a scale. Returns how many. */
int mk_chord_raw(const mk_chord_settings *chord, int note, uint8_t out[MK_CHORD_NOTES]);
/* Fit each note to the scale in place, dropping one that lands on an earlier one. Returns how many are left. */
int mk_chord_fit(uint8_t *notes, int n, int scale, int root);

/* Put `n` notes that are low to high into the order they are strummed in. */
void mk_chord_order(uint8_t *notes, int n, int strum_type, uint32_t (*random)(void));
/* Make these notes (any order, up to MK_CHORD_NOTES, within two octaves of the lowest) the user chord. */
void mk_chord_learn(mk_chord_settings *chord, const uint8_t *notes, int n);

/* ---- Mutate ---- */

/* Make the steps of `track`, within its length, a variation of the same steps of `original`: as
 * many steps with notes, the same velocities, some moved a step, some with the notes of another
 * step, a few an octave away. Never the same as the original while it has a note. */
void mk_mutate(mk_track *track, const mk_track *original, uint32_t (*random)(void));

/* ---- following an external clock: the ticks between two of its pulses, spread out ----
 *
 * Each pulse is worth a fixed number of ticks (4 for a MIDI clock, 24 for a sixteenth on the
 * sync jack). The first plays on the pulse, the rest at even spacing over the time the last
 * pulses took. Never more than a pulse's worth run ahead of the pulses, and whatever is still
 * owed when the next pulse comes plays at once, so no tick is lost or doubled however the
 * tempo moves. Call from interrupts that do not nest, or with interrupts off. */

enum { MK_SYNC_AUTO, MK_SYNC_INTERNAL, MK_SYNC_USB, MK_SYNC_DIN, MK_SYNC_JACK, MK_SYNC_COUNT };

typedef struct {
    void (*tick)(void);                         /* one engine tick */
    void (*restart)(uint32_t period_us);        /* the tick timer, from now, every period_us */
} mk_clock_io;

void mk_clock_init(const mk_clock_io *io);
void mk_clock_follow(int per_pulse, uint32_t guess_us); /* 0 = stop following; guess: the first interval */
void mk_clock_pulse(uint32_t now_us);           /* a pulse came in */
void mk_clock_timer(void);                      /* the tick timer ran out */
void mk_clock_arm(void);                        /* drop the ticks owed: the next pulse starts afresh */
void mk_clock_hold(int on);                     /* on: count pulses, play nothing; off: play what is due */
uint32_t mk_clock_interval(void);               /* microseconds between pulses, smoothed; 0 before two came */

/* ---- store ---- */

/* The CV outputs, with the stock firmware's settings and numbering. */
enum { MK_MOD_VELOCITY, MK_MOD_AFTERTOUCH, MK_MOD_WHEEL, MK_MOD_RANDOM, MK_MOD_PULSE, MK_MOD_SOURCES };
enum { MK_RETRIG_MONO, MK_RETRIG_LEGATO, MK_RETRIG_SLIDE, MK_RETRIGS };

typedef struct {
    uint8_t pitch_format;               /* 0 = volts per octave, 1 = hertz per volt */
    uint8_t pitch_ref;                  /* the note at 0 V, in octaves from C-2: 0 to 5 (3 = C1, note 36) */
    uint8_t gate_retrig;                /* MK_RETRIG_ */
    uint8_t gate_format;                /* 0 = S-trig, 1 = V-trig 5 V, 2 = V-trig 12 V */
    uint8_t mod_source[2];              /* MK_MOD_ */
    uint8_t mod_volts[2];               /* 1 to 10: the top of the output */
    uint8_t bend_range;                 /* semitones the Bend strip moves the pitch output, 1 to 24 */
    uint8_t vibrato;                    /* Wheel Vibrato: tenths of a semitone either way with the Mod strip at the
                                           top, 0 to 120 */
    uint8_t vibrato_rate;               /* tenths of a hertz, 3 to 100; 0 reads as 10 */
    uint8_t reserved[5];
} mk_cv_settings;

typedef struct {
    uint8_t version;                    /* MK_SETTINGS_VERSION */
    uint8_t home_channel;               /* 0 to 15 */
    uint8_t keys_on_track;              /* 1 = Track, 0 = Home */
    uint8_t slot;                       /* the pattern slot last used */
    uint8_t seq_on, arp_on;
    uint8_t overdub;
    uint8_t brightness;                 /* button lights, 0 to 255 */
    uint8_t pattern_change;             /* 0 = when track 1 wraps, 1 = at once */
    uint8_t tempo_follows_pattern;      /* 0 = only when stopped, 1 = always, 2 = never */
    uint8_t sync_source;                /* MK_SYNC_ */
    mk_arp_settings arp;
    uint8_t din_thru;                   /* 1 = what comes in on DIN goes out on DIN and USB */
    uint8_t usb_thru;                   /* 1 = what comes in on USB goes out on DIN */
    uint8_t options;                    /* MK_OPT_; 0 is what settings saved before these existed hold */
    uint8_t scale;                      /* the global scale, MK_SCALE_ */
    uint8_t root;                       /* and its root, 0 (C) to 11 (B) */
    mk_chord_settings chord;
    mk_cv_settings cv;
} mk_settings;
#define MK_SETTINGS_VERSION 2           /* 1 was 32 bytes, without cv */
enum {
    MK_OPT_REC_UNQUANTIZED = 1,         /* Rec Quantize Off: live notes keep their timing */
    MK_OPT_NO_AUTOM = 2,                /* Rec Autom Off: the strips, pressure and pedal are not recorded */
    MK_OPT_AFTERTOUCH = 12,             /* two bits: 0 = medium, 1 = low, 2 = high sensitivity, 3 = off */
};
#define MK_OPT_AFTERTOUCH_SHIFT 2

typedef struct {
    uint8_t on;
    uint8_t count;                      /* 0 to MK_CHAIN_MAX */
    uint8_t pattern[MK_CHAIN_MAX];      /* 0 to 15 within the bank */
} mk_chain;

enum { MK_STORE_OK = 0, MK_STORE_EMPTY = -1, MK_STORE_FULL = -2, MK_STORE_ERROR = -3 };

int mk_store_init(void);                                /* read the index; MK_STORE_OK or MK_STORE_ERROR */
int mk_store_has(int slot);                             /* 1 if a pattern is stored in slot 0-63 */
int mk_store_load(int slot, mk_pattern *pattern);       /* MK_STORE_OK, MK_STORE_EMPTY or MK_STORE_ERROR */
int mk_store_save(int slot, const mk_pattern *pattern); /* MK_STORE_OK, MK_STORE_FULL or MK_STORE_ERROR */
int mk_store_erase(int slot);
int mk_store_load_settings(mk_settings *settings);      /* MK_STORE_EMPTY leaves *settings alone */
int mk_store_save_settings(const mk_settings *settings);
int mk_store_load_chain(int bank, mk_chain *chain);
int mk_store_save_chain(int bank, const mk_chain *chain);
int mk_store_free_sectors(void);                        /* of the 31 in the SDK area */

#endif
