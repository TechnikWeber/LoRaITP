/*
 * Grayscale baseline JPEG encoder with restart markers.
 *
 * Why write one rather than use the camera's: the OV2640 encodes to JPEG
 * in hardware, but it gives no control over the restart interval, poor
 * control over the byte budget, and no grayscale-only output. All three
 * matter more here than encoder speed does, because the encoding costs
 * milliseconds and the transmission costs tens of minutes.
 *
 *   grayscale     - roughly 30-40% smaller than colour at equal detail,
 *                   and for "what does the camera see" chroma is the
 *                   first thing to spend
 *   byte budget   - the duty cycle constrains bytes, not quality, so the
 *                   encoder should aim at a size and not at a Q number
 *   restart marks - a baseline JPEG is one entropy-coded stream with a DC
 *                   prediction chain running through it, so a gap
 *                   anywhere destroys everything after it. Restart
 *                   markers break the chain at known points and let a
 *                   decoder resynchronise.
 *
 * A correction to SPEC.md 7 that writing this forced: restart markers
 * cannot be aligned to LoRaITP chunk boundaries exactly. DRI counts MCUs,
 * and the compressed size of an interval varies with the content, so
 * where a marker lands in the byte stream is not something the encoder
 * chooses. What is achievable - and what actually matters - is markers
 * frequent enough that a lost chunk damages a bounded strip rather than
 * the remainder of the picture. One marker per MCU row costs two bytes
 * each and bounds the damage at two rows.
 */
#include <string.h>

#include "jpeg.h"

/* ---------------------------------------------------- standard tables */

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ITU T.81 Annex K, luminance. */
static const uint8_t std_quant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,
    24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,
    72, 92, 95, 98,112,100,103, 99
};

static const uint8_t dc_bits[17] = {
    0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
static const uint8_t dc_vals[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };

static const uint8_t ac_bits[17] = {
    0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d
};
static const uint8_t ac_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,
    0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,
    0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,
    0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,
    0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,
    0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,
    0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,
    0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

/* ------------------------------------------------------- output state */

typedef struct {
    uint8_t *buf;
    size_t   cap, len;
    int      overflow;

    uint32_t bit_acc;
    int      bit_cnt;

    uint16_t dc_code[256];
    uint8_t  dc_size[256];
    uint16_t ac_code[256];
    uint8_t  ac_size[256];

    uint8_t  quant[64];      /* natural order */
} enc_t;

static void put_byte(enc_t *e, uint8_t b)
{
    if (e->len >= e->cap) { e->overflow = 1; return; }
    e->buf[e->len++] = b;
}

static void put_word(enc_t *e, uint16_t w)
{
    put_byte(e, (uint8_t)(w >> 8));
    put_byte(e, (uint8_t)w);
}

/*
 * Entropy-coded bytes: a literal 0xFF has to be followed by 0x00 so a
 * decoder never mistakes it for a marker.
 */
static void put_bits(enc_t *e, uint16_t code, int size)
{
    if (size == 0)
        return;
    e->bit_acc = (e->bit_acc << size) | (uint32_t)(code & ((1u << size) - 1u));
    e->bit_cnt += size;
    while (e->bit_cnt >= 8) {
        uint8_t b = (uint8_t)(e->bit_acc >> (e->bit_cnt - 8));
        put_byte(e, b);
        if (b == 0xFF)
            put_byte(e, 0x00);
        e->bit_cnt -= 8;
    }
}

static void flush_bits(enc_t *e)
{
    while (e->bit_cnt > 0) {
        /* Pad with 1 bits, as the standard requires. */
        put_bits(e, 0xFF, 1);
    }
    e->bit_acc = 0;
    e->bit_cnt = 0;
}

/* ------------------------------------------------------- Huffman setup */

static void build_huff(const uint8_t *bits, const uint8_t *vals,
                       uint16_t *codes, uint8_t *sizes)
{
    memset(codes, 0, 256 * sizeof(uint16_t));
    memset(sizes, 0, 256);

    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len]; i++) {
            codes[vals[k]] = code;
            sizes[vals[k]] = (uint8_t)len;
            code++;
            k++;
        }
        code <<= 1;
    }
}

/* Quality 1..100 scaled onto the standard table, the conventional way. */
static void build_quant(enc_t *e, int quality)
{
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (int i = 0; i < 64; i++) {
        int q = (std_quant[i] * scale + 50) / 100;
        if (q < 1) q = 1;
        if (q > 255) q = 255;
        e->quant[i] = (uint8_t)q;
    }
}

/* ------------------------------------------------------------ the DCT */

