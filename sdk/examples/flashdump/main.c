/* flashdump: sends the whole external flash out as MIDI SysEx, for tools/flashdump.py to
 * reassemble. It only reads the flash. Use it to keep a copy of a unit's patterns, settings and
 * factory calibration before any firmware writes there.
 *
 *   Play    send: over USB if a computer has the device open, else on the DIN output (about
 *           3.5 minutes for a full chip there). Blank areas are skipped.
 *   Stop    abort
 *
 * Messages: F0 7D 'K' 'S' <type> <body> F7. The body is packed seven bytes into eight, the
 * first of each eight holding the top bits. Numbers are little-endian.
 *   1 start   id[3], size u32, block u16
 *   2 data    address u32, 256 bytes, crc16 u16 of the address and the bytes
 *   3 blank   address u32, length u32 (all 0xFF), crc16 u16 of both
 *   4 end     crc32 u32 of the whole chip */
#include "keystep.h"

#define BLOCK 256u

static uint8_t group[7];
static int grouped;

static int over_usb;                        /* chosen when a dump starts */

static void send_byte(uint8_t byte) {
    if (over_usb) {
        while (ks_usb_midi_tx_free() < 3) { }
        ks_usb_midi_send_byte(byte);
    } else {
        while (ks_midi_tx_free() < 1) { }
        ks_midi_send_byte(byte);
    }
}

static void flush_group(void) {
    uint8_t top = 0;
    if (!grouped) return;
    for (int i = 0; i < grouped; i++) top |= (uint8_t)((group[i] >> 7) << i);
    send_byte(top);
    for (int i = 0; i < grouped; i++) send_byte(group[i] & 0x7F);
    grouped = 0;
}

static void begin(uint8_t type) {
    send_byte(0xF0);
    send_byte(0x7D);
    send_byte('K');
    send_byte('S');
    send_byte(type);
}

static void put(const void *data, uint32_t len) {
    const uint8_t *p = data;
    while (len--) {
        group[grouped++] = *p++;
        if (grouped == 7) flush_group();
    }
}

static void end(void) {
    flush_group();
    send_byte(0xF7);
}

static uint16_t crc16(uint16_t crc, const uint8_t *p, uint32_t len) {
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

static uint32_t crc32(uint32_t crc, const uint8_t *p, uint32_t len) {
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? crc >> 1 ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

static void hex(uint32_t value, int digits, char *out) {
    for (int i = 0; i < digits; i++) out[i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
    out[digits] = 0;
}

static void show(const char *line1, const char *line2) {
    char text[8];
    ks_oled_clear();
    ks_oled_text(0, 0, "flash dump");
    hex(ks_flash_id(), 6, text);
    ks_oled_text(80, 0, text);
    ks_oled_text(0, 12, line1);
    ks_oled_text(0, 22, line2);
    ks_oled_flush();
}

static void send_blank(uint32_t addr, uint32_t len) {
    uint32_t words[2] = { addr, len };
    uint16_t crc = crc16(0xFFFF, (const uint8_t *)words, 8);
    begin(3);
    put(words, 8);
    put(&crc, 2);
    end();
}

/* Returns 1 when the whole chip went out, 0 when Stop cut it short. */
static int dump(uint32_t *sum) {
    static uint8_t block[BLOCK];
    uint32_t id = ks_flash_id(), size = KS_FLASH_SIZE, crc = 0xFFFFFFFFu, blank_from = 0, blank = 0;
    uint16_t block_size = BLOCK;
    uint8_t id_bytes[3] = { (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };
    char text[8];
    over_usb = ks_usb_ready();
    begin(1);
    put(id_bytes, 3);
    put(&size, 4);
    put(&block_size, 2);
    end();
    for (uint32_t addr = 0; addr < KS_FLASH_SIZE; addr += BLOCK) {
        if (ks_buttons() & KS_BTN_STOP) return 0;
        ks_flash_read(addr, block, BLOCK);
        crc = crc32(crc, block, BLOCK);
        int empty = 1;
        for (uint32_t i = 0; i < BLOCK && empty; i++) empty = block[i] == 0xFF;
        if (empty) {
            if (!blank) blank_from = addr;
            blank += BLOCK;
        } else {
            if (blank) send_blank(blank_from, blank);
            blank = 0;
            uint16_t check = crc16(crc16(0xFFFF, (const uint8_t *)&addr, 4), block, BLOCK);
            begin(2);
            put(&addr, 4);
            put(block, BLOCK);
            put(&check, 2);
            end();
        }
        if (!(addr & 0x3FFF)) {                     /* every 16 KiB */
            ks_utoa(addr * 100 / KS_FLASH_SIZE, text);
            show(over_usb ? "usb        %" : "din        %", text);
            ks_led(KS_LED_PLAY, addr & 0x4000 ? 0x00008D00 : 0);
            ks_leds_show();
        }
    }
    if (blank) send_blank(blank_from, blank);
    crc = ~crc;
    begin(4);
    put(&crc, 4);
    end();
    *sum = crc;
    return 1;
}

int main(void) {
    char text[12];
    uint32_t previous = 0, sum;
    ks_init();
    show("Play: send", "reads only");
    for (;;) {
        uint32_t buttons = ks_buttons();
        if (buttons & ~previous & KS_BTN_PLAY) {
            if (dump(&sum)) {
                hex(sum, 8, text);
                show("done, crc32", text);
            } else {
                show("stopped", "Play: send");
            }
            ks_led(KS_LED_PLAY, 0);
            ks_leds_show();
            buttons = ks_buttons();
        }
        previous = buttons;
    }
}
