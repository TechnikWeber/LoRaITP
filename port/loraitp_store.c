#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loraitp_store.h"

#define PREFIX "img_"
#define IMG_EXT ".jpg"
#define META_EXT ".json"
#define PART_EXT ".part"

/* Zero-fill buffer for preallocation. 256 bytes keeps the stack small;
 * a 10 kB image is 40 writes, which is nothing next to the transfer. */
#define ZERO_CHUNK 256

/* Bounded so "<dir>/img_00000001.jpg.part" provably fits in path[]. The
 * compiler cannot reason about the length check in init() alone. */
#define STORE_DIR_MAX (LORAITP_STORE_PATH_MAX - 32)

struct loraitp_store {
    char     dir[STORE_DIR_MAX];
    char     path[LORAITP_STORE_PATH_MAX];   /* current file */
    uint32_t seq;                            /* of the current file */
    uint32_t next_seq;
    uint16_t keep;
    FILE    *fp;
    bool     writing;
    bool     renamed;      /* image_end already moved it into place */
};

size_t loraitp_store_size(void) { return sizeof(struct loraitp_store); }

/* ------------------------------------------------------------- naming */

static int parse_seq(const char *name, uint32_t *out)
{
    size_t plen = strlen(PREFIX);
    if (strncmp(name, PREFIX, plen) != 0)
        return 0;
    const char *digits = name + plen;
    char *end = NULL;
    unsigned long v = strtoul(digits, &end, 10);
    if (end == digits || end == NULL)
        return 0;
    if (strcmp(end, IMG_EXT) != 0)
        return 0;                 /* sidecars and .part files do not count */
    *out = (uint32_t)v;
    return 1;
}

static void make_path(char *out, size_t cap, const char *dir, uint32_t seq,
                      const char *ext)
{
    snprintf(out, cap, "%s/" PREFIX "%08lu%s", dir, (unsigned long)seq, ext);
}

/* --------------------------------------------------------- enumeration */

typedef int (*scan_cb)(void *user, const char *name, uint32_t seq);

static int scan(const char *dir, scan_cb cb, void *user)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return LORAITP_E_IO;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t seq;
        if (!parse_seq(e->d_name, &seq))
            continue;
        n++;
        if (cb && cb(user, e->d_name, seq) != 0)
            break;
    }
    closedir(d);
    return n;
}

struct max_ctx { uint32_t max; bool any; };
static int cb_max(void *user, const char *name, uint32_t seq)
{
    (void)name;
    struct max_ctx *c = user;
    if (!c->any || seq > c->max) { c->max = seq; c->any = true; }
    return 0;
}

struct min_ctx { uint32_t min; bool any; };
static int cb_min(void *user, const char *name, uint32_t seq)
{
    (void)name;
    struct min_ctx *c = user;
    if (!c->any || seq < c->min) { c->min = seq; c->any = true; }
    return 0;
}

static void write_sidecar(loraitp_store_t *st,
                          const loraitp_store_meta_t *m);

/* ----------------------------------------------------------- lifecycle */

