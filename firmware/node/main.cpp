/*
 * LoRaITP bench loopback.
 *
 * Two boards, a synthetic image, and no camera - the smallest thing that
 * proves the protocol works on real radios. Flash one as SENDER and one
 * as RECEIVER and watch the serial output.
 *
 * It runs on EU868_G4_LP by default: 869.7-870.0 MHz at 5 mW with no
 * duty-cycle limit (BNetzA Vfg. 91/2025 table 2 row 56a). That is the
 * right profile to develop against - it burns none of the daily budget
 * and needs no licence, so you can run a hundred transfers in an
 * afternoon. Move to EU868_G3 only once the link works.
 *
 * NOT YET RUN ON HARDWARE. The core and the port adapter are tested on a
 * host; this file has never been compiled against a real Arduino core.
 * Treat the first upload as the beginning of debugging, not the end.
 */
#include <Arduino.h>
#include <LittleFS.h>

#include "board.h"
#include "loraitp.h"
#include "loraitp_store.h"
#include "port_radiolib.h"

#ifndef LORAITP_STORE_DIR
#define LORAITP_STORE_DIR "/images"
#endif

/* Flash one board with -DLORAITP_ROLE_SENDER, the other without. */
#ifdef LORAITP_ROLE_SENDER
static const bool IS_SENDER = true;
#else
static const bool IS_SENDER = false;
#endif

#define IMAGE_LEN 4000

/* Erasure coding needs a whole block resident. The core allocates
 * nothing, so the buffer is ours to provide and ours to size. */
static uint8_t fec_scratch[24 * 1024];

static uint8_t ctx_mem[8 * 1024];
static uint8_t store_mem[512];
static loraitp_port_t port;
static loraitp_store_t *store;

/* Deterministic synthetic content, so the receiver can verify byte for
 * byte without a camera being involved yet. */
static uint8_t synth(uint32_t i)
{
    return (uint8_t)((i * 37u + (i >> 5) * 11u) & 0xFFu);
}

static void die(const char *what, int rc)
{
    for (;;) {
        Serial.printf("%s failed: %d (radio %d)\n", what, rc,
                      loraitp_radiolib_last_error());
        delay(5000);
    }
}

void setup(void)
{
    Serial.begin(115200);
    delay(2000);
    Serial.printf("\nLoRaITP bench - board %s, role %s\n",
                  LORAITP_BOARD.name, IS_SENDER ? "SENDER" : "RECEIVER");

    if (!LittleFS.begin(true, LORAITP_STORE_DIR))
        die("LittleFS mount", LORAITP_E_IO);

    store = (loraitp_store_t *)store_mem;
    if (loraitp_store_size() > sizeof(store_mem))
        die("store buffer too small", (int)loraitp_store_size());

    /*
     * Keep the last 32 images. Storage is not the constraint here - even
     * 8 MB of flash holds several hundred - but a ring keeps the
     * filesystem from ever filling, which is the failure that turns a
     * node into a brick in the field.
     */
    int rc0 = loraitp_store_init(store, LORAITP_STORE_DIR, 32);
    if (rc0 != LORAITP_OK)
        die("store init", rc0);
    Serial.printf("store: %d image(s) on flash\n", loraitp_store_count(store));

    loraitp_radiolib_cfg_t rcfg;
    loraitp_radiolib_defaults(&rcfg);
    rcfg.pin_nss  = LORAITP_BOARD.lora_nss;
    rcfg.pin_dio1 = LORAITP_BOARD.lora_dio1;
    rcfg.pin_rst  = LORAITP_BOARD.lora_rst;
    rcfg.pin_busy = LORAITP_BOARD.lora_busy;
    rcfg.pin_sck  = LORAITP_BOARD.lora_sck;
    rcfg.pin_miso = LORAITP_BOARD.lora_miso;
    rcfg.pin_mosi = LORAITP_BOARD.lora_mosi;
    rcfg.tcxo = LORAITP_BOARD.lora_tcxo;
    rcfg.tcxo_voltage = LORAITP_BOARD.lora_tcxo_v;

    /*
     * A board that names an antenna-switch pin has to have it driven; one
     * that does not steers the switch from DIO2 internally. Silently
     * getting this wrong is the single most likely reason for a radio
     * that configures cleanly and then hears nothing.
     */
    if (LORAITP_BOARD.lora_ant_sw != LORAITP_PIN_NONE) {
        rcfg.dio2_as_rf_switch = false;
        rcfg.pin_rf_sw = LORAITP_BOARD.lora_ant_sw;
        Serial.printf("RF switch on GPIO%u\n", LORAITP_BOARD.lora_ant_sw);
    }

    int rc = loraitp_radiolib_attach(&port, &rcfg);
    if (rc != LORAITP_OK)
        die("radio attach", rc);

    loraitp_store_attach(&port, store);

    Serial.printf("radio up: %.3f MHz, SF%u, %d dBm\n",
                  (double)rcfg.frequency_mhz, rcfg.spreading_factor,
                  rcfg.tx_power_dbm);
}

static void configure(loraitp_session_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = LORAITP_MODE_INTERACTIVE;
    cfg->region = LORAITP_REG_EU868_G4_LP;   /* 5 mW, no duty limit */
    cfg->frequency_hz = 869850000u;
    cfg->bandwidth_hz = 125000u;
    cfg->spreading_factor = 10;
    cfg->coding_rate = 1;
    cfg->tx_power_dbm = 7;
    cfg->session_timeout_ms = 5u * 60u * 1000u;
    cfg->fec_scratch = fec_scratch;
    cfg->fec_scratch_len = sizeof(fec_scratch);
}

