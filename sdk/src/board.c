/* Clock, pins, time base and the two interrupts everything else hangs on. */
#include "board.h"

static volatile uint32_t ticks;
static volatile uint32_t micros_high;
static volatile uint16_t micros_last;

void pin_out(uint32_t port, int pin, int level) {
    pin_set(port, pin, level);
    GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (2 * pin))) | (1u << (2 * pin));
}

void pin_in(uint32_t port, int pin, int pull) {
    GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << (2 * pin))) | ((uint32_t)pull << (2 * pin));
    GPIO_MODER(port) &= ~(3u << (2 * pin));
}

void pin_af(uint32_t port, int pin, int af, int open_drain) {
    int shift = 4 * (pin & 7);
    GPIO_AFR(port, pin) = (GPIO_AFR(port, pin) & ~(0xFu << shift)) | ((uint32_t)af << shift);
    if (open_drain) {
        GPIO_OTYPER(port) |= 1u << pin;
        GPIO_PUPDR(port) = (GPIO_PUPDR(port) & ~(3u << (2 * pin))) | (1u << (2 * pin));
    }
    GPIO_OSPEEDR(port) |= 3u << (2 * pin);
    GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (2 * pin))) | (2u << (2 * pin));
}

/* 64 MHz from the 8 MHz crystal: PLL x16 / 2, as the stock firmware sets it. The PLL may only be
 * changed while it is off, so step down to the internal oscillator first in case the bootloader
 * left the PLL running. */
static void clock_64mhz(void) {
    RCC_CR |= 1u << 8;                                  /* HSI on */
    while (!(RCC_CR & (1u << 10))) { }
    RCC_CFGR &= ~7u;                                    /* run from HSI */
    while (RCC_CFGR & 0x38) { }
    RCC_CR &= ~(1u << 24);                              /* PLL off */
    while (RCC_CR & (1u << 25)) { }
    FLASH_ACR = (FLASH_ACR & ~7u) | 2;                  /* two wait states for 64 MHz */
    RCC_CR |= 1u << 16;                                 /* crystal on */
    while (!(RCC_CR & (1u << 17))) { }
    RCC_PLLCFGR = 0x30001003;                           /* source HSE, M = 1, N = 16, R = 2, R output on */
    RCC_CR |= 1u << 24;
    while (!(RCC_CR & (1u << 25))) { }
    RCC_CFGR = (RCC_CFGR & ~7u) | 2;                    /* run from the PLL */
    while ((RCC_CFGR & 0x38) != 0x10) { }
}

void SysTick_Handler(void) {
    ticks++;
    input_tick_1ms();
    analog_tick_1ms();
    usb_tick_1ms();
}

/* TIM17 counts 16 bits of microseconds; the upper half is carried here. Interrupts off: this
 * must run at least once per 65 ms wrap, which the 65 us interrupt sees to. */
static uint32_t micros_extend(void) {
    uint16_t now = (uint16_t)TIM_CNT(TIM17);
    if (now < micros_last) micros_high += 0x10000;
    micros_last = now;
    return micros_high | now;
}

uint32_t ks_micros(void) {
    uint32_t state = irq_off(), now = micros_extend();
    irq_restore(state);
    return now;
}

void TIM7_IRQHandler(void) {
    TIM_SR(TIM7) = 0;
    micros_extend();
    keys_scan_step();
    sync_scan();
    midi_service();
}

/* ---- a timer of your own: TIM6 counting microseconds ---- */

static void (*timer_callback)(void);

/* An update that ks_timer_start cleared from another interrupt can still be pending in the
 * NVIC; the status flag says whether a period really ended. */
void TIM6_IRQHandler(void) {
    uint32_t sr = TIM_SR(TIM6);
    TIM_SR(TIM6) = 0;
    if ((sr & 1) && timer_callback) timer_callback();
}

void ks_timer_period(uint32_t period_us) {
    if (period_us < 50) period_us = 50;
    if (period_us > 65536) period_us = 65536;
    TIM_ARR(TIM6) = period_us - 1;                      /* takes effect from the next period */
}

void ks_timer_start(uint32_t period_us, void (*callback)(void)) {
    RCC_APBENR1 |= 1u << 4;
    TIM_CR1(TIM6) = 0;
    timer_callback = callback;
    TIM_PSC(TIM6) = 63;                                 /* 64 MHz / 64 = 1 MHz */
    ks_timer_period(period_us);
    TIM_EGR(TIM6) = 1;
    TIM_SR(TIM6) = 0;
    TIM_DIER(TIM6) = 1;
    TIM_CR1(TIM6) = 0x81;                               /* run, with the period buffered */
    NVIC_ISER = 1u << 17;
}

void ks_timer_stop(void) {
    TIM_CR1(TIM6) = 0;
    NVIC_ICER = 1u << 17;
}

uint32_t ks_millis(void) { return ticks; }

void ks_delay_ms(uint32_t ms) {
    uint32_t start = ticks;
    while (ticks - start < ms) { }
}

uint16_t ks_micros16(void) { return (uint16_t)TIM_CNT(TIM17); }

void ks_reset(void) {
    SCB_AIRCR = 0x05FA0004;
    for (;;) { }
}

/* What the stock firmware does when the host asks for update mode: a marker and its USB serial
 * number string (17 bytes with the terminator) in the RAM block shared with the bootloader,
 * then a reset. The bootloader reads the marker first and stays in update mode, with that serial
 * (docs/BOOTLOADER.md). tools/ksflash.py uses it on a unit for every flash from a running firmware. */
void ks_enter_update_mode(void) {
    volatile uint32_t *shared = (volatile uint32_t *)0x20000000;
    volatile uint8_t *text = (volatile uint8_t *)0x20000004;
    shared[0] = 0xDEADBEEF;
    for (int i = 0; i < 17; i++) text[i] = i < 16 ? (uint8_t)ks_usb_serial[i] : 0;
    ks_reset();
}

void ks_init(void) {
    clock_64mhz();
    RCC_IOPENR |= 0x2F;                                 /* ports A, B, C, D, F */
    RCC_AHBENR |= 1;                                    /* DMA1 */
    RCC_APBENR2 |= 1;                                   /* SYSCFG */
    REG(0x40010000) |= 0x600;                           /* no USB-C pull-downs on PA8, PB15, PD0, PD2 */
    RCC_APBENR1 |= (1u << 5) | (1u << 14) | (1u << 15) | (1u << 21);    /* TIM7, SPI2, SPI3, I2C1 */
    RCC_APBENR2 |= (1u << 12) | (1u << 14) | (1u << 17) | (1u << 18);   /* SPI1, USART1, TIM16, TIM17 */

    TIM_PSC(TIM17) = 63;                                /* microsecond counter */
    TIM_ARR(TIM17) = 0xFFFF;
    TIM_EGR(TIM17) = 1;
    TIM_CR1(TIM17) = 1;

    SYST_RVR = 63999;                                   /* 1 ms */
    SYST_CVR = 0;
    SYST_CSR = 7;
    __asm__("cpsie i");

    oled_init();
    leds_init();
    midi_init();
    flash_init();
    usb_init();
    input_init();
    cv_init();

    TIM_PSC(TIM7) = 63;                                 /* 65 us: one keybed phase, one MIDI byte */
    TIM_ARR(TIM7) = 64;
    TIM_EGR(TIM7) = 1;
    TIM_SR(TIM7) = 0;
    TIM_DIER(TIM7) = 1;
    TIM_CR1(TIM7) = 1;
    NVIC_ISER = 1u << 18;
}
