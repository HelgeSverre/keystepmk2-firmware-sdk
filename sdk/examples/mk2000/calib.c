/* KeyStep MK2000: the unit's CV calibration, read from the stock firmware's files.
 *
 * The factory calibration is in the stock file system (littlefs, 32 blocks of 4 KiB from 0x60000
 * of the external flash), as calib/pitch, calib/mod1 and calib/mod2: sixteen bytes each, four
 * 32-bit numbers (0, low, 65535, high). The stock firmware puts out low + value x (high - low) /
 * 65535 for a 16-bit value (docs/STOCK-UI.md). This reads the three files and nothing else, and
 * never writes.
 *
 * It is not a file system: just enough of littlefs's layout to follow root -> calib -> a file
 * held inside its directory block. A directory is a pair of blocks, the one with the later
 * revision number being current. A block is a revision number and then tags, each a big-endian
 * word XORed with the tag before it (valid bit, 11 bits of type, 10 of id, 10 of length) and its
 * data. A create tag moves the ids from its own upward by one, a delete tag moves them back; a
 * name tag gives an id its name; an inline tag holds a small file's bytes; a directory tag holds
 * its pair of blocks. Anything unexpected ends the walk, and a file whose numbers do not look
 * like a calibration is left out, so a unit without these files simply plays uncalibrated. */
#include "app.h"

#define FS_BASE 0x60000u
#define FS_BLOCK 0x1000u
#define FS_BLOCKS 32u
enum { T_FILE = 0x001, T_DIR = 0x002, T_DIRSTRUCT = 0x200, T_INLINE = 0x201, T_CREATE = 0x401, T_DELETE = 0x4FF, T_FCRC = 0x5FF };

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

static uint32_t revision(uint32_t block) {
    uint8_t word[4];
    ks_flash_read(FS_BASE + block * FS_BLOCK, word, 4);
    return le32(word);
}

/* Look in the directory `pair` for the entry called `name`, and copy the `want` bytes of its
 * inline file or directory tag to `out`. Returns 1 if it was there with that length. */
static int find(const uint32_t pair[2], const char *name, int kind, uint8_t *out, uint32_t want) {
    if (pair[0] >= FS_BLOCKS || pair[1] >= FS_BLOCKS) return 0;
    uint32_t block = revision(pair[0]) - revision(pair[1]) < 0x80000000u ? pair[0] : pair[1];
    uint32_t base = FS_BASE + block * FS_BLOCK, off = 4, before = 0xFFFFFFFFu;
    uint32_t name_len = 0;
    int target = -1, found = 0;
    while (name[name_len]) name_len++;
    while (off + 4 <= FS_BLOCK) {
        uint8_t word[4], data[32];
        ks_flash_read(base + off, word, 4);
        uint32_t tag = be32(word) ^ before;
        if (tag & 0x80000000u) break;                   /* not a tag: the end of what was written */
        uint32_t type = tag >> 20 & 0x7FF, size = tag & 0x3FF, length = size == 0x3FF ? 0 : size;
        int id = (int)(tag >> 10 & 0x3FF);
        if (off + 4 + length > FS_BLOCK) break;
        if (length <= sizeof data) ks_flash_read(base + off + 4, data, length);
        if (type == T_CREATE) {
            if (target >= id) target++;
        } else if (type == T_DELETE) {
            if (target == id) target = -1, found = 0;
            else if (target > id) target--;
        } else if ((type == T_FILE || type == T_DIR) && length == name_len) {
            int same = 1;
            for (uint32_t i = 0; i < name_len; i++) same &= data[i] == (uint8_t)name[i];
            if (same) target = id, found = 0;
        } else if ((int)type == kind && id == target && length == want) {
            for (uint32_t i = 0; i < want; i++) out[i] = data[i];
            found = 1;
        }
        off += 4 + length;
        before = tag;
        if (type >> 8 == 5 && type != T_FCRC) before ^= (type & 1) << 31;       /* a commit's CRC ends it */
    }
    return found;
}

/* low and high as the file gives them, when they look like a calibration: the two ends of the
 * range, each within a few per cent of where an ideal output would have them. */
static int read_file(const uint32_t pair[2], const char *name, int32_t *low, int32_t *high) {
    uint8_t bytes[16];
    if (!find(pair, name, T_INLINE, bytes, 16)) return 0;
    int32_t at_zero = (int32_t)le32(bytes + 4), at_top = (int32_t)le32(bytes + 12);
    if (le32(bytes) != 0 || le32(bytes + 8) != 65535) return 0;
    if (at_zero < -4000 || at_zero > 4000 || at_top < 65535 - 4000 || at_top > 65535 + 4000) return 0;
    *low = at_zero;
    *high = at_top;
    return 1;
}

int calib_read(app_calibration *out) {
    static const uint32_t root[2] = { 0, 1 };
    static const char *const names[3] = { "pitch", "mod1", "mod2" };
    uint8_t bytes[8];
    int got = 0;
    for (int i = 0; i < 3; i++) out->low[i] = 0, out->high[i] = 65535;
    if (!find(root, "calib", T_DIRSTRUCT, bytes, 8)) return 0;
    uint32_t pair[2] = { le32(bytes), le32(bytes + 4) };
    for (int i = 0; i < 3; i++) got |= read_file(pair, names[i], &out->low[i], &out->high[i]) << i;
    return got;
}

/* A 16-bit value as the unit's own output needs it. */
int32_t calib_apply(const app_calibration *c, int which, int32_t value) {
    int32_t out = value + c->low[which] + value * (c->high[which] - c->low[which] - 65535) / 65535;
    return out < 0 ? 0 : out > 65535 ? 65535 : out;
}
