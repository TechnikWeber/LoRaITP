/*
 * Grayscale baseline JPEG encoder with restart markers.
 * See jpeg.c for why this exists rather than using the camera's encoder.
 */
#ifndef LORAITP_JPEG_H
#define LORAITP_JPEG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORAITP_JPEG_E_ARG   (-1)
#define LORAITP_JPEG_E_SPACE (-2)

/*
 * Encode `gray` (w * h bytes, 8-bit) to baseline JPEG.
 *
 * restart_interval is in MCUs - for grayscale, one MCU is one 8x8 block.
 * Passing w/8 puts a marker at the start of every pixel row of blocks,
 * which costs 2 bytes each and bounds the damage from a lost packet to
 * roughly two rows. Zero disables them, which is what you want only if
 * the link is lossless.
 *
 * Width and height must be multiples of 8.
 * Returns the encoded length, or a negative error.
 */
int loraitp_jpeg_encode(const uint8_t *gray, uint16_t w, uint16_t h,
                        int quality, uint16_t restart_interval,
                        uint8_t *out, size_t cap);

/*
 * Encode to fit `budget` bytes, picking the highest quality that does.
 * The duty cycle constrains bytes rather than quality, so this is the
 * call the application actually wants.
 */
int loraitp_jpeg_encode_to_budget(const uint8_t *gray, uint16_t w, uint16_t h,
                                  size_t budget, uint16_t restart_interval,
                                  uint8_t *out, size_t cap, int *out_quality);

#ifdef __cplusplus
}
#endif
#endif
