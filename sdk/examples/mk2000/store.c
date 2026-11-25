/* MK2000 store: patterns, chains and settings in the SDK's flash area, 0x41000-0x5FFFF.
 *
 * On flash. The area is 31 sectors of 4 KiB. A record is kept in one or more whole sectors, its
 * parts; a sector never holds parts of two records. Each part is a 32-byte header and up to 4064
 * bytes of payload. Header (numbers little-endian):
 *
 *   0  magic "MK2S"                    10  payload bytes in this part (u16)
 *   4  format version, 1 or 2          12  seq (u32): new for every version saved, higher is newer
 *   5  kind: 1 pattern, 2 system       16  CRC-32 of this part's payload
 *   6  id: the pattern slot, or 0      20  CRC-32 of bytes 0-19
 *   7  part, 8 parts, 9 zero          24  live: FF FF FF FF; cleared to 0 to retire the version
 *                                      28  unused, FF
 *
 * A save writes the payload of every part, then the headers of parts 1 and up, and last the header
 * of part 0, which commits the version. Only then is the old version retired, by clearing the live
 * word of its part 0. At start-up a version counts if its part 0 header is intact and live; it is
 * damaged (loads report an error) if another part is missing, a payload CRC fails or the format
 * version is not 1 or 2. If two versions of a record survive (power lost before the retire), the
 * higher seq wins and the other is retired. Everything else - blank, half written, retired, erase
 * garbage, records of the old ks_store_* ("KSS1") - is free, and erased when it is next used.
 *
 * Pattern payload: tempo (u16), scale, root; then per track its 12 setting bytes, a 64-bit mask of
 * the steps that differ from an empty one, and those steps. A step starts with a byte saying which
 * fields differ: 1 notes, 2 gate, 4 probability, 8 ratchet, 16 condition, 32 flags, 64 controllers,
 * 128 offset (format 2 only). Notes are a byte of the poly slots in use, then note and velocity for
 * each; controllers a byte of those recorded, then their values; the others one byte each. FF
 * instead: the 25 bytes of note, velocity, gate, probability, ratchet, condition, flags and
 * controllers as they are, and in format 2 the offset after them, when that is shorter or every
 * field differs. A pattern is saved in format 1, which firmware from before the offsets reads too,
 * unless one of its steps has an offset.
 *
 * System payload (kind 2, id 0): a byte of what is present (bit 0 the settings, bits 1-4 the chains
 * of banks 0-3), the 48 bytes of mk_settings, then four mk_chain of 34 bytes. Saving one of them
 * rewrites the record: the settings and four chains share one sector instead of taking five. */
#include "keystep.h"
#include "mk.h"

#define SECTORS ((int)(KS_FLASH_USER_SIZE / KS_FLASH_SECTOR))   /* 31 */
#define HEADER 32u
#define HEADER_WRITTEN 24u                  /* the live word and the rest stay FF until retired */
#define LIVE 24u
#define PAYLOAD_MAX (KS_FLASH_SECTOR - HEADER)
#define MAGIC 0x53324B4Du                   /* "MK2S" */
#define FORMAT 1                            /* the system record, and patterns without offsets */
#define FORMAT_OFFSETS 2
#define KIND_PATTERN 1
#define KIND_SYSTEM 2
#define NO_SECTOR 0xFF

#define STEP_RAW 0xFF
#define STEP_NOTES 0x01
#define STEP_SINGLES 0x3E                   /* gate, probability, ratchet, condition, flags */
#define STEP_CONTROLLERS 0x40
#define STEP_OFFSET 0x80
#define TRACK_BYTES 12
#define STEP_BYTES (2 * MK_POLY + 5 + MK_CONTROLLERS + 1)     /* raw, in format 2 */
#define PATTERN_MAX (4 + MK_TRACKS * (TRACK_BYTES + 8 + MK_STEPS * (1 + STEP_BYTES)))
#define PARTS_MAX 2
_Static_assert(PATTERN_MAX <= PARTS_MAX * PAYLOAD_MAX, "the worst-case pattern must fit in PARTS_MAX sectors");
_Static_assert(SECTORS <= 32, "the sector map is a 32-bit word");
_Static_assert(sizeof(mk_settings) == 48, "the system record holds 48 bytes of settings");

