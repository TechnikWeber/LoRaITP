/*
 * Image store — the storage half of loraitp_port_t.
 *
 * Written against stdio rather than a filesystem API. ESP-IDF mounts
 * LittleFS into its VFS layer, so fopen/fseek/fread/fwrite/rename/remove
 * all work unchanged there — and unchanged on a host, which means this
 * layer is testable with real files and no hardware. That is worth more
 * than the small amount of indirection it costs.
 *
 * Layout under `dir`:
 *
 *   img_00000042.jpg      the image
 *   img_00000042.json     RSSI, SNR, loss, rounds, airtime
 *   img_00000043.jpg.part a transfer that was interrupted
 *
 * The sequence number is monotonic and comes from the highest one
 * already present, so there is no counter file to fall out of sync with
 * reality.
 */
#ifndef LORAITP_STORE_H
#define LORAITP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "loraitp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORAITP_STORE_PATH_MAX 96

typedef struct {
    uint16_t img_id;
    uint8_t  layer;
    uint8_t  codec;
    uint32_t img_len;
    uint32_t crc32;
    uint16_t width, height;

    /* What the transfer cost and what the link was like. Not decoration:
     * these are the measurements that turn the specification's guessed
     * constants into chosen ones. */
    uint16_t chunks_have, chunks_total;
    uint16_t rounds;
    uint32_t airtime_ms;
    int16_t  rssi_dbm;
    int8_t   snr_qdb;
    bool     complete;
    bool     crc_ok;
    uint32_t unix_time;      /* 0 if the node has no clock */
} loraitp_store_meta_t;

typedef struct loraitp_store loraitp_store_t;

size_t loraitp_store_size(void);

/*
 * `keep` is the ring depth: the oldest images beyond it are deleted on
 * mount and after each completed write. 0 keeps everything.
 *
 * Mounting also removes any leftover .part file, which is what a power
 * failure mid-transfer leaves behind.
 */
int  loraitp_store_init(loraitp_store_t *st, const char *dir, uint16_t keep);

/* Open a new image for writing. Preallocates img_len zero bytes, so a
 * chunk that never arrives reads back as zeros rather than as a hole —
 * with chunk-aligned restart markers that is a grey band in the picture,
 * not a decoder error. */
int  loraitp_store_begin_write(loraitp_store_t *st, uint32_t img_len);

/* Open an existing image for reading. NULL means the newest. */
int  loraitp_store_begin_read(loraitp_store_t *st, const char *name);

/* Close the current image. On a write this renames .part into place and
 * writes the sidecar; pass NULL to discard the transfer instead. */
int  loraitp_store_finish(loraitp_store_t *st,
                          const loraitp_store_meta_t *meta);

/*
 * Point the port's storage callbacks at this store, including the
 * image_begin / image_end lifecycle hooks - so the core opens and closes
 * files itself as transfers start and end, and the application only has
 * to supply the sidecar afterwards via loraitp_store_finish().
 */
void loraitp_store_attach(loraitp_port_t *port, loraitp_store_t *st);

/* Housekeeping and enumeration. */
int  loraitp_store_prune(loraitp_store_t *st);
int  loraitp_store_count(loraitp_store_t *st);
int  loraitp_store_newest(loraitp_store_t *st, char *out, size_t cap);

/* Enumerate newest first. Return non-zero from `cb` to stop. */
typedef int (*loraitp_store_cb)(void *user, const char *name, uint32_t bytes);
int  loraitp_store_list(loraitp_store_t *st, loraitp_store_cb cb, void *user);

/* Current image path, valid between begin_* and finish. */
const char *loraitp_store_current(const loraitp_store_t *st);

#ifdef __cplusplus
}
#endif
#endif /* LORAITP_STORE_H */
