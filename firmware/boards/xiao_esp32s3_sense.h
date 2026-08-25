/*
 * Seeed XIAO ESP32S3 Sense + Wio-SX1262 for XIAO (NON-KIT version).
 *
 * The camera occupies the B2B connector, so the radio has to come in on
 * the edge pins - which is exactly what the non-Kit Wio-SX1262 does. The
 * Kit version uses the B2B connector and therefore cannot be combined
 * with the camera at all. See docs/hardware.md.
 *
 * The SPI pins below are the XIAO's fixed edge assignments (D8/D9/D10).
 * The four control pins depend on how the non-Kit radio board is wired
 * and MUST be taken from the vendor pinout rather than assumed - the Kit
 * and non-Kit boards use different pins for exactly these signals.
 */
#ifndef LORAITP_BOARD_XIAO_ESP32S3_SENSE_H
#define LORAITP_BOARD_XIAO_ESP32S3_SENSE_H

/* XIAO edge pins -> ESP32-S3 GPIO */
#define XIAO_D0  1
#define XIAO_D1  2
#define XIAO_D2  3
#define XIAO_D3  4
#define XIAO_D4  5
#define XIAO_D5  6
#define XIAO_D6  43
#define XIAO_D7  44
#define XIAO_D8  7
#define XIAO_D9  8
#define XIAO_D10 9

static const loraitp_board_t LORAITP_BOARD = {
    .name = "xiao-esp32s3-sense",

    .lora_sck = XIAO_D8, .lora_miso = XIAO_D9, .lora_mosi = XIAO_D10,

    /* TODO: from the Wio-SX1262 (non-Kit) pinout. Do not guess. */
    .lora_nss  = XIAO_D1,
    .lora_rst  = XIAO_D2,
    .lora_busy = XIAO_D3,
    .lora_dio1 = XIAO_D0,

    .lora_ant_sw = LORAITP_PIN_NONE,
    .lora_tcxo = true, .lora_tcxo_v = 1.8f,
    .max_tx_dbm = 22,

    .led = 21, .button = LORAITP_PIN_NONE,   /* no user button on the XIAO */
    .vext_ctrl = LORAITP_PIN_NONE,
    .vbat_adc = LORAITP_PIN_NONE, .vbat_ctrl = LORAITP_PIN_NONE,
    .oled_sda = LORAITP_PIN_NONE, .oled_scl = LORAITP_PIN_NONE,
    .oled_rst = LORAITP_PIN_NONE,

    .has_camera = true,              /* OV2640 on the B2B connector */
    .has_wifi = true,
    .has_psram = true,               /* 8 MB */
    .flash_kb = 8 * 1024,
};

#endif
