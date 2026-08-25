/*
 * Test and simulator port.
 *
 * An in-memory radio and image store, plus a software AES-128 so the
 * CMAC path can be exercised on a host. The software AES lives here
 * rather than in the core deliberately: every part LoRaITP targets has
 * AES in hardware, and a fallback inside the core would let somebody
 * ship a slow one without noticing. A test port is exactly where it
 * belongs.
 */
#include <string.h>

#include <stdbool.h>
#include <stddef.h>

#include "loraitp_port.h"
#include "port_sim.h"

/* ------------------------------------------------------------- AES-128 */

static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const uint8_t rcon[10] = {1,2,4,8,16,32,64,128,0x1b,0x36};

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

void loraitp_sim_aes128(const uint8_t key[16], const uint8_t in[16],
                        uint8_t out[16])
{
    uint8_t w[176], s[16];
    memcpy(w, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, w + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t x = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
        }
        for (int j = 0; j < 4; j++)
            w[i * 4 + j] = (uint8_t)(w[(i - 4) * 4 + j] ^ t[j]);
    }

    memcpy(s, in, 16);
    for (int j = 0; j < 16; j++) s[j] ^= w[j];

    for (int rnd = 1; rnd <= 10; rnd++) {
        for (int j = 0; j < 16; j++) s[j] = sbox[s[j]];
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = s[((c + r) % 4) * 4 + r];
        memcpy(s, t, 16);
        if (rnd != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *p = s + c * 4;
                uint8_t a = p[0] ^ p[1] ^ p[2] ^ p[3];
                uint8_t o0 = p[0];
                p[0] ^= a ^ xtime((uint8_t)(p[0] ^ p[1]));
                p[1] ^= a ^ xtime((uint8_t)(p[1] ^ p[2]));
                p[2] ^= a ^ xtime((uint8_t)(p[2] ^ p[3]));
                p[3] ^= a ^ xtime((uint8_t)(p[3] ^ o0));
            }
        }
        for (int j = 0; j < 16; j++) s[j] ^= w[rnd * 16 + j];
    }
    memcpy(out, s, 16);
}

/* -------------------------------------------------------- the sim port */

#define SIM_QUEUE 256

typedef struct {
    uint8_t  buf[LORAITP_SIM_MAX_FRAME];
    uint8_t  len;
} sim_frame_t;

struct loraitp_sim {
    uint32_t now_ms;
    uint8_t  key[16];
    bool     have_key;

    /*
     * Separate directions. A single queue makes the endpoint receive its
     * own transmissions, which looks like a working loopback right up
     * until it silently eats the frames a test meant to hand to the
     * other side.
     */
    sim_frame_t txq[SIM_QUEUE];
    int       tx_head, tx_tail;
    sim_frame_t rxq[SIM_QUEUE];
    int       rx_head, rx_tail;

    uint8_t  *image;
    uint32_t  image_len;

    uint32_t  frames_sent;
    uint32_t  airtime_ms;
};

static int sim_configure(void *ctx, const loraitp_radio_cfg_t *cfg)
{ (void)ctx; (void)cfg; return LORAITP_OK; }

static int sim_send(void *ctx, const uint8_t *buf, uint8_t len,
                    uint32_t *toa_ms)
{
    struct loraitp_sim *s = ctx;
    if (s->tx_tail - s->tx_head >= SIM_QUEUE)
        return LORAITP_E_RADIO;
    sim_frame_t *f = &s->txq[s->tx_tail++ % SIM_QUEUE];
    memcpy(f->buf, buf, len);
    f->len = len;
    s->now_ms += *toa_ms;
    s->airtime_ms += *toa_ms;
    s->frames_sent++;
    return LORAITP_OK;
}

