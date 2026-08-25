/*
 * Sender and receiver state machines.  SPEC.md 4 and 5.
 *
 * Blocking, single-threaded, no allocation. The port's radio_send blocks
 * until the frame has left the antenna, so the flow reads top to bottom
 * rather than as a callback maze.
 */
#include <string.h>

#include "loraitp_internal.h"

#define DEFAULT_MAX_ROUNDS 8
#define DEFAULT_EOB_RETRY  3

/* --------------------------------------------------------- PHY timing */

uint32_t loraitp_time_on_air_us(uint8_t pl, uint8_t sf, uint32_t bw_hz,
                                uint8_t cr, uint8_t preamble)
{
    if (sf < 6 || sf > 12 || bw_hz == 0 || cr < 1 || cr > 4)
        return 0;

    /* Symbol time in nanoseconds, to keep the rounding honest at SF12. */
    uint64_t t_sym_ns = ((uint64_t)(1u << sf) * 1000000000ull) / bw_hz;
    int de = (t_sym_ns > 16380000ull) ? 1 : 0;   /* Semtech mandates LDRO */

    int32_t num = 8 * (int32_t)pl - 4 * (int32_t)sf + 28 + 16;
    int32_t den = 4 * ((int32_t)sf - 2 * de);
    int32_t blocks = (num + den - 1) / den;      /* ceil */
    if (blocks < 0)
        blocks = 0;
    uint32_t n_payload = 8u + (uint32_t)blocks * (cr + 4u);

    uint64_t preamble_ns = (t_sym_ns * (4u * preamble + 17u)) / 4u;
    uint64_t total_ns = preamble_ns + t_sym_ns * n_payload;
    return (uint32_t)(total_ns / 1000ull);
}

uint8_t loraitp_max_payload_for_toa(uint8_t sf, uint32_t bw_hz, uint8_t cr,
                                    uint32_t toa_us)
{
    uint8_t best = 1;
    for (unsigned pl = 1; pl <= LORAITP_MAX_FRAME; pl++) {
        if (loraitp_time_on_air_us((uint8_t)pl, sf, bw_hz, cr, 8) <= toa_us)
            best = (uint8_t)pl;
        else
            break;
    }
    return best;
}

/* Required SNR per SF, in quarter-dB. A property of the demodulator. */
static const int16_t req_snr_qdb[13] = {
    0, 0, 0, 0, 0, 0, 0, -30, -40, -50, -60, -70, -80
};

uint8_t loraitp_choose_sf(int16_t snr_qdb, int16_t margin_qdb)
{
    for (uint8_t sf = 7; sf <= 12; sf++)
        if (snr_qdb - req_snr_qdb[sf] >= margin_qdb)
            return sf;
    return 12;
}

/* ------------------------------------------------------------- context */

size_t loraitp_ctx_size(void) { return sizeof(struct loraitp_ctx); }

int loraitp_port_validate(const loraitp_port_t *p, int is_sender)
{
    if (p == NULL || p->radio_send == NULL || p->radio_receive == NULL
        || p->now_ms == NULL || p->sleep_ms == NULL)
        return LORAITP_E_ARG;
    if (is_sender && p->image_read == NULL)
        return LORAITP_E_ARG;
    if (!is_sender && p->image_write == NULL)
        return LORAITP_E_ARG;
    return LORAITP_OK;
}

