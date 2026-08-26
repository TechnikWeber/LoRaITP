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
    LORAITP_REG_LOCAL,          /* the operator states the limits; SPEC.md 6.5 */
    LORAITP_REG__COUNT
} loraitp_region_t;

/* --------------------------------------------------------- session */

/* ------------------------------------------------------------- tracing */

/*
 * An optional window into what the radio is doing, frame by frame.
 *
 * Without it the only visible sign of a session is its result, which is
 * no help at all when the answer is "nothing arrived" - the interesting
 * question is then whether anything was heard, and that is a per-frame
 * question. The callback is small, synchronous, and called from inside
 * the session, so an implementation must not block: append to a ring
 * buffer and return.
 */
typedef enum {
    LORAITP_EV_TX = 1,        /* a frame left the antenna */
    LORAITP_EV_RX,            /* a frame arrived and verified */
    LORAITP_EV_RX_TIMEOUT,    /* a listening window closed empty */
    LORAITP_EV_MAC_REJECT,    /* a frame failed its MAC - or was not ours */
    LORAITP_EV_DUTY_WAIT,     /* the governor made us wait */
    LORAITP_EV_ROUND          /* a repair round finished */
} loraitp_event_t;

typedef struct {
    uint8_t  ev;              /* loraitp_event_t */
    uint8_t  frame_type;      /* LORAITP_DATA, LORAITP_STAT, ... */
    uint16_t seq;             /* chunk index, or block for EOB/STAT */
    uint8_t  len;             /* bytes on the wire */
    int16_t  rssi_dbm;
    int8_t   snr_qdb;
    uint32_t value;           /* wait in ms, or chunks still missing */
} loraitp_trace_t;

typedef void (*loraitp_trace_cb)(void *user, const loraitp_trace_t *t);

/* Config flags */
#define LORAITP_CFG_ENCRYPTED (1u << 0)   /* refused in amateur mode */
#define LORAITP_CFG_MAC_DATA  (1u << 1)   /* authenticate bulk frames too */
#define LORAITP_CFG_PROBE     (1u << 2)   /* measure the link, pick an SF */

typedef struct {
    loraitp_mode_t   mode;
    loraitp_region_t region;

    uint32_t frequency_hz;
    uint8_t  spreading_factor;   /* 0 = probe and choose automatically */
    uint32_t bandwidth_hz;
    uint8_t  coding_rate;        /* 1..4 -> 4/5 .. 4/8 */
    int8_t   tx_power_dbm;

    uint8_t  chunk_len;          /* 0 = derive from LORAITP_MAX_TOA_MS */
    uint16_t block_size;         /* 0 = LORAITP_BLOCK_SIZE */
    uint8_t  parity_percent;     /* broadcast mode; 0 in interactive */

    uint32_t flags;

    /* Authentication. Ignored when the port has no AES block function. */
    uint8_t  key[16];

    /* Amateur service. Required when region == LORAITP_REG_AMATEUR. */
    const char *callsign;
    uint16_t ident_interval_s;   /* 0 = default 540 */

    /*
     * The duty cycle for LORAITP_REG_LOCAL, in percent; 0 means none.
     * Ignored for every other region, where the figure comes from the
     * published allocation rather than from whoever is holding the
     * board. See SPEC.md 6.5.
     */
    uint8_t  local_duty_percent;

    uint32_t session_timeout_ms;
    uint8_t  max_rounds;         /* 0 = 8 */
    uint8_t  eob_retry;          /* 0 = 3 */

    /*
     * Scratch for erasure coding, supplied by the caller so the core
     * allocates nothing. Required in broadcast mode, ignored otherwise.
     * loraitp_fec_scratch_needed() sizes it.
     */
    uint8_t *fec_scratch;
    size_t   fec_scratch_len;

    /* Optional; NULL disables tracing entirely, at no cost. */
    loraitp_trace_cb trace;
    void            *trace_user;
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
    uint32_t frames_tx, frames_rx;
    uint32_t retransmits;
    uint32_t mac_rejects;     /* frames discarded by the MAC check */
    uint32_t airtime_ms;
    uint16_t rounds;
    uint16_t chunks_have, chunks_total;
    uint16_t blocks_lost;     /* blocks that could not be reconstructed */
    int16_t  last_rssi_dbm;
    int8_t   last_snr_qdb;
} loraitp_stats_t;

/* Scratch needed for erasure coding a block of k source and r parity
 * chunks. Supplied by the caller so the core allocates nothing. */
size_t loraitp_fec_session_scratch(uint16_t k, uint16_t r,
                                   uint16_t chunk_len);

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

