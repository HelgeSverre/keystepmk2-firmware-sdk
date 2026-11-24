/* Shared between the SDK's own source files. */
#ifndef KS_BOARD_H
#define KS_BOARD_H
#include "keystep.h"
#include "regs.h"

void pin_out(uint32_t port, int pin, int level);
void pin_in(uint32_t port, int pin, int pull);                 /* pull: 0 none, 1 up, 2 down */
void pin_af(uint32_t port, int pin, int af, int open_drain);
static inline void pin_set(uint32_t port, int pin, int level) { GPIO_BSRR(port) = 1u << (pin + (level ? 0 : 16)); }
static inline int pin_get(uint32_t port, int pin) { return (GPIO_IDR(port) >> pin) & 1; }

/* Interrupts off, and back as they were: an application may call in with them already off. */
static inline uint32_t irq_off(void) {
    uint32_t state;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(state) :: "memory");
    return state;
}
static inline void irq_restore(uint32_t state) {
    if (!(state & 1)) __asm__ volatile("cpsie i" ::: "memory");
}

void oled_init(void);
void input_init(void);
void input_tick_1ms(void);      /* encoder sampling */
void keys_scan_step(void);      /* one keybed phase, every 65 us */
void leds_init(void);
void midi_init(void);
void flash_init(void);
void cv_init(void);
void analog_tick_1ms(void);    /* pedal and aftertouch conversions */
void sync_scan(void);           /* the clock input, every 65 us */
void usb_init(void);
void usb_tick_1ms(void);        /* the delayed jump to update mode */
void sysex_rx_byte(uint8_t byte); /* every byte from the USB host, in the interrupt */
void midi_service(void);        /* move one byte each way if the UART is ready */

#endif
