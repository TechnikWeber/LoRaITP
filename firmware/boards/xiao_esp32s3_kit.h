/*
 * Seeed XIAO ESP32S3 + Wio-SX1262 *Kit* - the cheap base station.
 *
 * This is the variant that plugs into the B2B connector underneath the
 * XIAO. It cannot be combined with the Sense camera, which wants the same
 * connector and, worse, three of the same GPIOs - see
 * xiao_esp32s3_sense.h. At the receiving end that does not matter: the
 * base station has no camera anyway.
 *
 * So the cheapest working pair is a XIAO Sense with a plug-on radio at
 * the camera end, and a plain XIAO with the Kit radio at the other. No
 * Heltec needed. What you give up is the OLED, which on a mains-powered
 * box next to a laptop is worth very little - the web page shows more
 * than the display could.
 *
 * PIN MAP FROM THE COMMUNITY MAPPING, NOT FROM A BOARD IN HAND. The
 * control pins are what Meshtastic and the Seeed documentation use for
 * this kit; the SPI pins are the XIAO's own. If the radio enumerates and
 * then hangs, BUSY is the first thing to check.
 */
#ifndef LORAITP_BOARD_XIAO_ESP32S3_KIT_H
#define LORAITP_BOARD_XIAO_ESP32S3_KIT_H

static const loraitp_board_t LORAITP_BOARD = {
    .name = "xiao-esp32s3-kit",

    .lora_sck = 7, .lora_miso = 8, .lora_mosi = 9,

    .lora_nss  = 41,
    .lora_rst  = 42,
    .lora_busy = 40,
    .lora_dio1 = 39,

    /* Same story as the plug-on board: the antenna switch is driven by
     * the host, not steered from DIO2. Wrong polarity means transmitting
     * into a matched load and hearing nothing - the settings page can
     * flip it. */
    .lora_ant_sw = 38,
    .lora_tcxo = true, .lora_tcxo_v = 1.8f,
    .max_tx_dbm = 22,

    .led = 21, .button = LORAITP_PIN_NONE,
    .vext_ctrl = LORAITP_PIN_NONE,
    .vbat_adc = LORAITP_PIN_NONE, .vbat_ctrl = LORAITP_PIN_NONE,
    .oled_sda = LORAITP_PIN_NONE, .oled_scl = LORAITP_PIN_NONE,
    .oled_rst = LORAITP_PIN_NONE,

    .has_camera = false,             /* the radio is in the camera's slot */
    .has_wifi = true,
    .has_psram = true,
    .flash_kb = 8 * 1024,
};

#endif
