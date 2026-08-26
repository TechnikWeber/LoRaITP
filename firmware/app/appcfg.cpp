#include <Preferences.h>
#include <string.h>

#include "appcfg.h"
#include "loraitp.h"

static const char *NS = "loraitp";

void loraitp_cfg_defaults(loraitp_appcfg_t *c, bool board_has_camera)
{
    memset(c, 0, sizeof(*c));

    /* A board with a camera is a sender; one without listens. That is
     * almost always right, and the web UI can override it. */
    c->role = board_has_camera ? LORAITP_ROLE_SENDER : LORAITP_ROLE_RECEIVER;

    c->ap_enabled = true;
    c->ap_auto_off = false;          /* see the note in appcfg.h */
    c->ap_timeout_s = 300;
    c->ap_password[0] = '\0';
    c->captive_portal = true;

    /*
     * EU868_G4_LP - 869.7-870.0 MHz, 5 mW, no duty-cycle limit
     * (BNetzA Vfg. 91/2025 row 56a). Twenty dB below what g3 allows, but
     * a hundred transfers cost no budget and need no licence, which is
     * what you want before the link works. Move to EU868_G3 from the web
     * UI once it does.
     */
    c->region = LORAITP_REG_EU868_G4_LP;
    c->frequency_hz = 869850000u;
    c->spreading_factor = 10;
    c->tx_power_dbm = 7;
    c->parity_percent = 0;
    c->broadcast = false;
    c->rf_sw_invert = false;      /* high to transmit, the convention */

    c->bandwidth_hz = 125000u;
    c->coding_rate = 1;           /* 4/5 */
    c->sync_word = 0x12;          /* private; 0x34 would be LoRaWAN's */
    c->log_level = 1;

    /*
     * One picture a day - the case the whole airtime budget is designed
     * around, and the one a board left alone should fall into. A first
     * transfer still runs a few seconds after boot, so a bench test is
     * not a day's wait; from then on the "send now" button is what
     * bring-up should use, because the interval that makes sense in the
     * field is exactly the one that makes testing unbearable.
     */
    c->interval_s = 24u * 3600u;
    c->deep_sleep = false;
    c->image_budget = 8000;
    c->keep_images = 32;
    c->callsign[0] = '\0';
}

void loraitp_cfg_load(loraitp_appcfg_t *c, bool board_has_camera)
{
    loraitp_cfg_defaults(c, board_has_camera);

    Preferences p;
    if (!p.begin(NS, true))
        return;                      /* nothing stored yet */

    c->role = p.getUChar("role", c->role);
    c->ap_enabled = p.getBool("ap_en", c->ap_enabled);
    c->ap_auto_off = p.getBool("ap_off", c->ap_auto_off);
    c->ap_timeout_s = p.getUShort("ap_to", c->ap_timeout_s);
    p.getString("ap_pw", c->ap_password, sizeof(c->ap_password));
    c->captive_portal = p.getBool("cportal", c->captive_portal);

    c->region = p.getUChar("region", c->region);
    c->frequency_hz = p.getULong("freq", c->frequency_hz);
    c->spreading_factor = p.getUChar("sf", c->spreading_factor);
    c->tx_power_dbm = (int8_t)p.getChar("pwr", c->tx_power_dbm);
    c->parity_percent = p.getUChar("parity", c->parity_percent);
    c->broadcast = p.getBool("bcast", c->broadcast);
    c->rf_sw_invert = p.getBool("rfinv", c->rf_sw_invert);
    c->bandwidth_hz = p.getULong("bw", c->bandwidth_hz);
    c->coding_rate = p.getUChar("cr", c->coding_rate);
    c->sync_word = p.getUChar("sync", c->sync_word);
    c->log_level = p.getUChar("log", c->log_level);

    c->interval_s = p.getULong("interval", c->interval_s);
    c->deep_sleep = p.getBool("dsleep", c->deep_sleep);
    c->image_budget = p.getUShort("budget", c->image_budget);
    c->keep_images = p.getUShort("keep", c->keep_images);
    p.getString("call", c->callsign, sizeof(c->callsign));
    p.end();
}

void loraitp_cfg_save(const loraitp_appcfg_t *c)
{
    Preferences p;
    if (!p.begin(NS, false))
        return;
    p.putUChar("role", c->role);
    p.putBool("ap_en", c->ap_enabled);
    p.putBool("ap_off", c->ap_auto_off);
    p.putUShort("ap_to", c->ap_timeout_s);
    p.putString("ap_pw", c->ap_password);
    p.putBool("cportal", c->captive_portal);
    p.putUChar("region", c->region);
    p.putULong("freq", c->frequency_hz);
    p.putUChar("sf", c->spreading_factor);
    p.putChar("pwr", c->tx_power_dbm);
    p.putUChar("parity", c->parity_percent);
    p.putBool("bcast", c->broadcast);
    p.putBool("rfinv", c->rf_sw_invert);
    p.putULong("bw", c->bandwidth_hz);
    p.putUChar("cr", c->coding_rate);
    p.putUChar("sync", c->sync_word);
    p.putUChar("log", c->log_level);
    p.putULong("interval", c->interval_s);
    p.putBool("dsleep", c->deep_sleep);
    p.putUShort("budget", c->image_budget);
    p.putUShort("keep", c->keep_images);
    p.putString("call", c->callsign);
    p.end();
}

const char *loraitp_cfg_region_name(uint8_t region)
{
    static const char *names[] = {
        "EU868_G3", "EU868_G1", "EU868_G2", "EU868_G4", "EU868_G4_LP",
        "EU433", "EU433_NARROW", "AMATEUR", "TEST_UNRESTRICTED"
    };
    if (region >= sizeof(names) / sizeof(names[0]))
        return "?";
    return names[region];
}
