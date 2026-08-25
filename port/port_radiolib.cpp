/*
 * LoRaITP port on RadioLib.  See port_radiolib.h.
 *
 * The RadioLib API surface this depends on is listed in
 * tests/mock/RadioLib.h - one place to look when a RadioLib version bump
 * breaks the build.
 *
 * Two things here are worth reading carefully, because the protocol's
 * correctness rests on them:
 *
 *  1. radio_send returns *measured* time on air, taken between the
 *     transmit command and the TxDone interrupt. The duty-cycle governor
 *     accounts against this number, so a modelled value would make the
 *     accounting a guess.
 *
 *  2. This port enforces no duty cycle of its own. That is the core's
 *     job, and doing it in both places corrupts the accounting in a way
 *     that is very hard to see - the core believes it has budget, the
 *     port silently delays, and measured airtime stops matching the
 *     model.
 */
#include <math.h>
#include <string.h>

#include <Arduino.h>
#include <RadioLib.h>

#include "loraitp.h"
#include "port_radiolib.h"

#if defined(ESP32) || defined(ESP_PLATFORM)
#  define LORAITP_ESP 1
#  include <esp_sleep.h>
#  if __has_include(<mbedtls/aes.h>)
#    include <mbedtls/aes.h>
#    define LORAITP_HAVE_MBEDTLS 1
#  endif
#endif

