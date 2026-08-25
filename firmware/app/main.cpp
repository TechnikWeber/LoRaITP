/*
 * LoRaITP node firmware. One binary for both ends of the link.
 *
 * Which end a board is gets decided at run time from a stored setting,
 * not at build time - so there is one firmware to build, one to flash,
 * and a base station can be turned into a second node from a phone
 * without a toolchain anywhere near it. A board with a camera defaults to
 * sending; one without defaults to listening.
 *
 * It starts on EU868_G4_LP: 869.7-870.0 MHz at 5 mW with no duty-cycle
 * limit (BNetzA Vfg. 91/2025 row 56a). That burns none of the daily
 * budget and needs no licence, so a hundred transfers cost an afternoon
 * rather than a week. Move to EU868_G3 from the settings page once the
 * link works.
 *
 * NOT YET RUN ON HARDWARE.
 */
#include <Arduino.h>
#include <LittleFS.h>

#include "appcfg.h"
#include "board.h"
#include "camera.h"
#include "loraitp.h"
#include "loraitp_store.h"
#include "port_radiolib.h"
#include "webui.h"

#ifndef LORAITP_STORE_DIR
#define LORAITP_STORE_DIR "/images"
#endif

static loraitp_appcfg_t cfg;
static loraitp_port_t port;
static loraitp_store_t *store;

static uint8_t store_mem[512];
static uint8_t ctx_mem[8 * 1024];
static uint8_t jpegbuf[32 * 1024];

/* Erasure coding needs a whole block resident. The core allocates
 * nothing, so the buffer is ours to provide and ours to size. Only
 * broadcast mode uses it. */
static uint8_t fec_scratch[24 * 1024];

static char last_result[48] = "-";
static uint32_t next_run_ms;

/* ------------------------------------------------------------- helpers */

static void die(const char *what, int rc)
{
    for (;;) {
        Serial.printf("%s failed: %d (radio %d)\n", what, rc,
                      loraitp_radiolib_last_error());
        delay(5000);
    }
}

static void configure_session(loraitp_session_cfg_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = cfg.broadcast ? LORAITP_MODE_BROADCAST
                            : LORAITP_MODE_INTERACTIVE;
    s->region = (loraitp_region_t)cfg.region;
    s->frequency_hz = cfg.frequency_hz;
    s->bandwidth_hz = 125000u;
    s->spreading_factor = cfg.spreading_factor;
    s->coding_rate = 1;
    s->tx_power_dbm = cfg.tx_power_dbm;
    s->parity_percent = cfg.parity_percent;
    s->session_timeout_ms = 10u * 60u * 1000u;
    s->fec_scratch = fec_scratch;
    s->fec_scratch_len = sizeof(fec_scratch);
    if (cfg.callsign[0])
        s->callsign = cfg.callsign;
}

static void status_cb(void *user, loraitp_webui_status_t *out)
{
    (void)user;
    loraitp_session_cfg_t s;
    configure_session(&s);

    /* A throwaway context purely to ask the governor. Cheap: it holds no
     * state beyond the rolling window, which starts empty. */
    static uint8_t probe_mem[8 * 1024];
    loraitp_ctx_t *c = (loraitp_ctx_t *)probe_mem;
    if (loraitp_init(c, &port, &s) == LORAITP_OK) {
        loraitp_budget_t b;
        loraitp_budget_query(c, &b);
        out->airtime_used_ms = b.airtime_used_ms;
        out->airtime_budget_ms = b.airtime_budget_ms;
        out->bytes_remaining = loraitp_budget_bytes_remaining(c);
    }
    out->last_result = last_result;
}

/*
 * The web server runs on the other core. A receive session blocks for
 * minutes at a time, and an access point that stops answering for the
 * length of a transfer is worse than no access point at all.
 */
