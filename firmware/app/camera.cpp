/*
 * Camera capture for the XIAO ESP32S3 Sense.
 *
 * Captures 8-bit grayscale and encodes it here rather than letting the
 * OV2640 produce JPEG. The sensor's own encoder is faster and worse on
 * all three axes that matter: no grayscale-only output, weak control of
 * the byte budget, and no say over the restart interval. Encoding costs a
 * few hundred milliseconds against a transmission of tens of minutes, so
 * that is not a close call.
 */
#include <Arduino.h>

#include "camera.h"

#if LORAITP_HAS_CAMERA

#include <esp_camera.h>

#include "jpeg.h"

/*
 * XIAO ESP32S3 Sense camera pins, from Seeed's reference design. All of
 * these reach the B2B connector and none of them appear on an edge pin,
 * which is why the radio can have the edge pins to itself.
 */
#define CAM_PWDN  (-1)
#define CAM_RESET (-1)
#define CAM_XCLK  10
#define CAM_SIOD  40
#define CAM_SIOC  39
#define CAM_Y9    48
#define CAM_Y8    11
#define CAM_Y7    12
#define CAM_Y6    14
#define CAM_Y5    16
#define CAM_Y4    18
#define CAM_Y3    17
#define CAM_Y2    15
#define CAM_VSYNC 38
#define CAM_HREF  47
#define CAM_PCLK  13

static bool g_ready = false;

bool loraitp_camera_init(void)
{
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0 = CAM_Y2;  c.pin_d1 = CAM_Y3;  c.pin_d2 = CAM_Y4;
    c.pin_d3 = CAM_Y5;  c.pin_d4 = CAM_Y6;  c.pin_d5 = CAM_Y7;
    c.pin_d6 = CAM_Y8;  c.pin_d7 = CAM_Y9;
    c.pin_xclk = CAM_XCLK;
    c.pin_pclk = CAM_PCLK;
    c.pin_vsync = CAM_VSYNC;
    c.pin_href = CAM_HREF;
    c.pin_sccb_sda = CAM_SIOD;
    c.pin_sccb_scl = CAM_SIOC;
    c.pin_pwdn = CAM_PWDN;
    c.pin_reset = CAM_RESET;
    c.xclk_freq_hz = 20000000;

    /*
     * Grayscale straight out of the sensor: half the buffer of RGB565 and
     * exactly what the encoder wants. 320x240 is 76.8 kB, which fits in
     * internal SRAM even on a board without PSRAM.
     */
    c.pixel_format = PIXFORMAT_GRAYSCALE;
    c.frame_size = FRAMESIZE_QVGA;          /* 320 x 240 */
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        Serial.printf("camera init failed: 0x%x\n", err);
        return false;
    }
    g_ready = true;
    return true;
}

void loraitp_camera_deinit(void)
{
    if (g_ready) {
        esp_camera_deinit();
        g_ready = false;
    }
}

int loraitp_camera_capture_jpeg(size_t budget, uint16_t restart_interval,
                                uint8_t *out, size_t cap,
                                loraitp_capture_info_t *info)
{
    if (!g_ready)
        return -1;

    /*
     * Throw the first frames away. The sensor's automatic exposure and
     * gain need a few frames to settle, and on a node that wakes from
     * deep sleep once a day the first frame is always wrong - usually
     * far too dark, which then compresses beautifully and looks like a
     * working system producing black pictures.
     */
    for (int i = 0; i < 3; i++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm)
            esp_camera_fb_return(warm);
        delay(50);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL)
        return -1;

    int quality = 0;
    int n = -1;
    if (fb->format == PIXFORMAT_GRAYSCALE
        && fb->len >= (size_t)fb->width * fb->height) {
        /*
         * Zero means "one marker per row of blocks", derived from the
         * frame we actually got rather than from what the caller assumed
         * the sensor would return. A hard-coded interval computed from an
         * expected width is wrong the moment the frame size changes.
         */
        uint16_t dri = restart_interval ? restart_interval
                                        : (uint16_t)(fb->width / 8);
        n = loraitp_jpeg_encode_to_budget(fb->buf, (uint16_t)fb->width,
                                          (uint16_t)fb->height, budget,
                                          dri, out, cap,
                                          &quality);
        if (info != NULL) {
            info->width = (uint16_t)fb->width;
            info->height = (uint16_t)fb->height;
            info->quality = quality;
            info->raw_bytes = fb->len;
        }
    }
    esp_camera_fb_return(fb);
    return n;
}

#else   /* no camera on this board */

bool loraitp_camera_init(void) { return false; }
void loraitp_camera_deinit(void) {}
int loraitp_camera_capture_jpeg(size_t, uint16_t, uint8_t *, size_t,
                                loraitp_capture_info_t *)
{
    return -1;
}

#endif
