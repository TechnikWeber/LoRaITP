/*
 * Compile-time configuration for the LoRaITP core.
 *
 * Everything here is a size or a policy that must be fixed at build
 * time because the core allocates no memory. Override by defining the
 * symbol before including, or with -D on the command line.
 */
#ifndef LORAITP_CONFIG_H
#define LORAITP_CONFIG_H

/* Largest LoRa PHY payload the radio will be asked to send. */
#ifndef LORAITP_MAX_FRAME
#define LORAITP_MAX_FRAME 255
#endif

/*
 * Chunks per block. Bounds the receive bitmap (BLOCK_SIZE/8 bytes) and,
 * with FEC enabled, the Reed-Solomon block. GF(256) requires
 * k + parity <= 255, so a FEC build cannot use the full 256.
 */
#ifndef LORAITP_BLOCK_SIZE
#define LORAITP_BLOCK_SIZE 128
#endif

#if LORAITP_BLOCK_SIZE > 256
#error "LORAITP_BLOCK_SIZE must not exceed 256"
#endif

/* Cap on time on air per frame. See docs/design-notes.md for why. */
#ifndef LORAITP_MAX_TOA_MS
#define LORAITP_MAX_TOA_MS 2000
#endif

/*
 * Amateur-service mode is a runtime profile, not a build option, and
 * this flag never gated anything - SPEC.md 6.4 has the correction. An
 * implementation that must not offer it removes the row from the table
 * in loraitp_governor.c, which is a change a compiler can check.
 */

/* Forward error correction. Required for broadcast (one-way) mode. */
#ifndef LORAITP_ENABLE_FEC
#define LORAITP_ENABLE_FEC 1
#endif

/* CMAC frame authentication. Needs port->aes128_encrypt_block. */
#ifndef LORAITP_ENABLE_MAC
#define LORAITP_ENABLE_MAC 1
#endif

/* Truncated CMAC length in bytes appended to authenticated frames. */
#ifndef LORAITP_MAC_LEN
#define LORAITP_MAC_LEN 4
#endif

#endif /* LORAITP_CONFIG_H */
