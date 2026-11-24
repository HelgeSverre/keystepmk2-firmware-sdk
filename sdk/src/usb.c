/* USB: the device controller at 0x40005C00, as a USB MIDI device with the stock firmware's
 * identity and descriptors (docs/BOOTLOADER.md), so a host sees a KeyStep mk2.
 *
 * Interface 0 is the stock "Firmware Updater" stub. Class request 0xF2 on it is the host asking
 * for update mode: like the stock firmware, this waits 500 ms and calls ks_enter_update_mode().
 * Requests 0xF0, 0xF1 and 0xF3-0xF5 are acknowledged without action, also like the stock.
 *
 * Clock: HSI48 without trimming, and PWR_CR2.USV set, as the stock firmware does.
 * Packet memory: 8 bytes of buffer table per endpoint, then four 64-byte buffers. */
#include "board.h"

#define USB_BASE 0x40005C00u
#define CHEP(n) REG(USB_BASE + 4 * (n))
#define USB_CNTR REG(USB_BASE + 0x40)
#define USB_ISTR REG(USB_BASE + 0x44)
#define USB_DADDR REG(USB_BASE + 0x4C)
#define USB_BCDR REG(USB_BASE + 0x58)
#define PMA 0x40009800u
#define BD_TX(n) REG(PMA + 8 * (n))
#define BD_RX(n) REG(PMA + 8 * (n) + 4)
#define RX_64 0x84000000u                   /* buffer size field: 64 bytes */

enum { EP0_TX = 0x40, EP0_RX = 0x80, MIDI_TX = 0xC0, MIDI_RX = 0x100 };
enum { STALL = 1, NAK = 2, VALID = 3 };
enum { MIDI_IN = 1, MIDI_OUT = 2 };         /* endpoint numbers: 0x81 to the host, 0x02 from it */

/* Endpoint registers: address and type are plain bits, the state bits flip when written as 1,
 * and the two transfer flags clear when written as 0. */
#define KEEP 0x070Fu
static void set_tx(int n, uint32_t state) {
    uint32_t r = CHEP(n);
    CHEP(n) = (r & KEEP) | 0x8080 | ((r ^ (state << 4)) & 0x30);
}
static void set_rx(int n, uint32_t state) {
    uint32_t r = CHEP(n);
    CHEP(n) = (r & KEEP) | 0x8080 | ((r ^ (state << 12)) & 0x3000);
}
static void clear_rx_flag(int n) { CHEP(n) = (CHEP(n) & KEEP) | 0x0080; }
static void clear_tx_flag(int n) { CHEP(n) = (CHEP(n) & KEEP) | 0x8000; }

static void pma_write(uint32_t at, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t word = 0;
        for (uint32_t k = 0; k < 4 && i + k < len; k++) word |= (uint32_t)data[i + k] << (8 * k);
        REG(PMA + at + i) = word;
    }
}

static void pma_read(uint32_t at, uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t word = REG(PMA + at + i);
        for (uint32_t k = 0; k < 4 && i + k < len; k++) data[i + k] = (uint8_t)(word >> (8 * k));
    }
}

/* ---- descriptors: the stock firmware's, byte for byte, except the release number ---- */

__attribute__((weak)) const char ks_usb_product[] = "KeyStep mk2";
__attribute__((weak)) const char ks_usb_serial[17] = "0123456789ABCDEF";

static const uint8_t device_descriptor[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x75, 0x1C, 0x28, 0x02, 0x09, 0x10, 1, 2, 3, 1,
};
static const uint8_t config_descriptor[] = {
    9, 2, 0x77, 0, 3, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 0xFE, 1, 2, 6,                       /* interface 0: firmware updater stub */
    9, 0x21, 0x0B, 0xFF, 0x00, 0x40, 0x00, 0x10, 0x01,
    9, 4, 1, 0, 0, 1, 1, 0, 0,                          /* interface 1: audio control */
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 2,
    9, 4, 2, 0, 2, 1, 3, 0, 0,                          /* interface 2: MIDI streaming */
    7, 0x24, 1, 0x00, 0x01, 0x41, 0,
    6, 0x24, 2, 1, 1, 7,
    6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 8,
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 0x02, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 3,
};