static void fdct8x8(const float *in, float *out)
{
    static const float c[8][8] = {
        /* cos((2x+1) u pi / 16) * (u ? 0.5 : 0.353553390593) */
        { 0.353553391f, 0.490392640f, 0.461939766f, 0.415734806f,
          0.353553391f, 0.277785117f, 0.191341716f, 0.097545161f },
        { 0.353553391f, 0.415734806f, 0.191341716f,-0.097545161f,
         -0.353553391f,-0.490392640f,-0.461939766f,-0.277785117f },
        { 0.353553391f, 0.277785117f,-0.191341716f,-0.490392640f,
         -0.353553391f, 0.097545161f, 0.461939766f, 0.415734806f },
        { 0.353553391f, 0.097545161f,-0.461939766f,-0.277785117f,
          0.353553391f, 0.415734806f,-0.191341716f,-0.490392640f },
        { 0.353553391f,-0.097545161f,-0.461939766f, 0.277785117f,
          0.353553391f,-0.415734806f,-0.191341716f, 0.490392640f },
        { 0.353553391f,-0.277785117f,-0.191341716f, 0.490392640f,
         -0.353553391f,-0.097545161f, 0.461939766f,-0.415734806f },
        { 0.353553391f,-0.415734806f, 0.191341716f, 0.097545161f,
         -0.353553391f, 0.490392640f,-0.461939766f, 0.277785117f },
        { 0.353553391f,-0.490392640f, 0.461939766f,-0.415734806f,
          0.353553391f,-0.277785117f, 0.191341716f,-0.097545161f }
    };
    float tmp[64];

    /* Rows, then columns - separable, so 2 x 8 x 8 x 8 rather than 64^2. */
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0.0f;
            for (int x = 0; x < 8; x++)
                s += in[y * 8 + x] * c[x][u];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0.0f;
            for (int y = 0; y < 8; y++)
                s += tmp[y * 8 + u] * c[y][v];
            out[v * 8 + u] = s;
        }
}

/* Bits needed to represent a coefficient, and its one's-complement form. */
static int magnitude(int v, uint16_t *out_bits)
{
    int a = v < 0 ? -v : v;
    int n = 0;
    while (a) { a >>= 1; n++; }
    if (v < 0)
        v += (1 << n) - 1;
    *out_bits = (uint16_t)v;
    return n;
}

