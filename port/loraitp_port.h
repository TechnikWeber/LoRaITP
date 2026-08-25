/*
 * LoRaITP port interface — the boundary between the portable core and
 * everything platform specific.
 *
 * This is the most important file in the repository. The core in src/
 * talks to the outside world exclusively through this struct. It never
 * touches a register, never calls malloc, never includes a vendor SDK
 * header, and never blocks except inside radio_receive().
 *
 * Consequences worth protecting:
 *   - the same core object code runs in tests/, in sim/ and on the ESP32
 *   - a bug found in the simulator is the same bug that was on hardware
 *   - splitting the core into its own library later is a directory move,
 *     not a rewrite
 *
 * If you find yourself wanting to add an ESP-IDF type here, the answer
 * is a new callback, not an #ifdef.
 */
#ifndef LORAITP_PORT_H
#define LORAITP_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ radio */

typedef struct {
    uint32_t frequency_hz;
    uint32_t bandwidth_hz;   /* 125000, 250000, 20830, ... */
    uint8_t  spreading_factor;  /* 7..12 */
    uint8_t  coding_rate;    /* 1..4 -> 4/5 .. 4/8 */
    uint8_t  preamble_symbols;
    int8_t   tx_power_dbm;
    uint8_t  sync_word;
} loraitp_radio_cfg_t;

typedef struct {
    int16_t  rssi_dbm;
    int8_t   snr_qdb;        /* SNR in quarter-dB, as the SX126x reports it */
    uint32_t timestamp_ms;
} loraitp_rx_meta_t;

/* Return codes. Negative values are errors throughout the core. */
#define LORAITP_OK          0
#define LORAITP_E_TIMEOUT  (-1)
#define LORAITP_E_RADIO    (-2)
#define LORAITP_E_IO       (-3)
#define LORAITP_E_ARG      (-4)
#define LORAITP_E_NOSUP    (-5)
/* Regulatory refusals. These are configuration errors, not runtime
 * conditions: the station must not transmit until they are fixed. */
#define LORAITP_E_CALLSIGN  (-6)
#define LORAITP_E_CRYPTO    (-7)
#define LORAITP_E_FREQ      (-8)
#define LORAITP_E_POWER     (-9)
#define LORAITP_E_BANDWIDTH (-10)
#define LORAITP_E_STATE     (-11)

typedef struct loraitp_port {
    /*
     * Radio. send() blocks until the frame has left the antenna and
     * returns the actual time on air in milliseconds via toa_ms, which
     * the duty-cycle governor needs. Ports must not enforce any duty
     * cycle of their own — that is the core's job, and doing it twice
     * makes the accounting wrong.
     */
    int (*radio_configure)(void *ctx, const loraitp_radio_cfg_t *cfg);
    int (*radio_send)(void *ctx, const uint8_t *buf, uint8_t len,
                      uint32_t *toa_ms);
    int (*radio_receive)(void *ctx, uint8_t *buf, uint8_t cap,
                         uint32_t timeout_ms, loraitp_rx_meta_t *meta);
    int (*radio_sleep)(void *ctx);

    /*
     * Monotonic milliseconds. Must not jump backwards and must survive
     * the longest session the application allows — 32 bits wraps after
     * 49 days, which the core handles, but the port must not reset it.
     */
    uint32_t (*now_ms)(void *ctx);

    /*
     * Sleep. On a battery node this should be a real low-power sleep;
     * the governor calls it with the exact duty-cycle off-time, which
     * at SF12 on a 1% band can be twelve minutes. A busy-wait here
     * costs more energy than the transmission did.
     */
    void (*sleep_ms)(void *ctx, uint32_t ms);

    /* Entropy for session nonces and collision-avoidance jitter. */
    int (*random_bytes)(void *ctx, uint8_t *out, size_t len);

    /*
     * Image access. The core never holds the whole image in RAM; it
     * pulls and pushes chunks. On the sender image_write is NULL, on
     * the receiver image_read may be NULL.
     */
    int (*image_read)(void *ctx, uint32_t offset, uint8_t *buf, uint16_t len);
    int (*image_write)(void *ctx, uint32_t offset, const uint8_t *buf,
                       uint16_t len);

    /*
     * Single AES-128 block encryption, used to build CMAC. Leave NULL
     * to compile without authentication. Every platform we target has
     * this in hardware; there is no software fallback in the core on
     * purpose, so nobody ships a slow one by accident.
     */
    int (*aes128_encrypt_block)(void *ctx, const uint8_t in[16],
                                uint8_t out[16]);

    /* Opaque, passed back to every callback. */
    void *ctx;
} loraitp_port_t;

/* Returns LORAITP_OK if every callback required for `role` is present. */
int loraitp_port_validate(const loraitp_port_t *port, int is_sender);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_PORT_H */