static int sim_receive(void *ctx, uint8_t *buf, uint8_t cap,
                       uint32_t timeout_ms, loraitp_rx_meta_t *meta)
{
    struct loraitp_sim *s = ctx;
    if (s->rx_head == s->rx_tail) {
        s->now_ms += timeout_ms;
        return 0;                       /* timeout */
    }
    sim_frame_t *f = &s->rxq[s->rx_head++ % SIM_QUEUE];
    uint8_t n = f->len < cap ? f->len : cap;
    memcpy(buf, f->buf, n);
    if (meta) {
        meta->rssi_dbm = -98;
        meta->snr_qdb = -20;
        meta->timestamp_ms = s->now_ms;
    }
    return n;
}

static int sim_sleep_radio(void *ctx) { (void)ctx; return LORAITP_OK; }
static uint32_t sim_now(void *ctx) { return ((struct loraitp_sim *)ctx)->now_ms; }
static void sim_sleep(void *ctx, uint32_t ms) { ((struct loraitp_sim *)ctx)->now_ms += ms; }

static int sim_random(void *ctx, uint8_t *out, size_t len)
{
    struct loraitp_sim *s = ctx;
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)(0xA5u ^ (s->now_ms + i));
    return LORAITP_OK;
}

static int sim_image_read(void *ctx, uint32_t off, uint8_t *buf, uint16_t len)
{
    struct loraitp_sim *s = ctx;
    if (off + len > s->image_len)
        return LORAITP_E_IO;
    memcpy(buf, s->image + off, len);
    return LORAITP_OK;
}

static int sim_image_write(void *ctx, uint32_t off, const uint8_t *buf,
                           uint16_t len)
{
    struct loraitp_sim *s = ctx;
    if (off + len > s->image_len)
        return LORAITP_E_IO;
    memcpy(s->image + off, buf, len);
    return LORAITP_OK;
}

static int sim_aes(void *ctx, const uint8_t in[16], uint8_t out[16])
{
    struct loraitp_sim *s = ctx;
    if (!s->have_key)
        return LORAITP_E_NOSUP;
    loraitp_sim_aes128(s->key, in, out);
    return LORAITP_OK;
}

void loraitp_sim_port(loraitp_port_t *port, struct loraitp_sim *sim)
{
    memset(port, 0, sizeof(*port));
    port->radio_configure = sim_configure;
    port->radio_send = sim_send;
    port->radio_receive = sim_receive;
    port->radio_sleep = sim_sleep_radio;
    port->now_ms = sim_now;
    port->sleep_ms = sim_sleep;
    port->random_bytes = sim_random;
    port->image_read = sim_image_read;
    port->image_write = sim_image_write;
    port->aes128_encrypt_block = sim->have_key ? sim_aes : NULL;
    port->ctx = sim;
}

size_t loraitp_sim_size(void) { return sizeof(struct loraitp_sim); }

/*
 * Returns NULL when the caller's buffer is too small. The first version
 * took no length and happily memset a megabyte into an 8 kB buffer - an
 * API that lets the caller get this wrong is the bug, not the caller.
 */
struct loraitp_sim *loraitp_sim_new(void *mem, size_t mem_len, uint8_t *image,
                                    uint32_t image_len, const uint8_t key[16])
{
    struct loraitp_sim *s = mem;
    if (mem == NULL || mem_len < sizeof(*s))
        return NULL;
    memset(s, 0, sizeof(*s));
    s->image = image;
    s->image_len = image_len;
    if (key) { memcpy(s->key, key, 16); s->have_key = true; }
    return s;
}

uint32_t loraitp_sim_frames(const struct loraitp_sim *s) { return s->frames_sent; }

/*
 * Hand everything one sim transmitted to another's receive queue. Lets a
 * test run a whole sender, then replay its output into a receiver
 * without needing two processes or a scheduler.
 */
void loraitp_sim_splice(struct loraitp_sim *dst, struct loraitp_sim *src)
{
    while (src->tx_head != src->tx_tail) {
        sim_frame_t *f = &src->txq[src->tx_head++ % SIM_QUEUE];
        if (dst->rx_tail - dst->rx_head >= SIM_QUEUE)
            break;
        dst->rxq[dst->rx_tail++ % SIM_QUEUE] = *f;
    }
}
