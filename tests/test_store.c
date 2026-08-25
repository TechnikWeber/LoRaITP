/*
 * Image store tests, against real files in a temporary directory.
 *
 * The store is written on stdio precisely so this is possible: ESP-IDF
 * mounts LittleFS into its VFS, so the same calls run there unchanged.
 * That means the awkward parts - out-of-order writes, ring pruning,
 * recovery from an interrupted transfer - can be exercised here rather
 * than discovered on a node in a field.
 */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loraitp.h"
#include "loraitp_store.h"

static int passed, failed;
static void check(int cond, const char *name)
{
    if (cond) { passed++; printf("  ok   %s\n", name); }
    else      { failed++; printf("  FAIL %s\n", name); }
}

static char g_dir[] = "/tmp/loraitp_store_testXXXXXX";
static uint8_t store_mem[512];
static loraitp_store_t *store;

static int file_exists(const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_dir, rel);
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void wipe(void)
{
    DIR *d = opendir(g_dir);
    if (!d) return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", g_dir, e->d_name);
        remove(p);
    }
    closedir(d);
}

static void meta_defaults(loraitp_store_meta_t *m, uint32_t len)
{
    memset(m, 0, sizeof(*m));
    m->img_id = 42; m->layer = 1; m->codec = 2;
    m->img_len = len; m->crc32 = 0xDEADBEEFu;
    m->width = 320; m->height = 240;
    m->chunks_have = 19; m->chunks_total = 19;
    m->rounds = 1; m->airtime_ms = 38875;
    m->rssi_dbm = -98; m->snr_qdb = -20;
    m->complete = true; m->crc_ok = true;
}

/* ------------------------------------------------------------- basics */

static void test_roundtrip(void)
{
    printf("\nwrite and read back\n");
    wipe();
    check(loraitp_store_size() <= sizeof(store_mem), "store fits its buffer");
    check(loraitp_store_init(store, g_dir, 5) == LORAITP_OK, "init");
    check(loraitp_store_count(store) == 0, "starts empty");

    const uint32_t len = 1000;
    uint8_t image[1000];
    for (uint32_t i = 0; i < len; i++)
        image[i] = (uint8_t)((i * 37) & 0xFF);

    loraitp_port_t port;
    memset(&port, 0, sizeof(port));
    loraitp_store_attach(&port, store);
    check(port.image_read && port.image_write && port.ctx == store,
          "attach fills the storage callbacks");

    check(loraitp_store_begin_write(store, len) == LORAITP_OK, "begin_write");

    /*
     * Out of order and with a gap, the way a lossy transfer arrives.
     * Chunk 3 is deliberately never written.
     */
    const uint16_t chunk = 100;
    int order[] = { 5, 0, 9, 2, 7, 1, 8, 4, 6 };   /* 3 is missing */
    int writes_ok = 1;
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint32_t off = (uint32_t)order[i] * chunk;
        if (port.image_write(port.ctx, off, image + off, chunk) != LORAITP_OK)
            writes_ok = 0;
    }
    check(writes_ok, "nine out-of-order chunk writes all succeed");

    loraitp_store_meta_t m;
    meta_defaults(&m, len);
    m.chunks_have = 9; m.chunks_total = 10; m.complete = false; m.crc_ok = false;
    check(loraitp_store_finish(store, &m) == LORAITP_OK, "finish");

    check(file_exists("img_00000001.jpg"), "image renamed into place");
    check(file_exists("img_00000001.json"), "sidecar written");
    check(!file_exists("img_00000001.jpg.part"), "no .part left behind");

    /* Read it back: written chunks intact, the missing one zeros. */
    check(loraitp_store_begin_read(store, NULL) == LORAITP_OK,
          "begin_read finds the newest");
    uint8_t back[1000];
    memset(back, 0xAA, sizeof(back));
    for (uint32_t off = 0; off < len; off += chunk)
        port.image_read(port.ctx, off, back + off, chunk);
    loraitp_store_finish(store, NULL);

    int intact = 1, gap_zero = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (i >= 300 && i < 400) {
            if (back[i] != 0) gap_zero = 0;
        } else if (back[i] != image[i]) {
            intact = 0;
        }
    }
    check(intact, "written chunks survive an out-of-order write");
    check(gap_zero,
          "a chunk that never arrived reads back as zeros, not as a hole");
}

/* ----------------------------------------------------------- the ring */

static void test_ring(void)
{
    printf("\nring pruning\n");
    wipe();
    loraitp_store_init(store, g_dir, 5);

    loraitp_port_t port;
    memset(&port, 0, sizeof(port));
    loraitp_store_attach(&port, store);

    uint8_t buf[64];
    memset(buf, 0x5A, sizeof(buf));
    for (int i = 0; i < 8; i++) {
        loraitp_store_begin_write(store, sizeof(buf));
        port.image_write(port.ctx, 0, buf, sizeof(buf));
        loraitp_store_meta_t m;
        meta_defaults(&m, sizeof(buf));
        m.img_id = (uint16_t)i;
        loraitp_store_finish(store, &m);
    }

    check(loraitp_store_count(store) == 5, "ring holds exactly `keep` images");
    check(!file_exists("img_00000001.jpg"), "oldest image deleted");
    check(!file_exists("img_00000001.json"), "its sidecar deleted too");
    check(file_exists("img_00000008.jpg"), "newest image kept");
    check(file_exists("img_00000004.jpg"), "the boundary image kept");

    char newest[64];
    check(loraitp_store_newest(store, newest, sizeof(newest)) == LORAITP_OK
          && strcmp(newest, "img_00000008.jpg") == 0, "newest is reported");

    /* Sequence numbers must keep climbing after a restart, or a new
     * image would overwrite one the ring still holds. */
    check(loraitp_store_init(store, g_dir, 5) == LORAITP_OK, "remount");
    loraitp_store_begin_write(store, 16);
    loraitp_store_meta_t m;
    meta_defaults(&m, 16);
    loraitp_store_finish(store, &m);
    check(file_exists("img_00000009.jpg"),
          "sequence continues across a remount");
}