int loraitp_init(loraitp_ctx_t *ctx, const loraitp_port_t *port,
                 const loraitp_session_cfg_t *cfg)
{
    if (ctx == NULL || port == NULL || cfg == NULL)
        return LORAITP_E_ARG;

    memset(ctx, 0, sizeof(*ctx));
    ctx->port = port;
    ctx->cfg = *cfg;

    if (ctx->cfg.bandwidth_hz == 0)
        ctx->cfg.bandwidth_hz = 125000u;
    if (ctx->cfg.coding_rate == 0)
        ctx->cfg.coding_rate = 1;
    if (ctx->cfg.spreading_factor == 0)
        ctx->cfg.spreading_factor = 10;
    if (ctx->cfg.block_size == 0)
        ctx->cfg.block_size = LORAITP_BLOCK_SIZE;
    if (ctx->cfg.block_size > LORAITP_BLOCK_SIZE)
        return LORAITP_E_ARG;
    if (ctx->cfg.max_rounds == 0)
        ctx->cfg.max_rounds = DEFAULT_MAX_ROUNDS;
    if (ctx->cfg.eob_retry == 0)
        ctx->cfg.eob_retry = DEFAULT_EOB_RETRY;
    if (ctx->cfg.chunk_len == 0) {
        uint8_t pl = loraitp_max_payload_for_toa(ctx->cfg.spreading_factor,
                                                 ctx->cfg.bandwidth_hz,
                                                 ctx->cfg.coding_rate,
                                                 LORAITP_MAX_TOA_MS * 1000u);
        ctx->cfg.chunk_len = (pl > LORAITP_DATA_HDR)
                             ? (uint8_t)(pl - LORAITP_DATA_HDR) : 1u;
    }
    if (ctx->cfg.mode == LORAITP_MODE_BROADCAST
        && ctx->cfg.parity_percent == 0)
        return LORAITP_E_ARG;   /* broadcast without FEC cannot work */

    return loraitp_gov_init(&ctx->gov, &ctx->cfg);
}

size_t loraitp_fec_session_scratch(uint16_t k, uint16_t r, uint16_t chunk_len)
{
    return (size_t)(k + r) * chunk_len + loraitp_fec_scratch(k) + chunk_len;
}

/* ----------------------------------------------------------- plumbing */

static bool mac_data(const loraitp_ctx_t *c)
{
    return (c->cfg.flags & LORAITP_CFG_MAC_DATA) != 0;
}

/*
 * The only path to the radio. IDENT injection lives here so that an
 * application cannot suppress it (SPEC.md 6.4), and so does the
 * duty-cycle wait.
 */
static int transmit(loraitp_ctx_t *c, size_t len, int depth)
{
    const loraitp_port_t *p = c->port;

    if (depth == 0 && loraitp_gov_ident_due(&c->gov, p->now_ms(p->ctx))) {
        uint8_t save[sizeof(c->txbuf)];
        size_t save_len = len;
        memcpy(save, c->txbuf, len);

        const char *cs = c->cfg.callsign;
        size_t n = strlen(cs);
        if (n > LORAITP_MAX_FRAME - 2u)
            n = LORAITP_MAX_FRAME - 2u;
        int il = loraitp_encode_simple(LORAITP_IDENT, c->sid,
                                       (const uint8_t *)cs, (uint8_t)n,
                                       c->txbuf, sizeof(c->txbuf));
        if (il > 0) {
            loraitp_gov_ident_sent(&c->gov, p->now_ms(p->ctx));
            (void)transmit(c, (size_t)il, 1);
        }
        memcpy(c->txbuf, save, save_len);
        len = save_len;
    }

    int sealed = loraitp_seal(p, c->nonce, mac_data(c), c->txbuf, len,
                              sizeof(c->txbuf));
    if (sealed < 0)
        return sealed;
    len = (size_t)sealed;

    uint32_t toa_est = loraitp_time_on_air_us((uint8_t)len,
                                              c->cfg.spreading_factor,
                                              c->cfg.bandwidth_hz,
                                              c->cfg.coding_rate, 8) / 1000u;
    uint32_t now = p->now_ms(p->ctx);
    uint32_t wait = loraitp_gov_delay_ms(&c->gov, now, toa_est);
    if (wait)
        p->sleep_ms(p->ctx, wait);

    uint32_t toa_ms = toa_est;
    int rc = p->radio_send(p->ctx, c->txbuf, (uint8_t)len, &toa_ms);
    if (rc != LORAITP_OK)
        return rc;

    loraitp_gov_record(&c->gov, p->now_ms(p->ctx), toa_ms);
    c->stats.airtime_ms += toa_ms;
    c->stats.frames_tx++;
    return LORAITP_OK;
}

