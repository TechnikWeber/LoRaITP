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
#include <esp_sleep.h>

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

/*
 * Tobs from BNetzA Vfg. 91/2025: the duty cycle is measured over a
 * rolling hour. The core defines this too, in loraitp_internal.h, but
 * that header is not installed and this is the only place the
 * application needs the number.
 */
#define DUTY_WINDOW_MS 3600000u

/* Set in setup(), before anything else touches the sleep API. */
static bool woke_from_timer;

/* RTC memory survives a deep sleep; plain RAM does not. Only used to
 * report how many times the board has woken, which is the one number
 * that says whether the schedule is running at all. */
RTC_DATA_ATTR static uint32_t rtc_wakes;

/*
 * The duty-cycle window, mirrored where a reboot cannot reach it.
 *
 * An hour of airtime is an hour of airtime whether or not the board
 * restarted in the middle of it. Every way this firmware can lose RAM -
 * a settings change, a deep sleep, a watchdog, a brown-out - used to end
 * with a governor that believed the whole hourly budget was untouched,
 * and a station that transmits over its budget in good faith is still a
 * station transmitting over its budget.
 *
 * RTC_NOINIT_ATTR rather than RTC_DATA_ATTR: the latter is reloaded from
 * flash by the bootloader on every reset that is not a deep-sleep wake,
 * which is exactly the case that matters most here. Nothing in it is
 * trusted - a magic and a CRC decide whether it is a budget or whatever
 * the last firmware left in that address.
 */
#define BUDGET_MAGIC 0x4C495442u        /* 'LITB' */

RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint32_t crc;                       /* over away_ms, len and the blob */
    uint32_t away_ms;
    uint32_t len;
    uint8_t  blob[LORAITP_BUDGET_STATE_MAX];
} rtc_budget;

/*
 * Radio core only. The governor is not locked, and pruning the window
 * mutates it - the web core must never call this.
 */
static void budget_mirror(uint32_t away_ms)
{
    if (ctx == NULL)
        return;
    int n = loraitp_budget_export(ctx, rtc_budget.blob,
                                  sizeof(rtc_budget.blob));
    if (n <= 0)
        return;
    rtc_budget.len = (uint32_t)n;
    rtc_budget.away_ms = away_ms;
    rtc_budget.crc = loraitp_crc32((const uint8_t *)&rtc_budget.away_ms,
                                   2u * sizeof(uint32_t) + (size_t)n);
    rtc_budget.magic = BUDGET_MAGIC;
}

static void budget_restore(void)
{
    if (ctx == NULL)
        return;
    if (rtc_budget.magic != BUDGET_MAGIC) {
        LOG("no duty-cycle history to restore - starting with an empty hour");
        return;
    }
    if (rtc_budget.len == 0 || rtc_budget.len > sizeof(rtc_budget.blob)
        || rtc_budget.crc != loraitp_crc32(
               (const uint8_t *)&rtc_budget.away_ms,
               2u * sizeof(uint32_t) + (size_t)rtc_budget.len)) {
        LOG("duty-cycle history is not readable - starting with an empty hour");
        return;
    }
    if (loraitp_budget_import(ctx, rtc_budget.blob, rtc_budget.len,
                              rtc_budget.away_ms) != LORAITP_OK) {
        LOG("duty-cycle history refused by the core");
        return;
    }

    loraitp_budget_t b;
    loraitp_budget_query(ctx, &b);
    LOG("duty-cycle history restored: %lu ms still in the window, "
        "%lu s away", (unsigned long)b.airtime_used_ms,
        (unsigned long)(rtc_budget.away_ms / 1000u));
}
static const char *camera_state = "not fitted";
static uint32_t next_run_ms;
static int16_t last_rssi;
static int8_t last_snr;

#define LORAITP_APP_VERSION "0.1.1-alpha"

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

/*
 * The core calls this from inside a session, on the radio core, right
 * after the governor has recorded the frame - so mirroring here keeps
 * the snapshot at most one frame behind the truth. That is what makes it
 * useful for the reboots nobody schedules: a watchdog or a brown-out
 * mid-transfer still wakes up owing the airtime it already spent.
 */
