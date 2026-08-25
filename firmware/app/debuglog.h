/*
 * A log readable from the web page, so a serial cable is optional.
 * See debuglog.cpp.
 */
#ifndef LORAITP_DEBUGLOG_H
#define LORAITP_DEBUGLOG_H

#include <stdint.h>

#include "loraitp.h"

#define LORAITP_LOG_OFF     0
#define LORAITP_LOG_NORMAL  1
#define LORAITP_LOG_VERBOSE 2   /* every DATA frame; noisy but definitive */

#ifdef __cplusplus
extern "C" {
#endif

void    loraitp_log_set_level(uint8_t level);
uint8_t loraitp_log_level(void);

void loraitp_log_write(uint8_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG(...)  loraitp_log_write(LORAITP_LOG_NORMAL, __VA_ARGS__)
#define LOGV(...) loraitp_log_write(LORAITP_LOG_VERBOSE, __VA_ARGS__)

uint32_t loraitp_log_next_seq(void);

typedef void (*loraitp_log_line_cb)(void *user, uint32_t seq, uint32_t ms,
                                    const char *line);

/* Replay everything after `since`. Returns the new next sequence number,
 * which the caller passes back next time. */
uint32_t loraitp_log_read(uint32_t since, loraitp_log_line_cb cb, void *user);

/* Hand this to loraitp_session_cfg_t.trace. */
void loraitp_log_trace(void *user, const loraitp_trace_t *t);

#ifdef __cplusplus
}
#endif
#endif