/* Receive one valid frame. Returns its type, or a negative error. */
static int receive(loraitp_ctx_t *c, uint32_t timeout_ms,
                   loraitp_rx_meta_t *meta, size_t *out_len)
{
    const loraitp_port_t *p = c->port;
    int n = p->radio_receive(p->ctx, c->rxbuf, sizeof(c->rxbuf),
                             timeout_ms, meta);
    if (n <= 0)
        return (n == 0) ? LORAITP_E_TIMEOUT : n;

    int body = loraitp_unseal(p, c->nonce, mac_data(c), c->rxbuf, (size_t)n);
    if (body < 0) {
        /* A bad MAC is not an error condition - it is somebody else's
         * traffic on a shared band, or a forgery. Either way the frame
         * simply did not arrive. */
        c->stats.mac_rejects++;
        return LORAITP_E_TIMEOUT;
    }
    *out_len = (size_t)body;
    c->stats.frames_rx++;
    return loraitp_frame_type(c->rxbuf, (size_t)body);
}

/* ------------------------------------------------------------- sender */

static int send_meta(loraitp_ctx_t *c)
{
    int len = loraitp_encode_meta(&c->meta, c->txbuf, sizeof(c->txbuf));
    if (len < 0)
        return len;
    return transmit(c, (size_t)len, 0);
}

static int send_chunk(loraitp_ctx_t *c, uint16_t seq, bool parity,
                      const uint8_t *payload, uint8_t plen)
{
    loraitp_data_t d = { c->sid, seq, payload, plen, parity };
    int len = loraitp_encode_data(&d, c->txbuf, sizeof(c->txbuf));
    if (len < 0)
        return len;
    return transmit(c, (size_t)len, 0);
}

static uint8_t chunk_len_at(const loraitp_meta_t *m, uint16_t seq)
{
    uint32_t off = (uint32_t)seq * m->chunk;
    uint32_t left = m->img_len - off;
    return (uint8_t)(left < m->chunk ? left : m->chunk);
}