/* Pruning the rolling window mutates it, so this is not const. */
void loraitp_budget_query(loraitp_ctx_t *ctx, loraitp_budget_t *out);

/*
 * How many image bytes the remaining budget allows at the current
 * settings. Lets an application decide "send the thumbnail only today".
 */
uint32_t loraitp_budget_bytes_remaining(loraitp_ctx_t *ctx);

/*
 * The rolling window, in a form that survives losing RAM.
 *
 * The window is the one piece of protocol state that must outlive the
 * program holding it. An hour of airtime is an hour of airtime whether
 * or not the board rebooted in the middle of it, and a station that
 * forgets what it sent transmits over its budget in perfect good faith -
 * which is exactly what a duty cycle is written to prevent. Deep sleep,
 * a settings change that restarts, a watchdog, a brown-out: all of them
 * end with an empty window and a clean conscience.
 *
 * So: export into memory that survives the reboot in question, import
 * once the context is up again. `away_ms` is how much real time passed
 * between the two - the sleep duration, or 0 for a restart, where a
 * couple of seconds under-counted only makes the result stricter.
 *
 * Entries that aged out while away are dropped. Nothing here is trusted:
 * a blob with the wrong magic, version or checksum is refused rather
 * than interpreted, because the alternative to "start with an empty
 * window" is "start with somebody else's".
 *
 * The snapshot is region-agnostic on purpose. Airtime carried into a
 * different band restricts the station rather than freeing it, and being
 * strict about a band you have left is not a fault worth code.
 */
#define LORAITP_BUDGET_STATE_MAX 1040u

/*
 * Returns bytes written, or LORAITP_E_ARG if cap is too small. Pruning
 * the window mutates it, so ctx is not const.
 *
 * Cheap enough for the caller to do after every transmission, which is
 * the only way the snapshot is current when an unplanned reboot takes
 * it: 128 entries at most, folded like a full ring if there are more.
 */
int loraitp_budget_export(loraitp_ctx_t *ctx, void *buf, size_t cap);

/* Returns LORAITP_OK, or LORAITP_E_ARG for a blob this build cannot
 * read. On failure the window is left exactly as it was. */
int loraitp_budget_import(loraitp_ctx_t *ctx, const void *buf, size_t len,
                          uint32_t away_ms);

/* Time on air in microseconds. Mirrors tools/airtime.py exactly. */
uint32_t loraitp_time_on_air_us(uint8_t payload_len, uint8_t sf,
                                uint32_t bw_hz, uint8_t cr,
                                uint8_t preamble_symbols);

/* Largest payload whose time on air stays within toa_us. */
uint8_t loraitp_max_payload_for_toa(uint8_t sf, uint32_t bw_hz, uint8_t cr,
                                    uint32_t toa_us);

/* Fastest spreading factor with `margin_qdb` quarter-dB to spare. */
uint8_t loraitp_choose_sf(int16_t snr_qdb, int16_t margin_qdb);

/* -------------------------------------------------------------- CRC-32 */

uint32_t loraitp_crc32(const uint8_t *buf, size_t len);
uint32_t loraitp_crc32_update(uint32_t crc, const uint8_t *buf, size_t len);

/* --------------------------------------------------- erasure coding */

size_t loraitp_fec_scratch(uint16_t k);
int loraitp_fec_encode(const uint8_t *data, uint16_t k, uint16_t chunk_len,
                       uint8_t *parity, uint16_t r);
int loraitp_fec_decode(uint8_t *chunks, const uint8_t *present, uint16_t k,
                       uint16_t r, uint16_t chunk_len, uint8_t *scratch,
                       size_t scratch_len, uint8_t *workspace);

/* -------------------------------------------------- authentication */

int  loraitp_cmac(const loraitp_port_t *port, const uint8_t *nonce,
                  size_t nonce_len, const uint8_t *msg, size_t msg_len,
                  uint8_t *out, size_t out_len);
bool loraitp_frame_is_authenticated(int ftype, bool mac_data);
int  loraitp_seal(const loraitp_port_t *port, const uint8_t *nonce,
                  bool mac_data, uint8_t *buf, size_t len, size_t cap);
int  loraitp_unseal(const loraitp_port_t *port, const uint8_t *nonce,
                    bool mac_data, const uint8_t *buf, size_t len);

/* ------------------------------------------------- STAT body coding */

int loraitp_stat_choose_enc(const uint16_t *missing, uint16_t n_missing,
                            uint16_t base);
int loraitp_stat_encode_body(int enc, const uint16_t *missing,
                             uint16_t n_missing, uint16_t base,
                             uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_H */
