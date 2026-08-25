#include <string.h>

#include "loraitp_frame.h"

/* Little-endian accessors. Written out rather than memcpy'd so the code
 * behaves the same on a big-endian host, which the tests run on. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get24(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }

static uint8_t ctrl(uint8_t type) { return (uint8_t)((LORAITP_VERSION << 5) | type); }

int loraitp_frame_type(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < 2)
        return LORAITP_E_ARG;
    if ((buf[0] >> 5) != LORAITP_VERSION)
        return LORAITP_E_NOSUP;
    return buf[0] & 0x1F;
}

/* ------------------------------------------------------ derived counts */

uint16_t loraitp_meta_n_chunks(const loraitp_meta_t *m)
{
    if (m->chunk == 0)
        return 0;
    return (uint16_t)((m->img_len + m->chunk - 1u) / m->chunk);
}

uint16_t loraitp_meta_n_blocks(const loraitp_meta_t *m)
{
    uint16_t n = loraitp_meta_n_chunks(m);
    uint16_t b = m->block ? m->block : 256u;
    return (uint16_t)((n + b - 1u) / b);
}

uint16_t loraitp_meta_block_k(const loraitp_meta_t *m, uint16_t blk)
{
    uint16_t n = loraitp_meta_n_chunks(m);
    uint16_t b = m->block ? m->block : 256u;
    uint32_t start = (uint32_t)blk * b;
    if (start >= n)
        return 0;
    uint32_t left = n - start;
    return (uint16_t)(left < b ? left : b);
}

uint32_t loraitp_meta_total_frames(const loraitp_meta_t *m)
{
    return (uint32_t)loraitp_meta_n_chunks(m)
         + (uint32_t)loraitp_meta_n_blocks(m) * m->n_parity;
}

uint32_t loraitp_frame_ordinal(const loraitp_meta_t *m, uint16_t seq,
                               bool is_parity)
{
    uint16_t b = m->block ? m->block : 256u;
    uint16_t blk, within;
    uint32_t before = 0;

    if (is_parity) {
        if (m->n_parity == 0)
            return 0;
        blk = (uint16_t)(seq / m->n_parity);
        within = (uint16_t)(loraitp_meta_block_k(m, blk) + (seq % m->n_parity));
    } else {
        blk = (uint16_t)(seq / b);
        within = (uint16_t)(seq % b);
    }
    for (uint16_t i = 0; i < blk; i++)
        before += (uint32_t)loraitp_meta_block_k(m, i) + m->n_parity;
    return before + within;
}

/* ------------------------------------------------------------ encoding */

int loraitp_encode_meta(const loraitp_meta_t *m, uint8_t *out, size_t cap)
{
    size_t need = LORAITP_META_HDR;
    for (uint8_t i = 0; i < m->n_tlv; i++)
        need += 2u + m->tlv[i].len;
    if (out == NULL || cap < need)
        return LORAITP_E_ARG;

    memset(out, 0, LORAITP_META_HDR);
    out[0] = ctrl(LORAITP_META);
    out[1] = m->sid;
    put16(out + 2, m->img_id);
    out[4] = m->layer;
    put24(out + 5, m->img_len);
    out[8] = m->chunk;
    out[9] = m->codec;
    put32(out + 10, m->crc32);
    put16(out + 14, m->width);
    put16(out + 16, m->height);
    out[18] = m->flags;
    out[19] = (m->block == 256u) ? 0u : (uint8_t)m->block;
    put16(out + 20, m->n_parity);
    memcpy(out + 22, m->nonce, 4);

    size_t o = LORAITP_META_HDR;
    for (uint8_t i = 0; i < m->n_tlv; i++) {
        out[o++] = m->tlv[i].type;
        out[o++] = m->tlv[i].len;
        memcpy(out + o, m->tlv[i].value, m->tlv[i].len);
        o += m->tlv[i].len;
    }
    return (int)o;
}

int loraitp_encode_data(const loraitp_data_t *d, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < (size_t)LORAITP_DATA_HDR + d->payload_len)
        return LORAITP_E_ARG;
    out[0] = ctrl(d->is_parity ? LORAITP_PARITY : LORAITP_DATA);
    out[1] = d->sid;
    put16(out + 2, d->seq);
    memcpy(out + LORAITP_DATA_HDR, d->payload, d->payload_len);
    return LORAITP_DATA_HDR + d->payload_len;
}