/* ---- endpoint 0 ---- */

static const uint8_t *tx_data;
static uint32_t tx_left;
static int tx_more;                         /* a data stage is in progress */
static uint8_t new_address;
static uint8_t configuration;
static uint8_t scratch[66];
static volatile uint32_t update_at;         /* ks_millis() at which to enter update mode, or 0 */

static void ep0_tx_packet(void) {
    uint32_t n = tx_left > 64 ? 64 : tx_left;
    pma_write(EP0_TX, tx_data, n);
    BD_TX(0) = EP0_TX | n << 16;
    tx_data += n;
    tx_left -= n;
    tx_more = n == 64;                      /* a short packet ends the data stage */
    set_tx(0, VALID);
}

static void ep0_send(const uint8_t *data, uint32_t len, uint32_t wanted) {
    tx_data = data;
    tx_left = len < wanted ? len : wanted;
    ep0_tx_packet();
}

static void ep0_ack(void) { ep0_send(0, 0, 0); }

static void ep0_stall(void) {
    set_tx(0, STALL);
    set_rx(0, STALL);
}

static void send_string(const char *text, uint32_t wanted) {
    uint32_t n = 0;
    while (text[n] && n < 32) n++;
    scratch[0] = (uint8_t)(2 + 2 * n);
    scratch[1] = 3;
    for (uint32_t i = 0; i < n; i++) {
        scratch[2 + 2 * i] = (uint8_t)text[i];
        scratch[3 + 2 * i] = 0;
    }
    ep0_send(scratch, 2 + 2 * n, wanted);
}

static void open_midi(void) {
    BD_TX(MIDI_IN) = MIDI_TX;
    CHEP(MIDI_IN) = MIDI_IN;                /* bulk */
    set_tx(MIDI_IN, NAK);
    BD_RX(MIDI_OUT) = MIDI_RX | RX_64;
    CHEP(MIDI_OUT) = MIDI_OUT;
    set_rx(MIDI_OUT, VALID);
}

static void midi_reset(void);

static void setup(const uint8_t *s) {
    uint8_t type = s[0], request = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), length = (uint16_t)(s[6] | s[7] << 8);
    if ((type & 0x60) == 0x20) {                        /* class */
        if (request == 0xF2) update_at = ks_millis() + 500;
        if (request >= 0xF0 && request <= 0xF5 && !length) ep0_ack();
        else ep0_stall();
        return;
    }
    if (type & 0x60) {
        ep0_stall();
        return;
    }
    switch (request) {
    case 0:                                             /* GET_STATUS */
        scratch[0] = scratch[1] = 0;
        ep0_send(scratch, 2, length);
        break;
    case 1: case 3: case 11:                            /* CLEAR_FEATURE, SET_FEATURE, SET_INTERFACE */
        ep0_ack();
        break;
    case 5:                                             /* SET_ADDRESS: takes effect after the status stage */
        new_address = (uint8_t)(value & 0x7F);
        ep0_ack();
        break;
    case 6:                                             /* GET_DESCRIPTOR */
        switch (value >> 8) {
        case 1: ep0_send(device_descriptor, sizeof device_descriptor, length); break;
        case 2: ep0_send(config_descriptor, sizeof config_descriptor, length); break;
        case 0x21: ep0_send(config_descriptor + 18, 9, length); break;
        case 3:
            switch (value & 0xFF) {
            case 0: scratch[0] = 4; scratch[1] = 3; scratch[2] = 0x09; scratch[3] = 0x04;
                ep0_send(scratch, 4, length); break;
            case 1: send_string("Arturia", length); break;
            case 2: send_string(ks_usb_product, length); break;
            case 3: send_string(ks_usb_serial, length); break;
            case 6: send_string("Firmware Updater", length); break;
            default: ep0_stall();
            }
            break;
        default: ep0_stall();
        }
        break;
    case 8:                                             /* GET_CONFIGURATION */
        ep0_send(&configuration, 1, length);
        break;
    case 9:                                             /* SET_CONFIGURATION */
        configuration = (uint8_t)value;
        midi_reset();
        if (configuration) open_midi();
        ep0_ack();
        break;
    case 10:                                            /* GET_INTERFACE */
        scratch[0] = 0;
        ep0_send(scratch, 1, length);
        break;
    default:
        ep0_stall();
    }
}

