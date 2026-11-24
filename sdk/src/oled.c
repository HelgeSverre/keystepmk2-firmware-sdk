/* The 128 x 32 OLED: SSD1306 command set on SPI1 (PB3 clock, PB5 data), D/C on PB6, reset on
 * PB7. The set-up bytes are the stock firmware's. */
#include "board.h"
#include "../third_party/font8x8_basic.h"

static uint8_t frame[KS_OLED_W * KS_OLED_H / 8];    /* four pages of 128 columns, bit 0 at the top */

static const uint8_t setup[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0x22, 0x00, 0x03,
    0xC8, 0xDA, 0x02, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
};

static void send(int data, const uint8_t *bytes, int n) {
    pin_set(GPIOB, 6, data);
    for (int i = 0; i < n; i++) {
        while (!(SPI_SR(SPI1) & 2)) { }             /* transmit buffer empty */
        SPI_DR8(SPI1) = bytes[i];
    }
    while (SPI_SR(SPI1) & 0x80) { }                 /* busy: D/C must hold until the last bit is out */
}

void oled_init(void) {
    pin_af(GPIOB, 3, 0, 0);
    pin_af(GPIOB, 5, 0, 0);
    pin_out(GPIOB, 6, 0);
    pin_out(GPIOB, 7, 1);
    SPI_CR2(SPI1) = 0x1700;                         /* 8-bit frames */
    SPI_CR1(SPI1) = 0x354;                          /* master, clock / 8 = 8 MHz, software select, on */
    pin_set(GPIOB, 7, 0);
    ks_delay_ms(1);
    pin_set(GPIOB, 7, 1);
    ks_delay_ms(1);
    send(0, setup, sizeof setup);
    ks_oled_clear();
    ks_oled_flush();
}

void ks_oled_flush(void) {
    static const uint8_t window[] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x03 };    /* all columns, all pages */
    send(0, window, sizeof window);
    send(1, frame, sizeof frame);
}

void ks_oled_clear(void) {
    for (unsigned i = 0; i < sizeof frame; i++) frame[i] = 0;
}

void ks_oled_pixel(int x, int y, int on) {
    if (x < 0 || x >= KS_OLED_W || y < 0 || y >= KS_OLED_H) return;
    if (on) frame[(y >> 3) * KS_OLED_W + x] |= (uint8_t)(1 << (y & 7));
    else frame[(y >> 3) * KS_OLED_W + x] &= (uint8_t)~(1 << (y & 7));
}

void ks_oled_rect(int x, int y, int w, int h, int on) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) ks_oled_pixel(i, j, on);
}

static void glyphs(int x, int y, const char *s, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const char *g = font8x8_basic[(unsigned char)*s & 0x7F];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (scale == 1) ks_oled_pixel(x + col, y + row, (g[row] >> col) & 1);
                else ks_oled_rect(x + col * scale, y + row * scale, scale, scale, (g[row] >> col) & 1);
    }
}

void ks_oled_text(int x, int y, const char *s) { glyphs(x, y, s, 1); }
void ks_oled_text_big(int x, int y, const char *s) { glyphs(x, y, s, 2); }

/* ---- proportional fonts: each glyph is stored as columns of bytes, eight pixels wide and the
 * font's height tall, most significant bit on the left ---- */

static uint8_t ink = 1;

void ks_oled_ink(int on) { ink = on != 0; }

static int glyph_index(const ks_font *font, char c) {
    int code = (unsigned char)c;
    return code < font->first || code > font->last ? 0 : code - font->first;
}

int ks_font_width(const ks_font *font, const char *s) {
    int width = 0;
    for (; *s; s++) width += font->widths[glyph_index(font, *s)];
    return width;
}

int ks_oled_text_font(const ks_font *font, int x, int y, const char *s) {
    for (; *s; s++) {
        int index = glyph_index(font, *s), width = font->widths[index];
        const uint8_t *bits = font->bits + font->offsets[index];
        for (int col = 0; col < width; col++)
            for (int row = 0; row < font->height; row++)
                if (bits[(col >> 3) * font->height + row] & (0x80 >> (col & 7))) ks_oled_pixel(x + col, y + row, ink);
        x += width;
    }
    return x;
}

char *ks_utoa(uint32_t value, char *out) {
    char tmp[11];
    int n = 0;
    char *p = out;
    do { tmp[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return out;
}