/*
 * Put one synthetic image in the store if it is empty, so the sender has
 * something to read. Once the camera exists this is what it replaces.
 */
static uint32_t seed_image(void)
{
    uint8_t buf[256];
    uint32_t crc = 0;

    if (loraitp_store_count(store) > 0) {
        /* Already there from a previous run - recompute the checksum. */
        for (uint32_t off = 0; off < IMAGE_LEN; off += sizeof(buf)) {
            uint16_t n = (uint16_t)((IMAGE_LEN - off < sizeof(buf))
                                    ? IMAGE_LEN - off : sizeof(buf));
            for (uint16_t i = 0; i < n; i++) buf[i] = synth(off + i);
            crc = loraitp_crc32_update(crc, buf, n);
        }
        return crc;
    }

    if (loraitp_store_begin_write(store, IMAGE_LEN) != LORAITP_OK)
        die("seed begin_write", LORAITP_E_IO);
    for (uint32_t off = 0; off < IMAGE_LEN; off += sizeof(buf)) {
        uint16_t n = (uint16_t)((IMAGE_LEN - off < sizeof(buf))
                                ? IMAGE_LEN - off : sizeof(buf));
        for (uint16_t i = 0; i < n; i++) buf[i] = synth(off + i);
        crc = loraitp_crc32_update(crc, buf, n);
        port.image_write(port.ctx, off, buf, n);
    }
    loraitp_store_meta_t m;
    memset(&m, 0, sizeof(m));
    m.img_id = 1; m.layer = 1; m.codec = 2; m.img_len = IMAGE_LEN;
    m.crc32 = crc; m.width = 320; m.height = 240;
    m.complete = true; m.crc_ok = true;
    loraitp_store_finish(store, &m);
    Serial.println("seeded a synthetic image");
    return crc;
}

static uint32_t g_crc;

void loop(void)
{
    loraitp_session_cfg_t cfg;
    configure(&cfg);

    loraitp_ctx_t *ctx = (loraitp_ctx_t *)ctx_mem;
    if (loraitp_ctx_size() > sizeof(ctx_mem))
        die("context buffer too small", (int)loraitp_ctx_size());

    int rc = loraitp_init(ctx, &port, &cfg);
    if (rc != LORAITP_OK)
        die("session init", rc);   /* a regulatory refusal lands here */

    loraitp_stats_t st;
    uint32_t t0 = millis();

    if (IS_SENDER) {
        if (g_crc == 0)
            g_crc = seed_image();

        loraitp_image_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.img_id = (uint16_t)(millis() / 1000u);
        desc.layer = 1;
        desc.img_len = IMAGE_LEN;
        desc.codec = 2;
        desc.img_crc32 = g_crc;
        desc.width = 320;
        desc.height = 240;

        Serial.printf("\nsending %u B, image %u\n",
                      (unsigned)desc.img_len, desc.img_id);
        /* The core opens the stored image itself through image_begin. */
        rc = loraitp_send_image(ctx, &desc, &st);
        Serial.printf("  rc %d  %u frames  %u ms airtime  %u rounds  "
                      "%u retransmits  %u ms wall\n",
                      rc, st.frames_tx, st.airtime_ms, st.rounds,
                      st.retransmits, millis() - t0);

        loraitp_budget_t b;
        loraitp_budget_query(ctx, &b);
        Serial.printf("  budget: %lu ms used of %lu in the last hour\n",
                      (unsigned long)b.airtime_used_ms,
                      (unsigned long)b.airtime_budget_ms);
        delay(10000);
        return;
    }

    loraitp_image_desc_t desc;
    loraitp_rx_result_t result;

    Serial.println("\nlistening...");
    rc = loraitp_receive_image(ctx, &desc, &result, &st);
    if (result == LORAITP_RX_TIMEOUT) {
        Serial.println("  nothing heard");
        return;
    }

    /* The core has already closed and renamed the file; all that is left
     * is the sidecar, which only we know the contents of. */
    loraitp_store_meta_t m;
    memset(&m, 0, sizeof(m));
    m.img_id = desc.img_id; m.layer = desc.layer; m.codec = desc.codec;
    m.img_len = desc.img_len; m.crc32 = desc.img_crc32;
    m.width = desc.width; m.height = desc.height;
    m.chunks_have = st.chunks_have; m.chunks_total = st.chunks_total;
    m.rounds = st.rounds; m.airtime_ms = st.airtime_ms;
    m.rssi_dbm = st.last_rssi_dbm; m.snr_qdb = st.last_snr_qdb;
    m.complete = (result == LORAITP_RX_COMPLETE);
    m.crc_ok = m.complete;
    loraitp_store_finish(store, &m);

    Serial.printf("  rc %d  result %d  %u/%u chunks  %u frames  "
                  "RSSI %d dBm  SNR %.2f dB\n",
                  rc, (int)result, st.chunks_have, st.chunks_total,
                  st.frames_rx, st.last_rssi_dbm, st.last_snr_qdb / 4.0);
    Serial.printf("  saved as %s  (%d image(s) on flash)\n",
                  loraitp_store_current(store), loraitp_store_count(store));

    /*
     * The number worth watching. Everything in SPEC.md that is still a
     * guess - the 2 s time-on-air cap above all - can only be settled by
     * loss rates measured on a real link.
     */
    if (st.chunks_total)
        Serial.printf("  loss: %u of %u chunks needed repair\n",
                      (unsigned)(st.chunks_total - st.chunks_have),
                      st.chunks_total);
}
