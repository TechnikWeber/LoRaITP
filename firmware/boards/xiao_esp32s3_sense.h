/*
 * Seeed XIAO ESP32S3 Sense + Wio-SX1262, wired to the edge pins.
 *
 * The good news, which corrects an earlier worry in docs/hardware.md:
 * the Sense camera does NOT touch a single edge pin. Its DVP bus lives
 * on GPIO 10-18, 38, 39, 40, 47 and 48, all of which reach the B2B
 * connector and nowhere else. Every one of D0..D10 is therefore
 * available for the radio.
 *
 * The bad news is worse than a connector clash. The *Kit* version of the
 * Wio-SX1262 plugs into that same B2B connector and uses:
 *
 *     NSS   GPIO41      DIO1  GPIO39      RESET GPIO42
 *     BUSY  GPIO40      RFSW  GPIO38
 *
 * GPIO 38, 39 and 40 are camera DVP lines, and 41/42 are the Sense's PDM
 * microphone. So the Kit radio and the camera do not merely compete for
 * a socket - they compete for the same GPIOs. No amount of rewiring
 * saves that combination.
 *
 * Wiring the radio to the edge pins sidesteps all of it, and SPI at a
 * few MHz over jumper leads is entirely reasonable. (The camera would
 * not be - 20 MHz parallel - which is exactly why it stays on the B2B.)
 *
 * VERIFY BEFORE POWER-UP: the assignment below is a free-pin choice, not
 * a vendor pinout. It says which XIAO pin each radio signal should be
 * wired to; the module's own labelling has to be matched to it by hand.
 * See firmware/boards/README.md.
 */
#ifndef LORAITP_BOARD_XIAO_ESP32S3_SENSE_H
#define LORAITP_BOARD_XIAO_ESP32S3_SENSE_H

/* XIAO edge pins -> ESP32-S3 GPIO (Seeed pinout) */
#define XIAO_D0  1
#define XIAO_D1  2
#define XIAO_D2  3    /* SD card CS on the Sense B2B - avoid if a card is in */
#define XIAO_D3  4
#define XIAO_D4  5    /* also I2C SDA */
#define XIAO_D5  6    /* also I2C SCL */
#define XIAO_D6  43   /* UART TX - keep for the serial console */
#define XIAO_D7  44   /* UART RX - keep for the serial console */
#define XIAO_D8  7    /* SPI SCK,  shared with the Sense SD card */
#define XIAO_D9  8    /* SPI MISO, shared with the Sense SD card */
#define XIAO_D10 9    /* SPI MOSI, shared with the Sense SD card */
#define XIAO_D11 42   /* PDM microphone clock on the Sense */
#define XIAO_D12 41   /* PDM microphone data on the Sense */

static const loraitp_board_t LORAITP_BOARD = {
    .name = "xiao-esp32s3-sense",

    /*
     * The SPI bus is shared with the Sense's SD card, which is normal -
     * SPI is a bus and the two devices have different chip selects. We
     * do not use the SD card (images live in internal flash), but if a
     * card is fitted its CS on GPIO3 must be driven high, or it will
     * drive MISO and corrupt every radio read.
     */
    .lora_sck = XIAO_D8, .lora_miso = XIAO_D9, .lora_mosi = XIAO_D10,

    .lora_nss  = XIAO_D1,    /* GPIO2 */
    .lora_rst  = XIAO_D3,    /* GPIO4 */
    .lora_busy = XIAO_D4,    /* GPIO5 */
    .lora_dio1 = XIAO_D0,    /* GPIO1 - any S3 GPIO can raise an interrupt */

    /*
     * The Kit wiring brings the RF switch out to GPIO38, which suggests
     * the module does not steer it from DIO2 internally. If that holds
     * for the board you have, set dio2_as_rf_switch false in the radio
     * config and wire the switch to D5 (GPIO6), which is left free for
     * exactly this. Getting it wrong gives a radio that transmits into a
     * matched load and hears nothing - indistinguishable from being out
     * of range.
     */
    .lora_ant_sw = LORAITP_PIN_NONE,   /* D5 / GPIO6 if the module needs it */
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

/*
 * Free after the radio: D5 (GPIO6, or the RF switch), D6/D7 (the serial
 * console), D11/D12 (the microphone, if you want it). D2 belongs to the
 * SD card. That is comfortable headroom, not a squeeze.
 */

#endif
