/* Arturia's SysEx requests on the USB MIDI port: F0 00 20 6B, then bytes packed into 7-bit
 * groups (least significant bit first), then F7. Unpacked, a request is a sequence number, a
 * two-byte operation and CBOR arguments; a reply is the sequence number with bit 7 set and one
 * CBOR value. The SDK answers the version request itself, so a host can tell what is running.
 * Every byte still reaches ks_usb_midi_get() as well. */
#include "board.h"

__attribute__((weak)) const char ks_version[] = "0.0.1.0";

/* Called from the USB interrupt for every other operation. Return the length of the CBOR reply
 * written to `reply`, or -1 to send nothing. */
__attribute__((weak)) int ks_sysex_request(uint16_t op, const uint8_t *args, int len, uint8_t *reply, int max) {
    (void)op; (void)args; (void)len; (void)reply; (void)max;
    return -1;
}

#define OP_VERSION 0xF9EA               /* EA F9 on the wire */

static uint8_t in[80], in_len, in_on;

static void answer(void) {
    uint8_t body[64], reply[2 + KS_SYSEX_REPLY_MAX], out[5 + (sizeof reply * 8 + 6) / 7];
    if (in_len < 3 + 4 || in[0] != 0x00 || in[1] != 0x20 || in[2] != 0x6B) return;
    int groups = in_len - 3, n = groups * 7 / 8;
    if (n < 3 || n > (int)sizeof body) return;
    for (int i = 0; i < n; i++) {
        uint32_t bit = (uint32_t)i * 8, g = bit / 7, s = bit % 7;
        body[i] = (uint8_t)(in[3 + g] >> s | (g + 1 < (uint32_t)groups ? in[4 + g] << (7 - s) : 0));
    }
    uint16_t op = (uint16_t)(body[1] | body[2] << 8);
    int len;
    if (op == OP_VERSION) {
        for (len = 0; ks_version[len] && len < 23; len++) reply[2 + len] = (uint8_t)ks_version[len];
        reply[1] = (uint8_t)(0x60 | len);               /* a CBOR text string */
        len++;
    } else {
        len = ks_sysex_request(op, body + 3, n - 3, reply + 1, KS_SYSEX_REPLY_MAX);
        if (len < 0 || len > KS_SYSEX_REPLY_MAX) return;
    }
    reply[0] = body[0] | 0x80;
    len++;
    int o = 0;
    out[o++] = 0xF0; out[o++] = 0x00; out[o++] = 0x20; out[o++] = 0x6B;
    for (int bit = 0; bit < len * 8; bit += 7) {
        int i = bit / 8, s = bit % 8;
        out[o++] = (uint8_t)(((reply[i] >> s) | (i + 1 < len ? reply[i + 1] << (8 - s) : 0)) & 0x7F);
    }
    out[o++] = 0xF7;
    ks_usb_sysex_send(out, (uint32_t)o);
}

void sysex_rx_byte(uint8_t byte) {
    if (byte >= 0xF8) return;                           /* real-time bytes may sit inside a message */
    if (byte == 0xF0) { in_on = 1; in_len = 0; return; }
    if (!in_on) return;
    if (byte == 0xF7) { in_on = 0; answer(); return; }
    if (byte & 0x80 || in_len == sizeof in) { in_on = 0; return; }
    in[in_len++] = byte;
}