namespace {

loraitp_radiolib_cfg_t g_cfg;
SX1262 *g_radio = nullptr;
int g_last_error = RADIOLIB_ERR_NONE;
uint32_t g_airtime_ms = 0;

/*
 * Set from the DIO1 interrupt for both TxDone and RxDone. A single flag
 * is enough because the protocol is strictly half duplex: we are never
 * waiting for both at once.
 */
volatile bool g_dio1 = false;

#if defined(LORAITP_ESP)
#  define LORAITP_ISR IRAM_ATTR
#else
#  define LORAITP_ISR
#endif

void LORAITP_ISR dio1_isr(void)
{
    g_dio1 = true;
}

/* ------------------------------------------------------------- helpers */

int radiolib_to_loraitp(int16_t state)
{
    switch (state) {
    case RADIOLIB_ERR_NONE:
        return LORAITP_OK;
    case RADIOLIB_ERR_RX_TIMEOUT:
        return LORAITP_E_TIMEOUT;
    default:
        g_last_error = state;
        return LORAITP_E_RADIO;
    }
}

int apply_params(const loraitp_radiolib_cfg_t *cfg)
{
    int16_t st = g_radio->setFrequency(cfg->frequency_mhz);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setBandwidth(cfg->bandwidth_khz);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setSpreadingFactor(cfg->spreading_factor);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setCodingRate(cfg->coding_rate);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setSyncWord(cfg->sync_word);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setPreambleLength(cfg->preamble_symbols);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setOutputPower(cfg->tx_power_dbm);
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setCurrentLimit(cfg->current_limit_ma);

    /*
     * Explicit header and a PHY CRC are not optional for LoRaITP. The
     * 4-byte frame header carries no length and no checksum precisely
     * because the PHY provides both - see SPEC.md 3. Turning either off
     * would silently break the wire format.
     */
    if (st == RADIOLIB_ERR_NONE) st = g_radio->explicitHeader();
    if (st == RADIOLIB_ERR_NONE) st = g_radio->setCRC(2);

    return radiolib_to_loraitp(st);
}

/* ------------------------------------------------------ port callbacks */

int rl_configure(void *ctx, const loraitp_radio_cfg_t *cfg)
{
    (void)ctx;
    if (g_radio == nullptr || cfg == nullptr)
        return LORAITP_E_ARG;

    g_cfg.frequency_mhz = (float)cfg->frequency_hz / 1000000.0f;
    g_cfg.bandwidth_khz = (float)cfg->bandwidth_hz / 1000.0f;
    g_cfg.spreading_factor = cfg->spreading_factor;
    g_cfg.coding_rate = (uint8_t)(cfg->coding_rate + 4u);  /* 1..4 -> 5..8 */
    g_cfg.preamble_symbols = cfg->preamble_symbols;
    g_cfg.tx_power_dbm = cfg->tx_power_dbm;
    g_cfg.sync_word = cfg->sync_word;
    return apply_params(&g_cfg);
}

int rl_send(void *ctx, const uint8_t *buf, uint8_t len, uint32_t *toa_ms)
{
    (void)ctx;
    if (g_radio == nullptr || buf == nullptr || toa_ms == nullptr)
        return LORAITP_E_ARG;

    g_dio1 = false;
    uint32_t t0 = micros();
    int16_t st = g_radio->startTransmit((uint8_t *)buf, len);
    if (st != RADIOLIB_ERR_NONE) {
        g_radio->standby();
        return radiolib_to_loraitp(st);
    }

    /*
     * Bound the wait generously. A SF12 frame at the 2 s cap takes about
     * two seconds; anything beyond four times the modelled figure means
     * the interrupt was missed or the radio wedged, and hanging forever
     * in a field deployment is worse than a lost frame.
     */
    uint32_t budget_ms = loraitp_time_on_air_us(len, g_cfg.spreading_factor,
                                                (uint32_t)(g_cfg.bandwidth_khz * 1000.0f),
                                                (uint8_t)(g_cfg.coding_rate - 4u),
                                                (uint8_t)g_cfg.preamble_symbols)
                         / 1000u;
    uint32_t limit = budget_ms * 4u + 500u;
    uint32_t start = millis();
    while (!g_dio1) {
        if (millis() - start > limit) {
            g_radio->standby();
            g_last_error = RADIOLIB_ERR_TX_TIMEOUT;
            return LORAITP_E_RADIO;
        }
        yield();
    }
    uint32_t t1 = micros();

    g_radio->finishTransmit();

    /*
     * Measured, not modelled. It over-counts slightly - the SetTx command
     * latency sits inside the window - and over-counting is the safe
     * direction for a duty-cycle budget.
     */
    uint32_t measured_us = t1 - t0;
    *toa_ms = (measured_us + 999u) / 1000u;
    g_airtime_ms += *toa_ms;
    return LORAITP_OK;
}

int rl_receive(void *ctx, uint8_t *buf, uint8_t cap, uint32_t timeout_ms,
               loraitp_rx_meta_t *meta)
{
    (void)ctx;
    if (g_radio == nullptr || buf == nullptr)
        return LORAITP_E_ARG;

    g_dio1 = false;
    int16_t st = g_radio->startReceive();
    if (st != RADIOLIB_ERR_NONE)
        return radiolib_to_loraitp(st);

    /*
     * The timeout is implemented here rather than in the radio's own
     * RX-timeout register. RadioLib has expressed that register in
     * different units across versions, and a wrong unit turns a 60-second
     * listening window into a 60-millisecond one - which looks exactly
     * like a bad antenna. A millis() loop cannot be got wrong.
     */
    uint32_t start = millis();
    while (!g_dio1) {
        if (millis() - start >= timeout_ms) {
            g_radio->standby();
            return 0;                       /* timeout, not an error */
        }
        yield();
    }

    size_t n = g_radio->getPacketLength();
    if (n == 0 || n > cap) {
        g_radio->standby();
        return 0;                           /* nothing usable */
    }

    st = g_radio->readData(buf, n);
    if (meta != nullptr) {
        meta->rssi_dbm = (int16_t)lrintf(g_radio->getRSSI());
        meta->snr_qdb = (int8_t)lrintf(g_radio->getSNR() * 4.0f);
        meta->timestamp_ms = millis();
    }
    g_radio->standby();

    /*
     * A CRC failure is not an error condition. The PHY caught a corrupted
     * frame, which from the protocol's point of view means the frame did
     * not arrive - and that is exactly the clean erasure the erasure
     * coding in SPEC.md 5.2 relies on.
     */
    if (st == RADIOLIB_ERR_CRC_MISMATCH)
        return 0;
    if (st != RADIOLIB_ERR_NONE)
        return radiolib_to_loraitp(st);

    return (int)n;
}

int rl_sleep_radio(void *ctx)
{
    (void)ctx;
    if (g_radio == nullptr)
        return LORAITP_E_ARG;
    /* Retain the configuration so waking does not need a full re-init. */
    return radiolib_to_loraitp(g_radio->sleep(true));
}

uint32_t rl_now_ms(void *ctx)
{
    (void)ctx;
    return millis();
}

void rl_sleep_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (ms == 0)
        return;

#if defined(LORAITP_ESP)
    if (g_cfg.allow_light_sleep && ms >= g_cfg.light_sleep_threshold_ms) {
        /*
         * The governor asks for waits of minutes on a 1% band. Spending
         * them awake costs more energy than the transmission did, which
         * would make the energy figures in SPEC.md 8.4 fiction.
         *
         * Light sleep keeps RAM and the millis() timebase, so the session
         * state machine simply continues afterwards.
         */
        if (g_radio != nullptr)
            g_radio->sleep(true);
        esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ull);
        esp_light_sleep_start();
        if (g_radio != nullptr)
            g_radio->standby();
        return;
    }