static void encode_block(enc_t *e, const float *pixels, int *dc_pred)
{
    float dct[64];
    int q[64];

    fdct8x8(pixels, dct);
    for (int i = 0; i < 64; i++) {
        float v = dct[i] / (float)e->quant[i];
        q[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }

    /* DC: difference against the previous block, which is exactly the
     * chain a restart marker breaks. */
    int diff = q[0] - *dc_pred;
    *dc_pred = q[0];
    if (diff == 0) {
        put_bits(e, e->dc_code[0], e->dc_size[0]);
    } else {
        uint16_t bits;
        int n = magnitude(diff, &bits);
        put_bits(e, e->dc_code[n], e->dc_size[n]);
        put_bits(e, bits, n);
    }

    /* AC in zigzag order, run-length coded. */
    int last = 0;
    for (int i = 1; i < 64; i++)
        if (q[zigzag[i]] != 0)
            last = i;

    int run = 0;
    for (int i = 1; i <= last; i++) {
        int v = q[zigzag[i]];
        if (v == 0) {
            run++;
            continue;
        }
        while (run > 15) {
            put_bits(e, e->ac_code[0xF0], e->ac_size[0xF0]);  /* ZRL */
            run -= 16;
        }
        uint16_t bits;
        int n = magnitude(v, &bits);
        int sym = (run << 4) | n;
        put_bits(e, e->ac_code[sym], e->ac_size[sym]);
        put_bits(e, bits, n);
        run = 0;
    }
    if (last < 63)
        put_bits(e, e->ac_code[0x00], e->ac_size[0x00]);      /* EOB */
}

/* ---------------------------------------------------------- the header */

static void write_headers(enc_t *e, uint16_t w, uint16_t h, uint16_t dri)
{
    put_word(e, 0xFFD8);                       /* SOI */

    put_word(e, 0xFFDB);                       /* DQT */
    put_word(e, 67);
    put_byte(e, 0x00);                         /* 8-bit, table 0 */
    for (int i = 0; i < 64; i++)
        put_byte(e, e->quant[zigzag[i]]);

    put_word(e, 0xFFC0);                       /* SOF0, baseline */
    put_word(e, 11);                           /* 8 + 3 * 1 component */
    put_byte(e, 8);
    put_word(e, h);
    put_word(e, w);
    put_byte(e, 1);                            /* one component: grayscale */
    put_byte(e, 1);                            /* id */
    put_byte(e, 0x11);                         /* 1x1 sampling */
    put_byte(e, 0);                            /* quant table 0 */

    put_word(e, 0xFFC4);                       /* DHT, DC */
    put_word(e, (uint16_t)(19 + 12));
    put_byte(e, 0x00);
    for (int i = 1; i <= 16; i++) put_byte(e, dc_bits[i]);
    for (int i = 0; i < 12; i++) put_byte(e, dc_vals[i]);

    put_word(e, 0xFFC4);                       /* DHT, AC */
    put_word(e, (uint16_t)(19 + 162));
    put_byte(e, 0x10);
    for (int i = 1; i <= 16; i++) put_byte(e, ac_bits[i]);
    for (int i = 0; i < 162; i++) put_byte(e, ac_vals[i]);

    if (dri > 0) {
        put_word(e, 0xFFDD);                   /* DRI */
        put_word(e, 4);
        put_word(e, dri);
    }

    put_word(e, 0xFFDA);                       /* SOS */
    put_word(e, 8);
    put_byte(e, 1);
    put_byte(e, 1);
    put_byte(e, 0x00);                         /* Huffman tables 0/0 */
    put_byte(e, 0);
    put_byte(e, 63);
    put_byte(e, 0);
}

/* ------------------------------------------------------------- public */

int loraitp_jpeg_encode(const uint8_t *gray, uint16_t w, uint16_t h,
                        int quality, uint16_t restart_interval,
                        uint8_t *out, size_t cap)
{
    if (gray == NULL || out == NULL || w == 0 || h == 0)
        return LORAITP_JPEG_E_ARG;
    if ((w % 8) != 0 || (h % 8) != 0)
        return LORAITP_JPEG_E_ARG;    /* no partial-MCU padding, by choice */

    enc_t e;
    memset(&e, 0, sizeof(e));
    e.buf = out;
    e.cap = cap;

    build_quant(&e, quality);
    build_huff(dc_bits, dc_vals, e.dc_code, e.dc_size);
    build_huff(ac_bits, ac_vals, e.ac_code, e.ac_size);
    write_headers(&e, w, h, restart_interval);

    int blocks_x = w / 8, blocks_y = h / 8;
    int dc_pred = 0;
    uint32_t mcu = 0;
    int rst = 0;

    for (int by = 0; by < blocks_y; by++) {
        for (int bx = 0; bx < blocks_x; bx++) {
            if (restart_interval > 0 && mcu > 0
                && (mcu % restart_interval) == 0) {
                /*
                 * Flush to a byte boundary, emit RSTn, and reset the DC
                 * predictor. This is the point a decoder can resynchronise
                 * at after losing data - which is the whole reason the
                 * markers are here.
                 */
                flush_bits(&e);
                put_byte(&e, 0xFF);
                put_byte(&e, (uint8_t)(0xD0 + (rst & 7)));
                rst++;
                dc_pred = 0;
            }

            float px[64];
            for (int y = 0; y < 8; y++) {
                size_t row = (size_t)(by * 8 + y) * (size_t)w
                             + (size_t)(bx * 8);
                for (int x = 0; x < 8; x++)
                    px[y * 8 + x] = (float)gray[row + (size_t)x] - 128.0f;
            }
            encode_block(&e, px, &dc_pred);
            mcu++;
        }
    }

    flush_bits(&e);
    put_word(&e, 0xFFD9);                      /* EOI */

    if (e.overflow)
        return LORAITP_JPEG_E_SPACE;
    return (int)e.len;
}

int loraitp_jpeg_encode_to_budget(const uint8_t *gray, uint16_t w, uint16_t h,
                                  size_t budget, uint16_t restart_interval,
                                  uint8_t *out, size_t cap, int *out_quality)
{
    /*
     * The duty cycle constrains bytes, not quality, so aim at a size.
     * Binary search over quality: eight encodes of a 320x240 frame is a
     * few hundred milliseconds against a transmission of tens of minutes,
     * which is not a trade worth thinking about twice.
     */
    int lo = 5, hi = 95, best_q = -1, best_len = LORAITP_JPEG_E_SPACE;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int n = loraitp_jpeg_encode(gray, w, h, mid, restart_interval,
                                    out, cap);
        if (n > 0 && (size_t)n <= budget) {
            best_q = mid;
            best_len = n;
            lo = mid + 1;                      /* try for better quality */
        } else {
            hi = mid - 1;
        }
    }

    if (best_q < 0) {
        /* Even the lowest quality overshoots. Emit it anyway and let the
         * caller decide: a picture over budget beats no picture. */
        best_q = lo > 5 ? lo : 5;
        best_len = loraitp_jpeg_encode(gray, w, h, best_q, restart_interval,
                                       out, cap);
        if (out_quality) *out_quality = best_q;
        return best_len;
    }

    /* Re-encode at the winning quality: the buffer holds the last attempt. */
    best_len = loraitp_jpeg_encode(gray, w, h, best_q, restart_interval,
                                   out, cap);
    if (out_quality) *out_quality = best_q;
    return best_len;
}
