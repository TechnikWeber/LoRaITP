/*
 * LoRaITP — LoRa Image Transfer Protocol
 * Public API of the portable core.  See SPEC.md for the wire format.
 *
 * Portability contract: C99, no dynamic allocation, no floating point
 * in the hot path, no platform headers. Everything reaches the outside
 * world through loraitp_port_t.
 */
#ifndef LORAITP_H
#define LORAITP_H

#include <stdbool.h>
#include <stdint.h>

#include "loraitp_config.h"
#include "../port/loraitp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAITP_VERSION 0

/* ------------------------------------------------------- frame types */

typedef enum {
    LORAITP_META     = 0x01,
    LORAITP_DATA     = 0x02,
    LORAITP_EOB      = 0x03,
    LORAITP_STAT     = 0x04,
    LORAITP_FIN      = 0x05,
    LORAITP_FINACK   = 0x06,
    LORAITP_PROBE    = 0x07,
    LORAITP_PROBEACK = 0x08,
    LORAITP_IDENT    = 0x09,
    LORAITP_PARITY   = 0x0A,
    LORAITP_ABORT    = 0x0B
} loraitp_frame_type_t;

/* --------------------------------------------------- transfer modes */

typedef enum {
    /*
     * Both ends transmit. The receiver reports missing chunks and the
     * sender repairs them. Cheapest when the link works, because you
     * retransmit exactly what was lost and nothing else.
     */
    LORAITP_MODE_INTERACTIVE = 0,

    /*
     * One-way. The receiver never transmits — it may be unable to, or
     * not allowed to, or there may be many receivers. The sender emits
     * source chunks plus parity and never learns whether any of it
     * arrived. Requires LORAITP_ENABLE_FEC.
     */
    LORAITP_MODE_BROADCAST = 1
} loraitp_mode_t;

/* ------------------------------------------------ regulatory profiles */

typedef enum {
    LORAITP_REG_EU868_G3 = 0,   /* 869.4-869.65, 500 mW ERP, 10%  (default) */
    LORAITP_REG_EU868_G1,       /* 868.0-868.6,   25 mW ERP,  1%  */
    LORAITP_REG_EU868_G2,       /* 868.7-869.2,   25 mW ERP,  0.1% */
    LORAITP_REG_EU868_G4,       /* 869.7-870.0,   25 mW ERP,  1%  */
    LORAITP_REG_EU868_G4_LP,    /* 869.7-870.0,    5 mW ERP, no duty limit */
    LORAITP_REG_EU433,          /* 433.05-434.79, 10 mW ERP, 10%  */
    LORAITP_REG_EU433_NARROW,   /* 434.04-434.79, 10 mW ERP, no limit, BW<=25k */
    LORAITP_REG_AMATEUR,        /* no duty limit; call sign mandatory */
    LORAITP_REG_TEST_UNRESTRICTED,  /* dummy load / simulator only */
    LORAITP_REG__COUNT
} loraitp_region_t;

/* --------------------------------------------------------- session */

typedef struct {
    loraitp_mode_t   mode;
    loraitp_region_t region;

    uint32_t frequency_hz;
    uint8_t  spreading_factor;   /* 0 = probe and choose automatically */
    uint32_t bandwidth_hz;
    int8_t   tx_power_dbm;

    uint8_t  chunk_len;          /* 0 = derive from LORAITP_MAX_TOA_MS */
    uint8_t  parity_percent;     /* broadcast mode; 0 in interactive */

    /* Authentication. Ignored when key_present is false. */
    bool     key_present;
    uint8_t  key[16];
    bool     mac_data_frames;    /* MAC bulk frames too; costs 2% airtime */

    /* Amateur service. Required when region == LORAITP_REG_AMATEUR. */
    const char *callsign;
    uint16_t ident_interval_s;   /* 0 = default 540 */

    uint32_t session_timeout_ms;
} loraitp_session_cfg_t;

