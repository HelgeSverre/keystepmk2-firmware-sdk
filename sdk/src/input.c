/* Buttons, encoder, keybed and touch strips. Pin roles and timings follow the stock firmware. */
#include "board.h"

/* ---- buttons: a 16-bit shift register on SPI3 (PC10 clock, PC11 data), loaded while PB4 is
 * low and shifted while it is high. Tempo is a pin of its own, PA15, high when pressed. ---- */

static int spi3_byte(void) {
    SPI_DR8(SPI3) = 0;                              /* clock eight bits in */
    for (int i = 0; i < 10000; i++)
        if (SPI_SR(SPI3) & 1) return SPI_DR8(SPI3);
    return 0;
}

static uint32_t buttons_read(void) {
    pin_set(GPIOB, 4, 1);
    uint32_t low = (uint32_t)spi3_byte();
    uint32_t high = (uint32_t)spi3_byte();
    pin_set(GPIOB, 4, 0);
    return (low | high << 8) | (pin_get(GPIOA, 15) ? (uint32_t)KS_BTN_TEMPO : 0);
}

/* Read every 2 ms from the tick; a reading counts once it has been seen twice in a row. */
static volatile uint32_t buttons;
static uint8_t buttons_ready;

static void buttons_tick(void) {
    static uint32_t last;
    static uint8_t phase;
    if (!buttons_ready || (++phase & 1)) return;
    uint32_t now = buttons_read();
    if (now == last) buttons = now;
    last = now;
}

uint32_t ks_buttons(void) { return buttons; }

/* ---- encoder: phases on PC12 and PC13 while its select line PB8 is low ---- */

static volatile int encoder_steps;      /* quarter detents not yet reported */

void input_tick_1ms(void) {
    static uint8_t last = 3;
    static const int8_t step[16] = { 0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0 };
    uint8_t now = (uint8_t)((GPIO_IDR(GPIOC) >> 12) & 3);
    encoder_steps += step[last << 2 | now];
    last = now;
    buttons_tick();
}

int ks_encoder(void) {
    uint32_t state = irq_off();
    int detents = encoder_steps / 4;
    encoder_steps -= detents * 4;
    irq_restore(state);
    return detents;
}

/* ---- keybed: eight phases, one per call. PB10-PB12 select the phase; PD0-PD6 and PD8 return
 * eight keys, low = contact closed. An even phase reads the first contact of a group of keys
 * and the next odd phase the second; the time between the two gives the velocity. ---- */

enum { IDLE, FIRST, DOWN };
static uint8_t key_state[KS_KEYS];
static uint16_t key_time[KS_KEYS];
static volatile uint32_t keys_down;
static ks_key_event events[32];
static volatile uint8_t event_head, event_tail;

static void emit(int key, int velocity, int on) {
    uint8_t next = (uint8_t)((event_head + 1) & 31);
    if (next == event_tail) return;                 /* full: drop */
    events[event_head] = (ks_key_event){ (uint8_t)key, (uint8_t)velocity, (uint8_t)on };
    event_head = next;
}

void keys_scan_step(void) {
    static uint8_t phase, first;
    uint32_t pins = ~GPIO_IDR(GPIOD);
    uint8_t closed = (uint8_t)((pins & 0x7F) | ((pins >> 1) & 0x80));
    if (!(phase & 1)) {
        first = closed;
    } else {
        int base = 24 - 8 * (phase >> 1);           /* phases 0-1 read keys 24-31, 6-7 read keys 0-7 */
        for (int i = 0; i < 8; i++) {
            int key = base + i, f = (first >> (7 - i)) & 1, s = (closed >> (7 - i)) & 1;
            uint8_t state = key_state[key];
            if (state == IDLE && f) {
                key_time[key] = ks_micros16();
                key_state[key] = FIRST;
            } else if (state == FIRST && s) {
                int velocity = 136 - (uint16_t)(ks_micros16() - key_time[key]) / 167;   /* 6 ms gives 100 */
                emit(key, velocity < 1 ? 1 : velocity > 127 ? 127 : velocity, 1);
                keys_down |= 1u << key;
                key_state[key] = DOWN;
            } else if (state == FIRST && !f) {
                key_state[key] = IDLE;
            } else if (state == DOWN && !f) {
                emit(key, 0, 0);
                keys_down &= ~(1u << key);
                key_state[key] = IDLE;
            }
        }
    }
    phase = (uint8_t)((phase + 1) & 7);
    GPIO_BSRR(GPIOB) = ((uint32_t)phase << 10) | ((uint32_t)(~phase & 7) << 26);
}