int loraitp_encode_eob(const loraitp_eob_t *e, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 6)
        return LORAITP_E_ARG;
    out[0] = ctrl(LORAITP_EOB);
    out[1] = e->sid;
    put16(out + 2, e->block);
    out[4] = e->round;
    out[5] = 0;
    return 6;
}

int loraitp_encode_stat(const loraitp_stat_t *s, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < (size_t)9 + s->body_len)
        return LORAITP_E_ARG;
    out[0] = ctrl(LORAITP_STAT);
    out[1] = s->sid;
    put16(out + 2, s->block);
    out[4] = s->round;
    out[5] = s->enc;
    out[6] = (uint8_t)s->rssi;
    out[7] = (uint8_t)s->snr_qdb;
    out[8] = s->body_len;
    if (s->body_len)
        memcpy(out + 9, s->body, s->body_len);
    return 9 + s->body_len;
}

int loraitp_encode_simple(uint8_t type, uint8_t sid, const uint8_t *extra,
                          uint8_t extra_len, uint8_t *out, size_t cap)
{
    if (out == NULL || cap < (size_t)2 + extra_len)
        return LORAITP_E_ARG;
    out[0] = ctrl(type);
    out[1] = sid;
    if (extra_len)
        memcpy(out + 2, extra, extra_len);
    return 2 + extra_len;
}

/* ------------------------------------------------------------ decoding */

int loraitp_decode_meta(const uint8_t *buf, size_t len, loraitp_meta_t *out)
{
    if (buf == NULL || out == NULL || len < LORAITP_META_HDR)
        return LORAITP_E_ARG;

    memset(out, 0, sizeof(*out));
    out->sid = buf[1];
    out->img_id = get16(buf + 2);
    out->layer = buf[4];
    out->img_len = get24(buf + 5);
    out->chunk = buf[8];
    out->codec = buf[9];
    out->crc32 = get32(buf + 10);
    out->width = get16(buf + 14);
    out->height = get16(buf + 16);
    out->flags = buf[18];
    out->block = buf[19] ? buf[19] : 256u;
    out->n_parity = get16(buf + 20);
    memcpy(out->nonce, buf + 22, 4);

    /* A chunk length of zero would make every derived count meaningless
     * and divide by zero downstream. Reject the frame instead. */
    if (out->chunk == 0)
        return LORAITP_E_ARG;

    size_t i = LORAITP_META_HDR;
    while (i + 2u <= len && out->n_tlv < LORAITP_MAX_TLV) {
        uint8_t t = buf[i], l = buf[i + 1];
        if (i + 2u + l > len)
            return LORAITP_E_ARG;         /* TLV overruns the frame */
        out->tlv[out->n_tlv].type = t;
        out->tlv[out->n_tlv].len = l;
        out->tlv[out->n_tlv].value = buf + i + 2;
        out->n_tlv++;
        i += 2u + l;
    }
    return LORAITP_OK;
}

int loraitp_decode_data(const uint8_t *buf, size_t len, loraitp_data_t *out)
{
    int t = loraitp_frame_type(buf, len);
    if (t < 0)
        return t;
    if (len < LORAITP_DATA_HDR || len > LORAITP_MAX_FRAME)
        return LORAITP_E_ARG;
    out->sid = buf[1];
    out->seq = get16(buf + 2);
    out->payload = buf + LORAITP_DATA_HDR;
    out->payload_len = (uint8_t)(len - LORAITP_DATA_HDR);
    out->is_parity = (t == LORAITP_PARITY);
    return LORAITP_OK;
}

int loraitp_decode_eob(const uint8_t *buf, size_t len, loraitp_eob_t *out)
{
    if (buf == NULL || out == NULL || len < 6)
        return LORAITP_E_ARG;
    out->sid = buf[1];
    out->block = get16(buf + 2);
    out->round = buf[4];
    return LORAITP_OK;
}

int loraitp_decode_stat(const uint8_t *buf, size_t len, uint16_t base,
                        loraitp_stat_t *out)
{
    if (buf == NULL || out == NULL || len < 9)
        return LORAITP_E_ARG;
    uint8_t n = buf[8];
    if ((size_t)9 + n > len)
        return LORAITP_E_ARG;
    out->sid = buf[1];
    out->block = get16(buf + 2);
    out->round = buf[4];
    out->enc = buf[5];
    out->rssi = (int8_t)buf[6];
    out->snr_qdb = (int8_t)buf[7];
    out->body_len = n;
    out->body = buf + 9;
    out->base = base;
    return LORAITP_OK;
}