static void trace_cb(void *user, const loraitp_trace_t *t)
{
    loraitp_log_trace(user, t);
    if (t->ev == LORAITP_EV_TX)
        budget_mirror(0);
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
    s->trace = trace_cb;               /* every frame reaches the web log */
    if (cfg.callsign[0])
        s->callsign = cfg.callsign;
}

/*
 * A snapshot the radio core publishes and the web core reads.
 *
 * The web server runs pinned to the other core, so having it call
 * loraitp_budget_query() directly was a data race: querying the budget
 * prunes the governor's rolling window, and the radio core writes that
 * same ring while transmitting. Two cores, shared mutable state, no lock.
 *
 * Rather than put a mutex in the core - which would make every port pay
 * for a problem this application created - the radio core copies what the
 * page needs into plain words. A torn read of a number that is only
 * displayed does no harm, and the core stays lock-free.
 */
static struct {
    uint32_t used_ms, budget_ms, bytes_left;
    uint8_t  chunk_len;
    uint16_t toa_ms;
} g_snap;

static void publish_status(void)
{
    if (ctx == NULL)
        return;
    loraitp_budget_t b;
    loraitp_budget_query(ctx, &b);
    g_snap.used_ms = b.airtime_used_ms;
    g_snap.budget_ms = b.airtime_budget_ms;
    g_snap.bytes_left = loraitp_budget_bytes_remaining(ctx);

    uint8_t pl = loraitp_max_payload_for_toa(cfg.spreading_factor,
                                             cfg.bandwidth_hz,
                                             cfg.coding_rate,
                                             LORAITP_MAX_TOA_MS * 1000u);
    g_snap.chunk_len = (pl > 4) ? (uint8_t)(pl - 4) : 1;
    g_snap.toa_ms = (uint16_t)(loraitp_time_on_air_us(
        pl, cfg.spreading_factor, cfg.bandwidth_hz, cfg.coding_rate, 8)
        / 1000u);
}

static void status_cb(void *user, loraitp_webui_status_t *out)
{
    (void)user;

    /* A throwaway context purely to ask the governor. Cheap: it holds no
     * state beyond the rolling window, which starts empty. */
    out->airtime_used_ms = g_snap.used_ms;
    out->airtime_budget_ms = g_snap.budget_ms;
    out->bytes_remaining = g_snap.bytes_left;
    out->chunk_len = g_snap.chunk_len;
    out->frame_toa_ms = g_snap.toa_ms;

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
    woke_from_timer = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    if (woke_from_timer)
        rtc_wakes++;

    Serial.begin(115200);

    /*
     * Two seconds for a USB-CDC host to enumerate, so the first lines of
     * a bring-up are not lost. On a timer wake there is nobody watching
     * and it is two seconds of a battery, so skip it.
     */
    if (!woke_from_timer)
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

    /*
     * A refusal here is not fatal, and it took a near miss to see why.
     *
     * Everything in that configuration comes from the settings page, and
     * the settings page is the only way to undo it. die() would take the
     * access point down with it, so a value the radio will not accept -
     * 27 dBm, say, which is what EU868_G3's 500 mW ERP limit invites
     * somebody to type, while the SX1262 stops at 22 - would leave no
     * way back in that does not involve a USB cable and a board off its
     * mast. The recovery mechanism has to outlive the thing it recovers
     * from.
     *
     * So the page comes up, says what the radio refused, and lets it be
     * corrected. This is the same shape as the governor refusal below;
     * the two now behave alike because they are the same kind of
     * mistake.
     */
    bool radio_up = (loraitp_radiolib_attach(&port, &r) == LORAITP_OK);
    loraitp_store_attach(&port, store);
    if (radio_up) {
        LOG("radio up: %.3f MHz SF%u BW%lu CR4/%u %d dBm sync %02X",
            (double)r.frequency_mhz, r.spreading_factor,
            (unsigned long)cfg.bandwidth_hz / 1000u, cfg.coding_rate + 4,
            r.tx_power_dbm, cfg.sync_word);
    } else {
        snprintf(last_result, sizeof(last_result),
                 "radio refused this configuration (RadioLib %d)",
                 loraitp_radiolib_last_error());
        LOG("%s - check frequency, bandwidth, spreading factor and TX "
            "power; the SX1262 accepts -9 to 22 dBm and 150-960 MHz",
            last_result);
    }

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
    rc = radio_up ? loraitp_init(ctx, &port, &s) : LORAITP_E_RADIO;
    if (rc != LORAITP_OK) {
        /* A regulatory refusal lands here: amateur mode without a call
         * sign, a frequency outside the region, power above its ERP
         * limit. Refusing is the intended behaviour, so keep the access
         * point up and say why rather than dying silently. */
        ctx = NULL;
        if (radio_up) {
            snprintf(last_result, sizeof(last_result),
                     "refused by the duty-cycle governor (error %d)", rc);
            LOG("%s - check region, frequency, power and call sign",
                last_result);
        }
    } else {
        /* Before anything transmits: what this board already owes from
         * before the reboot. */
        budget_restore();
    }

    /*
     * On a scheduled wake the access point stays down.
     *
     * That is the whole point of deep sleep: WiFi draws 100-150 mA, more
     * than the radio does while transmitting, so bringing it up for every
     * picture would give back most of what the sleep saved. The board
     * wakes, sends and powers down again without ever being visible.
     *
     * RESET is the way back in. Any boot that is not a timer wake - the
     * button, a power cycle, a fresh flash - brings the page up as
     * normal, which is also what makes the setting reversible.
     */
    bool headless = woke_from_timer && cfg.deep_sleep;
    if (headless) {
        LOG("scheduled wake #%lu - access point stays down, press RESET "
            "for the web page", (unsigned long)rtc_wakes);
    } else {
        loraitp_webui_begin(&cfg, store, status_cb, trigger_cb, NULL);
        xTaskCreatePinnedToCore(web_task, "web", 4096, NULL, 1, NULL, 0);
    }

    publish_status();

    /* Three seconds for the access point to settle before the radio takes
     * the CPU for minutes at a time. Nothing to settle when there is no
     * access point. */
    next_run_ms = millis() + (headless ? 0u : 3000u);
}