int ks_key_get(ks_key_event *event) {
    if (event_tail == event_head) return 0;
    *event = events[event_tail];
    event_tail = (uint8_t)((event_tail + 1) & 31);
    return 1;
}

uint32_t ks_keys_down(void) { return keys_down; }

/* ---- touch strips: a controller at I2C address 0x15 (PA9 clock, PA10 data). It pulls PC6 low
 * when it has a report: four bytes, two per strip, the position in the low byte. ---- */

#define TOUCH 0x15
static int16_t strip[2] = { -1, -1 };

static int i2c_wait(uint32_t flag) {
    for (int i = 0; i < 200000; i++) {
        uint32_t isr = I2C_ISR(I2C1);
        if (isr & flag) return 1;
        if (isr & 0x10) break;                      /* no acknowledge */
    }
    I2C_ICR(I2C1) = 0x30;
    return 0;
}

static int i2c_write(const uint8_t *data, int n) {
    I2C_CR2(I2C1) = (TOUCH << 1) | ((uint32_t)n << 16) | (1u << 25) | (1u << 13);
    for (int i = 0; i < n; i++) {
        if (!i2c_wait(2)) return 0;
        I2C_TXDR(I2C1) = data[i];
    }
    int ok = i2c_wait(0x20);
    I2C_ICR(I2C1) = 0x30;
    return ok;
}

static int i2c_read(uint8_t *data, int n) {
    I2C_CR2(I2C1) = (TOUCH << 1) | (1u << 10) | ((uint32_t)n << 16) | (1u << 25) | (1u << 13);
    for (int i = 0; i < n; i++) {
        if (!i2c_wait(4)) return 0;
        data[i] = (uint8_t)I2C_RXDR(I2C1);
    }
    int ok = i2c_wait(0x20);
    I2C_ICR(I2C1) = 0x30;
    return ok;
}

int ks_touch(int index) {
    static uint32_t checked;
    uint8_t report[4];
    uint32_t now = ks_millis();
    if (now == checked) return strip[index & 1];    /* the controller reports far slower than this */
    checked = now;
    if (!pin_get(GPIOC, 6) && i2c_read(report, 4)) {
        for (int i = 0; i < 2; i++) {               /* about 160 at the bottom, 22 at the top, 0 for no finger */
            int raw = report[2 * i], position = (160 - raw) * 255 / 138;
            strip[i] = (int16_t)(!raw ? -1 : position < 0 ? 0 : position > 255 ? 255 : position);
        }
    }
    return strip[index & 1];
}

void input_init(void) {
    pin_out(GPIOB, 4, 0);                           /* buttons */
    pin_af(GPIOC, 10, 4, 0);
    pin_af(GPIOC, 11, 4, 0);
    SPI_CR2(SPI3) = 0x1700;
    SPI_CR1(SPI3) = 0x36C;                          /* master, clock / 64 = 1 MHz, software select, on */
    pin_in(GPIOA, 15, 2);                           /* Tempo */
    buttons = buttons_read();
    buttons_ready = 1;

    pin_out(GPIOB, 8, 0);                           /* encoder selected for good */
    pin_out(GPIOB, 9, 1);
    pin_out(GPIOD, 9, 1);
    pin_in(GPIOC, 12, 1);
    pin_in(GPIOC, 13, 1);

    pin_out(GPIOB, 10, 0);                          /* keybed */
    pin_out(GPIOB, 11, 0);
    pin_out(GPIOB, 12, 0);
    pin_out(GPIOB, 2, 0);
    for (int pin = 0; pin <= 8; pin++)
        if (pin != 7) pin_in(GPIOD, pin, 0);

    pin_in(GPIOC, 6, 0);                            /* touch controller */
    pin_out(GPIOC, 7, 0);
    pin_af(GPIOA, 9, 6, 1);
    pin_af(GPIOA, 10, 6, 1);
    I2C_TIMINGR(I2C1) = 0x10303AFF;                 /* about 100 kHz */
    I2C_CR1(I2C1) = 1;
    ks_delay_ms(50);
    pin_set(GPIOC, 7, 1);
    ks_delay_ms(50);
    static const uint8_t start[] = { 0x64, 0x28, 0x05 }, status = 0x04;
    uint8_t reply[4];
    i2c_write(start, sizeof start);                 /* the stock firmware's start-up exchange */
    ks_delay_ms(50);
    if (i2c_write(&status, 1)) i2c_read(reply, 4);
}