#endif
    delay(ms);
}

int rl_random(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    if (out == nullptr)
        return LORAITP_E_ARG;
    /*
     * RadioLib's randomByte() samples the radio's own wideband RSSI - a
     * physical source rather than a seeded PRNG. Slow, but a session
     * nonce is four bytes once per transfer.
     *
     * There is deliberately no software fallback. A nonce from a
     * predictable source is worse than refusing, because it silently
     * removes the replay protection the caller thinks it has.
     */
    if (g_radio == nullptr)
        return LORAITP_E_NOSUP;
    for (size_t i = 0; i < len; i++)
        out[i] = g_radio->randomByte();
    return LORAITP_OK;
}

#if defined(LORAITP_HAVE_MBEDTLS)
uint8_t g_key[16];
bool g_have_key = false;

int rl_aes(void *ctx, const uint8_t in[16], uint8_t out[16])
{
    (void)ctx;
    if (!g_have_key)
        return LORAITP_E_NOSUP;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, g_key, 128);
    if (rc == 0)
        rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&aes);
    return (rc == 0) ? LORAITP_OK : LORAITP_E_NOSUP;
}
#endif

}  /* namespace */

/* ---------------------------------------------------------- public API */

extern "C" void loraitp_radiolib_defaults(loraitp_radiolib_cfg_t *cfg)
{
    if (cfg == nullptr)
        return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->pin_nss = cfg->pin_dio1 = cfg->pin_rst = cfg->pin_busy = LORAITP_PIN_NONE;
    cfg->pin_sck = cfg->pin_miso = cfg->pin_mosi = LORAITP_PIN_NONE;
    cfg->pin_rf_sw = LORAITP_PIN_NONE;
    cfg->pin_rx_en = cfg->pin_tx_en = LORAITP_PIN_NONE;

    cfg->dio2_as_rf_switch = true;      /* true for most SX1262 modules */
    cfg->tcxo = true;
    cfg->tcxo_voltage = 1.8f;

    /*
     * EU868_G4_LP by default: 869.7-870.0 MHz at 5 mW with no duty-cycle
     * limit. That is the right profile to develop against - it burns no
     * budget and needs no licence - and a default that cannot get anyone
     * into trouble is worth more than a fast one.
     */
    cfg->frequency_mhz = 869.85f;
    cfg->bandwidth_khz = 125.0f;
    cfg->spreading_factor = 10;
    cfg->coding_rate = 5;
    cfg->sync_word = 0x12;              /* private network, not LoRaWAN */
    cfg->tx_power_dbm = 7;              /* 5 mW */
    cfg->preamble_symbols = 8;
    cfg->current_limit_ma = 140.0f;

    cfg->light_sleep_threshold_ms = 200;
    cfg->allow_light_sleep = true;
}