static void bus_reset(void) {
    configuration = 0;
    new_address = 0;
    tx_more = 0;
    midi_reset();
    BD_TX(0) = EP0_TX;
    BD_RX(0) = EP0_RX | RX_64;
    CHEP(0) = 0x0200;                                   /* control, address 0 */
    set_tx(0, NAK);
    set_rx(0, VALID);
    USB_DADDR = 0x80;                                   /* enabled, address 0 */
}

static void ep0_event(uint32_t reg) {
    if (reg & 0x8000) {                                 /* received */
        if (reg & 0x0800) {
            uint8_t packet[8];
            pma_read(EP0_RX, packet, 8);
            clear_rx_flag(0);
            setup(packet);
            if (((CHEP(0) >> 12) & 3) != STALL) set_rx(0, VALID);
        } else {
            clear_rx_flag(0);                           /* the status stage of a read */
            set_rx(0, VALID);
        }
    }
    if (reg & 0x0080) {                                 /* sent */
        clear_tx_flag(0);
        if (tx_more) {
            ep0_tx_packet();
        } else if (new_address) {
            USB_DADDR = 0x80u | new_address;
            new_address = 0;
        }
    }
}

/* ---- MIDI: 4-byte packets on the bulk endpoints ---- */

static uint8_t txq[64][4];
static volatile uint8_t txq_head, txq_count, tx_busy;
static uint8_t rxq[256];
static volatile uint8_t rx_head, rx_tail, rx_held;
static uint8_t msg[3], msg_have, msg_need, in_sysex;

static void midi_reset(void) {
    txq_head = txq_count = tx_busy = 0;
    rx_head = rx_tail = rx_held = 0;
    msg_have = in_sysex = 0;
}

static void tx_kick(void) {
    uint32_t n = txq_count > 16 ? 16 : txq_count;
    for (uint32_t i = 0; i < n; i++) pma_write(MIDI_TX + 4 * i, txq[(txq_head + i) & 63], 4);
    txq_head = (uint8_t)((txq_head + n) & 63);
    txq_count = (uint8_t)(txq_count - n);
    BD_TX(MIDI_IN) = MIDI_TX | (4 * n) << 16;
    tx_busy = 1;
    set_tx(MIDI_IN, VALID);
}

