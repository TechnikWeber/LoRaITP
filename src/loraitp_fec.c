/*
 * Reed-Solomon erasure coding over GF(256).  SPEC.md 5.2.
 *
 * Systematic, with a Cauchy generator: every k x k submatrix of [I ; C]
 * is invertible, which is exactly what erasure decoding needs, because
 * we do not get to choose which k of the k + r frames arrive.
 *
 * Because the PHY CRC turns a corrupted frame into a missing one, this
 * only ever faces erasures at known positions, never errors at unknown
 * ones. That is why r parity chunks recover exactly r losses.
 *
 * The log/antilog tables are built once at init - 768 bytes of static
 * data rather than 64 kB of multiplication table, which matters on a
 * part where SRAM is the scarce resource.
 */
#include <string.h>

#include "loraitp.h"

#define GF_PRIM 0x11D

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static bool gf_ready = false;

static void gf_init(void)
{
    if (gf_ready)
        return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x = (uint16_t)(x << 1);
        if (x & 0x100)
            x = (uint16_t)(x ^ GF_PRIM);
    }
    for (int i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];
    gf_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;
    return gf_exp[(int)gf_log[a] + (int)gf_log[b]];
}

static uint8_t gf_inv(uint8_t a)
{
    return gf_exp[255 - gf_log[a]];
}

/* C[i][j] = 1 / (x_i ^ y_j), with the two index sets disjoint. */
static uint8_t cauchy(uint16_t i, uint16_t j, uint16_t rows)
{
    return gf_inv((uint8_t)(i ^ (rows + j)));
}

/*
 * Multiply-accumulate a whole chunk: dst ^= coeff * src.
 * The per-coefficient lookup keeps the inner loop to one table read and
 * one xor, which is what makes this affordable on a microcontroller.
 */
static void mac_row(uint8_t *dst, const uint8_t *src, uint8_t coeff,
                    uint16_t len)
{
    if (coeff == 0)
        return;
    int lg = gf_log[coeff];
    for (uint16_t i = 0; i < len; i++) {
        uint8_t v = src[i];
        if (v)
            dst[i] ^= gf_exp[lg + (int)gf_log[v]];
    }
}

int loraitp_fec_encode(const uint8_t *data, uint16_t k, uint16_t chunk_len,
                       uint8_t *parity, uint16_t r)
{
    if (data == NULL || parity == NULL || k == 0 || r == 0)
        return LORAITP_E_ARG;
    if ((uint32_t)k + r > 255u)
        return LORAITP_E_ARG;      /* the GF(256) limit in SPEC.md 5.2 */
    gf_init();

    memset(parity, 0, (size_t)r * chunk_len);
    for (uint16_t i = 0; i < r; i++)
        for (uint16_t j = 0; j < k; j++)
            mac_row(parity + (size_t)i * chunk_len,
                    data + (size_t)j * chunk_len, cauchy(i, j, r), chunk_len);
    return LORAITP_OK;
}

/*
 * Gauss-Jordan inversion in place, on a k x 2k augmented matrix supplied
 * by the caller. Callers own the scratch so the core stays free of
 * dynamic allocation; loraitp_fec_scratch() says how big it must be.
 */
static int invert(uint8_t *a, uint16_t k)
{
    uint16_t w = (uint16_t)(2u * k);
    for (uint16_t col = 0; col < k; col++) {
        uint16_t piv = k;
        for (uint16_t r = col; r < k; r++)
            if (a[(size_t)r * w + col]) { piv = r; break; }
        if (piv == k)
            return LORAITP_E_ARG;   /* impossible for a Cauchy generator */
        if (piv != col)
            for (uint16_t c = 0; c < w; c++) {
                uint8_t t = a[(size_t)col * w + c];
                a[(size_t)col * w + c] = a[(size_t)piv * w + c];
                a[(size_t)piv * w + c] = t;
            }
        uint8_t p = gf_inv(a[(size_t)col * w + col]);
        for (uint16_t c = 0; c < w; c++)
            a[(size_t)col * w + c] = gf_mul(a[(size_t)col * w + c], p);
        for (uint16_t r = 0; r < k; r++) {
            if (r == col)
                continue;
            uint8_t f = a[(size_t)r * w + col];
            if (!f)
                continue;
            for (uint16_t c = 0; c < w; c++)
                a[(size_t)r * w + c] ^= gf_mul(f, a[(size_t)col * w + c]);
        }
    }
    return LORAITP_OK;
}

size_t loraitp_fec_scratch(uint16_t k)
{
    return (size_t)k * 2u * k;
}

int loraitp_fec_decode(uint8_t *chunks, const uint8_t *present, uint16_t k,
                       uint16_t r, uint16_t chunk_len, uint8_t *scratch,
                       size_t scratch_len, uint8_t *workspace)
{
    if (chunks == NULL || present == NULL || scratch == NULL)
        return LORAITP_E_ARG;
    if (scratch_len < loraitp_fec_scratch(k))
        return LORAITP_E_ARG;
    gf_init();

    /* Pick the first k frames we actually have. */
    uint16_t idx[255];
    uint16_t have = 0;
    for (uint16_t i = 0; i < (uint16_t)(k + r) && have < k; i++)
        if (present[i])
            idx[have++] = i;
    if (have < k)
        return LORAITP_E_TIMEOUT;   /* not recoverable - SPEC.md 5.3 */

    uint16_t w = (uint16_t)(2u * k);
    memset(scratch, 0, (size_t)k * w);
    for (uint16_t row = 0; row < k; row++) {
        uint16_t src = idx[row];
        if (src < k)
            scratch[(size_t)row * w + src] = 1;
        else
            for (uint16_t j = 0; j < k; j++)
                scratch[(size_t)row * w + j] = cauchy((uint16_t)(src - k), j, r);
        scratch[(size_t)row * w + k + row] = 1;
    }
    int rc = invert(scratch, k);
    if (rc != LORAITP_OK)
        return rc;

    for (uint16_t out = 0; out < k; out++) {
        if (present[out])
            continue;
        uint8_t *dst = workspace;
        memset(dst, 0, chunk_len);
        for (uint16_t pos = 0; pos < k; pos++)
            mac_row(dst, chunks + (size_t)idx[pos] * chunk_len,
                    scratch[(size_t)out * w + k + pos], chunk_len);
        memcpy(chunks + (size_t)out * chunk_len, dst, chunk_len);
    }
    return LORAITP_OK;
}