int loraitp_send_image(loraitp_ctx_t *c, const loraitp_image_desc_t *desc,
                       loraitp_stats_t *out)
{
    const loraitp_port_t *p = c->port;
    int rc = loraitp_port_validate(p, 1);
    if (rc != LORAITP_OK)
        return rc;

    memset(&c->stats, 0, sizeof(c->stats));
    c->sid++;
    if (p->random_bytes)
        p->random_bytes(p->ctx, c->nonce, 4);

    loraitp_meta_t *m = &c->meta;
    memset(m, 0, sizeof(*m));
    m->sid = c->sid;
    m->img_id = desc->img_id;
    m->layer = desc->layer;
    m->img_len = desc->img_len;
    m->chunk = c->cfg.chunk_len;
    m->codec = desc->codec;
    m->crc32 = desc->img_crc32;
    m->width = desc->width;
    m->height = desc->height;
    m->block = c->cfg.block_size;
    memcpy(m->nonce, c->nonce, 4);

    if (p->image_begin != NULL) {
        rc = p->image_begin(p->ctx, m->img_len, false);
        if (rc != LORAITP_OK)
            return rc;
    }

    uint16_t n_chunks = loraitp_meta_n_chunks(m);
    if (c->cfg.parity_percent) {
        uint16_t k = (n_chunks < m->block) ? n_chunks : m->block;
        uint32_t r = ((uint32_t)k * c->cfg.parity_percent + 99u) / 100u;
        if (r == 0) r = 1;
        if (k + r > 255u) r = 255u - k;
        m->n_parity = (uint16_t)r;
        m->flags |= LORAITP_F_PARITY;
    }
    if (c->cfg.mode == LORAITP_MODE_BROADCAST)
        m->flags |= LORAITP_F_BCAST;
    if (mac_data(c))
        m->flags |= LORAITP_F_MAC_DATA;

    const loraitp_region_info_t *ri = loraitp_region(c->cfg.region);
    if (ri && ri->amateur) {
        m->flags |= LORAITP_F_AMATEUR;
        m->tlv[0].type = 0x01;
        m->tlv[0].len = (uint8_t)strlen(c->cfg.callsign);
        m->tlv[0].value = (const uint8_t *)c->cfg.callsign;
        m->n_tlv = 1;
    }

    uint16_t n_blocks = loraitp_meta_n_blocks(m);
    uint8_t chunkbuf[LORAITP_MAX_FRAME];

    /* ---- broadcast: source chunks then parity, META sprinkled through */
    if (c->cfg.mode == LORAITP_MODE_BROADCAST) {
        uint16_t k_max = (n_chunks < m->block) ? n_chunks : m->block;
        if (c->cfg.fec_scratch == NULL
            || c->cfg.fec_scratch_len < loraitp_fec_session_scratch(
                   k_max, m->n_parity, m->chunk))
            return LORAITP_E_ARG;

        uint8_t *blockbuf = c->cfg.fec_scratch;
        uint8_t *paritybuf = blockbuf + (size_t)k_max * m->chunk;

        for (int i = 0; i < 3; i++) {
            rc = send_meta(c);
            if (rc != LORAITP_OK) return rc;
        }
        unsigned since_meta = 0;

        for (uint16_t b = 0; b < n_blocks; b++) {
            uint16_t k = loraitp_meta_block_k(m, b);
            memset(blockbuf, 0, (size_t)k * m->chunk);
            for (uint16_t i = 0; i < k; i++) {
                uint16_t seq = (uint16_t)(b * m->block + i);
                p->image_read(p->ctx, (uint32_t)seq * m->chunk,
                              blockbuf + (size_t)i * m->chunk,
                              chunk_len_at(m, seq));
            }
            rc = loraitp_fec_encode(blockbuf, k, m->chunk, paritybuf,
                                    m->n_parity);
            if (rc != LORAITP_OK) return rc;

            for (uint16_t i = 0; i < k; i++) {
                uint16_t seq = (uint16_t)(b * m->block + i);
                rc = send_chunk(c, seq, false,
                                blockbuf + (size_t)i * m->chunk,
                                chunk_len_at(m, seq));
                if (rc != LORAITP_OK) return rc;
                if (++since_meta >= 16u) { since_meta = 0; send_meta(c); }
            }
            for (uint16_t i = 0; i < m->n_parity; i++) {
                rc = send_chunk(c, (uint16_t)(b * m->n_parity + i), true,
                                paritybuf + (size_t)i * m->chunk, m->chunk);
                if (rc != LORAITP_OK) return rc;
                if (++since_meta >= 16u) { since_meta = 0; send_meta(c); }
            }
        }
        for (int i = 0; i < 3; i++) {
            int fl = loraitp_encode_simple(LORAITP_FIN, c->sid, NULL, 0,
                                           c->txbuf, sizeof(c->txbuf));
            transmit(c, (size_t)fl, 0);
        }
        if (p->image_end != NULL)
            p->image_end(p->ctx, true);
        if (out) *out = c->stats;
        return LORAITP_OK;
    }

    /* ---- interactive: send a block, ask, repair */
    for (int i = 0; i < 3; i++) {
        rc = send_meta(c);
        if (rc != LORAITP_OK) return rc;
    }

    uint32_t stat_toa = loraitp_time_on_air_us(48, c->cfg.spreading_factor,
                                               c->cfg.bandwidth_hz,
                                               c->cfg.coding_rate, 8) / 1000u;
    uint32_t t_stat = 4u * stat_toa + 500u;

    for (uint16_t b = 0; b < n_blocks; b++) {
        uint16_t k = loraitp_meta_block_k(m, b);
        uint16_t base = (uint16_t)(b * m->block);
        uint16_t todo[LORAITP_BLOCK_SIZE];
        uint16_t n_todo = k;
        for (uint16_t i = 0; i < k; i++)
            todo[i] = (uint16_t)(base + i);

        for (uint8_t round = 0; round < c->cfg.max_rounds; round++) {
            for (uint16_t i = 0; i < n_todo; i++) {
                uint8_t cl = chunk_len_at(m, todo[i]);
                p->image_read(p->ctx, (uint32_t)todo[i] * m->chunk,
                              chunkbuf, cl);
                rc = send_chunk(c, todo[i], false, chunkbuf, cl);
                if (rc != LORAITP_OK) return rc;
                if (round > 0) c->stats.retransmits++;
            }
            c->stats.rounds++;

            bool got_stat = false;
            for (uint8_t retry = 0; retry < c->cfg.eob_retry && !got_stat;
                 retry++) {
                loraitp_eob_t e = { c->sid, b, round };
                int el = loraitp_encode_eob(&e, c->txbuf, sizeof(c->txbuf));
                rc = transmit(c, (size_t)el, 0);
                if (rc != LORAITP_OK) return rc;

                loraitp_rx_meta_t rm;
                size_t rl;
                int t = receive(c, t_stat, &rm, &rl);
                if (t != LORAITP_STAT)
                    continue;

                loraitp_stat_t s;
                if (loraitp_decode_stat(c->rxbuf, rl, base, &s) != LORAITP_OK)
                    continue;
                if (s.block != b || s.round != round)
                    continue;

                got_stat = true;
                c->stats.last_rssi_dbm = rm.rssi_dbm;
                c->stats.last_snr_qdb = rm.snr_qdb;

                if (s.enc == LORAITP_ENC_COMPLETE) { n_todo = 0; break; }
                int n = loraitp_stat_decode_body(&s, todo, LORAITP_BLOCK_SIZE);
                if (n < 0) { n_todo = 0; break; }
                /* Keep only what belongs to this block. */
                uint16_t keep = 0;
                for (int i = 0; i < n; i++)
                    if (todo[i] >= base && todo[i] < base + k)
                        todo[keep++] = todo[i];
                n_todo = keep;
            }
            if (!got_stat || n_todo == 0)
                break;
        }
    }

    int fl = loraitp_encode_simple(LORAITP_FIN, c->sid, NULL, 0,
                                   c->txbuf, sizeof(c->txbuf));
    transmit(c, (size_t)fl, 0);

    loraitp_rx_meta_t rm;
    size_t rl;
    int t = receive(c, 2u * t_stat, &rm, &rl);
    rc = LORAITP_OK;
    if (t == LORAITP_FINACK && rl >= 3 && c->rxbuf[2] != 0)
        rc = LORAITP_E_IO;
    if (p->image_end != NULL)
        p->image_end(p->ctx, rc == LORAITP_OK);
    if (out) *out = c->stats;
    return rc;
}

