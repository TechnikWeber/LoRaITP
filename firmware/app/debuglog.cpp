/*
 * A log you can read from the web page instead of a serial cable.
 *
 * The reason this exists: the first question when a link does not work is
 * never "what was the result" - it is "was anything heard at all". That
 * is a frame-by-frame question, and until now the only way to see it was
 * a USB cable and a terminal. On a node that is up a mast, or simply in
 * another room, that is not a realistic answer.
 *
 * Lines go into a fixed ring in RAM and out to the serial port as well,
 * so nothing is lost if a cable does happen to be attached. Each line
 * carries a sequence number, so the browser polls for "everything after
 * N" and gets only what is new.
 */
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debuglog.h"

#define LOG_LINES 160
#define LOG_WIDTH 96

static char     g_line[LOG_LINES][LOG_WIDTH];
static uint32_t g_stamp[LOG_LINES];
static uint32_t g_next_seq = 1;      /* seq of the next line to be written */
static uint8_t  g_level = LORAITP_LOG_NORMAL;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void loraitp_log_set_level(uint8_t level) { g_level = level; }
uint8_t loraitp_log_level(void) { return g_level; }

void loraitp_log_write(uint8_t level, const char *fmt, ...)
{
    if (level > g_level)
        return;

    char buf[LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Serial.println(buf);

    /*
     * The trace callback runs inside the session on one core while the
     * web server reads the ring on the other, so the ring needs a lock.
     * A spinlock is right here: the critical section is a memcpy of at
     * most 96 bytes and must not block the radio.
     */
    portENTER_CRITICAL(&g_mux);
    uint32_t slot = g_next_seq % LOG_LINES;
    memcpy(g_line[slot], buf, sizeof(buf));
    g_line[slot][LOG_WIDTH - 1] = '\0';
    g_stamp[slot] = millis();
    g_next_seq++;
    portEXIT_CRITICAL(&g_mux);
}

uint32_t loraitp_log_next_seq(void) { return g_next_seq; }

uint32_t loraitp_log_read(uint32_t since, loraitp_log_line_cb cb, void *user)
{
    if (cb == NULL)
        return g_next_seq;

    uint32_t oldest = (g_next_seq > LOG_LINES) ? g_next_seq - LOG_LINES : 1;
    if (since < oldest)
        since = oldest;               /* the caller fell behind; skip ahead */

    for (uint32_t s = since; s < g_next_seq; s++) {
        char copy[LOG_WIDTH];
        uint32_t ms;
        portENTER_CRITICAL(&g_mux);
        /* Re-check: the writer may have lapped us while we were reading. */
        if (s < g_next_seq - LOG_LINES && g_next_seq > LOG_LINES) {
            portEXIT_CRITICAL(&g_mux);
            continue;
        }
        memcpy(copy, g_line[s % LOG_LINES], sizeof(copy));
        ms = g_stamp[s % LOG_LINES];
        portEXIT_CRITICAL(&g_mux);
        cb(user, s, ms, copy);
    }
    return g_next_seq;
}

/* --------------------------------------------------- the trace callback */

static const char *frame_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "META";
    case 0x02: return "DATA";
    case 0x03: return "EOB";
    case 0x04: return "STAT";
    case 0x05: return "FIN";
    case 0x06: return "FINACK";
    case 0x07: return "PROBE";
    case 0x08: return "PROBEACK";
    case 0x09: return "IDENT";
    case 0x0A: return "PARITY";
    case 0x0B: return "ABORT";
    default:   return "?";
    }
}

void loraitp_log_trace(void *user, const loraitp_trace_t *t)
{
    (void)user;
    switch (t->ev) {
    case LORAITP_EV_TX:
        /*
         * Bulk frames are logged only in verbose mode. At SF10 a transfer
         * is fifty of them, and a log that scrolls away the one line you
         * needed is worse than no log.
         */
        loraitp_log_write((t->frame_type == 0x02 || t->frame_type == 0x0A)
                              ? LORAITP_LOG_VERBOSE : LORAITP_LOG_NORMAL,
                          "TX  %-8s seq %-5u %3u B  %lu ms air",
                          frame_name(t->frame_type), t->seq, t->len,
                          (unsigned long)t->value);
        break;
    case LORAITP_EV_RX:
        loraitp_log_write((t->frame_type == 0x02 || t->frame_type == 0x0A)
                              ? LORAITP_LOG_VERBOSE : LORAITP_LOG_NORMAL,
                          "RX  %-8s seq %-5u %3u B  %d dBm  %.2f dB",
                          frame_name(t->frame_type), t->seq, t->len,
                          t->rssi_dbm, t->snr_qdb / 4.0);
        break;
    case LORAITP_EV_RX_TIMEOUT:
        loraitp_log_write(LORAITP_LOG_VERBOSE, "..  listened %lu ms, nothing",
                          (unsigned long)t->value);
        break;
    case LORAITP_EV_MAC_REJECT:
        /*
         * Worth seeing at normal level: on a shared band this is usually
         * somebody else's traffic rather than an attack, and knowing the
         * channel is busy explains a lot of otherwise puzzling losses.
         */
        loraitp_log_write(LORAITP_LOG_NORMAL,
                          "!!  frame rejected (not ours?) %3u B  %d dBm",
                          t->len, t->rssi_dbm);
        break;
    case LORAITP_EV_DUTY_WAIT:
        loraitp_log_write(LORAITP_LOG_NORMAL,
                          "..  duty cycle: waiting %lu ms",
                          (unsigned long)t->value);
        break;
    case LORAITP_EV_ROUND:
        loraitp_log_write(LORAITP_LOG_NORMAL,
                          "--  block %u: %lu chunk(s) still missing",
                          t->seq, (unsigned long)t->value);
        break;
    default:
        break;
    }
}