/*
 * Power the board down until the next transfer is due.
 *
 * Deep sleep ends in a reboot, and that is the difficulty: the
 * duty-cycle governor's rolling window lives in RAM, so a board that
 * slept through it wakes up believing its whole hourly budget is
 * untouched. On a 1% or 10% band that is not a cosmetic error, it is an
 * offence - and it is the same bug the firmware already had once, when
 * the window was rebuilt before every transfer.
 *
 * Rather than move the window into RTC memory and re-base every
 * timestamp against a millis() that restarts at zero, sleep only when
 * the answer does not matter: either the band has no limit and there is
 * no window to lose, or the sleep is longer than the window itself, in
 * which case everything recorded before it has aged out by the time the
 * board wakes. Both cases are exact rather than approximate.
 *
 * A pause too short for that is simply spent awake. Deep sleep is for
 * the node that sends a picture every few hours, which is the only case
 * where it is worth anything.
 */
static void maybe_deep_sleep(uint32_t sleep_ms)
{
    if (!cfg.deep_sleep || cfg.role != LORAITP_ROLE_SENDER)
        return;

    uint32_t floor_ms = g_snap.budget_ms ? DUTY_WINDOW_MS : 20000u;
    if (sleep_ms < floor_ms) {
        LOG("staying awake for %lu s: %s", (unsigned long)(sleep_ms / 1000u),
            g_snap.budget_ms
              ? "shorter than the duty-cycle window, and the airtime "
                "already used has to be remembered"
              : "too short to be worth a reboot");
        return;
    }

    LOG("deep sleep for %lu s - the access point goes with it",
        (unsigned long)(sleep_ms / 1000u));

    /* Record how long the window is about to go unwatched, so what
     * aged out while the board was off is not counted on the way back. */
    budget_mirror(sleep_ms);
    Serial.flush();

    if (port.radio_sleep)
        port.radio_sleep(port.ctx);      /* the SX1262 has its own sleep */

    esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ull);
    esp_deep_sleep_start();              /* does not return */
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
     * 0 asks for one restart marker per row of blocks, derived from the
     * frame the sensor actually returns. Markers cannot be aligned to
     * chunk boundaries - DRI counts MCUs and the compressed size of an
     * interval varies with the picture - but one per row costs about 1%
     * of the file and cuts the damage from a lost packet from 72 rows to
     * 16, measured.
     */
    n = loraitp_camera_capture_jpeg(cfg.image_budget, 0,
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

    /*
     * The core fills these on every path it finishes, but it has two it
     * does not: a port that fails validation, and an image_begin that
     * refuses - a full filesystem, most plausibly. Both return before
     * writing a byte here, and reading the stack that was left behind
     * put invented frame counts on the status page and "sent 8000 B" in
     * a line where nothing had been sent. The page exists to be believed.
     */
    loraitp_stats_t st;
    memset(&st, 0, sizeof(st));
    int rc = loraitp_send_image(ctx, &d, &st);
    if (rc != LORAITP_OK) {
        snprintf(last_result, sizeof(last_result),
                 "send refused (error %d) after %u frames", rc, st.frames_tx);
        LOG("%s", last_result);
        return;
    }

    LOG("sent: rc %d, %u frames, %u ms airtime, %u round(s), %u ms wall",
        rc, st.frames_tx, st.airtime_ms, st.rounds, millis() - t0);
    snprintf(last_result, sizeof(last_result),
             "sent %d B, %u frames, %u ms air", n, st.frames_tx,
             st.airtime_ms);
}

