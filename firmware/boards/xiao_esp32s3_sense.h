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
 * The assignment below is READ OFF THE MODULE, not chosen by us. The
 * Wio-SX1262 for XIAO is a plug-on board with two 7-pin sockets matching
 * the XIAO's own edge pins, silkscreened (viewed from underneath):
 *
 *     left  column, top down:  VIN GND 3V3 MOSI MISO SCK D7
 *     right column, top down:  D0 DIO1 RST BUSY NSS RF_SW D6
 *
 * Flipping that to the top view gives the map below.
 *
 * TWO THINGS TO KNOW:
 *
 * 1. RF_SW is a real pin. This module does NOT steer its antenna switch
 *    from DIO2 internally - it expects the host to drive GPIO6. Configure
 *    dio2_as_rf_switch = false or the radio transmits into a matched load
 *    and hears nothing, which is indistinguishable from being out of
 *    range and a miserable thing to debug.
 *
 * 2. RST lands on GPIO3, which is also the SD card's chip select on the
 *    Sense expansion board, on the same SPI bus. With a card fitted,
 *    every radio reset would select the SD card and it would drive MISO.
 *    Leave the SD slot empty, or move RST to D0 (GPIO1) with a jumper.
 *    Images live in internal flash, so the card is not needed.
 */
#ifndef LORAITP_BOARD_XIAO_ESP32S3_SENSE_H
#define LORAITP_BOARD_XIAO_ESP32S3_SENSE_H

/* XIAO edge pins -> ESP32-S3 GPIO (Seeed pinout) */
#define XIAO_D0  1
#define XIAO_D1  2
#define XIAO_D2  3    /* radio RST here; also SD card CS on the Sense B2B */
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

    .lora_dio1 = XIAO_D1,    /* GPIO2  - silkscreened DIO1 */
    .lora_rst  = XIAO_D2,    /* GPIO3  - silkscreened RST; also SD CS, see above */
    .lora_busy = XIAO_D3,    /* GPIO4  - silkscreened BUSY */
    .lora_nss  = XIAO_D4,    /* GPIO5  - silkscreened NSS */

    .lora_ant_sw = XIAO_D5,  /* GPIO6  - silkscreened RF_SW, must be driven */
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
 * The module passes D0, D6 and D7 through without using them. D6/D7 are
 * the serial console, so D0 (GPIO1) is the one genuinely free edge pin -
 * enough for a button to raise the access point, which is all the
 * application needs. D11/D12 remain available on the B2B side if the
 * microphone is ever wanted.
 */

#endif