/* Moves a received packet into the byte queue. Returns 0 if the queue has no room yet. */
static int rx_take(void) {
    static const uint8_t sizes[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
    uint8_t packet[64];
    uint32_t len = (BD_RX(MIDI_OUT) >> 16) & 0x3FF;
    if (len > 64) len = 64;
    if ((uint8_t)(rx_head - rx_tail) > 255 - 48) return 0;      /* 16 packets hold 48 bytes at most */
    pma_read(MIDI_RX, packet, len);
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        for (int k = 0; k < sizes[packet[i] & 15]; k++) {
            uint8_t byte = packet[i + 1 + k];
            rxq[rx_head++] = byte;
            sysex_rx_byte(byte);
            if (byte >= 0xF8) ks_midi_realtime(KS_MIDI_USB, byte);
        }
    return 1;
}

static void midi_event(int n, uint32_t reg) {
    if (n == MIDI_OUT && reg & 0x8000) {
        clear_rx_flag(n);
        if (rx_take()) set_rx(MIDI_OUT, VALID);
        else rx_held = 1;                               /* the host waits until the queue drains */
    }
    if (n == MIDI_IN && reg & 0x0080) {
        clear_tx_flag(n);
        tx_busy = 0;
        if (txq_count) tx_kick();
    }
}

static void emit(uint8_t code, uint8_t a, uint8_t b, uint8_t c) {
    if (!configuration) return;
    uint32_t state = irq_off();
    if (txq_count < 64) {
        uint8_t *p = txq[(txq_head + txq_count) & 63];
        p[0] = code; p[1] = a; p[2] = b; p[3] = c;
        txq_count++;
        if (!tx_busy) tx_kick();
    }
    irq_restore(state);
}

int ks_usb_ready(void) { return configuration != 0; }

int ks_usb_midi_tx_free(void) { return configuration ? (64 - txq_count) * 3 : 192; }

void ks_usb_midi_send_byte(uint8_t byte) {
    if (byte >= 0xF8) {
        emit(0x0F, byte, 0, 0);
    } else if (byte == 0xF0) {
        in_sysex = 1;
        msg[0] = byte;
        msg_have = 1;
    } else if (in_sysex && (byte == 0xF7 || byte < 0x80)) {
        msg[msg_have++] = byte;
        if (byte == 0xF7) {
            emit((uint8_t)(4 + msg_have), msg[0], msg_have > 1 ? msg[1] : 0, msg_have > 2 ? msg[2] : 0);
            in_sysex = 0;
            msg_have = 0;
            msg[0] = 0;
        } else if (msg_have == 3) {
            emit(4, msg[0], msg[1], msg[2]);
            msg_have = 0;
        }
    } else if (byte & 0x80) {
        in_sysex = 0;
        msg[0] = byte;
        msg_have = 1;
        msg_need = byte >= 0xF4 ? 1 : byte == 0xF2 ? 3 : byte >= 0xF0 || (byte & 0xE0) == 0xC0 ? 2 : 3;
        if (msg_need == 1) {
            emit(5, byte, 0, 0);
            msg_have = 0;
        }
    } else if (msg[0] & 0x80) {
        if (!msg_have) msg_have = 1;                    /* running status */
        msg[msg_have++] = byte;
        if (msg_have >= msg_need) {
            uint8_t code = msg[0] < 0xF0 ? msg[0] >> 4 : msg_need;      /* system common: 2 or 3 */
            emit(code, msg[0], msg[1], msg_need == 3 ? msg[2] : 0);
            msg_have = 0;
            if (msg[0] >= 0xF0) msg[0] = 0;
        }
    }
}

/* A whole SysEx message as packets of its own, so it can be sent from an interrupt without
 * disturbing a message the application is part-way through. */
void ks_usb_sysex_send(const uint8_t *message, uint32_t len) {
    if (!len) return;
    for (; len > 3; message += 3, len -= 3) emit(4, message[0], message[1], message[2]);
    emit((uint8_t)(4 + len), message[0], len > 1 ? message[1] : 0, len > 2 ? message[2] : 0);
}

void ks_usb_midi_send(uint8_t status, uint8_t d1, uint8_t d2) {
    ks_usb_midi_send_byte(status);
    ks_usb_midi_send_byte(d1);
    if ((status & 0xE0) != 0xC0) ks_usb_midi_send_byte(d2);
}

int ks_usb_midi_get(void) {
    if (rx_tail == rx_head) return -1;
    int byte = rxq[rx_tail++];
    if (rx_held) {
        uint32_t state = irq_off();
        if (rx_held && rx_take()) {
            rx_held = 0;
            set_rx(MIDI_OUT, VALID);
        }
        irq_restore(state);
    }
    return byte;
}

/* ---- interrupt and start ---- */

void USB_IRQHandler(void) {
    uint32_t status;
    while ((status = USB_ISTR) & 0x8000) {              /* a transfer finished */
        int n = status & 15;
        uint32_t reg = CHEP(n);
        if (n == 0) ep0_event(reg);
        else midi_event(n, reg);
    }
    if (status & 0x0400) {
        USB_ISTR = ~0x0400u;
        bus_reset();
    }
}

void usb_tick_1ms(void) {
    if (update_at && (int32_t)(ks_millis() - update_at) >= 0) ks_enter_update_mode();
}

void usb_init(void) {
    RCC_CR |= 1u << 22;                                 /* HSI48 */
    while (!(RCC_CR & (1u << 23))) { }
    RCC_APBENR1 |= (1u << 13) | (1u << 28);             /* USB, PWR */
    REG(0x40007004) |= 1u << 10;                        /* PWR_CR2.USV: the USB supply is valid */
    USB_CNTR = 1;                                       /* out of power-down, held in reset */
    for (volatile int i = 0; i < 100; i++) { }
    USB_CNTR = 0;
    USB_ISTR = 0;
    USB_CNTR = 0x8400;                                  /* interrupts: transfer done, bus reset */
    NVIC_ISER = 1u << 8;
    USB_BCDR = 1u << 15;                                /* pull D+ up: the host sees a device */
}