/* ------------------------------------------------------------- receiver */

static void run_receiver(loraitp_ctx_t *ctx)
{
    /* Zeroed for the same reason the sender's are: the core has an
     * early return that writes none of them, and a sidecar assembled
     * from whatever was on the stack is worse than no sidecar. */
    loraitp_image_desc_t d;
    loraitp_rx_result_t result = LORAITP_RX_TIMEOUT;
    loraitp_stats_t st;
    memset(&d, 0, sizeof(d));
    memset(&st, 0, sizeof(st));

    LOG("listening...");
    int rc = loraitp_receive_image(ctx, &d, &result, &st);
    if (rc != LORAITP_OK) {
        snprintf(last_result, sizeof(last_result),
                 "receive refused (error %d)", rc);
        LOG("%s - the radio or the port is not usable", last_result);
        delay(1000);          /* do not spin on a fault that will persist */
        return;
    }
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
        publish_status();     /* keep the page's numbers fresh while idle */

        /*
         * Re-mirror the window every ten seconds while idle. The entries
         * are stored as ages, so a snapshot left over from the last
         * transmission makes the airtime look younger than it is, which
         * only ever over-restricts - but a settings restart an hour
         * after a transfer would then carry a window that should long
         * since have emptied. Ten seconds keeps it honest for nothing.
         */
        static uint32_t last_mirror;
        if (millis() - last_mirror >= 10000u) {
            last_mirror = millis();
            budget_mirror(0);
        }

        delay(200);
        return;
    }

    if (ctx == NULL) {
        /* Configuration was refused at startup. The web page is up and
         * says so; there is nothing useful to do on the radio.
         *
         * Unless nobody can read it: after a scheduled wake there is no
         * access point either, and spinning here would flatten the
         * battery within a day without fixing anything. Sleep the
         * interval instead - a refused configuration is still refused
         * when the board wakes, but the cell survives long enough for
         * someone to walk out and press RESET. */
        next_run_ms = millis() + 30000;
        maybe_deep_sleep(cfg.interval_s ? cfg.interval_s * 1000u : 60000u);
        return;
    }

    if (cfg.role == LORAITP_ROLE_SENDER) {
        uint32_t started = millis();
        run_sender(ctx);
        publish_status();

        /*
         * The interval is a period, not a gap. A 10 kB picture at SF12 on
         * a 10% band takes 72 minutes of wall clock, and measuring the
         * wait from the end of that would turn "every six hours" into
         * "every seven and a bit" - with the error accumulating, so a
         * daily picture walks right around the clock over a month. Count
         * from the moment the transfer started instead.
         *
         * If a transfer overran the period entirely, start the next one
         * now rather than trying to catch up on a schedule the link has
         * already shown it cannot keep.
         */
        uint32_t period_ms = cfg.interval_s ? cfg.interval_s * 1000u : 60000u;
        next_run_ms = started + period_ms;
        if ((int32_t)(millis() - next_run_ms) >= 0)
            next_run_ms = millis();
        else
            maybe_deep_sleep(next_run_ms - millis());
        return;
    }

    run_receiver(ctx);
    publish_status();

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
