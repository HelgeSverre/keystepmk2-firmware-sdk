/* Pitch and mod CV, gate, sync and the two analog inputs. Pin roles and register values follow
 * the stock firmware (docs/HARDWARE.md). The analog stages behind the pins are not mapped, so
 * every value here is the raw number the processor puts out or reads, not a voltage. */
#include "board.h"

/* ---- pitch: two 8-bit PWM outputs at 62.7 kHz on PC1 (fine) and PC2 (coarse), summed on the
 * board into one 16-bit level ---- */

void ks_cv_pitch(uint16_t value) {
    TIM_CCR1(TIM15) = value & 0xFF;
    TIM_CCR2(TIM15) = value >> 8;
}

/* ---- mod 1 and mod 2: the 12-bit DAC on PA4 and PA5 ---- */

void ks_cv_mod(int output, uint16_t value) {
    if (value > 4095) value = 4095;
    if (output) DAC_DHR12R2 = value;
    else DAC_DHR12R1 = value;
}

/* ---- gate: PC9. PB1 and PC8 switch the output stage between the three gate formats; with
 * format 0 the pin is low while the gate is on, with the others high. ---- */

static uint8_t gate_format = 1, gate_on;

static void gate_apply(void) { pin_set(GPIOC, 9, gate_format ? gate_on : !gate_on); }

void ks_gate(int on) {
    gate_on = on != 0;
    gate_apply();
}

void ks_gate_format(int format) {
    if (format < 0 || format > 2) return;
    gate_format = (uint8_t)format;
    pin_set(GPIOB, 1, format == 2);
    pin_set(GPIOC, 8, format == 1);
    gate_apply();
}

/* ---- sync: clock on PC3 (out) and PA2 (in), run on PC0 (out) and PA3 (in), all active low at
 * the pin. The clock input is sampled every 65 us from the keybed timer. ---- */

static volatile uint16_t sync_edges;
static uint8_t sync_last;

void ks_sync_out(int line, int on) { pin_set(GPIOC, line == KS_SYNC_RUN ? 0 : 3, !on); }

int ks_sync_in(int line) { return !pin_get(GPIOA, line == KS_SYNC_RUN ? 3 : 2); }

__attribute__((weak)) void ks_sync_clock_in(void) { }

void sync_scan(void) {
    uint8_t now = (uint8_t)!pin_get(GPIOA, 2);
    if (now && !sync_last) {
        sync_edges++;
        ks_sync_clock_in();
    }
    sync_last = now;
}

int ks_sync_pulses(void) {
    uint32_t state = irq_off();
    int n = sync_edges;
    sync_edges = 0;
    irq_restore(state);
    return n;
}

/* ---- analog inputs: the aftertouch strip on ADC channel 0 (PA0) and the pedal socket on
 * channel 1 (PA1), as the stock firmware's calibration code and a real unit show; converted in turn from the 1 ms tick ---- */

static volatile uint16_t analog[2];
static volatile uint32_t conversions;
static uint8_t channel, converting;

void analog_tick_1ms(void) {
    uint32_t isr = ADC_ISR;
    if (converting) {
        if (!(isr & 4)) return;                         /* not finished */
        analog[channel] = (uint16_t)ADC_DR;
        conversions++;
        converting = 0;
        channel ^= 1;
        ADC_ISR = 1u << 13;
        ADC_CHSELR = 1u << channel;
    } else if (isr & (1u << 13)) {                      /* the channel change has taken effect */
        ADC_CR |= 4;                                    /* start */
        converting = 1;
    }
}

int ks_aftertouch(void) { return analog[0]; }
int ks_pedal(void) { return analog[1]; }
uint32_t ks_analog_count(void) { return conversions; }

void cv_init(void) {
    RCC_APBENR1 |= 1u << 29;                            /* DAC1 */
    RCC_APBENR2 |= (1u << 16) | (1u << 20);             /* TIM15, ADC */

    pin_af(GPIOC, 1, 2, 0);                             /* pitch */
    pin_af(GPIOC, 2, 2, 0);
    TIM_PSC(TIM15) = 3;
    TIM_ARR(TIM15) = 254;
    TIM_CCMR1(TIM15) = 0x6868;                          /* PWM mode 1 with preload, both channels */
    TIM_CCER(TIM15) = 0x11;
    TIM_BDTR(TIM15) = 1u << 15;                         /* outputs on */
    TIM_EGR(TIM15) = 1;
    TIM_CR1(TIM15) = 0x81;

    DAC_MCR = 0x00020002;                               /* both channels: to the pin, buffer off */
    DAC_CR = 0x00010001;

    pin_out(GPIOC, 9, 0);                               /* gate off in format 1, the power-on format */
    pin_out(GPIOB, 1, 0);
    pin_out(GPIOC, 8, 1);
    pin_out(GPIOC, 0, 1);                               /* sync outputs idle */
    pin_out(GPIOC, 3, 1);
    pin_in(GPIOA, 2, 0);
    pin_in(GPIOA, 3, 0);

    ADC_CR = 1u << 28;                                  /* voltage regulator on */
    ks_delay_ms(1);
    ADC_CR |= 1u << 31;                                 /* calibrate */
    for (int i = 0; i < 100000 && (ADC_CR & (1u << 31)); i++) { }
    ADC_CFGR2 = 1u << 30;                               /* clock: PCLK / 2 */
    ADC_SMPR = 7;                                       /* 160.5 cycles, as the stock firmware */
    ADC_ISR = 1;
    ADC_CR |= 1;                                        /* enable */
    for (int i = 0; i < 100000 && !(ADC_ISR & 1); i++) { }
    ADC_CHSELR = 1;
}
