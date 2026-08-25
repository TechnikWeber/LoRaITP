/*
 * JPEG encoder tests.
 *
 * Structural checks here; the real verification is tests/verify_jpeg.py,
 * which decodes the output with an independent decoder and compares it
 * against the input. An encoder that produces a plausible-looking file
 * nothing can decode is exactly the failure worth catching before it
 * reaches a node in a field.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg.h"

static int passed, failed;
static void check(int cond, const char *name)
{
    if (cond) { passed++; printf("  ok   %s\n", name); }
    else      { failed++; printf("  FAIL %s\n", name); }
}

#define W 320
#define H 240
static uint8_t gray[W * H];
static uint8_t out[64 * 1024];

/* A scene with gradients, edges and texture - flat grey compresses to
 * nothing and would hide most encoder bugs. */
static void make_image(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int v = (x * 255) / W;                       /* gradient */
            if (((x / 16) + (y / 16)) % 2)               /* checker */
                v = 255 - v;
            if (y > H / 2)                               /* texture */
                v = (v + ((x * 7 + y * 13) & 0x3F)) & 0xFF;
            gray[y * W + x] = (uint8_t)v;
        }
}

static int count_markers(const uint8_t *b, size_t n, uint8_t lo, uint8_t hi)
{
    int c = 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (b[i] == 0xFF && b[i + 1] >= lo && b[i + 1] <= hi)
            c++;
    return c;
}

int main(int argc, char **argv)
{
    printf("JPEG encoder\n");
    make_image();

    int n = loraitp_jpeg_encode(gray, W, H, 50, 0, out, sizeof(out));
    check(n > 0, "encodes at quality 50");
    check(out[0] == 0xFF && out[1] == 0xD8, "starts with SOI");
    check(out[n - 2] == 0xFF && out[n - 1] == 0xD9, "ends with EOI");
    printf("       %d bytes at Q50, no restart markers\n", n);

    /* Sizes must move the right way with quality. */
    int lo = loraitp_jpeg_encode(gray, W, H, 10, 0, out, sizeof(out));
    int hi = loraitp_jpeg_encode(gray, W, H, 90, 0, out, sizeof(out));
    check(lo > 0 && hi > 0 && lo < hi, "lower quality gives a smaller file");
    printf("       Q10 %d B, Q90 %d B\n", lo, hi);

    /* Restart markers. */
    int rows = H / 8;
    n = loraitp_jpeg_encode(gray, W, H, 50, W / 8, out, sizeof(out));
    check(n > 0, "encodes with one marker per MCU row");
    int rst = count_markers(out, (size_t)n, 0xD0, 0xD7);
    check(rst == rows - 1, "one RST between every pair of rows");
    printf("       %d RST markers for %d rows, file %d B\n", rst, rows, n);

    int without = loraitp_jpeg_encode(gray, W, H, 50, 0, out, sizeof(out));
    n = loraitp_jpeg_encode(gray, W, H, 50, W / 8, out, sizeof(out));
    double overhead = 100.0 * (n - without) / without;
    check(overhead < 5.0, "restart markers cost under 5% of the file");
    printf("       restart overhead %.2f%%\n", overhead);

    /* Budget mode is the one the application uses. */
    int q = 0;
    n = loraitp_jpeg_encode_to_budget(gray, W, H, 8000, W / 8, out,
                                      sizeof(out), &q);
    check(n > 0 && n <= 8000, "budget mode fits 8000 bytes");
    printf("       budget 8000 -> %d B at Q%d\n", n, q);

    int n2 = loraitp_jpeg_encode_to_budget(gray, W, H, 3000, W / 8, out,
                                           sizeof(out), &q);
    check(n2 > 0 && n2 <= 3000, "budget mode fits 3000 bytes");
    check(n2 < n, "a tighter budget gives a smaller file");
    printf("       budget 3000 -> %d B at Q%d\n", n2, q);

    /* Bad inputs. */
    check(loraitp_jpeg_encode(NULL, W, H, 50, 0, out, sizeof(out))
          == LORAITP_JPEG_E_ARG, "null input rejected");
    check(loraitp_jpeg_encode(gray, 321, H, 50, 0, out, sizeof(out))
          == LORAITP_JPEG_E_ARG, "non-multiple-of-8 width rejected");
    check(loraitp_jpeg_encode(gray, W, H, 90, 0, out, 100)
          == LORAITP_JPEG_E_SPACE, "an undersized buffer is reported");

    /* Dump files for verify_jpeg.py to decode independently. */
    if (argc > 1) {
        char path[256];
        struct { int q; uint16_t dri; const char *tag; } cases[] = {
            { 50, 0,     "q50" },
            { 50, W / 8, "q50_rst" },
            { 90, W / 8, "q90_rst" },
            { 15, W / 8, "q15_rst" },
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            int len = loraitp_jpeg_encode(gray, W, H, cases[i].q,
                                          cases[i].dri, out, sizeof(out));
            snprintf(path, sizeof(path), "%s/%s.jpg", argv[1], cases[i].tag);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(out, 1, (size_t)len, f); fclose(f); }
        }
        snprintf(path, sizeof(path), "%s/source.raw", argv[1]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(gray, 1, sizeof(gray), f); fclose(f); }
        printf("       wrote samples to %s\n", argv[1]);
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
