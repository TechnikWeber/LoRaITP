/*
 * AES-128-CMAC (RFC 4493), truncated to 4 bytes.  SPEC.md 11.
 *
 * The block cipher comes from the port. There is deliberately no
 * software AES here: every part LoRaITP targets has it in hardware, and
 * a fallback would let somebody ship a slow one without noticing.
 */
#include <string.h>

#include "loraitp.h"
#include "loraitp_frame.h"

static void shift_left(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
        uint8_t c = (uint8_t)(in[i] >> 7);
        out[i] = (uint8_t)((in[i] << 1) | carry);
        carry = c;
    }
}

static void xor16(uint8_t *dst, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++)
        dst[i] = a[i] ^ b[i];
}

static int subkeys(const loraitp_port_t *port, uint8_t k1[16], uint8_t k2[16])
{
    uint8_t l[16], zero[16];
    memset(zero, 0, sizeof(zero));
    int rc = port->aes128_encrypt_block(port->ctx, zero, l);
    if (rc != LORAITP_OK)
        return rc;

    shift_left(l, k1);
    if (l[0] & 0x80)
        k1[15] ^= 0x87;
    shift_left(k1, k2);
    if (k1[0] & 0x80)
        k2[15] ^= 0x87;
    return LORAITP_OK;
}

/*
 * CMAC over nonce || msg, without concatenating them in a buffer: the
 * nonce is simply fed in first. Saves a copy and a bound.
 */
int loraitp_cmac(const loraitp_port_t *port, const uint8_t *nonce,
                 size_t nonce_len, const uint8_t *msg, size_t msg_len,
                 uint8_t *out, size_t out_len)
{
    if (port == NULL || port->aes128_encrypt_block == NULL)
        return LORAITP_E_NOSUP;
    if (out_len > 16)
        return LORAITP_E_ARG;

    uint8_t k1[16], k2[16], x[16], blk[16], tmp[16];
    int rc = subkeys(port, k1, k2);
    if (rc != LORAITP_OK)
        return rc;

    size_t total = nonce_len + msg_len;
    size_t n_blocks = (total + 15u) / 16u;
    if (n_blocks == 0)
        n_blocks = 1;

    memset(x, 0, sizeof(x));
    for (size_t b = 0; b < n_blocks; b++) {
        size_t off = b * 16u;
        size_t take = (total - off < 16u) ? (total - off) : 16u;

        memset(blk, 0, sizeof(blk));
        for (size_t i = 0; i < take; i++) {
            size_t p = off + i;
            blk[i] = (p < nonce_len) ? nonce[p] : msg[p - nonce_len];
        }

        if (b + 1u == n_blocks) {
            if (take == 16u) {
                xor16(blk, blk, k1);
            } else {
                blk[take] = 0x80;
                xor16(blk, blk, k2);
            }
        }
        xor16(tmp, x, blk);
        rc = port->aes128_encrypt_block(port->ctx, tmp, x);
        if (rc != LORAITP_OK)
            return rc;
    }
    memcpy(out, x, out_len);
    return LORAITP_OK;
}

/*
 * SPEC.md 11: META is authenticated under an all-zero nonce, everything
 * else under the session nonce META carries.
 *
 * META is the frame that *delivers* the nonce, so a receiver verifying
 * it does not have the nonce yet. Authenticating META under the session
 * nonce is circular, and in the reference implementation it rejected
 * every META until this was corrected. The nonce sits inside META's own
 * authenticated bytes, so a replayed META is bit-identical and is caught
 * by tracking seen (IMG_ID, NONCE) pairs instead.
 */
static const uint8_t zero_nonce[4] = { 0, 0, 0, 0 };

static const uint8_t *mac_nonce(int ftype, const uint8_t *session_nonce)
{
    return (ftype == LORAITP_META) ? zero_nonce : session_nonce;
}

bool loraitp_frame_is_authenticated(int ftype, bool mac_data)
{
    switch (ftype) {
    case LORAITP_META: case LORAITP_EOB:   case LORAITP_STAT:
    case LORAITP_FIN:  case LORAITP_FINACK: case LORAITP_PROBE:
    case LORAITP_PROBEACK: case LORAITP_ABORT:
        return true;
    case LORAITP_DATA: case LORAITP_PARITY:
        return mac_data;
    default:
        return false;
    }
}

int loraitp_seal(const loraitp_port_t *port, const uint8_t *nonce,
                 bool mac_data, uint8_t *buf, size_t len, size_t cap)
{
    if (port == NULL || port->aes128_encrypt_block == NULL)
        return (int)len;                      /* authentication disabled */
    int t = loraitp_frame_type(buf, len);
    if (t < 0)
        return t;
    if (!loraitp_frame_is_authenticated(t, mac_data))
        return (int)len;
    if (len + LORAITP_MAC_LEN > cap)
        return LORAITP_E_ARG;

    int rc = loraitp_cmac(port, mac_nonce(t, nonce), 4, buf, len,
                          buf + len, LORAITP_MAC_LEN);
    return (rc == LORAITP_OK) ? (int)(len + LORAITP_MAC_LEN) : rc;
}

int loraitp_unseal(const loraitp_port_t *port, const uint8_t *nonce,
                   bool mac_data, const uint8_t *buf, size_t len)
{
    if (port == NULL || port->aes128_encrypt_block == NULL)
        return (int)len;
    int t = loraitp_frame_type(buf, len);
    if (t < 0)
        return t;
    if (!loraitp_frame_is_authenticated(t, mac_data))
        return (int)len;
    if (len < (size_t)LORAITP_MAC_LEN + 2u)
        return LORAITP_E_ARG;

    size_t body = len - LORAITP_MAC_LEN;
    uint8_t want[LORAITP_MAC_LEN];
    int rc = loraitp_cmac(port, mac_nonce(t, nonce), 4, buf, body,
                          want, LORAITP_MAC_LEN);
    if (rc != LORAITP_OK)
        return rc;

    /* Constant-time compare. The timing almost certainly does not matter
     * over a link this slow, but a variable-time memcmp on a MAC is the
     * kind of thing that gets copied into somewhere it does matter. */
    uint8_t diff = 0;
    for (size_t i = 0; i < (size_t)LORAITP_MAC_LEN; i++)
        diff |= (uint8_t)(buf[body + i] ^ want[i]);
    return diff ? LORAITP_E_ARG : (int)body;
}
