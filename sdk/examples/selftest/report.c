/* The log of results and the SysEx messages that carry it (format: top of main.c). */
#include "selftest.h"

#define LOG_MAX 48

static result entries[LOG_MAX];
int results;
uint8_t tally[4];
uint32_t host_messages;
static uint8_t run, finished, dropped, replay_wanted;
static uint32_t run_id, start_ms, end_ms;

/* ---- one message at a time, built whole so that USB can keep it together ---- */

static uint8_t message[160];
static uint32_t length;
static uint8_t group[7];
static int grouped;
static uint16_t crc;

static void flush_group(void) {
    uint8_t top = 0;
    if (!grouped) return;
    for (int i = 0; i < grouped; i++) top |= (uint8_t)((group[i] >> 7) << i);
    message[length++] = top;
    for (int i = 0; i < grouped; i++) message[length++] = group[i] & 0x7F;
    grouped = 0;
}

static void begin(uint8_t type) {
    static const uint8_t head[4] = { 0xF0, 0x7D, 'K', 'S' };
    for (length = 0; length < 4; length++) message[length] = head[length];
    message[length++] = type;
    grouped = 0;
    crc = 0xFFFF;
}

static void put(const void *data, uint32_t len) {
    const uint8_t *p = data;
    while (len--) {
        crc ^= (uint16_t)(*p << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
        group[grouped++] = *p++;
        if (grouped == 7) flush_group();
    }
}

static void put8(uint8_t value) { put(&value, 1); }

/* Adds the checksum and sends: over USB once a host has configured the device, else on the DIN
 * output. A message that finds no room in time is dropped whole and counted, never cut: a host
 * that is not reading must not stall the test, and a replay can fetch it later. */
static void end(void) {
    uint16_t check = crc;
    put(&check, 2);
    flush_group();
    message[length++] = 0xF7;
    uint32_t start = ks_millis();
    if (ks_usb_ready()) {
        while (ks_usb_midi_tx_free() < (int)length + 3)
            if (ks_millis() - start > 100) goto drop;
        ks_usb_sysex_send(message, length);
    } else {
        while (ks_midi_tx_free() < (int)length)
            if (ks_millis() - start > 200) goto drop;
        for (uint32_t i = 0; i < length; i++) ks_midi_send_byte(message[i]);
    }
    return;
drop:
    if (dropped < 255) dropped++;
}

static void send_start(void) {
    begin(0x10);
    put8(1);                                        /* format */
    put8(VERSION_MAJOR);
    put8(VERSION_MINOR);
    put8(run);
    put(&run_id, 4);
    put(&start_ms, 4);
    put8(CHECKS);
    end();
}

static void send_result(int index) {
    const result *r = &entries[index];
    uint32_t len = 0;
    while (r->text[len]) len++;
    begin(0x11);
    put8((uint8_t)index);
    put8(r->id);
    put8(r->status);
    put8(r->count);
    put(&r->ms, 4);
    put(r->values, 4u * r->count);
    put(r->text, len);
    end();
}

static void send_end(void) {
    begin(0x12);
    put8((uint8_t)results);
    put(tally, 4);
    put8(dropped);
    put(&end_ms, 4);
    end();
}

/* ---- the log ---- */

const result *report_get(int index) { return &entries[index]; }

void report_start(void) {
    run++;
    run_id = ks_micros();                           /* tells this run from any other, after a restart too */
    start_ms = ks_millis();
    results = finished = dropped = 0;
    host_messages = 0;
    for (int i = 0; i < 4; i++) tally[i] = 0;
    send_start();
}

void report_result(int id, int status, const char *text, const int32_t *values, int count) {
    if (results >= LOG_MAX) return;
    result *r = &entries[results];
    r->id = (uint8_t)id;
    r->status = (uint8_t)status;
    r->count = (uint8_t)(count > VALUES_MAX ? VALUES_MAX : count);
    r->ms = ks_millis();
    for (int i = 0; i < r->count; i++) r->values[i] = values[i];
    int n = 0;
    for (; n < TEXT_MAX && text[n]; n++) r->text[n] = text[n] & 0x7F;
    r->text[n] = 0;
    tally[status & 3]++;
    send_result(results++);
}

void report_end(void) {
    finished = 1;
    end_ms = ks_millis();
    send_end();
}

void report_replay(void) {
    send_start();
    for (int i = 0; i < results; i++) send_result(i);
    if (finished) send_end();
}

/* ---- from the host: F0 7D 'K' 'S' 20 <n> F7 is a ping, answered with n and the time;
 * F0 7D 'K' 'S' 21 F7 asks for everything again ---- */

static void pong(uint8_t nonce) {
    uint32_t ms = ks_millis(), us = ks_micros();
    begin(0x13);
    put8(nonce);
    put(&ms, 4);
    put(&us, 4);
    end();
}

static void host_byte(uint8_t byte) {
    static const uint8_t head[4] = { 0xF0, 0x7D, 'K', 'S' };
    static uint8_t have, type, argument;
    if (byte >= 0xF8) return;
    if (byte == 0xF0) {
        have = 1;
    } else if (have >= 1 && have < 4) {
        have = byte == head[have] ? (uint8_t)(have + 1) : 0;
    } else if (have == 4 && byte < 0x80) {
        type = byte;
        argument = 0;
        have = 5;
    } else if (have == 5 && byte < 0x80) {
        argument = byte;
    } else {
        if (have == 5 && byte == 0xF7 && (type == 0x20 || type == 0x21)) {
            host_messages++;
            if (type == 0x20) pong(argument);
            else replay_wanted = 1;
        }
        have = 0;
    }
}

void report_poll(void) {
    int byte;
    while ((byte = ks_usb_midi_get()) >= 0) host_byte((uint8_t)byte);
    if (replay_wanted) {
        replay_wanted = 0;
        report_replay();
    }
}
