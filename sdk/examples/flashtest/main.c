/* flashtest: checks the external flash driver and shows the result on the display.
 *
 * It erases and writes one sector, the last of the SDK's area (0x5F000), and nothing else.
 * Three lines: the chip id, whether writes to the stock firmware's area are refused, and
 * whether a write across a page boundary reads back. */
#include "keystep.h"

#define TEST_ADDR (KS_FLASH_USER + KS_FLASH_USER_SIZE - KS_FLASH_SECTOR)

static void hex(uint32_t value, int digits, char *out) {
    for (int i = 0; i < digits; i++) out[i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
    out[digits] = 0;
}

static int guard_holds(void) {
    static const uint8_t zero[4];
    return ks_flash_erase(0) == KS_FLASH_RANGE                         /* pattern 1 */
        && ks_flash_write(0x40000, zero, 4) == KS_FLASH_RANGE          /* settings */
        && ks_flash_erase(0x60000) == KS_FLASH_RANGE                   /* calibration */
        && ks_flash_write(KS_FLASH_USER - 1, zero, 4) == KS_FLASH_RANGE
        && ks_flash_write(KS_FLASH_USER + KS_FLASH_USER_SIZE - 2, zero, 4) == KS_FLASH_RANGE;
}

static int round_trip(void) {
    static uint8_t out[300], in[300];
    for (unsigned i = 0; i < sizeof out; i++) out[i] = (uint8_t)(i * 7 + ks_millis());
    if (ks_flash_erase(TEST_ADDR)) return 0;
    ks_flash_read(TEST_ADDR + 200, in, sizeof in);
    for (unsigned i = 0; i < sizeof in; i++)
        if (in[i] != 0xFF) return 0;                                    /* erased */
    if (ks_flash_write(TEST_ADDR + 200, out, sizeof out)) return 0;     /* crosses a page boundary */
    ks_flash_read(TEST_ADDR + 200, in, sizeof in);
    for (unsigned i = 0; i < sizeof in; i++)
        if (in[i] != out[i]) return 0;
    return ks_flash_erase(TEST_ADDR) == 0;
}

int main(void) {
    char text[8];
    ks_init();
    uint32_t id = ks_flash_id();
    int known = id == 0x1F8401 || id == 0xC84013;
    ks_oled_clear();
    ks_oled_text(0, 0, "id");
    hex(id, 6, text);
    ks_oled_text(24, 0, text);
    ks_oled_text(80, 0, known ? "ok" : "new?");
    ks_oled_text(0, 12, guard_holds() ? "guard ok" : "guard FAIL");
    /* An unknown chip may use other commands: leave it alone. */
    ks_oled_text(0, 22, !known ? "rw skipped" : round_trip() ? "rw ok" : "rw FAIL");
    ks_oled_flush();
    for (;;) { }
}
