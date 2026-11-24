/* MIDI on the DIN sockets: USART1 at 31250 baud, PC4 out, PC5 in. Bytes move through two
 * rings, serviced from the 65 us timer interrupt, so sending never blocks. */
#include "board.h"

static uint8_t tx[256], rx[64];
static volatile uint8_t tx_head, tx_tail, rx_head, rx_tail;

__attribute__((weak)) void ks_midi_realtime(int port, uint8_t byte) { (void)port; (void)byte; }

void midi_service(void) {
    uint32_t isr = USART_ISR(USART1);
    if (isr & 0x20) {                                   /* a byte arrived */
        uint8_t next = (uint8_t)((rx_head + 1) & 63);
        uint8_t byte = (uint8_t)USART_RDR(USART1);
        if (next != rx_tail) { rx[rx_head] = byte; rx_head = next; }
        if (byte >= 0xF8) ks_midi_realtime(KS_MIDI_DIN, byte);
    }
    if (isr & 0x08) USART_ICR(USART1) = 0x08;           /* overrun */
    if ((isr & 0x80) && tx_tail != tx_head) USART_TDR(USART1) = tx[tx_tail++];
}

void ks_midi_send_byte(uint8_t byte) {
    uint8_t next = (uint8_t)(tx_head + 1);
    if (next == tx_tail) return;                        /* full: drop */
    tx[tx_head] = byte;
    tx_head = next;
}

void ks_midi_send(uint8_t status, uint8_t d1, uint8_t d2) {
    ks_midi_send_byte(status);
    ks_midi_send_byte(d1);
    ks_midi_send_byte(d2);
}

int ks_midi_tx_free(void) { return 255 - (uint8_t)(tx_head - tx_tail); }

int ks_midi_get(void) {
    if (rx_tail == rx_head) return -1;
    int byte = rx[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) & 63);
    return byte;
}

void midi_init(void) {
    pin_af(GPIOC, 4, 1, 0);
    pin_af(GPIOC, 5, 1, 0);
    USART_BRR(USART1) = 0x800;                          /* 64 MHz / 31250 */
    USART_CR1(USART1) = 0x0D;                           /* on, receive, transmit */
}
