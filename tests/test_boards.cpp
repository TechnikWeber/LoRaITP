/*
 * Compile every board header as C++.
 *
 * The pin maps are included from .cpp translation units on the target,
 * and C++ is stricter than C about designated initialisers: it requires
 * them in declaration order, where C allows any order. A header that
 * passes a C syntax check can therefore still break the firmware build,
 * which is exactly what happened once.
 *
 * Building here is cheap and catches it in seconds instead of in a
 * three-minute cross-compile.
 */
#include <stdio.h>

#define LORAITP_PIN_NONE 0xFF

#include <stdbool.h>
#include <stdint.h>

/* The real struct, from board.h. Kept in one place; each board header is
 * then included inside its own namespace so four definitions of
 * LORAITP_BOARD can coexist in one translation unit. */
struct loraitp_board_t {
    const char *name;
    uint8_t lora_sck, lora_miso, lora_mosi;
    uint8_t lora_nss, lora_rst, lora_busy, lora_dio1;
    uint8_t lora_ant_sw;
    bool    lora_tcxo;
    float   lora_tcxo_v;
    int8_t  max_tx_dbm;
    uint8_t led, button;
    uint8_t vext_ctrl;
    uint8_t vbat_adc, vbat_ctrl;
    uint8_t oled_sda, oled_scl, oled_rst;
    bool    has_camera, has_wifi, has_psram;
    uint32_t flash_kb;
};

/* A preprocessor directive has to own its line, so the closing brace
 * cannot share one with the #include. */
namespace heltec_v3 {
typedef ::loraitp_board_t loraitp_board_t;
#include "heltec_v3.h"
}
namespace heltec_v4 {
typedef ::loraitp_board_t loraitp_board_t;
#include "heltec_v4.h"
}
namespace xiao_esp32s3 {
typedef ::loraitp_board_t loraitp_board_t;
#include "xiao_esp32s3_sense.h"
}

/* Both XIAO headers name their edge pins the same; drop them
 * between includes so the second set is not a redefinition. */
#undef XIAO_D0
#undef XIAO_D1
#undef XIAO_D2
#undef XIAO_D3
#undef XIAO_D4
#undef XIAO_D5
#undef XIAO_D6
#undef XIAO_D7
#undef XIAO_D8
#undef XIAO_D9
#undef XIAO_D10
#undef XIAO_D11
#undef XIAO_D12
namespace xiao_kit {
typedef ::loraitp_board_t loraitp_board_t;
#include "xiao_esp32s3_kit.h"
}
namespace xiao_nrf52840 {
typedef ::loraitp_board_t loraitp_board_t;
#include "xiao_nrf52840.h"
}

static int passed, failed;
static void check(bool cond, const char *name)
{
    if (cond) { passed++; printf("  ok   %s\n", name); }
    else      { failed++; printf("  FAIL %s\n", name); }
}

static void one(const ::loraitp_board_t &b)
{
    char msg[96];

    snprintf(msg, sizeof(msg), "%s compiles as C++", b.name);
    check(true, msg);

    /* A radio needs all four control lines; a missing one is a hang on
     * the first SPI command, not an error message. */
    snprintf(msg, sizeof(msg), "%s has NSS, RST, BUSY and DIO1", b.name);
    check(b.lora_nss != LORAITP_PIN_NONE && b.lora_rst != LORAITP_PIN_NONE
          && b.lora_busy != LORAITP_PIN_NONE
          && b.lora_dio1 != LORAITP_PIN_NONE, msg);

    snprintf(msg, sizeof(msg), "%s has an SPI bus", b.name);
    check(b.lora_sck != LORAITP_PIN_NONE && b.lora_miso != LORAITP_PIN_NONE
          && b.lora_mosi != LORAITP_PIN_NONE, msg);

    /* Two signals on one pin is a wiring mistake that looks like a
     * mysterious radio fault. */
    uint8_t pins[] = { b.lora_sck, b.lora_miso, b.lora_mosi, b.lora_nss,
                       b.lora_rst, b.lora_busy, b.lora_dio1, b.lora_ant_sw };
    bool clash = false;
    for (unsigned i = 0; i < sizeof(pins); i++)
        for (unsigned j = i + 1; j < sizeof(pins); j++)
            if (pins[i] != LORAITP_PIN_NONE && pins[i] == pins[j])
                clash = true;
    snprintf(msg, sizeof(msg), "%s assigns no pin twice", b.name);
    check(!clash, msg);

    snprintf(msg, sizeof(msg), "%s declares a plausible power ceiling", b.name);
    check(b.max_tx_dbm > 0 && b.max_tx_dbm <= 60, msg);
}

int main(void)
{
    printf("board pin maps\n");
    one(heltec_v3::LORAITP_BOARD);
    one(heltec_v4::LORAITP_BOARD);
    one(xiao_esp32s3::LORAITP_BOARD);
    one(xiao_kit::LORAITP_BOARD);
    one(xiao_nrf52840::LORAITP_BOARD);

    /* The XIAO's radio reset shares GPIO3 with the Sense SD card's chip
     * select. That is documented and deliberate - assert it so nobody
     * "fixes" the header without reading why. */
    check(xiao_esp32s3::LORAITP_BOARD.lora_rst == 3,
          "XIAO RST is on GPIO3, the pin the SD card also uses");
    check(xiao_esp32s3::LORAITP_BOARD.lora_ant_sw == 6,
          "XIAO RF_SW is on GPIO6 and must be driven");

    /* The Kit radio sits where the camera would; the two cannot coexist
     * and the header must not claim otherwise. */
    check(!xiao_kit::LORAITP_BOARD.has_camera,
          "the Kit board declares no camera - it occupies that connector");
    check(xiao_kit::LORAITP_BOARD.lora_busy == 40
          && xiao_kit::LORAITP_BOARD.lora_dio1 == 39,
          "Kit control pins are on the B2B GPIOs, not the edge");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
