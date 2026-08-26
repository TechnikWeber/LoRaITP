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
    /* LOCAL: a placeholder. Every field is replaced at init from what
     * the operator declared, so the only thing this row supplies is the
     * name. */
    { "LOCAL",               0u,         0u, DC_NONE,127,      0, false },
};

const loraitp_region_info_t *loraitp_region(loraitp_region_t r)
{
    if ((unsigned)r >= LORAITP_REG__COUNT)
        return NULL;
    return &regions[r];
}

static uint32_t budget_of(const loraitp_region_info_t *info)
{
    if (info == NULL || info->duty_ppm == DC_NONE)
        return 0;
    return (uint32_t)(((uint64_t)LORAITP_DC_WINDOW_MS * info->duty_ppm)
                      / 1000000u);
}

uint32_t loraitp_region_budget_ms(loraitp_region_t r)
{
    /* The published figure for a published allocation. LORAITP_REG_LOCAL
     * has none by definition - ask the governor, not the table. */
    return budget_of(loraitp_region(r));
}

uint32_t loraitp_gov_budget_ms(const loraitp_gov_t *g)
{
    return budget_of(g->info);
}

/* Signed difference, so a 32-bit millisecond wrap at 49.7 days is handled. */
static int32_t delta(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

int loraitp_gov_init(loraitp_gov_t *g, const loraitp_session_cfg_t *cfg)
{
    const loraitp_region_info_t *info = loraitp_region(cfg->region);
    if (info == NULL)
        return LORAITP_E_ARG;

    if (cfg->region == LORAITP_REG_LOCAL && cfg->local_duty_percent > 100u)
        return LORAITP_E_ARG;

    memset(g, 0, sizeof(*g));
    g->region = cfg->region;

    if (cfg->region == LORAITP_REG_LOCAL) {
        /*
         * The operator's own profile. There is no band, no power ceiling
         * and no bandwidth rule here, because this firmware has no way to
         * know which jurisdiction the board is standing in - but the duty
         * cycle is a number the operator can state, and once stated it is
         * enforced exactly like a published one. That is the whole point:
         * the governor stops being a table lookup and stays an
         * accountant. SPEC.md 6.5.
         */
        g->local = *info;
        g->local.duty_ppm = cfg->local_duty_percent
                            ? (uint32_t)cfg->local_duty_percent * 10000u
                            : DC_NONE;
        info = &g->local;
    }
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
    uint32_t budget = loraitp_gov_budget_ms(g);
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

/* ------------------------------------------- window across a reboot */

/*
 * Fixed little-endian layout. It crosses a reboot rather than a link,
 * but the discipline is the same, and one part of it matters more here
 * than on the air: entries carry an *age*, not a timestamp. millis()
 * restarts at zero after the reboot this exists to survive, so an
 * absolute stamp from the old timebase means nothing in the new one.
 * "How long ago" survives the change; "when" does not.
 *
 *   0  magic          8  count             20  entries[count]
 *   4  version       12  airtime_total_ms      { int32 age_ms, u32 toa_ms }
 *                    16  blocked_remaining     4 bytes CRC-32 at the end
 */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int loraitp_gov_export(loraitp_gov_t *g, uint32_t now, void *buf, size_t cap)
{
    if (buf == NULL || cap < LORAITP_BUDGET_STATE_MAX)
        return LORAITP_E_ARG;

    prune(g, now);

    uint8_t *p = (uint8_t *)buf;
    uint8_t *e = p + STATE_HDR;
    uint16_t n = 0;

    /*
     * More entries than the snapshot holds: fold the oldest ones onto
     * the first that is kept, exactly as a full ring folds. The survivor
     * carries the later expiry, so folded airtime leaves the window
     * later than it really would - over-counting delays the station,
     * under-counting would put it over budget.
     *
     * A transfer is a few dozen frames, so a real schedule never takes
     * this path. It is here because the ring holds twice what the
     * snapshot does, and silently dropping the difference is the one
     * outcome that must not happen.
     */
    uint16_t skip = (g->n_slots > STATE_MAX_ENTRIES)
                    ? (uint16_t)(g->n_slots - STATE_MAX_ENTRIES) : 0u;
    uint32_t folded = 0;

    for (uint16_t i = 0; i < g->n_slots; i++) {
        uint16_t s = (uint16_t)((g->head + i) % LORAITP_DC_SLOTS);
        if (i < skip) {
            folded += g->slot_toa[s];
            continue;
        }
        uint32_t toa = g->slot_toa[s] + folded;
        folded = 0;
        put32(e, (uint32_t)delta(now, g->slot_end[s]));   /* age, signed */
        put32(e + 4, toa);
        e += STATE_ENTRY;
        n++;
    }

    int32_t blocked = delta(g->blocked_until, now);
    put32(p,      STATE_MAGIC);
    put32(p + 4,  STATE_VERSION);
    put32(p + 8,  (uint32_t)n);
    put32(p + 12, g->airtime_total_ms);
    put32(p + 16, blocked > 0 ? (uint32_t)blocked : 0u);

    size_t body = STATE_HDR + (size_t)n * STATE_ENTRY;
    put32(p + body, loraitp_crc32(p, body));
    return (int)(body + STATE_TAIL);
}

int loraitp_gov_import(loraitp_gov_t *g, uint32_t now, const void *buf,
                       size_t len, uint32_t away_ms)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (buf == NULL || len < STATE_HDR + STATE_TAIL)
        return LORAITP_E_ARG;
    if (get32(p) != STATE_MAGIC || get32(p + 4) != STATE_VERSION)
        return LORAITP_E_ARG;

    uint32_t n = get32(p + 8);
    if (n > STATE_MAX_ENTRIES)
        return LORAITP_E_ARG;
    size_t body = STATE_HDR + (size_t)n * STATE_ENTRY;
    if (len < body + STATE_TAIL)
        return LORAITP_E_ARG;
    if (get32(p + body) != loraitp_crc32(p, body))
        return LORAITP_E_ARG;

    /* Read whole and checked before anything is written, so a refusal
     * leaves the window as it was rather than half replaced. */
    g->head = 0;
    g->n_slots = 0;
    g->airtime_total_ms = get32(p + 12);

    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = p + STATE_HDR + (size_t)i * STATE_ENTRY;
        int32_t age = (int32_t)get32(e);
        uint32_t toa = get32(e + 4);

        /*
         * Age at export plus the time away. A negative age is a frame
         * that had not finished leaving the antenna when the snapshot
         * was taken, and the arithmetic handles that without a case of
         * its own.
         */
        int64_t aged = (int64_t)age + (int64_t)away_ms;
        if (aged >= (int64_t)LORAITP_DC_WINDOW_MS)
            continue;                          /* aged out while away */

        g->slot_end[g->n_slots] = (uint32_t)((int64_t)now - aged);
        g->slot_toa[g->n_slots] = toa;
        g->n_slots++;
    }

    uint32_t blocked = get32(p + 16);
    g->blocked_until = (blocked > away_ms) ? now + (blocked - away_ms) : now;

    /*
     * Identification is deliberately not restored. An amateur station
     * that has just rebooted should say who it is again, and the rule
     * sets a floor on how often rather than a ceiling - so forgetting
     * can only make the station more correct.
     */
    g->has_identified = false;
    g->ident_pending = false;
    return LORAITP_OK;
}
