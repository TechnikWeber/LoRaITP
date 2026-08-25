/*
 * LoRaITP port on RadioLib.
 *
 * One adapter for every board LoRaITP targets: RadioLib drives the
 * SX1262 on both ESP32-S3 and nRF52840, so four boards need four pin
 * maps rather than four drivers.
 *
 * This fills in the *radio* half of loraitp_port_t - transmit, receive,
 * clock, sleep, entropy. Storage (image_read / image_write) and the AES
 * block function are the application's to provide, because where images
 * live is a firmware decision and not a radio one. See
 * loraitp_radiolib_attach().
 */
#ifndef LORAITP_PORT_RADIOLIB_H
#define LORAITP_PORT_RADIOLIB_H

#include <stdbool.h>
#include <stdint.h>

#include "loraitp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Also defined by firmware/boards/board.h; same value. */
#ifndef LORAITP_PIN_NONE
#define LORAITP_PIN_NONE 0xFF
#endif

typedef struct {
    /* Pins, normally taken straight from firmware/boards/<board>.h */
    uint8_t pin_nss, pin_dio1, pin_rst, pin_busy;
    uint8_t pin_sck, pin_miso, pin_mosi;

    /*
     * RF switch, in three flavours. Getting this wrong gives a radio that
     * transmits into a matched load and hears nothing - which looks
     * exactly like being out of range.
     *
     *   dio2_as_rf_switch  the module steers it internally from DIO2.
     *                      True for most SX1262 modules.
     *   pin_rf_sw          one host-driven line: high in TX, low in RX.
     *                      This is what the Seeed Wio-SX1262 needs; it
     *                      silkscreens the pin RF_SW.
     *   pin_rx_en/pin_tx_en  two separate lines.
     *
     * Set exactly one of the three.
     */
    bool    dio2_as_rf_switch;
    uint8_t pin_rf_sw;               /* LORAITP_PIN_NONE if unused */
    uint8_t pin_rx_en, pin_tx_en;    /* LORAITP_PIN_NONE if unused */

    bool    tcxo;
    float   tcxo_voltage;

    /* Radio parameters. Changed mid-session when the probe picks an SF. */
    float   frequency_mhz;
    float   bandwidth_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;             /* 5..8 for 4/5..4/8 */
    uint8_t sync_word;
    int8_t  tx_power_dbm;
    uint16_t preamble_symbols;
    float   current_limit_ma;

    /*
     * Below this, the duty-cycle wait is a plain busy delay. Above it,
     * the port may put the part into light sleep - which is what makes
     * the energy budget in SPEC.md 8.4 true rather than aspirational,
     * because the governor asks for waits of minutes.
     */
    uint32_t light_sleep_threshold_ms;
    bool    allow_light_sleep;
} loraitp_radiolib_cfg_t;

/* Sensible defaults; caller then fills in the pins and the frequency. */
void loraitp_radiolib_defaults(loraitp_radiolib_cfg_t *cfg);

/*
 * Bring the radio up and fill in the radio half of `port`.
 *
 * Leaves port->image_read, port->image_write and
 * port->aes128_encrypt_block untouched, so the caller can set them
 * before or after. A NULL aes128_encrypt_block simply disables frame
 * authentication.
 *
 * Returns LORAITP_OK, or LORAITP_E_RADIO with the RadioLib status code
 * available from loraitp_radiolib_last_error().
 */
int  loraitp_radiolib_attach(loraitp_port_t *port,
                             const loraitp_radiolib_cfg_t *cfg);

int  loraitp_radiolib_last_error(void);

/* Cumulative measured time on air, for cross-checking the governor. */
uint32_t loraitp_radiolib_airtime_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_PORT_RADIOLIB_H */
