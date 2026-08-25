/*
 * Camera capture. Present on boards whose pin map says has_camera.
 */
#ifndef LORAITP_CAMERA_H
#define LORAITP_CAMERA_H

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#ifndef LORAITP_HAS_CAMERA
#  if defined(LORAITP_BOARD_XIAO_ESP32S3_SENSE)
#    define LORAITP_HAS_CAMERA 1
#  else
#    define LORAITP_HAS_CAMERA 0
#  endif
#endif

typedef struct {
    uint16_t width, height;
    int      quality;        /* the JPEG quality the budget allowed */
    size_t   raw_bytes;
} loraitp_capture_info_t;

bool loraitp_camera_init(void);
void loraitp_camera_deinit(void);

/*
 * Capture one frame and encode it to fit `budget` bytes. Returns the
 * encoded length or a negative value.
 *
 * `budget` should come from loraitp_budget_bytes_remaining() rather than
 * from a constant: what the picture may cost is a regulatory question,
 * and it changes through the day.
 */
int loraitp_camera_capture_jpeg(size_t budget, uint16_t restart_interval,
                                uint8_t *out, size_t cap,
                                loraitp_capture_info_t *info);

#endif