/* ----------------------------------------------------------- receiver */

/*
 * How many of block b's frames the sender has already emitted, inferred
 * from the highest ordinal we have actually seen.
 *
 * That under-estimates when frames are lost, so `recoverable` below errs
 * towards keeping the receiver listening. This is the safe direction: we
 * never abandon a block that was still recoverable.
 */
static uint16_t frames_sent_for_block(const loraitp_ctx_t *c, uint16_t b)
{
    const loraitp_meta_t *m = &c->meta;
    uint32_t start = 0;
    for (uint16_t i = 0; i < b; i++)
        start += (uint32_t)loraitp_meta_block_k(m, i) + m->n_parity;
    uint32_t total = (uint32_t)loraitp_meta_block_k(m, b) + m->n_parity;
    uint32_t elapsed = c->saw_any ? c->max_ordinal + 1u : 0u;
    if (elapsed <= start)
        return 0;
    uint32_t done = elapsed - start;
    return (uint16_t)(done > total ? total : done);
}

static uint16_t have_in_block(const loraitp_ctx_t *c)
{
    uint16_t k = loraitp_meta_block_k(&c->meta, c->cur_block);
    uint16_t n = 0;
    for (uint16_t i = 0; i < k + c->meta.n_parity; i++)
        if (bitget(c->present, i))
            n++;
    return n;
}

bool loraitp_rx_still_recoverable(const loraitp_ctx_t *c)
{
    if (!c->have_meta)
        return true;                     /* nothing decided yet */
    const loraitp_meta_t *m = &c->meta;
    uint16_t k = loraitp_meta_block_k(m, c->cur_block);
    if (have_in_block(c) >= k)
        return true;

    uint16_t total = (uint16_t)(k + m->n_parity);
    uint16_t remaining = (uint16_t)(total - frames_sent_for_block(c,
                                                                 c->cur_block));
    return (uint16_t)(have_in_block(c) + remaining) >= k;
}

uint8_t loraitp_rx_progress(const loraitp_ctx_t *c)
{
    if (!c->have_meta || c->stats.chunks_total == 0)
        return 0;
    return (uint8_t)((uint32_t)c->stats.chunks_have * 100u
                     / c->stats.chunks_total);
}

