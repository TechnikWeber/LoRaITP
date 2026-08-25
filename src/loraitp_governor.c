/*
 * The duty-cycle governor.  SPEC.md 6.
 *
 * Normative, not a helper: every transmission passes through here, and
 * there is no code path that transmits without asking.
 *
 * Regional data comes from BNetzA Vfg. 91/2025 table 2 (November 2025,
 * valid to 31.12.2035), which also fixes the definition this implements -
 * the duty cycle is on-time over a *rolling* one-hour window, not a
 * calendar hour.
 */
#include <string.h>

#include "loraitp_internal.h"

/* duty is expressed in parts per million to keep this integer-only. */
#define DC_NONE 0u

static const loraitp_region_info_t regions[LORAITP_REG__COUNT] = {
    /* name              f_lo         f_hi        duty_ppm  erp  bw_max  amateur */
    { "EU868_G3",   869400000u, 869650000u, 100000u, 27,      0, false },
    { "EU868_G1",   868000000u, 868600000u,  10000u, 14,      0, false },
    { "EU868_G2",   868700000u, 869200000u,   1000u, 14,      0, false },
    { "EU868_G4",   869700000u, 870000000u,  10000u, 14,      0, false },
    { "EU868_G4_LP",869700000u, 870000000u, DC_NONE,  7,      0, false },
    { "EU433",      433050000u, 434790000u, 100000u, 10,      0, false },
    { "EU433_NARROW",434040000u,434790000u, DC_NONE, 10,  25000, false },
    { "AMATEUR",             0u,         0u, DC_NONE, 53,      0, true  },
    { "TEST_UNRESTRICTED",   0u,         0u, DC_NONE,127,      0, false },
};

const loraitp_region_info_t *loraitp_region(loraitp_region_t r)
{
    if ((unsigned)r >= LORAITP_REG__COUNT)
        return NULL;
    return &regions[r];
}

uint32_t loraitp_region_budget_ms(loraitp_region_t r)
{
    const loraitp_region_info_t *info = loraitp_region(r);
    if (info == NULL || info->duty_ppm == DC_NONE)
        return 0;
    return (uint32_t)(((uint64_t)LORAITP_DC_WINDOW_MS * info->duty_ppm)
                      / 1000000u);
}

/* Signed difference, so a 32-bit millisecond wrap at 49.7 days is handled. */
static int32_t delta(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

int loraitp_gov_init(loraitp_gov_t *g, const loraitp_session_cfg_t *cfg)
{
    const loraitp_region_info_t *info = loraitp_region(cfg->region);
    if (info == NULL)
        return LORAITP_E_ARG;

    memset(g, 0, sizeof(*g));
    g->region = cfg->region;
    g->info = info;
    g->ident_interval_ms = (cfg->ident_interval_s ? cfg->ident_interval_s
                                                  : LORAITP_IDENT_INTERVAL_S)
                           * 1000u;
    g->ident_pending = false;
    g->has_identified = false;

    if (info->amateur) {
        /* SPEC.md 6.4. Hard failures, deliberately - a misconfigured
         * amateur station must not transmit at all. */
        if (cfg->callsign == NULL || cfg->callsign[0] == '\0')
            return LORAITP_E_CALLSIGN;
        if (cfg->flags & LORAITP_CFG_ENCRYPTED)
            return LORAITP_E_CRYPTO;
        if (cfg->frequency_hz == 0)
            return LORAITP_E_FREQ;
    } else {
        if (info->f_lo_hz != 0 && cfg->frequency_hz != 0
            && (cfg->frequency_hz < info->f_lo_hz
                || cfg->frequency_hz > info->f_hi_hz))
            return LORAITP_E_FREQ;
        if (cfg->tx_power_dbm > info->max_erp_dbm)
            return LORAITP_E_POWER;
    }
    if (info->max_bw_hz != 0 && cfg->bandwidth_hz > info->max_bw_hz)
        return LORAITP_E_BANDWIDTH;

    return LORAITP_OK;
}

static void prune(loraitp_gov_t *g, uint32_t now)
{
    while (g->n_slots > 0) {
        uint32_t t_end = g->slot_end[g->head];
        if (delta(now, t_end) < (int32_t)LORAITP_DC_WINDOW_MS)
            break;
        g->head = (uint16_t)((g->head + 1u) % LORAITP_DC_SLOTS);
        g->n_slots--;
    }
}

uint32_t loraitp_gov_airtime_in_window(loraitp_gov_t *g, uint32_t now)
{
    prune(g, now);
    uint32_t sum = 0;
    for (uint16_t i = 0; i < g->n_slots; i++)
        sum += g->slot_toa[(g->head + i) % LORAITP_DC_SLOTS];
    return sum;
}

uint32_t loraitp_gov_delay_ms(loraitp_gov_t *g, uint32_t now, uint32_t toa_ms)
{
    if (g->info->duty_ppm == DC_NONE)
        return 0;

    uint32_t wait = 0;
    if (delta(g->blocked_until, now) > 0)
        wait = (uint32_t)delta(g->blocked_until, now);

    /* The off-time rule alone satisfies the ratio, but the rolling window
     * is what the regulation actually says. Take the stricter of the two. */
    uint32_t budget = loraitp_region_budget_ms(g->region);
    uint32_t used = loraitp_gov_airtime_in_window(g, now);
    if (used + toa_ms > budget) {
        uint32_t need = used + toa_ms - budget;
        uint32_t freed = 0;
        for (uint16_t i = 0; i < g->n_slots; i++) {
            uint16_t s = (uint16_t)((g->head + i) % LORAITP_DC_SLOTS);
            freed += g->slot_toa[s];
            if (freed >= need) {
                uint32_t until = g->slot_end[s] + LORAITP_DC_WINDOW_MS;
                if (delta(until, now) > 0 && (uint32_t)delta(until, now) > wait)
                    wait = (uint32_t)delta(until, now);
                break;
            }
        }
    }
    return wait;
}

void loraitp_gov_record(loraitp_gov_t *g, uint32_t now, uint32_t toa_ms)
{
    if (g->n_slots == LORAITP_DC_SLOTS) {
        /* Ring full. Fold the oldest two entries together rather than
         * dropping one: over-counting delays us slightly, under-counting
         * would put the station over its budget. */
        uint16_t a = g->head;
        uint16_t b = (uint16_t)((g->head + 1u) % LORAITP_DC_SLOTS);
        g->slot_toa[b] += g->slot_toa[a];
        g->head = b;
        g->n_slots--;
    }
    uint16_t tail = (uint16_t)((g->head + g->n_slots) % LORAITP_DC_SLOTS);
    g->slot_end[tail] = now + toa_ms;
    g->slot_toa[tail] = toa_ms;
    g->n_slots++;

    if (g->info->duty_ppm != DC_NONE) {
        uint64_t off = ((uint64_t)toa_ms * 1000000u) / g->info->duty_ppm;
        g->blocked_until = now + toa_ms + (uint32_t)(off - toa_ms);
    }
    g->airtime_total_ms += toa_ms;
}

bool loraitp_gov_ident_due(const loraitp_gov_t *g, uint32_t now)
{
    if (!g->info->amateur)
        return false;
    if (!g->has_identified)
        return true;
    return delta(now, g->last_ident_ms) >= (int32_t)g->ident_interval_ms;
}

void loraitp_gov_ident_sent(loraitp_gov_t *g, uint32_t now)
{
    g->last_ident_ms = now;
    g->has_identified = true;
}
