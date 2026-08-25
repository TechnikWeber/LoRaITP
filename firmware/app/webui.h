/*
 * Access point and web interface. See webui.cpp.
 */
#ifndef LORAITP_WEBUI_H
#define LORAITP_WEBUI_H

#include <stdbool.h>
#include <stdint.h>

#include "appcfg.h"
#include "loraitp_store.h"

typedef struct {
    uint32_t airtime_used_ms;
    uint32_t airtime_budget_ms;
    uint32_t bytes_remaining;
    const char *last_result;
} loraitp_webui_status_t;

typedef void (*loraitp_webui_status_cb)(void *user,
                                        loraitp_webui_status_t *out);

void loraitp_webui_begin(loraitp_appcfg_t *cfg, loraitp_store_t *store,
                         loraitp_webui_status_cb cb, void *user);
void loraitp_webui_stop(void);
bool loraitp_webui_running(void);

/* Call often from the main loop. Handles requests and, when the auto-off
 * is enabled, drops the access point once it has been idle. */
void loraitp_webui_poll(void);

#endif
