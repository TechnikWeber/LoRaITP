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
#include "debuglog.h"
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
static loraitp_ctx_t *ctx;
static uint8_t jpegbuf[32 * 1024];

/* Erasure coding needs a whole block resident. The core allocates
 * nothing, so the buffer is ours to provide and ours to size. Only
 * broadcast mode uses it. */
static uint8_t fec_scratch[24 * 1024];

static char last_result[64] = "nothing yet";
static const char *camera_state = "not fitted";
static uint32_t next_run_ms;
static int16_t last_rssi;
static int8_t last_snr;

#define LORAITP_APP_VERSION "0.1.0"

/* What the demodulator needs, per spreading factor, in dB. */
static float required_snr(uint8_t sf)
{
    static const float t[13] = { 0,0,0,0,0,0,0, -7.5f,-10.f,-12.5f,
                                 -15.f,-17.5f,-20.f };
    return (sf >= 7 && sf <= 12) ? t[sf] : -20.0f;
}

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
    s->spreading_factor = cfg.spreading_factor;
    s->coding_rate = 1;
    s->tx_power_dbm = cfg.tx_power_dbm;
    s->parity_percent = cfg.parity_percent;
    s->session_timeout_ms = 10u * 60u * 1000u;
    s->bandwidth_hz = cfg.bandwidth_hz;
    s->coding_rate = cfg.coding_rate;
    s->fec_scratch = fec_scratch;
    s->fec_scratch_len = sizeof(fec_scratch);
    s->trace = loraitp_log_trace;      /* every frame reaches the web log */
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
    /*
     * Ask the live context, not a fresh one.
     *
     * The governor's rolling window lives in the context, and creating a
     * new one to answer a status request would report an empty window -
     * always 0% used, however much had just been transmitted. Worse, the
     * loop used to re-init before every transfer, which reset the window
     * for real: each session would believe it had the whole hourly budget
     * to itself, and the duty-cycle accounting the whole design rests on
     * would have been decoration.
     */
    if (ctx != NULL) {
        loraitp_budget_t b;
        loraitp_budget_query(ctx, &b);
        out->airtime_used_ms = b.airtime_used_ms;
        out->airtime_budget_ms = b.airtime_budget_ms;
        out->bytes_remaining = loraitp_budget_bytes_remaining(ctx);

        uint8_t pl = loraitp_max_payload_for_toa(cfg.spreading_factor,
                                                 cfg.bandwidth_hz,
                                                 cfg.coding_rate,
                                                 LORAITP_MAX_TOA_MS * 1000u);
        out->chunk_len = (pl > 4) ? (uint8_t)(pl - 4) : 1;
        out->frame_toa_ms = (uint16_t)(loraitp_time_on_air_us(
            pl, cfg.spreading_factor, cfg.bandwidth_hz,
            cfg.coding_rate, 8) / 1000u);
    }
    out->duty_percent = (out->airtime_budget_ms
                         ? (uint8_t)((out->airtime_budget_ms * 100u) / 3600000u)
                         : 0);

    uint32_t now = millis();
    out->next_run_ms = ((int32_t)(next_run_ms - now) > 0)
                       ? (next_run_ms - now) : 0;
    out->last_rssi_dbm = last_rssi;
    out->last_snr_qdb = last_snr;
    out->link_margin_db = last_rssi
        ? (last_snr / 4.0f) - required_snr(cfg.spreading_factor) : 0.0f;

    out->last_result = last_result;
    out->camera = camera_state;
    out->board = LORAITP_BOARD.name;
    out->version = LORAITP_APP_VERSION;
}