static void reset_block(loraitp_ctx_t *c, uint16_t b)
{
    c->cur_block = b;
    memset(c->present, 0, sizeof(c->present));
}

/*
 * Decode what we can and hand the block to storage. A block that fails
 * RS decoding still contributes the source chunks that did arrive - with
 * chunk-aligned restart markers those are readable strips, not garbage
 * (SPEC.md 5.3, 7).
 */
static void flush_block(loraitp_ctx_t *c, uint8_t *blockbuf)
{
    const loraitp_port_t *p = c->port;
    loraitp_meta_t *m = &c->meta;
    uint16_t b = c->cur_block;
    uint16_t k = loraitp_meta_block_k(m, b);
    if (k == 0)
        return;

    bool complete = true;
    for (uint16_t i = 0; i < k; i++)
        if (!bitget(c->present, i)) { complete = false; break; }

    if (!c->fec_ok) {
        /* Chunks were written as they arrived; nothing to flush. */
        if (!complete)
            c->stats.blocks_lost++;
        return;
    }

    if (!complete && m->n_parity && have_in_block(c) >= k) {
        uint8_t *scratch = blockbuf + (size_t)(k + m->n_parity) * m->chunk;
        uint8_t *work = scratch + loraitp_fec_scratch(k);
        for (uint16_t i = 0; i < k + m->n_parity; i++)
            c->flat[i] = bitget(c->present, i) ? 1u : 0u;
        if (loraitp_fec_decode(blockbuf, c->flat, k, m->n_parity, m->chunk,
                               scratch, loraitp_fec_scratch(k), work)
            == LORAITP_OK) {
            for (uint16_t i = 0; i < k; i++)
                bitset(c->present, i);
            complete = true;
        }
    }

    for (uint16_t i = 0; i < k; i++) {
        if (!bitget(c->present, i))
            continue;
        uint16_t seq = (uint16_t)(b * m->block + i);
        p->image_write(p->ctx, (uint32_t)seq * m->chunk,
                       blockbuf + (size_t)i * m->chunk,
                       chunk_len_at(m, seq));
        c->stats.chunks_have++;
    }
    if (!complete)
        c->stats.blocks_lost++;
}

