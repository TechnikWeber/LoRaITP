/*
 * STAT body encoding.  SPEC.md 3.5.
 *
 * Two encodings, and the receiver picks whichever is smaller. The bitmap
 * costs (highest_missing / 8 + 1) bytes and does not grow with the loss
 * rate; the list costs 2 bytes per missing chunk. The crossover is at
 * about 6% of a block for scattered losses, but for a burst at the start
 * of a block the bitmap wins immediately - which is why both stay.
 */
#include <string.h>

#include "loraitp_frame.h"

int loraitp_stat_choose_enc(const uint16_t *missing, uint16_t n_missing,
                            uint16_t base)
{
    if (n_missing == 0)
        return LORAITP_ENC_COMPLETE;

    uint16_t rel_max = 0;
    for (uint16_t i = 0; i < n_missing; i++) {
        uint16_t rel = (uint16_t)(missing[i] - base);
        if (rel > rel_max)
            rel_max = rel;
    }
    uint16_t bitmap_bytes = (uint16_t)(rel_max / 8u + 1u);
    uint16_t list_bytes = (uint16_t)(n_missing * 2u);
    return (list_bytes < bitmap_bytes) ? LORAITP_ENC_LIST : LORAITP_ENC_BITMAP;
}

int loraitp_stat_encode_body(int enc, const uint16_t *missing,
                             uint16_t n_missing, uint16_t base,
                             uint8_t *out, size_t cap)
{
    if (enc == LORAITP_ENC_COMPLETE)
        return 0;

    if (enc == LORAITP_ENC_LIST) {
        size_t need = (size_t)n_missing * 2u;
        if (need > cap || need > 255u)
            return LORAITP_E_ARG;
        for (uint16_t i = 0; i < n_missing; i++) {
            out[i * 2u] = (uint8_t)missing[i];
            out[i * 2u + 1u] = (uint8_t)(missing[i] >> 8);
        }
        return (int)need;
    }

    uint16_t rel_max = 0;
    for (uint16_t i = 0; i < n_missing; i++) {
        uint16_t rel = (uint16_t)(missing[i] - base);
        if (rel > rel_max)
            rel_max = rel;
    }
    size_t need = (size_t)(rel_max / 8u + 1u);
    if (need > cap || need > 255u)
        return LORAITP_E_ARG;
    memset(out, 0, need);
    for (uint16_t i = 0; i < n_missing; i++) {
        uint16_t rel = (uint16_t)(missing[i] - base);
        out[rel / 8u] |= (uint8_t)(1u << (rel % 8u));
    }
    return (int)need;
}

int loraitp_stat_decode_body(const loraitp_stat_t *s, uint16_t *out,
                             uint16_t cap)
{
    uint16_t n = 0;

    if (s->enc == LORAITP_ENC_COMPLETE)
        return 0;

    if (s->enc == LORAITP_ENC_LIST) {
        for (uint8_t i = 0; i + 1u < s->body_len; i += 2u) {
            if (n >= cap)
                return LORAITP_E_ARG;
            out[n++] = (uint16_t)(s->body[i] | ((uint16_t)s->body[i + 1] << 8));
        }
        return n;
    }

    for (uint8_t i = 0; i < s->body_len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            if (s->body[i] & (1u << bit)) {
                if (n >= cap)
                    return LORAITP_E_ARG;
                /* The bitmap is relative to the block being reported on.
                 * Decoding it against a base of zero is wrong for every
                 * block but the first - and it was wrong in the reference
                 * implementation until the simulator caught it. */
                out[n++] = (uint16_t)(s->base + i * 8u + (unsigned)bit);
            }
        }
    }
    return n;
}
