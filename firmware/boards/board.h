/*
 * Board pin maps.
 *
 * The only genuinely board-specific part of the firmware. Kept as data
 * in one place rather than scattered through #ifdefs, so adding a board
 * is a new header and nothing else.
 *
 * Select with -DLORAITP_BOARD_<NAME> at build time.
 */
#ifndef LORAITP_BOARD_H
#define LORAITP_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#define LORAITP_PIN_NONE 0xFF

typedef struct {
    const char *name;

    /* SX1262 over SPI. A wrong BUSY pin gives a radio that looks like it
     * works and then hangs, so these are worth checking twice. */
    uint8_t lora_sck, lora_miso, lora_mosi;
    uint8_t lora_nss, lora_rst, lora_busy, lora_dio1;
    uint8_t lora_ant_sw;          /* RF switch control, NONE if automatic */
    bool    lora_tcxo;            /* TCXO fitted; voltage set in the port */
    float   lora_tcxo_v;

    /* Highest ERP the hardware can produce. The governor refuses a
     * configuration above the region's limit, but the board can still
     * physically exceed it - the V4 high-power variant does on g3. */
    int8_t  max_tx_dbm;

    /* Peripherals. NONE where the board has none. */
    uint8_t led, button;
    uint8_t vext_ctrl;            /* peripheral power gate, active low */
    uint8_t vbat_adc, vbat_ctrl;
    uint8_t oled_sda, oled_scl, oled_rst;

    bool    has_camera;
    bool    has_wifi;
    bool    has_psram;
    uint32_t flash_kb;
} loraitp_board_t;

#if   defined(LORAITP_BOARD_HELTEC_V3)
#  include "heltec_v3.h"
#elif defined(LORAITP_BOARD_HELTEC_V4)
#  include "heltec_v4.h"
#elif defined(LORAITP_BOARD_XIAO_ESP32S3_SENSE)
#  include "xiao_esp32s3_sense.h"
#elif defined(LORAITP_BOARD_XIAO_NRF52840)
#  include "xiao_nrf52840.h"
#else
#  error "Define one of LORAITP_BOARD_HELTEC_V3, _HELTEC_V4, \
_XIAO_ESP32S3_SENSE, _XIAO_NRF52840"
#endif

#endif /* LORAITP_BOARD_H */
