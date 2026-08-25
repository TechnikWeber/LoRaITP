/*
 * Contract test for the RadioLib port, against tests/mock/.
 *
 * This proves the adapter satisfies loraitp_port_t and that the core can
 * drive it. It does NOT prove the adapter matches the real RadioLib -
 * the mock and the adapter were written from the same understanding, so
 * they agree by construction. The first build against the genuine
 * library is still the real test. What this catches is the other half of
 * the problem: an adapter that compiles but wires the callbacks up
 * wrongly, mismeasures time on air, or hangs on a timeout.
 */
#include <stdio.h>
#include <string.h>

#include "Arduino.h"
#include "RadioLib.h"

#include "loraitp.h"
#include "loraitp_frame.h"
#include "port_radiolib.h"

static int passed, failed;
static void check(bool cond, const char *name)
{
    if (cond) { passed++; printf("  ok   %s\n", name); }
    else      { failed++; printf("  FAIL %s\n", name); }
}

static uint8_t image[512];
static int img_read(void *, uint32_t off, uint8_t *b, uint16_t n)
{ memcpy(b, image + off, n); return LORAITP_OK; }
static int img_write(void *, uint32_t off, const uint8_t *b, uint16_t n)
{ memcpy(image + off, b, n); return LORAITP_OK; }

int main(void)
{
    printf("RadioLib port contract\n");
    mock_reset();

    loraitp_radiolib_cfg_t cfg;
    loraitp_radiolib_defaults(&cfg);

    check(cfg.frequency_mhz > 869.7f && cfg.frequency_mhz < 870.0f,
          "defaults land in EU868_G4_LP (5 mW, no duty limit)");
    check(cfg.tx_power_dbm == 7, "default power is 5 mW");
    check(cfg.sync_word == 0x12, "default sync word is private, not LoRaWAN");

    loraitp_port_t port;
    memset(&port, 0, sizeof(port));

    /* A missing BUSY pin hangs the first SPI command; refuse up front. */
    check(loraitp_radiolib_attach(&port, &cfg) == LORAITP_E_ARG,
          "attach refuses a config with no pins set");

    cfg.pin_nss = 8; cfg.pin_sck = 9; cfg.pin_mosi = 10; cfg.pin_miso = 11;
    cfg.pin_rst = 12; cfg.pin_busy = 13; cfg.pin_dio1 = 14;
    check(loraitp_radiolib_attach(&port, &cfg) == LORAITP_OK, "attach succeeds");

    check(port.radio_send && port.radio_receive && port.now_ms
          && port.sleep_ms && port.random_bytes, "radio callbacks are set");
    check(port.image_read == nullptr && port.image_write == nullptr,
          "storage is left to the application, not claimed by the port");
    check(port.aes128_encrypt_block == nullptr,
          "authentication is off until a key is supplied");

    check(loraitp_port_validate(&port, 1) == LORAITP_E_ARG,
          "port_validate rejects a sender with no image_read");
    port.image_read = img_read;
    port.image_write = img_write;
    check(loraitp_port_validate(&port, 1) == LORAITP_OK,
          "port_validate passes once storage is attached");

    /* Measured time on air, and the governor's number to compare with. */
    uint8_t frame[64];
    int fl = loraitp_encode_simple(LORAITP_FIN, 1, nullptr, 0, frame,
                                   sizeof(frame));
    uint32_t toa = 0;
    check(port.radio_send(port.ctx, frame, (uint8_t)fl, &toa) == LORAITP_OK,
          "radio_send succeeds");
    check(toa > 0, "radio_send reports a measured time on air");
    check(loraitp_radiolib_airtime_ms() == toa,
          "cumulative airtime tracks the reported figure");
    check(mock_tx_count() == 1, "exactly one transmission reached the radio");

    /*
     * Receive: a timeout must be a timeout, not an error.
     *
     * Note the capacity argument is uint8_t, so 255 is the largest a
     * caller can express - which matches the LoRa PHY maximum. Passing
     * 256 truncates to zero; the adapter then rejects every frame as
     * oversized rather than overflowing, which is the right failure, but
     * it is a trap worth knowing about.
     */
    uint8_t rx[LORAITP_MAX_FRAME];
    loraitp_rx_meta_t meta;
    int n = port.radio_receive(port.ctx, rx, (uint8_t)sizeof(rx), 50, &meta);
    check(n == 0, "radio_receive returns 0 on timeout, not an error code");

    mock_set_rx_frame(frame, (size_t)fl);
    n = port.radio_receive(port.ctx, rx, (uint8_t)sizeof(rx), 1000, &meta);
    check(n == fl, "radio_receive returns the frame length");
    check(memcmp(rx, frame, (size_t)fl) == 0, "frame arrives intact");
    check(meta.rssi_dbm == -98 && meta.snr_qdb == -20,
          "RSSI and SNR reach the caller in the units the core expects");

    /*
     * The RF switch, in all three flavours. This is the setting that
     * fails silently: a radio configured with the wrong one transmits
     * into a matched load and hears nothing, which is indistinguishable
     * from being out of range.
     */
    {
        loraitp_radiolib_cfg_t c2;
        loraitp_port_t p2;

        loraitp_radiolib_defaults(&c2);
        c2.pin_nss = 8; c2.pin_busy = 13; c2.pin_dio1 = 14;
        check(c2.dio2_as_rf_switch, "DIO2 control is the default");
        memset(&p2, 0, sizeof(p2));
        loraitp_radiolib_attach(&p2, &c2);
        check(mock_dio2_rfsw(), "DIO2 switch is actually enabled");

        /* Single RF_SW line, as the Seeed Wio-SX1262 needs. */
        c2.dio2_as_rf_switch = false;
        c2.pin_rf_sw = 6;
        memset(&p2, 0, sizeof(p2));
        loraitp_radiolib_attach(&p2, &c2);
        check(mock_rfsw_tx() == 6 && mock_rfsw_rx() == RADIOLIB_NC,
              "a single RF_SW pin is driven high to transmit");

        /*
         * The inverted case matters as much as the default: which slot
         * the pin occupies IS the polarity, and getting it wrong gives a
         * radio that hears nothing while looking perfectly configured.
         */
        c2.rf_sw_inverted = true;
        memset(&p2, 0, sizeof(p2));
        loraitp_radiolib_attach(&p2, &c2);
        check(mock_rfsw_rx() == 6 && mock_rfsw_tx() == RADIOLIB_NC,
              "inverting RF_SW swaps which phase drives it high");
        c2.rf_sw_inverted = false;

        /* Two separate lines. */
        c2.pin_rf_sw = LORAITP_PIN_NONE;
        c2.pin_rx_en = 4; c2.pin_tx_en = 5;
        memset(&p2, 0, sizeof(p2));
        loraitp_radiolib_attach(&p2, &c2);
        check(mock_rfsw_rx() == 4 && mock_rfsw_tx() == 5,
              "a two-pin RF switch is wired through");
    }

    /* Reconfiguring mid-session is what the SF probe does. */
    loraitp_radio_cfg_t rc;
    memset(&rc, 0, sizeof(rc));
    rc.frequency_hz = 869850000u; rc.bandwidth_hz = 125000u;
    rc.spreading_factor = 12; rc.coding_rate = 1; rc.preamble_symbols = 8;
    rc.tx_power_dbm = 7; rc.sync_word = 0x12;
    check(port.radio_configure(port.ctx, &rc) == LORAITP_OK,
          "radio_configure accepts a mid-session SF change");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
