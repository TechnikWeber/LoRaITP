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

#include "board.h"
#include "loraitp.h"
#include "port_radiolib.h"

/* Flash one board with -DLORAITP_ROLE_SENDER, the other without. */
#ifdef LORAITP_ROLE_SENDER
static const bool IS_SENDER = true;
#else
static const bool IS_SENDER = false;
#endif

/* A synthetic 4 kB "image". Deterministic, so the receiver can verify it
 * byte for byte without needing a camera. */
static uint8_t image[4000];
static uint8_t received[sizeof(image)];

/* Erasure coding needs a whole block resident. The core allocates
 * nothing, so the buffer is ours to provide and ours to size. */
static uint8_t fec_scratch[24 * 1024];

static uint8_t ctx_mem[8 * 1024];
static loraitp_port_t port;

static int img_read(void *, uint32_t off, uint8_t *b, uint16_t n)
{
    if (off + n > sizeof(image)) return LORAITP_E_IO;
    memcpy(b, image + off, n);
    return LORAITP_OK;
}

static int img_write(void *, uint32_t off, const uint8_t *b, uint16_t n)
{
    if (off + n > sizeof(received)) return LORAITP_E_IO;
    memcpy(received + off, b, n);
    return LORAITP_OK;
}

static void fill_image(void)
{
    for (size_t i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)((i * 37 + (i >> 5) * 11) & 0xFF);
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

    fill_image();

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

    int rc = loraitp_radiolib_attach(&port, &rcfg);
    if (rc != LORAITP_OK)
        die("radio attach", rc);

    port.image_read = img_read;
    port.image_write = img_write;

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
        loraitp_image_desc_t desc;
        memset(&desc, 0, sizeof(desc));
        desc.img_id = (uint16_t)(millis() / 1000u);
        desc.layer = 1;
        desc.img_len = sizeof(image);
        desc.codec = 2;
        desc.img_crc32 = loraitp_crc32(image, sizeof(image));
        desc.width = 320;
        desc.height = 240;

        Serial.printf("\nsending %u B, image %u\n",
                      (unsigned)desc.img_len, desc.img_id);
        rc = loraitp_send_image(ctx, &desc, &st);
        Serial.printf("  rc %d  %u frames  %u ms airtime  %u rounds  "
                      "%u retransmits  %u ms wall\n",
                      rc, st.frames_tx, st.airtime_ms, st.rounds,
                      st.retransmits, millis() - t0);
        delay(10000);
    } else {
        memset(received, 0, sizeof(received));
        loraitp_image_desc_t desc;
        loraitp_rx_result_t result;

        Serial.println("\nlistening...");
        rc = loraitp_receive_image(ctx, &desc, &result, &st);
        if (result == LORAITP_RX_TIMEOUT) {
            Serial.println("  nothing heard");
            return;
        }

        bool ok = (loraitp_crc32(received, desc.img_len) == desc.img_crc32);
        Serial.printf("  rc %d  result %d  %u/%u chunks  %u frames  "
                      "RSSI %d dBm  SNR %.2f dB\n",
                      rc, (int)result, st.chunks_have, st.chunks_total,
                      st.frames_rx, st.last_rssi_dbm,
                      st.last_snr_qdb / 4.0);
        Serial.printf("  image %u: %s\n", desc.img_id,
                      ok ? "CRC OK" : "CRC MISMATCH or incomplete");

        /*
         * The number worth watching. Everything in SPEC.md that is still
         * a guess - the 2 s time-on-air cap above all - can only be
         * settled by loss rates measured on a real link.
         */
        if (st.chunks_total)
            Serial.printf("  loss: %u of %u chunks needed repair\n",
                          (unsigned)(st.chunks_total - st.chunks_have),
                          st.chunks_total);
    }
}