/* ------------------------------------------------------- interruption */

static void test_recovery(void)
{
    printf("\nrecovery from an interrupted transfer\n");
    wipe();
    loraitp_store_init(store, g_dir, 5);

    loraitp_port_t port;
    memset(&port, 0, sizeof(port));
    loraitp_store_attach(&port, store);

    /* Begin a write and never finish it - a power failure mid-transfer. */
    loraitp_store_begin_write(store, 256);
    uint8_t buf[32];
    memset(buf, 1, sizeof(buf));
    port.image_write(port.ctx, 0, buf, sizeof(buf));
    check(file_exists("img_00000001.jpg.part"), "an unfinished write is .part");

    /* Simulate the reboot: a fresh mount over the same directory. */
    check(loraitp_store_init(store, g_dir, 5) == LORAITP_OK, "remount");
    check(!file_exists("img_00000001.jpg.part"),
          "the stale .part is cleared on mount");
    check(!file_exists("img_00000001.jpg"),
          "it is not promoted to a real image");
    check(loraitp_store_count(store) == 0, "store is empty, not confused");

    /* Discarding a transfer explicitly leaves nothing behind either. */
    loraitp_store_begin_write(store, 64);
    port.image_write(port.ctx, 0, buf, sizeof(buf));
    check(loraitp_store_finish(store, NULL) == LORAITP_OK, "discard");
    check(loraitp_store_count(store) == 0, "a discarded transfer leaves nothing");
}

/* ----------------------------------------------------------- listing */

static char g_seen[8][64];
static int g_n_seen;
static int list_cb(void *user, const char *name, uint32_t bytes)
{
    (void)user;
    if (g_n_seen < 8 && bytes > 0)
        snprintf(g_seen[g_n_seen++], 64, "%s", name);
    return 0;
}

static void test_list(void)
{
    printf("\nlisting\n");
    wipe();
    loraitp_store_init(store, g_dir, 0);      /* keep everything */

    loraitp_port_t port;
    memset(&port, 0, sizeof(port));
    loraitp_store_attach(&port, store);

    uint8_t buf[32];
    memset(buf, 7, sizeof(buf));
    for (int i = 0; i < 4; i++) {
        loraitp_store_begin_write(store, sizeof(buf));
        port.image_write(port.ctx, 0, buf, sizeof(buf));
        loraitp_store_meta_t m;
        meta_defaults(&m, sizeof(buf));
        loraitp_store_finish(store, &m);
    }

    g_n_seen = 0;
    int n = loraitp_store_list(store, list_cb, NULL);
    check(n == 4, "list returns every image");
    check(g_n_seen == 4 && strcmp(g_seen[0], "img_00000004.jpg") == 0
          && strcmp(g_seen[3], "img_00000001.jpg") == 0,
          "listing is newest first");
    check(loraitp_store_count(store) == 4, "keep=0 prunes nothing");
}

/* --------------------------------------------------------- bad inputs */

static void test_errors(void)
{
    printf("\nbad inputs\n");
    check(loraitp_store_init(store, "/nonexistent/nowhere", 5) == LORAITP_E_IO,
          "mounting a missing directory fails cleanly");

    loraitp_store_init(store, g_dir, 5);
    loraitp_port_t port;
    memset(&port, 0, sizeof(port));
    loraitp_store_attach(&port, store);

    uint8_t buf[8];
    check(port.image_write(port.ctx, 0, buf, sizeof(buf)) == LORAITP_E_IO,
          "writing with no image open is refused");
    check(port.image_read(port.ctx, 0, buf, sizeof(buf)) == LORAITP_E_IO,
          "reading with no image open is refused");

    loraitp_store_begin_write(store, 64);
    check(loraitp_store_begin_write(store, 64) == LORAITP_E_ARG,
          "a second begin_write is refused");
    check(port.image_read(port.ctx, 0, buf, sizeof(buf)) == LORAITP_E_IO,
          "reading from an image opened for writing is refused");
    loraitp_store_finish(store, NULL);

    check(loraitp_store_begin_read(store, "img_99999999.jpg") == LORAITP_E_IO,
          "opening an image that does not exist fails cleanly");
}

int main(void)
{
    if (mkdtemp(g_dir) == NULL) {
        printf("cannot create a temporary directory\n");
        return 2;
    }
    printf("LoRaITP image store  (%s)\n", g_dir);
    printf("  sizeof(loraitp_store_t) = %zu bytes\n", loraitp_store_size());

    store = (loraitp_store_t *)store_mem;

    test_roundtrip();
    test_ring();
    test_recovery();
    test_list();
    test_errors();

    wipe();
    rmdir(g_dir);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