#define SYSTEM MK_SLOTS                     /* the record number of the system record */
#define RECORDS (MK_SLOTS + 1)
#define HAS_SETTINGS 1u
#define SETTINGS_AT 1u
#define CHAINS_AT (SETTINGS_AT + sizeof(mk_settings))
#define SYSTEM_SIZE (CHAINS_AT + MK_BANKS * sizeof(mk_chain))

enum { NONE, VALID, DAMAGED };

typedef struct {
    uint8_t version, kind, id, part, parts;
    uint16_t len;
    uint32_t seq, crc;
    int live;
} header;

/* The RAM index: where the current version of each record is. */
static struct { uint8_t sector[PARTS_MAX], parts, state; } records[RECORDS];
static uint32_t used;                       /* bit per sector holding a part of an indexed version */
static uint32_t next_seq;
static int last;                            /* the sector taken last: the search for a free one starts after it */
static int ready;

static uint8_t buf[256];                    /* flash reads and writes go through here */
static uint8_t format;                      /* of the record being saved or loaded */
static uint8_t sys[SYSTEM_SIZE];            /* the system record while it is loaded or saved */

/* ---- flash, never outside the area ---- */

static uint32_t sector_addr(int s) { return KS_FLASH_USER + (uint32_t)s * KS_FLASH_SECTOR; }

static int in_area(uint32_t addr, uint32_t len) {
    return addr >= KS_FLASH_USER && len <= KS_FLASH_USER_SIZE && addr - KS_FLASH_USER <= KS_FLASH_USER_SIZE - len;
}

static int area_read(uint32_t addr, void *data, uint32_t len) {
    if (!in_area(addr, len)) return -1;
    return ks_flash_read(addr, data, len) ? -1 : 0;
}

static int area_write(uint32_t addr, const void *data, uint32_t len) {
    if (!in_area(addr, len)) return -1;
    return ks_flash_write(addr, data, len) ? -1 : 0;
}

static int area_erase(int s) {
    if (s < 0 || s >= SECTORS) return -1;
    return ks_flash_erase(sector_addr(s)) ? -1 : 0;
}

/* ---- checksums and numbers ---- */

