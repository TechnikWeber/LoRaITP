/*
 * Persisted settings.
 *
 * One firmware serves both ends of the link, and which end a board is
 * gets decided at run time rather than at build time. That means one
 * binary to build, one to flash, and a base station that can be turned
 * into a second node without a toolchain.
 */
#ifndef LORAITP_APPCFG_H
#define LORAITP_APPCFG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORAITP_ROLE_RECEIVER 0
#define LORAITP_ROLE_SENDER   1

typedef struct {
    uint8_t  role;

    /*
     * The access point is ON by default and stays on.
     *
     * It costs 100-150 mA continuously, which on a battery node dominates
     * everything else - the radio draws less than that even while
     * transmitting. So the power-saving behaviour is built and tested,
     * but it is opt-in: while the thing is on a bench being debugged, an
     * access point that disappears is a nuisance rather than a feature.
     */
    bool     ap_enabled;
    bool     ap_auto_off;        /* default false */
    uint16_t ap_timeout_s;       /* how long without traffic before it drops */
    char     ap_password[24];    /* empty = open network */
    bool     captive_portal;     /* pop the page up automatically */

    /* Radio. Defaults to EU868_G4_LP: 5 mW, no duty-cycle limit, no
     * licence needed - the right place to develop. */
    uint8_t  region;
    uint32_t frequency_hz;
    uint8_t  spreading_factor;
    int8_t   tx_power_dbm;
    uint8_t  parity_percent;
    bool     broadcast;

    /*
     * Antenna-switch polarity for modules that bring RF_SW out to a pin,
     * like the Seeed Wio-SX1262. False drives it high to transmit.
     *
     * This is on the settings page rather than buried in a header because
     * the wrong value is invisible: the radio configures cleanly and
     * simply never hears anything. Being able to flip it in the field
     * turns a lost afternoon into a tap.
     */
    bool     rf_sw_invert;

    /* Radio detail. Left out of the first version because the defaults
     * are right for almost everyone - but "almost" is why they are here. */
    uint32_t bandwidth_hz;       /* 125000, 250000, 62500, 20830 */
    uint8_t  coding_rate;        /* 1..4 for 4/5 .. 4/8 */
    uint8_t  sync_word;          /* 0x12 private, 0x34 is LoRaWAN's */

    /* 0 off, 1 normal, 2 every frame. */
    uint8_t  log_level;

    /* Capture and schedule. */
    uint32_t interval_s;         /* between transfers */
    uint16_t image_budget;       /* bytes; 0 = ask the duty-cycle governor */
    uint16_t keep_images;

    char     callsign[12];       /* required by the AMATEUR region */
} loraitp_appcfg_t;

void loraitp_cfg_defaults(loraitp_appcfg_t *c, bool board_has_camera);
void loraitp_cfg_load(loraitp_appcfg_t *c, bool board_has_camera);
void loraitp_cfg_save(const loraitp_appcfg_t *c);

const char *loraitp_cfg_region_name(uint8_t region);

#ifdef __cplusplus
}
#endif
#endif
