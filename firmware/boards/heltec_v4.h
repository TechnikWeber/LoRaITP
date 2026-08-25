/*
 * Heltec WiFi LoRa 32 (V4) - ESP32-S3R2 + SX1262.
 *
 * Heltec states the V4 keeps the form factor and pin layout of the V3,
 * so the map below starts as a copy. TREAT IT AS UNVERIFIED until it has
 * been checked against the V4 pinout - "pin compatible" in a marketing
 * page is not the same as every GPIO landing in the same place.
 *
 * Two real differences from V3: 16 MB flash, 2 MB PSRAM, and a
 * high-power variant that reaches 28 dBm. That last one is above the
 * 27 dBm ERP ceiling of EU868_G3, so the governor will refuse it
 * (LORAITP_E_POWER) unless the configured power is capped - and antenna
 * gain counts towards ERP too.
 */
#ifndef LORAITP_BOARD_HELTEC_V4_H
#define LORAITP_BOARD_HELTEC_V4_H

static const loraitp_board_t LORAITP_BOARD = {
    .name = "heltec-v4",

    .lora_sck = 9, .lora_miso = 11, .lora_mosi = 10,
    .lora_nss = 8, .lora_rst = 12, .lora_busy = 13, .lora_dio1 = 14,
    .lora_ant_sw = LORAITP_PIN_NONE,
    .lora_tcxo = true, .lora_tcxo_v = 1.8f,
    .max_tx_dbm = 28,                /* high-power variant; 22 on the LP one */

    .led = 35, .button = 0,
    .vext_ctrl = 36,
    .vbat_adc = 1, .vbat_ctrl = 37,
    .oled_sda = 17, .oled_scl = 18, .oled_rst = 21,

    .has_camera = false,
    .has_wifi = true,
    .has_psram = true,               /* 2 MB */
    .flash_kb = 16 * 1024,
};

#endif
