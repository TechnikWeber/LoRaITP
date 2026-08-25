/*
 * LoRaITP wire format.  SPEC.md 3.
 *
 * Encoding and decoding write into caller-supplied buffers; nothing here
 * allocates. Decoders validate length before touching a field, so a
 * truncated or hostile frame produces LORAITP_E_ARG rather than a read
 * past the end of the buffer.
 */
#ifndef LORAITP_FRAME_H
#define LORAITP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "loraitp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAITP_META_HDR 26
#define LORAITP_DATA_HDR 4
#define LORAITP_MAX_TLV  8

/* META FLAGS bits */
#define LORAITP_F_PARITY   (1u << 0)
#define LORAITP_F_ENCRYPT  (1u << 1)
#define LORAITP_F_LAST     (1u << 2)
#define LORAITP_F_AMATEUR  (1u << 3)
#define LORAITP_F_BCAST    (1u << 4)
#define LORAITP_F_MAC_DATA (1u << 5)

/* STAT encodings */
#define LORAITP_ENC_BITMAP   0
#define LORAITP_ENC_LIST     1
#define LORAITP_ENC_COMPLETE 2

typedef struct {
    uint8_t  type;
    uint8_t  len;
    const uint8_t *value;
} loraitp_tlv_t;

typedef struct {
    uint8_t  sid;
    uint16_t img_id;
    uint8_t  layer;
    uint32_t img_len;
    uint8_t  chunk;
    uint8_t  codec;
    uint32_t crc32;
    uint16_t width, height;
    uint8_t  flags;
    uint16_t block;          /* 0 on the wire means 256 */
    uint16_t n_parity;
    uint8_t  nonce[4];
    loraitp_tlv_t tlv[LORAITP_MAX_TLV];
    uint8_t  n_tlv;
} loraitp_meta_t;

typedef struct {
    uint8_t  sid;
    uint16_t seq;
    const uint8_t *payload;
    uint8_t  payload_len;
    bool     is_parity;
} loraitp_data_t;

typedef struct {
    uint8_t  sid;
    uint16_t block;
    uint8_t  round;
} loraitp_eob_t;

typedef struct {
    uint8_t  sid;
    uint16_t block;
    uint8_t  round;
    uint8_t  enc;
    int8_t   rssi;
    int8_t   snr_qdb;
    const uint8_t *body;
    uint8_t  body_len;
    uint16_t base;           /* first seq of the block; not on the wire */
} loraitp_stat_t;

/* ---------------------------------------------------------- accessors */

int  loraitp_frame_type(const uint8_t *buf, size_t len);

/* Derived counts. These are computed, never transmitted (SPEC.md 3.2). */
uint16_t loraitp_meta_n_chunks(const loraitp_meta_t *m);
uint16_t loraitp_meta_n_blocks(const loraitp_meta_t *m);
uint16_t loraitp_meta_block_k(const loraitp_meta_t *m, uint16_t blk);
uint32_t loraitp_meta_total_frames(const loraitp_meta_t *m);

/*
 * Position of a frame in the sender's transmission order. The broadcast
 * receiver needs it to work out how much is still to come (SPEC.md 5.3).
 */
uint32_t loraitp_frame_ordinal(const loraitp_meta_t *m, uint16_t seq,
                               bool is_parity);

/* ------------------------------------------------------------ encoding */

int loraitp_encode_meta(const loraitp_meta_t *m, uint8_t *out, size_t cap);
int loraitp_encode_data(const loraitp_data_t *d, uint8_t *out, size_t cap);
int loraitp_encode_eob(const loraitp_eob_t *e, uint8_t *out, size_t cap);
int loraitp_encode_stat(const loraitp_stat_t *s, uint8_t *out, size_t cap);
int loraitp_encode_simple(uint8_t type, uint8_t sid, const uint8_t *extra,
                          uint8_t extra_len, uint8_t *out, size_t cap);

/* ------------------------------------------------------------ decoding */

int loraitp_decode_meta(const uint8_t *buf, size_t len, loraitp_meta_t *out);
int loraitp_decode_data(const uint8_t *buf, size_t len, loraitp_data_t *out);
int loraitp_decode_eob(const uint8_t *buf, size_t len, loraitp_eob_t *out);
int loraitp_decode_stat(const uint8_t *buf, size_t len, uint16_t base,
                        loraitp_stat_t *out);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_FRAME_H */