static void web_task(void *)
{
    for (;;) {
        loraitp_webui_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------------------------------------------------------- setup */

void setup(void)
{
    Serial.begin(115200);
    delay(2000);

    if (!LittleFS.begin(true, LORAITP_STORE_DIR, 10, "images"))
        die("LittleFS mount", LORAITP_E_IO);

    loraitp_cfg_load(&cfg, LORAITP_BOARD.has_camera);

    Serial.printf("\nLoRaITP on %s - %s, region %s\n", LORAITP_BOARD.name,
                  cfg.role == LORAITP_ROLE_SENDER ? "SENDER" : "RECEIVER",
                  loraitp_cfg_region_name(cfg.region));

    store = (loraitp_store_t *)store_mem;
    if (loraitp_store_size() > sizeof(store_mem))
        die("store buffer too small", (int)loraitp_store_size());
    int rc = loraitp_store_init(store, LORAITP_STORE_DIR, cfg.keep_images);
    if (rc != LORAITP_OK)
        die("store init", rc);
    Serial.printf("store: %d image(s)\n", loraitp_store_count(store));

    loraitp_radiolib_cfg_t r;
    loraitp_radiolib_defaults(&r);
    r.pin_nss  = LORAITP_BOARD.lora_nss;
    r.pin_dio1 = LORAITP_BOARD.lora_dio1;
    r.pin_rst  = LORAITP_BOARD.lora_rst;
    r.pin_busy = LORAITP_BOARD.lora_busy;
    r.pin_sck  = LORAITP_BOARD.lora_sck;
    r.pin_miso = LORAITP_BOARD.lora_miso;
    r.pin_mosi = LORAITP_BOARD.lora_mosi;
    r.tcxo = LORAITP_BOARD.lora_tcxo;
    r.tcxo_voltage = LORAITP_BOARD.lora_tcxo_v;
    r.frequency_mhz = (float)cfg.frequency_hz / 1000000.0f;
    r.spreading_factor = cfg.spreading_factor;
    r.tx_power_dbm = cfg.tx_power_dbm;

    /*
     * A board that names an antenna-switch pin has to have it driven; one
     * that does not steers the switch from DIO2 internally. Getting this
     * wrong gives a radio that configures cleanly and hears nothing,
     * which is indistinguishable from being out of range.
     */
    if (LORAITP_BOARD.lora_ant_sw != LORAITP_PIN_NONE) {
        r.dio2_as_rf_switch = false;
        r.pin_rf_sw = LORAITP_BOARD.lora_ant_sw;
        Serial.printf("RF switch driven on GPIO%u\n", LORAITP_BOARD.lora_ant_sw);
    }

    rc = loraitp_radiolib_attach(&port, &r);
    if (rc != LORAITP_OK)
        die("radio attach", rc);
    loraitp_store_attach(&port, store);
    Serial.printf("radio up: %.3f MHz SF%u %d dBm\n",
                  (double)r.frequency_mhz, r.spreading_factor,
                  r.tx_power_dbm);

    if (cfg.role == LORAITP_ROLE_SENDER && LORAITP_BOARD.has_camera) {
        if (loraitp_camera_init())
            Serial.println("camera ready");
        else
            Serial.println("camera missing - will send a test pattern");
    }

    loraitp_webui_begin(&cfg, store, status_cb, NULL);
    xTaskCreatePinnedToCore(web_task, "web", 4096, NULL, 1, NULL, 0);

    next_run_ms = millis() + 3000;
}

/* --------------------------------------------------------------- sender */

/* Fallback when there is no camera: a deterministic pattern, so a
 * receiver can verify the transfer byte for byte. */
static uint8_t synth(uint32_t i)
{
    return (uint8_t)((i * 37u + (i >> 5) * 11u) & 0xFFu);
}

static int make_image(uint32_t *out_crc, uint16_t *w, uint16_t *h)
{
    loraitp_capture_info_t info;
    memset(&info, 0, sizeof(info));

    int n = -1;
#if LORAITP_HAS_CAMERA
    /*
     * The restart interval is one MCU row. Markers cannot be aligned to
     * chunk boundaries - DRI counts MCUs and the compressed size of an
     * interval varies - but one per row costs about 1% of the file and
     * bounds the damage from a lost packet to a couple of rows.
     */
    n = loraitp_camera_capture_jpeg(cfg.image_budget, 320 / 8,
                                    jpegbuf, sizeof(jpegbuf), &info);
#endif
    if (n <= 0) {
        n = (int)(cfg.image_budget ? cfg.image_budget : 4000);
        if (n > (int)sizeof(jpegbuf))
            n = (int)sizeof(jpegbuf);
        for (int i = 0; i < n; i++)
            jpegbuf[i] = synth((uint32_t)i);
        info.width = 320;
        info.height = 240;
        info.quality = 0;
    } else {
        Serial.printf("captured %ux%u -> %d B at Q%d\n", info.width,
                      info.height, n, info.quality);
    }

    *out_crc = loraitp_crc32(jpegbuf, (size_t)n);
    *w = info.width;
    *h = info.height;

    /* Store it, then let the core read it back out through the port. */
    if (loraitp_store_begin_write(store, (uint32_t)n) != LORAITP_OK)
        return -1;
    for (int off = 0; off < n; off += 240) {
        uint16_t k = (uint16_t)((n - off < 240) ? n - off : 240);
        port.image_write(port.ctx, (uint32_t)off, jpegbuf + off, k);
    }
    loraitp_store_meta_t m;
    memset(&m, 0, sizeof(m));
    m.img_id = (uint16_t)(millis() / 1000u);
    m.layer = 1; m.codec = 2; m.img_len = (uint32_t)n; m.crc32 = *out_crc;
    m.width = info.width; m.height = info.height;
    m.chunks_have = m.chunks_total = 1;
    m.complete = true; m.crc_ok = true;
    loraitp_store_finish(store, &m);
    return n;
}

static void run_sender(loraitp_ctx_t *ctx)
{
    uint32_t crc = 0;
    uint16_t w = 0, h = 0;
    int n = make_image(&crc, &w, &h);
    if (n <= 0) {
        snprintf(last_result, sizeof(last_result), "capture failed");
        return;
    }

    loraitp_image_desc_t d;
    memset(&d, 0, sizeof(d));
    d.img_id = (uint16_t)(millis() / 1000u);
    d.layer = 1;
    d.img_len = (uint32_t)n;
    d.codec = 2;
    d.img_crc32 = crc;
    d.width = w;
    d.height = h;

    Serial.printf("\nsending %d B\n", n);
    uint32_t t0 = millis();
    loraitp_stats_t st;
    int rc = loraitp_send_image(ctx, &d, &st);

    Serial.printf("  rc %d  %u frames  %u ms airtime  %u rounds  %u ms wall\n",
                  rc, st.frames_tx, st.airtime_ms, st.rounds, millis() - t0);
    snprintf(last_result, sizeof(last_result),
             "sent %d B, %u frames, %u ms air", n, st.frames_tx,
             st.airtime_ms);
}

/* ------------------------------------------------------------- receiver */

static void run_receiver(loraitp_ctx_t *ctx)
{
    loraitp_image_desc_t d;
    loraitp_rx_result_t result;
    loraitp_stats_t st;

    Serial.println("\nlistening...");
    int rc = loraitp_receive_image(ctx, &d, &result, &st);
    if (result == LORAITP_RX_TIMEOUT) {
        Serial.println("  nothing heard");
        return;
    }

    /* The core closed and renamed the file already; the sidecar is the
     * part only we know the contents of. */
    loraitp_store_meta_t m;
    memset(&m, 0, sizeof(m));
    m.img_id = d.img_id; m.layer = d.layer; m.codec = d.codec;
    m.img_len = d.img_len; m.crc32 = d.img_crc32;
    m.width = d.width; m.height = d.height;
    m.chunks_have = st.chunks_have; m.chunks_total = st.chunks_total;
    m.rounds = st.rounds; m.airtime_ms = st.airtime_ms;
    m.rssi_dbm = st.last_rssi_dbm; m.snr_qdb = st.last_snr_qdb;
    m.complete = (result == LORAITP_RX_COMPLETE);
    m.crc_ok = m.complete;
    loraitp_store_finish(store, &m);

    Serial.printf("  rc %d result %d  %u/%u chunks  RSSI %d dBm  SNR %.2f dB\n",
                  rc, (int)result, st.chunks_have, st.chunks_total,
                  st.last_rssi_dbm, st.last_snr_qdb / 4.0);
    Serial.printf("  saved %s\n", loraitp_store_current(store));

    /*
     * The number worth watching. Everything in SPEC.md that is still a
     * guess - the 2 s time-on-air cap above all - can only be settled by
     * loss rates measured on a real link.
     */
    snprintf(last_result, sizeof(last_result), "%u/%u chunks, RSSI %d dBm",
             st.chunks_have, st.chunks_total, st.last_rssi_dbm);
}

/* ----------------------------------------------------------------- loop */

void loop(void)
{
    if ((int32_t)(millis() - next_run_ms) < 0) {
        delay(50);
        return;
    }

    loraitp_session_cfg_t s;
    configure_session(&s);

    loraitp_ctx_t *ctx = (loraitp_ctx_t *)ctx_mem;
    if (loraitp_ctx_size() > sizeof(ctx_mem))
        die("context buffer too small", (int)loraitp_ctx_size());

    int rc = loraitp_init(ctx, &port, &s);
    if (rc != LORAITP_OK) {
        /*
         * A regulatory refusal lands here: amateur mode without a call
         * sign, a frequency outside the region, power above its ERP
         * limit. Refusing to transmit is the intended behaviour, so say
         * why and wait rather than pretending.
         */
        snprintf(last_result, sizeof(last_result),
                 "config refused by the governor: %d", rc);
        Serial.printf("%s\n", last_result);
        next_run_ms = millis() + 30000;
        return;
    }

    if (cfg.role == LORAITP_ROLE_SENDER)
        run_sender(ctx);
    else
        run_receiver(ctx);

    next_run_ms = millis() + (cfg.interval_s ? cfg.interval_s * 1000u : 60000u);
}