int loraitp_receive_image(loraitp_ctx_t *c, loraitp_image_desc_t *out_desc,
                          loraitp_rx_result_t *out_result,
                          loraitp_stats_t *out)
{
    const loraitp_port_t *p = c->port;
    int rc = loraitp_port_validate(p, 0);
    if (rc != LORAITP_OK)
        return rc;

    memset(&c->stats, 0, sizeof(c->stats));
    c->have_meta = false;
    c->saw_any = false;
    c->max_ordinal = 0;
    memset(c->nonce, 0, sizeof(c->nonce));

    /*
     * No hidden fallback buffer. A block needs (k + r) * chunk bytes,
     * which depends on what the *sender* announces - sizing an on-stack
     * array from an unrelated constant is how you get a silent stack
     * smash on a part with 8 kB of task stack. Either the caller's
     * scratch is big enough, or we run without erasure coding and write
     * each chunk straight to storage as it arrives (see c->fec_ok).
     */
    uint8_t *blockbuf = c->cfg.fec_scratch;
    c->fec_ok = false;

    uint32_t start = p->now_ms(p->ctx);
    uint32_t deadline = c->cfg.session_timeout_ms
                        ? c->cfg.session_timeout_ms : 4u * 3600000u;
    loraitp_rx_result_t result = LORAITP_RX_TIMEOUT;

    for (;;) {
        uint32_t now = p->now_ms(p->ctx);
        if ((uint32_t)(now - start) >= deadline)
            break;
        uint32_t left = deadline - (uint32_t)(now - start);
        uint32_t slice = left < 60000u ? left : 60000u;

        loraitp_rx_meta_t rm;
        size_t rl = 0;
        int t = receive(c, slice, &rm, &rl);

        if (t == LORAITP_E_TIMEOUT) {
            if (c->have_meta && c->cfg.mode == LORAITP_MODE_BROADCAST
                && !loraitp_rx_still_recoverable(c)) {
                result = LORAITP_RX_UNRECOVERABLE;
                break;
            }
            continue;
        }
        if (t < 0)
            continue;

        c->stats.last_rssi_dbm = rm.rssi_dbm;
        c->stats.last_snr_qdb = rm.snr_qdb;

        if (t == LORAITP_META) {
            loraitp_meta_t nm;
            if (loraitp_decode_meta(c->rxbuf, rl, &nm) != LORAITP_OK)
                continue;
            if (!c->have_meta || c->meta.img_id != nm.img_id) {
                c->meta = nm;
                c->have_meta = true;

                /* Can we buffer a whole block and decode it? */
                uint16_t kmax = loraitp_meta_block_k(&nm, 0);
                size_t need = loraitp_fec_session_scratch(kmax, nm.n_parity,
                                                          nm.chunk);
                c->fec_ok = (blockbuf != NULL
                             && c->cfg.fec_scratch_len >= need);
                memcpy(c->nonce, nm.nonce, 4);
                c->stats.chunks_total = loraitp_meta_n_chunks(&nm);
                c->stats.chunks_have = 0;
                if (p->image_begin != NULL
                    && p->image_begin(p->ctx, nm.img_len, true)
                       != LORAITP_OK) {
                    c->have_meta = false;   /* no store, no transfer */
                    continue;
                }
                c->saw_any = false;
                c->max_ordinal = 0;
                reset_block(c, 0);
            }
            continue;
        }

        if (!c->have_meta)
            continue;                    /* nothing is interpretable yet */

        if (t == LORAITP_IDENT)
            continue;

        if (t == LORAITP_DATA || t == LORAITP_PARITY) {
            loraitp_data_t d;
            if (loraitp_decode_data(c->rxbuf, rl, &d) != LORAITP_OK)
                continue;
            loraitp_meta_t *m = &c->meta;

            uint32_t ord = loraitp_frame_ordinal(m, d.seq, d.is_parity);
            if (!c->saw_any || ord > c->max_ordinal)
                c->max_ordinal = ord;
            c->saw_any = true;

            uint16_t n_par = (m->n_parity != 0u) ? m->n_parity : 1u;
            uint16_t blk;
            if (d.is_parity)
                blk = (uint16_t)(d.seq / n_par);
            else
                blk = (uint16_t)(d.seq / m->block);
            if (blk != c->cur_block) {
                flush_block(c, blockbuf);
                reset_block(c, blk);
            }
            uint16_t k = loraitp_meta_block_k(m, blk);
            uint16_t slot;
            if (d.is_parity)
                slot = (uint16_t)(k + (uint16_t)(d.seq % n_par));
            else
                slot = (uint16_t)(d.seq % m->block);
            if (slot >= k + m->n_parity)
                continue;

            if (c->fec_ok) {
                uint8_t *dst = blockbuf + (size_t)slot * m->chunk;
                memset(dst, 0, m->chunk);
                memcpy(dst, d.payload, d.payload_len);
                bitset(c->present, slot);
            } else {
                /* Straight through to storage. Interactive mode never
                 * needs the block resident, so this is the normal path
                 * there, not a degraded one. */
                if (d.is_parity)
                    continue;
                if (!bitget(c->present, slot)) {
                    p->image_write(p->ctx, (uint32_t)d.seq * m->chunk,
                                   d.payload, d.payload_len);
                    bitset(c->present, slot);
                    c->stats.chunks_have++;
                }
            }

            if (c->cfg.mode == LORAITP_MODE_BROADCAST) {
                bool last = (blk + 1u == loraitp_meta_n_blocks(m));
                if (last && have_in_block(c) >= k) {
                    flush_block(c, blockbuf);
                    result = c->stats.blocks_lost ? LORAITP_RX_INCOMPLETE
                                                  : LORAITP_RX_COMPLETE;
                    goto done;
                }
                if (!loraitp_rx_still_recoverable(c)) {
                    flush_block(c, blockbuf);
                    result = LORAITP_RX_UNRECOVERABLE;
                    goto done;
                }
            }
            continue;
        }

        if (t == LORAITP_EOB) {
            loraitp_eob_t e;
            if (loraitp_decode_eob(c->rxbuf, rl, &e) != LORAITP_OK)
                continue;
            if (e.block != c->cur_block) {
                flush_block(c, blockbuf);
                reset_block(c, e.block);
            }
            uint16_t k = loraitp_meta_block_k(&c->meta, e.block);
            uint16_t base = (uint16_t)(e.block * c->meta.block);
            uint16_t n = 0;
            for (uint16_t i = 0; i < k; i++)
                if (!bitget(c->present, i))
                    c->missing[n++] = (uint16_t)(base + i);

            int enc = loraitp_stat_choose_enc(c->missing, n, base);
            int bl = loraitp_stat_encode_body(enc, c->missing, n, base,
                                              c->statbody,
                                              sizeof(c->statbody));
            if (bl < 0)
                continue;
            loraitp_stat_t s = { e.sid, e.block, e.round, (uint8_t)enc,
                                 (int8_t)rm.rssi_dbm, rm.snr_qdb,
                                 c->statbody, (uint8_t)bl, base };
            int sl = loraitp_encode_stat(&s, c->txbuf, sizeof(c->txbuf));
            if (sl > 0)
                transmit(c, (size_t)sl, 0);
            continue;
        }

        if (t == LORAITP_FIN) {
            flush_block(c, blockbuf);
            bool complete = (c->stats.chunks_have == c->stats.chunks_total);
            result = complete ? LORAITP_RX_COMPLETE : LORAITP_RX_INCOMPLETE;
            if (c->cfg.mode == LORAITP_MODE_INTERACTIVE) {
                uint8_t status = complete ? 0u : 2u;
                int al = loraitp_encode_simple(LORAITP_FINACK, c->sid ?
                                               c->sid : c->meta.sid,
                                               &status, 1,
                                               c->txbuf, sizeof(c->txbuf));
                if (al > 0)
                    transmit(c, (size_t)al, 0);
            }
            goto done;
        }
    }

done:
    if (p->image_end != NULL && c->have_meta)
        p->image_end(p->ctx, result == LORAITP_RX_COMPLETE);
    if (out_desc) {
        out_desc->img_id = c->meta.img_id;
        out_desc->layer = c->meta.layer;
        out_desc->img_len = c->meta.img_len;
        out_desc->chunk_len = c->meta.chunk;
        out_desc->codec = c->meta.codec;
        out_desc->img_crc32 = c->meta.crc32;
        out_desc->width = c->meta.width;
        out_desc->height = c->meta.height;
        out_desc->n_chunks = loraitp_meta_n_chunks(&c->meta);
        out_desc->n_parity = c->meta.n_parity;
        out_desc->block_size = (uint8_t)c->meta.block;
    }
    if (out_result) *out_result = result;
    if (out) *out = c->stats;
    return LORAITP_OK;
}

