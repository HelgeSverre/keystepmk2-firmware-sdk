/* The external flash: 512 KiB SPI NOR on SPI2 (PB13 clock, PB14 in, PB15 out), chip select PA8.
 * Commands as the stock firmware uses them: 66 99 reset, 9F id, 03 read, 06 write enable,
 * 02 page program, 20 erase 4 KiB, 05 status.
 *
 * What the stock firmware keeps there (docs/HARDWARE.md):
 *   0x00000-0x3FFFF  64 patterns, one 4 KiB block each
 *   0x40000-0x40FFF  settings
 *   0x41000-0x5FFFF  nothing: the SDK's area
 *   0x60000-0x7FFFF  littlefs with the factory calibration
 * Writes outside the SDK's area are refused unless ks_flash_allow_stock_area(1) was called. */
#include "board.h"

#define PAGE 256u
#define STORE_MAGIC 0x3153534Bu                     /* "KSS1" */

static int stock_area_allowed;

static uint8_t xfer(uint8_t out) {
    SPI_DR8(SPI2) = out;
    for (int i = 0; i < 100000; i++)
        if (SPI_SR(SPI2) & 1) return SPI_DR8(SPI2);
    return 0xFF;
}

static void select(void) { pin_set(GPIOA, 8, 0); }
static void deselect(void) { pin_set(GPIOA, 8, 1); }

static void command(uint8_t op) {
    select();
    xfer(op);
    deselect();
}

static void command_at(uint8_t op, uint32_t addr) {
    select();
    xfer(op);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

static int wait_ready(uint32_t timeout_ms) {
    uint32_t start = ks_millis();
    for (;;) {
        select();
        xfer(0x05);
        uint8_t status = xfer(0);
        deselect();
        if (!(status & 1)) return 0;
        if (ks_millis() - start > timeout_ms) return KS_FLASH_TIMEOUT;
    }
}

static int writable(uint32_t addr, uint32_t len) {
    if (len > KS_FLASH_SIZE || addr > KS_FLASH_SIZE - len) return 0;
    if (stock_area_allowed) return 1;
    return addr >= KS_FLASH_USER && addr + len <= KS_FLASH_USER + KS_FLASH_USER_SIZE;
}

void flash_init(void) {
    pin_out(GPIOA, 8, 1);
    pin_af(GPIOB, 13, 0, 0);
    pin_af(GPIOB, 14, 0, 0);
    pin_af(GPIOB, 15, 0, 0);
    SPI_CR2(SPI2) = 0x1700;                         /* 8-bit frames */
    SPI_CR1(SPI2) = 0x34C;                          /* master, clock / 4 = 16 MHz, software select, on */
    command(0x66);
    command(0x99);                                  /* reset: ends any operation a reset interrupted */
    ks_delay_ms(1);
}

uint32_t ks_flash_id(void) {
    select();
    xfer(0x9F);
    uint32_t id = (uint32_t)xfer(0) << 16;
    id |= (uint32_t)xfer(0) << 8;
    id |= xfer(0);
    deselect();
    return id;
}

void ks_flash_allow_stock_area(int allow) { stock_area_allowed = allow; }

int ks_flash_read(uint32_t addr, void *data, uint32_t len) {
    uint8_t *p = data;
    if (len > KS_FLASH_SIZE || addr > KS_FLASH_SIZE - len) return KS_FLASH_RANGE;
    command_at(0x03, addr);
    while (len--) *p++ = xfer(0);
    deselect();
    return 0;
}

int ks_flash_erase(uint32_t addr) {
    addr &= ~(KS_FLASH_SECTOR - 1);
    if (!writable(addr, KS_FLASH_SECTOR)) return KS_FLASH_RANGE;
    command(0x06);
    command_at(0x20, addr);
    deselect();
    return wait_ready(500);
}

int ks_flash_write(uint32_t addr, const void *data, uint32_t len) {
    const uint8_t *p = data;
    if (!writable(addr, len)) return KS_FLASH_RANGE;
    while (len) {
        uint32_t n = PAGE - addr % PAGE;            /* a program must not cross a page */
        if (n > len) n = len;
        command(0x06);
        command_at(0x02, addr);
        for (uint32_t i = 0; i < n; i++) xfer(p[i]);
        deselect();
        int result = wait_ready(20);
        if (result) return result;
        addr += n;
        p += n;
        len -= n;
    }
    return 0;
}

/* ---- store: one record per 4 KiB sector of the SDK's area ---- */

typedef struct { uint32_t magic; uint16_t len, crc; } store_header;

static uint16_t crc16(const uint8_t *p, uint32_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

static uint16_t crc16_flash(uint32_t addr, uint32_t len) {
    uint16_t crc = 0xFFFF;
    command_at(0x03, addr);
    while (len--) {
        crc ^= (uint16_t)(xfer(0) << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    deselect();
    return crc;
}

int ks_store_save(int slot, const void *data, uint32_t len) {
    if (slot < 0 || slot >= KS_STORE_SLOTS || len > KS_STORE_MAX) return KS_FLASH_RANGE;
    uint32_t addr = KS_FLASH_USER + (uint32_t)slot * KS_FLASH_SECTOR;
    store_header header = { STORE_MAGIC, (uint16_t)len, crc16(data, len) };
    int result = ks_flash_erase(addr);
    if (!result) result = ks_flash_write(addr + sizeof header, data, len);
    if (!result) result = ks_flash_write(addr, &header, sizeof header);     /* last: marks it valid */
    if (!result && crc16_flash(addr + sizeof header, len) != header.crc) result = KS_FLASH_VERIFY;
    return result;
}

int ks_store_load(int slot, void *data, uint32_t max) {
    store_header header;
    if (slot < 0 || slot >= KS_STORE_SLOTS) return KS_FLASH_RANGE;
    uint32_t addr = KS_FLASH_USER + (uint32_t)slot * KS_FLASH_SECTOR;
    ks_flash_read(addr, &header, sizeof header);
    if (header.magic != STORE_MAGIC || header.len > KS_STORE_MAX || header.len > max) return KS_FLASH_EMPTY;
    if (crc16_flash(addr + sizeof header, header.len) != header.crc) return KS_FLASH_EMPTY;
    ks_flash_read(addr + sizeof header, data, header.len);
    return header.len;
}
