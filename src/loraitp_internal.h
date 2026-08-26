/*
 * Internal definitions. Not part of the public API and not installed.
 */
#ifndef LORAITP_INTERNAL_H
#define LORAITP_INTERNAL_H

#include "loraitp.h"
#include "loraitp_frame.h"

/* Tobs from BNetzA Vfg. 91/2025: a rolling hour. */
#define LORAITP_DC_WINDOW_MS 3600000u

/* Transmissions tracked in the rolling window. At SF10 on a 10% band the
 * off-time is ~17 s per frame, so an hour holds about 200. */
#ifndef LORAITP_DC_SLOTS
#define LORAITP_DC_SLOTS 256
#endif

#define LORAITP_IDENT_INTERVAL_S 540u

/*
 * Layout of a duty-cycle snapshot (loraitp_gov_export). Here rather than
 * in the .c file so the tests read the same numbers the encoder writes.
 */
#define STATE_MAGIC   0x5744434Cu      /* 'LCDW' */
#define STATE_VERSION 1u
#define STATE_HDR     20u
#define STATE_ENTRY   8u
#define STATE_TAIL    4u               /* the CRC */
#define STATE_MAX_ENTRIES \
    ((LORAITP_BUDGET_STATE_MAX - STATE_HDR - STATE_TAIL) / STATE_ENTRY)

typedef struct {
    const char *name;
    uint32_t f_lo_hz, f_hi_hz;
    uint32_t duty_ppm;        /* 0 = no limit */
    int8_t   max_erp_dbm;
    uint32_t max_bw_hz;       /* 0 = no limit */
    bool     amateur;
} loraitp_region_info_t;

typedef struct {
    loraitp_region_t region;

    /*
     * Normally a row of the static table. For LORAITP_REG_LOCAL it
     * points at `local` below, which the operator filled in - so the
     * governor enforces a profile rather than a jurisdiction, and the
     * code that does the enforcing does not have to know the difference.
     */
    const loraitp_region_info_t *info;
    loraitp_region_info_t local;
    uint32_t slot_end[LORAITP_DC_SLOTS];
    uint32_t slot_toa[LORAITP_DC_SLOTS];
    uint16_t head, n_slots;
    uint32_t blocked_until;
    uint32_t airtime_total_ms;
    uint32_t last_ident_ms;
    uint32_t ident_interval_ms;
    bool     has_identified;
    bool     ident_pending;
} loraitp_gov_t;

struct loraitp_ctx {
    const loraitp_port_t *port;
    loraitp_session_cfg_t cfg;
    loraitp_gov_t gov;

    loraitp_meta_t meta;
    bool     have_meta;
    uint8_t  sid;
    uint8_t  nonce[4];

    /* One frame in each direction, plus room for the MAC. */
    uint8_t  txbuf[LORAITP_MAX_FRAME + LORAITP_MAC_LEN];
    uint8_t  rxbuf[LORAITP_MAX_FRAME];

    /* Reception bookkeeping for the current block. */
    uint8_t  present[(LORAITP_BLOCK_SIZE + 255 + 7) / 8];
    uint16_t missing[LORAITP_BLOCK_SIZE];
    uint8_t  statbody[255];

    uint8_t  flat[LORAITP_BLOCK_SIZE + 255];  /* erasure map for FEC decode */
    bool     fec_ok;         /* caller's scratch can hold a whole block */
    uint16_t cur_block;
    uint32_t max_ordinal;
    bool     saw_any;
    loraitp_stats_t stats;
};

const loraitp_region_info_t *loraitp_region(loraitp_region_t r);
uint32_t loraitp_region_budget_ms(loraitp_region_t r);

/* The budget of the profile actually in force, table row or not. */
uint32_t loraitp_gov_budget_ms(const loraitp_gov_t *g);

int  loraitp_gov_init(loraitp_gov_t *g, const loraitp_session_cfg_t *cfg);
uint32_t loraitp_gov_delay_ms(loraitp_gov_t *g, uint32_t now, uint32_t toa_ms);
void loraitp_gov_record(loraitp_gov_t *g, uint32_t now, uint32_t toa_ms);
uint32_t loraitp_gov_airtime_in_window(loraitp_gov_t *g, uint32_t now);
int  loraitp_gov_export(loraitp_gov_t *g, uint32_t now, void *buf, size_t cap);
int  loraitp_gov_import(loraitp_gov_t *g, uint32_t now, const void *buf,
                        size_t len, uint32_t away_ms);
bool loraitp_gov_ident_due(const loraitp_gov_t *g, uint32_t now);
void loraitp_gov_ident_sent(loraitp_gov_t *g, uint32_t now);

int loraitp_stat_decode_body(const loraitp_stat_t *s, uint16_t *out,
                             uint16_t cap);

static inline void bitset(uint8_t *bm, uint16_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7u)); }
static inline bool bitget(const uint8_t *bm, uint16_t i)
{
    return ((unsigned)bm[i >> 3] >> (unsigned)(i & 7u)) & 1u;
}

#endif /* LORAITP_INTERNAL_H */