static void trigger_cb(void *user)
{
    (void)user;
    next_run_ms = millis();      /* the loop picks this up on its next pass */
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

    loraitp_log_set_level(cfg.log_level);
    LOG("LoRaITP %s on %s - %s, region %s", LORAITP_APP_VERSION,
        LORAITP_BOARD.name,
        cfg.role == LORAITP_ROLE_SENDER ? "SENDER" : "RECEIVER",
        loraitp_cfg_region_name(cfg.region));

    store = (loraitp_store_t *)store_mem;
    if (loraitp_store_size() > sizeof(store_mem))
        die("store buffer too small", (int)loraitp_store_size());
    int rc = loraitp_store_init(store, LORAITP_STORE_DIR, cfg.keep_images);
    if (rc != LORAITP_OK)
        die("store init", rc);
    LOG("store: %d image(s) on flash", loraitp_store_count(store));

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
    r.bandwidth_khz = (float)cfg.bandwidth_hz / 1000.0f;
    r.spreading_factor = cfg.spreading_factor;
    r.coding_rate = (uint8_t)(cfg.coding_rate + 4u);
    r.sync_word = cfg.sync_word;
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
        r.rf_sw_inverted = cfg.rf_sw_invert;
        LOG("RF switch on GPIO%u, high to %s", LORAITP_BOARD.lora_ant_sw,
            cfg.rf_sw_invert ? "receive (inverted)" : "transmit");
    }

    rc = loraitp_radiolib_attach(&port, &r);
    if (rc != LORAITP_OK)
        die("radio attach", rc);
    loraitp_store_attach(&port, store);
    LOG("radio up: %.3f MHz SF%u BW%lu CR4/%u %d dBm sync %02X",
        (double)r.frequency_mhz, r.spreading_factor,
        (unsigned long)cfg.bandwidth_hz / 1000u, cfg.coding_rate + 4,
        r.tx_power_dbm, cfg.sync_word);

    if (cfg.role == LORAITP_ROLE_SENDER && LORAITP_BOARD.has_camera) {
        if (loraitp_camera_init()) {
            camera_state = "ready";
            LOG("camera ready");
        } else {
            camera_state = "not detected - sending a test pattern";
            LOG("no camera - will send a test pattern instead");
        }
    }

    /*
     * One context for the life of the firmware. Settings changes reboot
     * the board, so there is never a reason to rebuild it - and rebuilding
     * it would throw away the duty-cycle history.
     */
    ctx = (loraitp_ctx_t *)ctx_mem;
    if (loraitp_ctx_size() > sizeof(ctx_mem))
        die("context buffer too small", (int)loraitp_ctx_size());

    loraitp_session_cfg_t s;
    configure_session(&s);
    rc = loraitp_init(ctx, &port, &s);
    if (rc != LORAITP_OK) {
        /* A regulatory refusal lands here: amateur mode without a call
         * sign, a frequency outside the region, power above its ERP
         * limit. Refusing is the intended behaviour, so keep the access
         * point up and say why rather than dying silently. */
        ctx = NULL;
        snprintf(last_result, sizeof(last_result),
                 "refused by the duty-cycle governor (error %d)", rc);
        LOG("%s - check region, frequency, power and call sign", last_result);
    }

    loraitp_webui_begin(&cfg, store, status_cb, trigger_cb, NULL);
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
        LOG("captured %ux%u -> %d B at quality %d", info.width, info.height,
            n, info.quality);
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

    LOG("sending %d B", n);
    uint32_t t0 = millis();
    loraitp_stats_t st;
    int rc = loraitp_send_image(ctx, &d, &st);

    LOG("sent: rc %d, %u frames, %u ms airtime, %u round(s), %u ms wall",
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

    LOG("listening...");
    int rc = loraitp_receive_image(ctx, &d, &result, &st);
    if (result == LORAITP_RX_TIMEOUT) {
        LOG("nothing heard in this window");
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

    last_rssi = st.last_rssi_dbm;
    last_snr = st.last_snr_qdb;
    LOG("received: rc %d, result %d, %u/%u chunks, RSSI %d dBm, SNR %.2f dB",
        rc, (int)result, st.chunks_have, st.chunks_total,
        st.last_rssi_dbm, st.last_snr_qdb / 4.0);
    LOG("saved %s", loraitp_store_current(store));

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

    if (ctx == NULL) {
        /* Configuration was refused at startup. The web page is up and
         * says so; there is nothing useful to do on the radio. */
        next_run_ms = millis() + 30000;
        return;
    }

    if (cfg.role == LORAITP_ROLE_SENDER) {
        run_sender(ctx);
        next_run_ms = millis()
                      + (cfg.interval_s ? cfg.interval_s * 1000u : 60000u);
        return;
    }

    run_receiver(ctx);

    /*
     * A receiver goes straight back to listening. The interval is the
     * sender's business: applying it here would leave the radio deaf for
     * five minutes out of every fifteen, and since the two ends have no
     * common clock the gap would land wherever it liked. Most transfers
     * would simply be missed, and the symptom - "sometimes it works" - is
     * about the worst one to have to diagnose over a 30 km link.
     */
    next_run_ms = millis();
}
