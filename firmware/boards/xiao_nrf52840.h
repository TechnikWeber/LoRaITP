/*
 * Seeed XIAO nRF52840 + Wio-SX1262 for XIAO.
 *
 * No WiFi and no camera interface. This board cannot be the camera node
 * and cannot serve the access point; it is a low-power relay or a pocket
 * receiver, and images leave it over USB, BLE or NFC.
 *
 * Images live in the 2 MB onboard QSPI flash, not the 1 MB internal
 * flash the application runs from.
 *
 * The Wio-SX1262 for XIAO nRF52840 connects through ordinary pin
 * headers. TODO: take the four control pins from the vendor pinout.
 */
#ifndef LORAITP_BOARD_XIAO_NRF52840_H
#define LORAITP_BOARD_XIAO_NRF52840_H

/* XIAO edge pins -> nRF52840 port.pin, encoded as (port * 32 + pin) */
#define NRF_P(port, pin) ((uint8_t)((port) * 32 + (pin)))
#define XIAO_D0  NRF_P(0, 2)
#define XIAO_D1  NRF_P(0, 3)
#define XIAO_D2  NRF_P(0, 28)
#define XIAO_D3  NRF_P(0, 29)
#define XIAO_D4  NRF_P(0, 4)
#define XIAO_D5  NRF_P(0, 5)
#define XIAO_D6  NRF_P(1, 11)
#define XIAO_D7  NRF_P(1, 12)
#define XIAO_D8  NRF_P(1, 13)
#define XIAO_D9  NRF_P(1, 14)
#define XIAO_D10 NRF_P(1, 15)

static const loraitp_board_t LORAITP_BOARD = {
    .name = "xiao-nrf52840",

    .lora_sck = XIAO_D8, .lora_miso = XIAO_D9, .lora_mosi = XIAO_D10,

    /* TODO: from the Wio-SX1262 for XIAO nRF52840 pinout. Do not guess. */
    .lora_nss  = XIAO_D1,
    .lora_rst  = XIAO_D2,
    .lora_busy = XIAO_D3,
    .lora_dio1 = XIAO_D0,

    .lora_ant_sw = LORAITP_PIN_NONE,
    .lora_tcxo = true, .lora_tcxo_v = 1.8f,
    .max_tx_dbm = 22,

    .led = NRF_P(0, 26), .button = LORAITP_PIN_NONE,
    .vext_ctrl = LORAITP_PIN_NONE,
    .vbat_adc = LORAITP_PIN_NONE, .vbat_ctrl = LORAITP_PIN_NONE,
    .oled_sda = LORAITP_PIN_NONE, .oled_scl = LORAITP_PIN_NONE,
    .oled_rst = LORAITP_PIN_NONE,

    .has_camera = false,
    .has_wifi = false,
    .has_psram = false,
    .flash_kb = 1024,                /* plus 2 MB QSPI for images */
};

#endif
