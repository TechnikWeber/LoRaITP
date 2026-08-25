/*
 * Access point, captive portal and web interface. See webui.cpp.
 */
#ifndef LORAITP_WEBUI_H
#define LORAITP_WEBUI_H

#include <stdbool.h>
#include <stdint.h>

#include "appcfg.h"
#include "loraitp_store.h"

typedef struct {
    uint32_t airtime_used_ms;
    uint32_t airtime_budget_ms;   /* 0 = the band has no duty-cycle limit */
    uint32_t bytes_remaining;
    uint8_t  duty_percent;

    /* What one frame currently costs - the number that turns an abstract
     * budget into "so that is why it takes an hour". */
    uint8_t  chunk_len;
    uint16_t frame_toa_ms;

    uint32_t next_run_ms;         /* until the next transfer */
    int16_t  last_rssi_dbm;
    int8_t   last_snr_qdb;
    float    link_margin_db;      /* over what the current SF needs */

    const char *last_result;
    const char *camera;
    const char *board;
    const char *version;
} loraitp_webui_status_t;

typedef void (*loraitp_webui_status_cb)(void *user,
                                        loraitp_webui_status_t *out);

/*
 * Run the next transfer immediately instead of waiting for the schedule.
 *
 * Standing next to two boards during a first bring-up and waiting five
 * minutes for the timer is not debugging, it is loitering - and the
 * interval that makes sense in the field is exactly the one that makes
 * testing unbearable.
 */
typedef void (*loraitp_webui_trigger_cb)(void *user);

void loraitp_webui_begin(loraitp_appcfg_t *cfg, loraitp_store_t *store,
                         loraitp_webui_status_cb cb,
                         loraitp_webui_trigger_cb trigger, void *user);
void loraitp_webui_stop(void);
bool loraitp_webui_running(void);

/* Call often from the main loop: serves requests, answers captive-portal
 * DNS, and drops the access point once idle if that is enabled. */
void loraitp_webui_poll(void);

#endif