static uint32_t crc32(uint32_t crc, const uint8_t *p, uint32_t len) {
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? crc >> 1 ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

static int crc32_flash(uint32_t addr, uint32_t len, uint32_t *crc) {
    uint32_t c = 0xFFFFFFFFu;
    while (len) {
        uint32_t n = len < sizeof buf ? len : sizeof buf;
        if (area_read(addr, buf, n)) return -1;
        c = crc32(c, buf, n);
        addr += n;
        len -= n;
    }
    *crc = ~c;
    return 0;
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
}

static int bits(unsigned x) {
    int n = 0;
    for (; x; x &= x - 1) n++;
    return n;
}

/* ---- headers ---- */

static void make_header(uint8_t *raw, const header *h) {
    for (unsigned i = 0; i < HEADER; i++) raw[i] = 0xFF;
    put32(raw, MAGIC);
    raw[4] = h->version;
    raw[5] = h->kind;
    raw[6] = h->id;
    raw[7] = h->part;
    raw[8] = h->parts;
    raw[9] = 0;
    raw[10] = (uint8_t)h->len;
    raw[11] = (uint8_t)(h->len >> 8);
    put32(raw + 12, h->seq);
    put32(raw + 16, h->crc);
    put32(raw + 20, ~crc32(0xFFFFFFFFu, raw, 20));
}

/* 1 if sector s starts with an intact header, 0 if not, -1 if the flash cannot be read. */
static int read_header(int s, header *h) {
    uint8_t raw[HEADER];
    if (area_read(sector_addr(s), raw, HEADER)) return -1;
    if (get32(raw) != MAGIC || get32(raw + 20) != ~crc32(0xFFFFFFFFu, raw, 20)) return 0;
    h->version = raw[4];
    h->kind = raw[5];
    h->id = raw[6];
    h->part = raw[7];
    h->parts = raw[8];
    h->len = (uint16_t)(raw[10] | raw[11] << 8);
    h->seq = get32(raw + 12);
    h->crc = get32(raw + 16);
    h->live = get32(raw + LIVE) == 0xFFFFFFFFu;     /* half cleared is retired too */
    return 1;
}

/* Header fields this firmware can index. The header layout stays the same in later formats. */
static int record_of(const header *h) {
    if (h->parts < 1 || h->parts > PARTS_MAX || h->part >= h->parts || h->len > PAYLOAD_MAX) return -1;
    if (h->kind == KIND_PATTERN && h->id < MK_SLOTS) return h->id;
    if (h->kind == KIND_SYSTEM && h->id == 0) return SYSTEM;
    return -1;
}

static int known(const header *h) {
    return h->version == FORMAT || (h->version == FORMAT_OFFSETS && h->kind == KIND_PATTERN);
}

static int retire(int s) {
    static const uint8_t zero[4];
    return area_write(sector_addr(s) + LIVE, zero, sizeof zero);
}

/* ---- the sector map ---- */

static int free_count(void) { return SECTORS - bits(used); }

static int blank(int s) {
    for (uint32_t off = 0; off < KS_FLASH_SECTOR; off += sizeof buf) {
        if (area_read(sector_addr(s) + off, buf, sizeof buf)) return 0;
        for (unsigned i = 0; i < sizeof buf; i++)
            if (buf[i] != 0xFF) return 0;
    }
    return 1;
}

/* Round-robin from the sector taken last, so erases spread over the area. */
static int take_free(uint32_t taken) {
    for (int i = 1; i <= SECTORS; i++) {
        int s = (last + i) % SECTORS;
        if (!((used | taken) >> s & 1)) return s;
    }
    return -1;
}

/* ---- writing a payload: put() counts bytes, or streams them into the parts' sectors ---- */

static struct {
    int writing, error, part;
    uint32_t total, off;
    unsigned fill;
    uint8_t sector[PARTS_MAX];
    uint32_t crc[PARTS_MAX];
} out;

static void flush(void) {
    if (out.fill && !out.error) {
        uint32_t addr = sector_addr(out.sector[out.part]) + HEADER + out.off - out.fill;
        if (area_write(addr, buf, out.fill)) out.error = 1;
    }
    out.fill = 0;
}

static void put(uint8_t b) {
    out.total++;
    if (!out.writing || out.error) return;
    if (out.off == PAYLOAD_MAX) {
        flush();
        if (++out.part >= PARTS_MAX) {
            out.error = 1;
            return;
        }
        out.off = 0;
    }
    buf[out.fill++] = b;
    out.crc[out.part] = crc32(out.crc[out.part], &b, 1);
    out.off++;
    if (out.fill == sizeof buf) flush();
}

static void put_n(const uint8_t *p, int n) {
    while (n--) put(*p++);
}

/* ---- reading a payload back, across its parts ---- */

static struct {
    uint8_t sector[PARTS_MAX];
    uint16_t len[PARTS_MAX];
    int parts, part, error;
    uint32_t off, left;
    unsigned pos, have;
} in;

static uint8_t get(void) {
    if (in.error || !in.left) {
        in.error = 1;
        return 0;
    }
    if (in.pos == in.have) {
        while (in.off == in.len[in.part]) {
            in.part++;
            in.off = 0;
        }
        uint32_t n = in.len[in.part] - in.off;
        if (n > sizeof buf) n = sizeof buf;
        if (area_read(sector_addr(in.sector[in.part]) + HEADER + in.off, buf, n)) {
            in.error = 1;
            return 0;
        }
        in.off += n;
        in.have = n;
        in.pos = 0;
    }
    in.left--;
    return buf[in.pos++];
}

static void get_n(uint8_t *p, int n) {
    while (n--) *p++ = get();
}

/* Checks every part of record r against its headers and CRCs, then sets up get(). */
static int open_record(int r) {
    if (!ready || records[r].state == NONE) return MK_STORE_EMPTY;
    if (records[r].state != VALID) return MK_STORE_ERROR;
    header first = { 0 }, h;
    in.parts = records[r].parts;
    in.part = in.error = 0;
    in.off = in.left = 0;
    in.pos = in.have = 0;
    for (int k = 0; k < in.parts; k++) {
        int s = records[r].sector[k];
        uint32_t crc;
        if (read_header(s, &h) != 1 || !known(&h) || record_of(&h) != r || h.part != k) return MK_STORE_ERROR;
        if (k == 0) first = h;
        if (h.seq != first.seq || h.parts != first.parts || !first.live) return MK_STORE_ERROR;
        if (crc32_flash(sector_addr(s) + HEADER, h.len, &crc) || crc != h.crc) return MK_STORE_ERROR;
        in.sector[k] = (uint8_t)s;
        in.len[k] = h.len;
        in.left += h.len;
    }
    format = first.version;
    return MK_STORE_OK;
}

/* ---- saving ---- */

/* Erases and writes one version into the given sectors. 0, -1 when the flash fails, 1 when it
 * reads back wrong (a sector that looked blank but was not fully erased). */
static int write_version(int r, const uint8_t *sectors, int parts, int force_erase,
                         void (*encode)(const void *), const void *data) {
    for (int k = 0; k < parts; k++)
        if ((force_erase || !blank(sectors[k])) && area_erase(sectors[k])) return -1;

    out.writing = 1;
    out.error = out.part = 0;
    out.total = out.off = 0;
    out.fill = 0;
    for (int k = 0; k < parts; k++) {
        out.sector[k] = sectors[k];
        out.crc[k] = 0xFFFFFFFFu;
    }
    encode(data);
    flush();
    if (out.error) return -1;

    header h = { format, r == SYSTEM ? KIND_SYSTEM : KIND_PATTERN, (uint8_t)(r == SYSTEM ? 0 : r), 0,
                 (uint8_t)parts, 0, next_seq++, 0, 1 };
    uint8_t raw[HEADER];
    for (int k = parts - 1; k >= 0; k--) {          /* part 0 last: its header commits the version */
        uint32_t crc;
        h.part = (uint8_t)k;
        h.len = (uint16_t)(k < parts - 1 ? PAYLOAD_MAX : out.total - (uint32_t)k * PAYLOAD_MAX);
        h.crc = ~out.crc[k];
        if (crc32_flash(sector_addr(sectors[k]) + HEADER, h.len, &crc)) return -1;
        if (crc != h.crc) return 1;
        make_header(raw, &h);
        if (area_write(sector_addr(sectors[k]), raw, HEADER_WRITTEN)) {
            if (k == 0) retire(sectors[0]);         /* it may have committed: do not leave it behind */
            return -1;
        }
        header back;
        int result = read_header(sectors[k], &back);
        if (result != 1 || back.seq != h.seq || back.part != k || !back.live) {
            if (k == 0) retire(sectors[0]);
            return result < 0 ? -1 : 1;
        }
    }
    return 0;
}

static int save_record(int r, void (*encode)(const void *), const void *data) {
    if (!ready) return MK_STORE_ERROR;
    out.writing = 0;
    out.total = 0;
    encode(data);
    int parts = (int)((out.total + PAYLOAD_MAX - 1) / PAYLOAD_MAX);
    if (parts < 1 || parts > PARTS_MAX) return MK_STORE_ERROR;
    if (free_count() < parts) return MK_STORE_FULL;     /* the old version stays until the new one is in */

    uint8_t sectors[PARTS_MAX];
    uint32_t taken = 0;
    for (int k = 0; k < parts; k++) {
        int s = take_free(taken);
        sectors[k] = (uint8_t)s;
        taken |= 1u << s;
    }
    int result = write_version(r, sectors, parts, 0, encode, data);
    if (result > 0) result = write_version(r, sectors, parts, 1, encode, data);
    if (result) return MK_STORE_ERROR;

    /* Committed. Retiring the old version may fail; then start-up sees the higher seq win. */
    if (records[r].state != NONE) {
        if (records[r].sector[0] != NO_SECTOR) retire(records[r].sector[0]);
        for (int k = 0; k < records[r].parts; k++)
            if (records[r].sector[k] != NO_SECTOR) used &= ~(1u << records[r].sector[k]);
    }
    for (int k = 0; k < parts; k++) records[r].sector[k] = sectors[k];
    records[r].parts = (uint8_t)parts;
    records[r].state = VALID;
    used |= taken;
    last = sectors[parts - 1];
    return MK_STORE_OK;
}

/* ---- start-up ---- */

static uint32_t seq_of(int s) {
    header h;
    return read_header(s, &h) == 1 ? h.seq : 0;
}

/* Part 0 of a live version is in sector s: find its other parts, check it, and keep it if it beats
 * the one already indexed. The loser is retired so it cannot come back after an erase. */
static int consider(int s, const header *first) {
    int r = record_of(first);
    uint8_t sectors[PARTS_MAX];
    int state = known(first) ? VALID : DAMAGED;
    header h;
    uint32_t crc;

    sectors[0] = (uint8_t)s;
    for (int k = 1; k < first->parts; k++) {
        sectors[k] = NO_SECTOR;
        for (int t = 0; t < SECTORS; t++) {
            int result = read_header(t, &h);
            if (result < 0) return -1;
            if (result && record_of(&h) == r && h.seq == first->seq && h.part == k) sectors[k] = (uint8_t)t;
        }
        if (sectors[k] == NO_SECTOR) state = DAMAGED;
    }
    for (int k = 0; k < first->parts && state == VALID; k++) {
        if (read_header(sectors[k], &h) != 1) return -1;
        if (crc32_flash(sector_addr(sectors[k]) + HEADER, h.len, &crc)) return -1;
        if (crc != h.crc) state = DAMAGED;
    }

    if (records[r].state != NONE) {
        int old_sector = records[r].sector[0];
        int newer = seq_of(old_sector) < first->seq;
        int better = state == VALID && records[r].state == DAMAGED;
        int worse = state == DAMAGED && records[r].state == VALID;
        if (worse || (!better && !newer)) {
            retire(s);
            return 0;
        }
        retire(old_sector);
    }
    for (int k = 0; k < first->parts; k++) records[r].sector[k] = sectors[k];
    records[r].parts = first->parts;
    records[r].state = (uint8_t)state;
    return 0;
}

int mk_store_init(void) {
    header h;
    uint32_t newest = 0;

    ready = 0;
    for (int r = 0; r < RECORDS; r++) records[r].state = NONE;
    used = 0;
    next_seq = 1;
    last = SECTORS - 1;

    /* seq must stay above every header on the flash, even half-saved ones, so a new version
     * never shares its seq with leftover parts. */
    for (int s = 0; s < SECTORS; s++) {
        int result = read_header(s, &h);
        if (result < 0) return MK_STORE_ERROR;
        if (!result) continue;
        if (h.seq >= next_seq) next_seq = h.seq + 1;
        if (h.seq >= newest) {
            newest = h.seq;
            last = s;
        }
    }
    for (int s = 0; s < SECTORS; s++) {
        int result = read_header(s, &h);
        if (result < 0) return MK_STORE_ERROR;
        if (result && h.part == 0 && h.live && record_of(&h) >= 0 && consider(s, &h)) return MK_STORE_ERROR;
    }
    for (int r = 0; r < RECORDS; r++) {
        if (records[r].state == NONE) continue;
        for (int k = 0; k < records[r].parts; k++)
            if (records[r].sector[k] != NO_SECTOR) used |= 1u << records[r].sector[k];
    }
    ready = 1;
    return MK_STORE_OK;
}

int mk_store_free_sectors(void) { return free_count(); }

/* ---- patterns ---- */

/* The fields of a step that differ from an empty one, as the flags byte of the format. */
static int step_fields(const mk_step *step, uint8_t *notes, uint8_t *controllers) {
    int fields = 0;
    *notes = *controllers = 0;
    for (int i = 0; i < MK_POLY; i++)
        if (step->note[i] || step->velocity[i]) *notes |= (uint8_t)(1u << i);
    for (int c = 0; c < MK_CONTROLLERS; c++)
        if (step->controller[c] != MK_CTL_NONE) *controllers |= (uint8_t)(1u << c);
    if (*notes) fields |= STEP_NOTES;
    if (step->gate) fields |= 0x02;
    if (step->probability != 100) fields |= 0x04;
    if (step->ratchet != 1) fields |= 0x08;
    if (step->condition) fields |= 0x10;
    if (step->flags) fields |= 0x20;
    if (*controllers) fields |= STEP_CONTROLLERS;
    if (step->offset) fields |= STEP_OFFSET;
    return fields;
}

static void encode_step(const mk_step *step) {
    uint8_t notes, controllers;
    int fields = step_fields(step, &notes, &controllers);
    int size = 1 + (notes ? 1 + 2 * bits(notes) : 0) + bits(fields & (STEP_SINGLES | STEP_OFFSET)) +
               (controllers ? 1 + bits(controllers) : 0);
    int raw = 1 + STEP_BYTES - (format != FORMAT_OFFSETS);
    if (size > raw || fields == STEP_RAW) {
        put(STEP_RAW);
        put_n(step->note, MK_POLY);
        put_n(step->velocity, MK_POLY);
        put(step->gate);
        put(step->probability);
        put(step->ratchet);
        put(step->condition);
        put(step->flags);
        put_n(step->controller, MK_CONTROLLERS);
        if (format == FORMAT_OFFSETS) put((uint8_t)step->offset);
        return;
    }
    put((uint8_t)fields);
    if (notes) {
        put(notes);
        for (int i = 0; i < MK_POLY; i++)
            if (notes >> i & 1) {
                put(step->note[i]);
                put(step->velocity[i]);
            }
    }
    if (fields & 0x02) put(step->gate);
    if (fields & 0x04) put(step->probability);
    if (fields & 0x08) put(step->ratchet);
    if (fields & 0x10) put(step->condition);
    if (fields & 0x20) put(step->flags);
    if (controllers) {
        put(controllers);
        for (int c = 0; c < MK_CONTROLLERS; c++)
            if (controllers >> c & 1) put(step->controller[c]);
    }
    if (fields & STEP_OFFSET) put((uint8_t)step->offset);
}

static void encode_pattern(const void *data) {
    const mk_pattern *pattern = data;
    put((uint8_t)pattern->tempo);
    put((uint8_t)(pattern->tempo >> 8));
    put(pattern->scale);
    put(pattern->root);
    for (int t = 0; t < MK_TRACKS; t++) {
        const mk_track *track = &pattern->track[t];
        uint8_t notes, controllers;
        uint32_t mask[2] = { 0, 0 };
        put(track->channel);
        put(track->length);
        put(track->division);
        put(track->swing);
        put(track->gate);
        put(track->direction);
        put(track->muted);
        put(track->follows_transpose);
        put((uint8_t)track->shift);
        put(track->spice);
        put(track->dice);
        put(track->reserved);
        for (int s = 0; s < MK_STEPS; s++)
            if (step_fields(&track->step[s], &notes, &controllers)) mask[s / 32] |= 1u << s % 32;
        for (int i = 0; i < 8; i++) put((uint8_t)(mask[i / 4] >> 8 * (i % 4)));
        for (int s = 0; s < MK_STEPS; s++)
            if (mask[s / 32] >> s % 32 & 1) encode_step(&track->step[s]);
    }
}

static void decode_step(mk_step *step) {
    int fields = get();
    if (fields == STEP_RAW) {
        get_n(step->note, MK_POLY);
        get_n(step->velocity, MK_POLY);
        step->gate = get();
        step->probability = get();
        step->ratchet = get();
        step->condition = get();
        step->flags = get();
        get_n(step->controller, MK_CONTROLLERS);
        if (format == FORMAT_OFFSETS) step->offset = (int8_t)get();
        return;
    }
    if (!fields || (fields & STEP_OFFSET && format != FORMAT_OFFSETS)) in.error = 1;
    if (fields & STEP_NOTES) {
        uint8_t notes = get();
        for (int i = 0; i < MK_POLY; i++)
            if (notes >> i & 1) {
                step->note[i] = get();
                step->velocity[i] = get();
            }
    }
    if (fields & 0x02) step->gate = get();
    if (fields & 0x04) step->probability = get();
    if (fields & 0x08) step->ratchet = get();
    if (fields & 0x10) step->condition = get();
    if (fields & 0x20) step->flags = get();
    if (fields & STEP_CONTROLLERS) {
        uint8_t controllers = get();
        for (int c = 0; c < MK_CONTROLLERS; c++)
            if (controllers >> c & 1) step->controller[c] = get();
    }
    if (fields & STEP_OFFSET) step->offset = (int8_t)get();
}

static void decode_pattern(mk_pattern *pattern) {
    mk_pattern_init(pattern);                       /* what the format leaves out is the default */
    pattern->tempo = get();
    pattern->tempo |= (uint16_t)(get() << 8);
    pattern->scale = get();
    pattern->root = get();
    for (int t = 0; t < MK_TRACKS && !in.error; t++) {
        mk_track *track = &pattern->track[t];
        uint8_t mask[8];
        track->channel = get();
        track->length = get();
        track->division = get();
        track->swing = get();
        track->gate = get();
        track->direction = get();
        track->muted = get();
        track->follows_transpose = get();
        track->shift = (int8_t)get();
        track->spice = get();
        track->dice = get();
        track->reserved = get();
        get_n(mask, 8);
        for (int s = 0; s < MK_STEPS && !in.error; s++)
            if (mask[s / 8] >> s % 8 & 1) decode_step(&track->step[s]);
    }
}

int mk_store_has(int slot) { return ready && slot >= 0 && slot < MK_SLOTS && records[slot].state != NONE; }

int mk_store_load(int slot, mk_pattern *pattern) {
    if (slot < 0 || slot >= MK_SLOTS) return MK_STORE_ERROR;
    int result = open_record(slot);
    if (result) return result;
    decode_pattern(pattern);
    return in.error || in.left ? MK_STORE_ERROR : MK_STORE_OK;
}

int mk_store_save(int slot, const mk_pattern *pattern) {
    if (slot < 0 || slot >= MK_SLOTS) return MK_STORE_ERROR;
    format = FORMAT;
    for (int t = 0; t < MK_TRACKS; t++)
        for (int s = 0; s < MK_STEPS; s++)
            if (pattern->track[t].step[s].offset) format = FORMAT_OFFSETS;
    return save_record(slot, encode_pattern, pattern);
}

/* Retiring part 0 is enough: the other parts are then leftovers, free at the next start-up. */
int mk_store_erase(int slot) {
    if (!ready || slot < 0 || slot >= MK_SLOTS) return MK_STORE_ERROR;
    if (records[slot].state == NONE) return MK_STORE_OK;
    header h;
    int s = records[slot].sector[0];
    if (retire(s)) return MK_STORE_ERROR;
    int result = read_header(s, &h);
    if (result < 0 || (result && h.live)) return MK_STORE_ERROR;
    for (int k = 0; k < records[slot].parts; k++)
        if (records[slot].sector[k] != NO_SECTOR) used &= ~(1u << records[slot].sector[k]);
    records[slot].state = NONE;
    return MK_STORE_OK;
}

/* ---- settings and chains: the system record ---- */

static void encode_system(const void *data) { put_n(data, (int)SYSTEM_SIZE); }

static int load_system(void) {
    int result = open_record(SYSTEM);
    if (result) return result;
    get_n(sys, SYSTEM_SIZE);
    return in.error || in.left ? MK_STORE_ERROR : MK_STORE_OK;
}

static int load_part(void *to, uint32_t at, uint32_t len, unsigned bit) {
    int result = load_system();
    if (result) return result;
    if (!(sys[0] & bit)) return MK_STORE_EMPTY;
    uint8_t *p = to;
    for (uint32_t i = 0; i < len; i++) p[i] = sys[at + i];
    return MK_STORE_OK;
}

/* A damaged system record cannot be kept: the new one then holds only what is being saved. */
static int save_part(const void *from, uint32_t at, uint32_t len, unsigned bit) {
    if (!ready) return MK_STORE_ERROR;
    if (load_system() != MK_STORE_OK)
        for (unsigned i = 0; i < SYSTEM_SIZE; i++) sys[i] = 0;
    const uint8_t *p = from;
    for (uint32_t i = 0; i < len; i++) sys[at + i] = p[i];
    sys[0] |= (uint8_t)bit;
    format = FORMAT;
    return save_record(SYSTEM, encode_system, sys);
}

int mk_store_load_settings(mk_settings *settings) {
    return load_part(settings, SETTINGS_AT, sizeof *settings, HAS_SETTINGS);
}

int mk_store_save_settings(const mk_settings *settings) {
    return save_part(settings, SETTINGS_AT, sizeof *settings, HAS_SETTINGS);
}

int mk_store_load_chain(int bank, mk_chain *chain) {
    if (bank < 0 || bank >= MK_BANKS) return MK_STORE_ERROR;
    return load_part(chain, CHAINS_AT + (uint32_t)bank * sizeof *chain, sizeof *chain, 2u << bank);
}

int mk_store_save_chain(int bank, const mk_chain *chain) {
    if (bank < 0 || bank >= MK_BANKS) return MK_STORE_ERROR;
    return save_part(chain, CHAINS_AT + (uint32_t)bank * sizeof *chain, sizeof *chain, 2u << bank);
}