/* ---------------------------------------------------- budget reporting */

void loraitp_budget_query(loraitp_ctx_t *c, loraitp_budget_t *out)
{
    loraitp_gov_t *g = &c->gov;
    uint32_t now = c->port->now_ms(c->port->ctx);
    out->airtime_used_ms = loraitp_gov_airtime_in_window(g, now);
    out->airtime_budget_ms = loraitp_region_budget_ms(c->cfg.region);
    out->blocked_for_ms = loraitp_gov_delay_ms(g, now, 0);
    out->airtime_today_ms = g->airtime_total_ms;
}

uint32_t loraitp_budget_bytes_remaining(loraitp_ctx_t *c)
{
    loraitp_budget_t b;
    loraitp_budget_query(c, &b);
    if (b.airtime_budget_ms == 0)
        return 0xFFFFFFFFu;              /* no duty-cycle limit */
    if (b.airtime_used_ms >= b.airtime_budget_ms)
        return 0;

    uint32_t left = b.airtime_budget_ms - b.airtime_used_ms;
    uint8_t frame = (uint8_t)(c->cfg.chunk_len + LORAITP_DATA_HDR);
    uint32_t toa = loraitp_time_on_air_us(frame, c->cfg.spreading_factor,
                                          c->cfg.bandwidth_hz,
                                          c->cfg.coding_rate, 8) / 1000u;
    if (toa == 0)
        return 0;
    return (left / toa) * c->cfg.chunk_len;
}