typedef struct {
    uint16_t img_id;
    uint8_t  layer;
    uint32_t img_len;
    uint8_t  chunk_len;
    uint8_t  codec;
    uint32_t img_crc32;
    uint16_t width, height;
    uint16_t n_chunks;
    uint16_t n_parity;
    uint8_t  block_size;
} loraitp_image_desc_t;

typedef struct {
    uint16_t chunks_sent, chunks_received, chunks_missing;
    uint16_t rounds;
    uint32_t airtime_ms;
    int16_t  last_rssi_dbm;
    int8_t   last_snr_qdb;
} loraitp_stats_t;

/* Opaque; size is fixed at compile time, allocate it yourself. */
typedef struct loraitp_ctx loraitp_ctx_t;
size_t loraitp_ctx_size(void);

int loraitp_init(loraitp_ctx_t *ctx, const loraitp_port_t *port,
                 const loraitp_session_cfg_t *cfg);

/* ------------------------------------------------------------ sender */

int loraitp_send_image(loraitp_ctx_t *ctx, const loraitp_image_desc_t *desc,
                       loraitp_stats_t *out_stats);

/* ---------------------------------------------------------- receiver */

/*
 * Result of a receive attempt. Note that INCOMPLETE is not a failure:
 * with chunk-aligned restart markers a partial image still decodes into
 * a partial picture, so the caller should write it out regardless.
 */
typedef enum {
    LORAITP_RX_COMPLETE = 0,    /* all chunks present, image CRC verified */
    LORAITP_RX_INCOMPLETE,      /* usable partial image */
    LORAITP_RX_UNRECOVERABLE,   /* provably cannot be completed, see below */
    LORAITP_RX_CRC_MISMATCH,    /* complete but CRC failed: undetected PHY error */
    LORAITP_RX_TIMEOUT,         /* nothing heard */
    LORAITP_RX_ABORTED
} loraitp_rx_result_t;

int loraitp_receive_image(loraitp_ctx_t *ctx, loraitp_image_desc_t *out_desc,
                          loraitp_rx_result_t *out_result,
                          loraitp_stats_t *out_stats);

/*
 * Broadcast-mode feasibility test.
 *
 * The receiver in broadcast mode has no way to ask for anything, so its
 * only decisions are "keep listening" and "stop". Both matter: listening
 * costs power, and stopping too early loses the image.
 *
 * With an erasure code, the question is exactly decidable. A block of k
 * source chunks decodes from any k of the k+r frames sent for it, so at
 * any moment:
 *
 *     achievable = received_distinct + (frames_total - frames_elapsed)
 *
 * If achievable < k the block can never be completed, however long we
 * listen. Once that holds for every block, the receiver powers down
 * immediately instead of waiting out the session timeout.
 *
 * Returns true while at least one block is still recoverable.
 */
bool loraitp_rx_still_recoverable(const loraitp_ctx_t *ctx);

/* Fraction of the image in hand, 0..100. Meaningful mid-transfer. */
uint8_t loraitp_rx_progress(const loraitp_ctx_t *ctx);

/* ------------------------------------------------- duty cycle governor */

typedef struct {
    uint32_t airtime_used_ms;      /* in the trailing observation window */
    uint32_t airtime_budget_ms;    /* what the region allows in that window */
    uint32_t blocked_for_ms;       /* 0 = may transmit now */
    uint32_t airtime_today_ms;
} loraitp_budget_t;

void loraitp_budget_query(const loraitp_ctx_t *ctx, loraitp_budget_t *out);

/*
 * How many image bytes the remaining budget allows at the current
 * settings. Lets an application decide "send the thumbnail only today".
 */
uint32_t loraitp_budget_bytes_remaining(const loraitp_ctx_t *ctx);

/* Time on air in microseconds. Mirrors tools/airtime.py exactly. */
uint32_t loraitp_time_on_air_us(uint8_t payload_len, uint8_t sf,
                                uint32_t bw_hz, uint8_t cr,
                                uint8_t preamble_symbols);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_H */