static void remove_stale_parts(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *e;
    /* Sized so any directory entry provably fits: POSIX caps d_name at
     * 255. Called once at mount, so the stack cost does not matter. */
    char path[STORE_DIR_MAX + 258];
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        size_t elen = strlen(PART_EXT);
        if (len <= elen || strcmp(e->d_name + len - elen, PART_EXT) != 0)
            continue;
        /* A .part file is what a power failure mid-transfer leaves. The
         * image it held was incomplete and its sidecar was never written,
         * so there is nothing to salvage. */
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

int loraitp_store_init(loraitp_store_t *st, const char *dir, uint16_t keep)
{
    if (st == NULL || dir == NULL)
        return LORAITP_E_ARG;
    if (strlen(dir) >= STORE_DIR_MAX)
        return LORAITP_E_ARG;

    memset(st, 0, sizeof(*st));
    snprintf(st->dir, sizeof(st->dir), "%s", dir);
    st->keep = keep;

    DIR *d = opendir(dir);
    if (d == NULL)
        return LORAITP_E_IO;      /* caller mounts the filesystem first */
    closedir(d);

    remove_stale_parts(dir);

    /* The next sequence number comes from what is on disk, so there is no
     * counter file that can disagree with reality after a crash. */
    struct max_ctx m = { 0, false };
    scan(dir, cb_max, &m);
    st->next_seq = m.any ? m.max + 1u : 1u;

    return loraitp_store_prune(st);
}

int loraitp_store_prune(loraitp_store_t *st)
{
    if (st == NULL)
        return LORAITP_E_ARG;
    if (st->keep == 0)
        return LORAITP_OK;

    for (;;) {
        int n = scan(st->dir, NULL, NULL);
        if (n < 0)
            return n;
        if ((uint16_t)n <= st->keep)
            return LORAITP_OK;

        struct min_ctx lo = { 0, false };
        scan(st->dir, cb_min, &lo);
        if (!lo.any)
            return LORAITP_OK;

        char p[LORAITP_STORE_PATH_MAX];
        make_path(p, sizeof(p), st->dir, lo.min, IMG_EXT);
        remove(p);
        make_path(p, sizeof(p), st->dir, lo.min, META_EXT);
        remove(p);
    }
}

int loraitp_store_begin_write(loraitp_store_t *st, uint32_t img_len)
{
    if (st == NULL || st->fp != NULL)
        return LORAITP_E_ARG;

    st->seq = st->next_seq;
    st->renamed = false;
    make_path(st->path, sizeof(st->path), st->dir, st->seq,
              IMG_EXT PART_EXT);

    st->fp = fopen(st->path, "wb+");
    if (st->fp == NULL)
        return LORAITP_E_IO;
    st->writing = true;

    /*
     * Preallocate. Seeking past the end and writing would leave a hole,
     * and LittleFS has no sparse files - so a chunk that never arrives
     * would read back as whatever was there before. Zeros are what the
     * image layer expects: with chunk-aligned restart markers a run of
     * zeros decodes as a grey band rather than as a decoder error.
     */
    static const uint8_t zeros[ZERO_CHUNK] = { 0 };
    uint32_t left = img_len;
    while (left > 0) {
        size_t n = (left < ZERO_CHUNK) ? left : ZERO_CHUNK;
        if (fwrite(zeros, 1, n, st->fp) != n) {
            fclose(st->fp);
            st->fp = NULL;
            remove(st->path);
            return LORAITP_E_IO;
        }
        left -= (uint32_t)n;
    }
    fflush(st->fp);
    return LORAITP_OK;
}

int loraitp_store_begin_read(loraitp_store_t *st, const char *name)
{
    if (st == NULL || st->fp != NULL)
        return LORAITP_E_ARG;

    if (name != NULL) {
        /*
         * Parse and rebuild rather than concatenating. The name may come
         * from an HTTP request once the access point exists, and a path
         * assembled from an unchecked string is how "../.." walks out of
         * the image directory.
         */
        uint32_t seq = 0;
        if (!parse_seq(name, &seq))
            return LORAITP_E_ARG;
        st->seq = seq;
        make_path(st->path, sizeof(st->path), st->dir, st->seq, IMG_EXT);
    } else {
        struct max_ctx m = { 0, false };
        scan(st->dir, cb_max, &m);
        if (!m.any)
            return LORAITP_E_IO;
        st->seq = m.max;
        make_path(st->path, sizeof(st->path), st->dir, st->seq, IMG_EXT);
    }

    st->fp = fopen(st->path, "rb");
    if (st->fp == NULL)
        return LORAITP_E_IO;
    st->writing = false;
    return LORAITP_OK;
}

static void write_sidecar(loraitp_store_t *st,
                          const loraitp_store_meta_t *m)
{
    char p[LORAITP_STORE_PATH_MAX];
    make_path(p, sizeof(p), st->dir, st->seq, META_EXT);
    FILE *f = fopen(p, "w");
    if (f == NULL)
        return;
    fprintf(f,
            "{\"img_id\":%u,\"layer\":%u,\"codec\":%u,\"bytes\":%lu,"
            "\"crc32\":\"%08lx\",\"width\":%u,\"height\":%u,"
            "\"chunks\":[%u,%u],\"rounds\":%u,\"airtime_ms\":%lu,"
            "\"rssi_dbm\":%d,\"snr_db\":%.2f,"
            "\"complete\":%s,\"crc_ok\":%s,\"unix_time\":%lu}\n",
            m->img_id, m->layer, m->codec, (unsigned long)m->img_len,
            (unsigned long)m->crc32, m->width, m->height,
            m->chunks_have, m->chunks_total, m->rounds,
            (unsigned long)m->airtime_ms, m->rssi_dbm, m->snr_qdb / 4.0,
            m->complete ? "true" : "false",
            m->crc_ok ? "true" : "false",
            (unsigned long)m->unix_time);
    fclose(f);
}

int loraitp_store_finish(loraitp_store_t *st,
                         const loraitp_store_meta_t *meta)
{
    if (st == NULL)
        return LORAITP_E_ARG;

    /* image_end may already have closed and renamed. Finishing then means
     * writing the sidecar the application could not supply earlier. */
    if (st->fp == NULL && st->renamed) {
        st->renamed = false;
        if (meta != NULL)
            write_sidecar(st, meta);
        st->next_seq = st->seq + 1u;
        return loraitp_store_prune(st);
    }
    if (st->fp == NULL)
        return LORAITP_E_ARG;

    fflush(st->fp);
    fclose(st->fp);
    st->fp = NULL;

    if (!st->writing)
        return LORAITP_OK;

    if (meta == NULL) {
        remove(st->path);              /* caller discarded the transfer */
        return LORAITP_OK;
    }

    /*
     * Rename regardless of whether the image is complete. An incomplete
     * image is still worth keeping - it is a partial picture plus the
     * loss statistics that explain why - and the sidecar records exactly
     * how complete it is. Rename is atomic on LittleFS, so an image
     * either exists whole or does not exist.
     */
    char final[LORAITP_STORE_PATH_MAX];
    make_path(final, sizeof(final), st->dir, st->seq, IMG_EXT);
    if (rename(st->path, final) != 0) {
        remove(st->path);
        return LORAITP_E_IO;
    }
    snprintf(st->path, sizeof(st->path), "%s", final);

    write_sidecar(st, meta);
    st->next_seq = st->seq + 1u;
    return loraitp_store_prune(st);
}

/* ------------------------------------------------------ port callbacks */

static int store_read(void *ctx, uint32_t off, uint8_t *buf, uint16_t len)
{
    loraitp_store_t *st = ctx;
    if (st == NULL || st->fp == NULL || st->writing)
        return LORAITP_E_IO;
    if (fseek(st->fp, (long)off, SEEK_SET) != 0)
        return LORAITP_E_IO;
    return (fread(buf, 1, len, st->fp) == len) ? LORAITP_OK : LORAITP_E_IO;
}

static int store_write(void *ctx, uint32_t off, const uint8_t *buf,
                       uint16_t len)
{
    loraitp_store_t *st = ctx;
    if (st == NULL || st->fp == NULL || !st->writing)
        return LORAITP_E_IO;
    if (fseek(st->fp, (long)off, SEEK_SET) != 0)
        return LORAITP_E_IO;
    return (fwrite(buf, 1, len, st->fp) == len) ? LORAITP_OK : LORAITP_E_IO;
}

/*
 * The core calls these once it knows the image length, which is the
 * earliest a file-backed store can do anything useful.
 */
static int store_begin(void *ctx, uint32_t img_len, bool for_write)
{
    loraitp_store_t *st = ctx;
    if (st == NULL)
        return LORAITP_E_ARG;
    if (st->fp != NULL)
        loraitp_store_finish(st, NULL);     /* a previous attempt */
    return for_write ? loraitp_store_begin_write(st, img_len)
                     : loraitp_store_begin_read(st, NULL);
}

static int store_end(void *ctx, bool complete)
{
    loraitp_store_t *st = ctx;
    (void)complete;
    if (st == NULL || st->fp == NULL)
        return LORAITP_OK;

    /*
     * Close and, on a write, rename into place - but leave the sidecar
     * to the application, which is the only party that knows the loss
     * rate and the airtime. loraitp_store_finish() writes it afterwards.
     */
    fflush(st->fp);
    fclose(st->fp);
    st->fp = NULL;

    if (!st->writing)
        return LORAITP_OK;

    char final[LORAITP_STORE_PATH_MAX];
    make_path(final, sizeof(final), st->dir, st->seq, IMG_EXT);
    if (rename(st->path, final) != 0) {
        remove(st->path);
        return LORAITP_E_IO;
    }
    snprintf(st->path, sizeof(st->path), "%s", final);
    st->renamed = true;
    return LORAITP_OK;
}

void loraitp_store_attach(loraitp_port_t *port, loraitp_store_t *st)
{
    if (port == NULL || st == NULL)
        return;
    port->image_read = store_read;
    port->image_write = store_write;
    port->image_begin = store_begin;
    port->image_end = store_end;
    /*
     * The store owns port->ctx, which the radio port does not use - it
     * keeps its state in file-scope statics because there is exactly one
     * radio. If a port ever needs ctx for itself, this is the line that
     * has to change.
     */
    port->ctx = st;
}

/* ------------------------------------------------------------ queries */

int loraitp_store_count(loraitp_store_t *st)
{
    return (st == NULL) ? LORAITP_E_ARG : scan(st->dir, NULL, NULL);
}

int loraitp_store_newest(loraitp_store_t *st, char *out, size_t cap)
{
    if (st == NULL || out == NULL)
        return LORAITP_E_ARG;
    struct max_ctx m = { 0, false };
    scan(st->dir, cb_max, &m);
    if (!m.any)
        return LORAITP_E_IO;
    snprintf(out, cap, PREFIX "%08lu" IMG_EXT, (unsigned long)m.max);
    return LORAITP_OK;
}

struct list_ctx {
    loraitp_store_t *st;
    loraitp_store_cb cb;
    void *user;
    uint32_t below;
    uint32_t best;
    bool found;
};

static int cb_below(void *user, const char *name, uint32_t seq)
{
    (void)name;
    struct list_ctx *c = user;
    if (seq < c->below && (!c->found || seq > c->best)) {
        c->best = seq;
        c->found = true;
    }
    return 0;
}

int loraitp_store_list(loraitp_store_t *st, loraitp_store_cb cb, void *user)
{
    if (st == NULL || cb == NULL)
        return LORAITP_E_ARG;

    /*
     * Newest first, by repeatedly finding the largest sequence below the
     * previous one. O(n^2) in the number of images, which is fine for the
     * few hundred a node holds and costs no memory - readdir gives no
     * ordering guarantee and sorting would need a buffer we do not have.
     */
    struct list_ctx c = { st, cb, user, 0xFFFFFFFFu, 0, false };
    int n = 0;
    for (;;) {
        c.found = false;
        scan(st->dir, cb_below, &c);
        if (!c.found)
            break;

        char p[LORAITP_STORE_PATH_MAX], name[64];
        make_path(p, sizeof(p), st->dir, c.best, IMG_EXT);
        snprintf(name, sizeof(name), PREFIX "%08lu" IMG_EXT,
                 (unsigned long)c.best);

        uint32_t bytes = 0;
        FILE *f = fopen(p, "rb");
        if (f != NULL) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long sz = ftell(f);
                if (sz > 0)
                    bytes = (uint32_t)sz;
            }
            fclose(f);
        }
        n++;
        if (cb(user, name, bytes) != 0)
            break;
        c.below = c.best;
    }
    return n;
}

const char *loraitp_store_current(const loraitp_store_t *st)
{
    return (st != NULL) ? st->path : NULL;
}
