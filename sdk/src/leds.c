/* 54 addressable LEDs on one data line (PA6, TIM16 channel 1 at 800 kHz, compare values by
 * DMA). Two chains share the line: 39 above the keys, selected by PB0 low, and 15 in the
 * buttons, selected by PA7 low. The frame layout and bit timings are the stock firmware's. */
#include "board.h"

#define CHAIN0 39
#define SLOTS 1113                      /* 1 + 175 slots of reset gap, 39 x 24 bits, 1 trailing zero */
#define BIT1 38                         /* 594 ns high out of 1.25 us */
#define BIT0 19

static uint32_t colour[KS_LED_COUNT];
static uint16_t slots[SLOTS];

void ks_led(int led, uint32_t rgb) {
    if (led >= 0 && led < KS_LED_COUNT) colour[led] = rgb;
}

static void send(int first, int count) {
    int n = 0;
    slots[n++] = BIT0;
    while (n < 176) slots[n++] = 0;
    for (int i = 0; i < count; i++) {
        uint32_t c = colour[first + i];
        uint32_t grb = ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | (c & 0xFF);
        for (int bit = 23; bit >= 0; bit--) slots[n++] = (grb >> bit) & 1 ? BIT1 : BIT0;
    }
    while (n < SLOTS) slots[n++] = 0;

    pin_set(first ? GPIOA : GPIOB, first ? 7 : 0, 0);
    DMA_CCR(4) = 0;
    DMA_IFCR = 0xFu << 12;
    DMA_CPAR(4) = TIM16 + 0x34;
    DMA_CMAR(4) = (uint32_t)slots;
    DMA_CNDTR(4) = SLOTS;
    TIM_CNT(TIM16) = 0;
    DMA_CCR(4) = 0x591;                 /* 16-bit, memory to timer, on */
    TIM_CR1(TIM16) = 1;
    uint32_t start = ks_millis();
    while (!(DMA_ISR & (1u << 13)) && ks_millis() - start < 5) { }
    TIM_CR1(TIM16) = 0;
    DMA_CCR(4) = 0;
    TIM_CCR1(TIM16) = 0;
    pin_set(first ? GPIOA : GPIOB, first ? 7 : 0, 1);
}

void ks_leds_show(void) {
    send(0, CHAIN0);
    send(CHAIN0, KS_LED_COUNT - CHAIN0);
}

void leds_init(void) {
    pin_out(GPIOB, 0, 1);
    pin_out(GPIOA, 7, 1);
    pin_af(GPIOA, 6, 5, 0);
    TIM_PSC(TIM16) = 0;
    TIM_ARR(TIM16) = 79;
    TIM_CCR1(TIM16) = 0;
    TIM_CCMR1(TIM16) = 0x68;            /* PWM mode 1, compare value preloaded */
    TIM_CCER(TIM16) = 1;
    TIM_BDTR(TIM16) = 0x8000;           /* outputs on */
    TIM_DIER(TIM16) = 1u << 9;          /* a DMA request per period */
    TIM_EGR(TIM16) = 1;
    DMAMUX_CCR(4) = 44;                 /* TIM16 channel 1 */
    ks_leds_show();
}
