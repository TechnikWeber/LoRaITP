/*
 * Heltec WiFi LoRa 32 (V3) - ESP32-S3FN8 + SX1262.
 *
 * These assignments are the ones Heltec's own examples and Meshtastic's
 * variant use, and they have been stable across the V3 production run.
 * Check them against your board revision before the first power-up
 * anyway: the cost of being wrong is a radio that enumerates and then
 * hangs on BUSY.
 */
#ifndef LORAITP_BOARD_HELTEC_V3_H
#define LORAITP_BOARD_HELTEC_V3_H

static const loraitp_board_t LORAITP_BOARD = {
    .name = "heltec-v3",

    .lora_sck = 9, .lora_miso = 11, .lora_mosi = 10,
    .lora_nss = 8, .lora_rst = 12, .lora_busy = 13, .lora_dio1 = 14,
    .lora_ant_sw = LORAITP_PIN_NONE,
    .lora_tcxo = true, .lora_tcxo_v = 1.8f,
    .max_tx_dbm = 22,

    .led = 35, .button = 0,          /* PRG */
    .vext_ctrl = 36,
    .vbat_adc = 1, .vbat_ctrl = 37,
    .oled_sda = 17, .oled_scl = 18, .oled_rst = 21,

    .has_camera = false,
    .has_wifi = true,
    .has_psram = false,              /* VERIFY on your variant */
    .flash_kb = 8 * 1024,
};

#endif