extern "C" int loraitp_radiolib_attach(loraitp_port_t *port,
                                       const loraitp_radiolib_cfg_t *cfg)
{
    if (port == nullptr || cfg == nullptr)
        return LORAITP_E_ARG;
    if (cfg->pin_nss == LORAITP_PIN_NONE || cfg->pin_busy == LORAITP_PIN_NONE
        || cfg->pin_dio1 == LORAITP_PIN_NONE)
        return LORAITP_E_ARG;   /* a missing BUSY hangs on the first command */

    g_cfg = *cfg;
    g_airtime_ms = 0;

    if (cfg->pin_sck != LORAITP_PIN_NONE)
        SPI.begin(cfg->pin_sck, cfg->pin_miso, cfg->pin_mosi, cfg->pin_nss);

    static Module module(cfg->pin_nss, cfg->pin_dio1, cfg->pin_rst,
                         cfg->pin_busy);
    static SX1262 radio(&module);
    g_radio = &radio;

    int16_t st = radio.begin(g_cfg.frequency_mhz, g_cfg.bandwidth_khz,
                             g_cfg.spreading_factor, g_cfg.coding_rate,
                             g_cfg.sync_word, g_cfg.tx_power_dbm,
                             g_cfg.preamble_symbols,
                             cfg->tcxo ? cfg->tcxo_voltage : 0.0f,
                             false);
    if (st != RADIOLIB_ERR_NONE) {
        g_last_error = st;
        return LORAITP_E_RADIO;
    }

    /*
     * The earlier version of this only handled a two-pin switch, and
     * silently did nothing at all when a module brought out a single
     * RF_SW line - the failure being a radio that appears configured and
     * cannot hear anything.
     */
    if (cfg->dio2_as_rf_switch) {
        st = radio.setDio2AsRfSwitch(true);
        if (st != RADIOLIB_ERR_NONE) {
            g_last_error = st;
            return LORAITP_E_RADIO;
        }
    } else if (cfg->pin_rf_sw != LORAITP_PIN_NONE) {
        /*
         * One line, driven high to transmit. RadioLib raises txEn during
         * transmit and rxEn during receive, so passing the pin as txEn
         * with no rxEn gives exactly that.
         *
         * The polarity is the convention, not something measured on this
         * module. If the link works in one direction only, invert it here
         * first - it is a one-line change and by far the most likely
         * cause.
         */
        radio.setRfSwitchPins(RADIOLIB_NC, cfg->pin_rf_sw);
    } else if (cfg->pin_rx_en != LORAITP_PIN_NONE
               || cfg->pin_tx_en != LORAITP_PIN_NONE) {
        radio.setRfSwitchPins(cfg->pin_rx_en, cfg->pin_tx_en);
    }

    int rc = apply_params(&g_cfg);
    if (rc != LORAITP_OK)
        return rc;

    radio.setDio1Action(dio1_isr);
    radio.standby();

    /* Fill in the radio half only. Storage and AES stay the caller's. */
    port->radio_configure = rl_configure;
    port->radio_send = rl_send;
    port->radio_receive = rl_receive;
    port->radio_sleep = rl_sleep_radio;
    port->now_ms = rl_now_ms;
    port->sleep_ms = rl_sleep_ms;
    port->random_bytes = rl_random;
    port->ctx = nullptr;

    return LORAITP_OK;
}

extern "C" int loraitp_radiolib_last_error(void)
{
    return g_last_error;
}

extern "C" uint32_t loraitp_radiolib_airtime_ms(void)
{
    return g_airtime_ms;
}

#if defined(LORAITP_HAVE_MBEDTLS)
extern "C" void loraitp_radiolib_set_key(loraitp_port_t *port,
                                         const uint8_t key[16])
{
    if (key == nullptr) {
        g_have_key = false;
        port->aes128_encrypt_block = nullptr;
        return;
    }
    memcpy(g_key, key, 16);
    g_have_key = true;
    port->aes128_encrypt_block = rl_aes;
}
#endif
