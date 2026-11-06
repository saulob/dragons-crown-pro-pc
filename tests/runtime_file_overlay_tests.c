/* runtime_file_overlay borrows its patch data: nothing is copied, so the caller's buffers stay the source of truth for
 * the life of the process. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#define make_dir(path) mkdir(path, 0777)
#endif

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static int32_t error_slot;
int32_t *runtime_errno(void) { return &error_slot; }
int32_t runtime_guest_errno(int host_errno) { return host_errno; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}

static void write_host_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file) {
        fwrite(text, 1, strlen(text), file);
        fclose(file);
    }
}

static struct { char guest[64]; uint64_t offset, size; int calls; } observed;
static void observe(const char *guest, uint64_t offset, uint64_t size) {
    snprintf(observed.guest, sizeof(observed.guest), "%s", guest);
    observed.offset = offset;
    observed.size = size;
    ++observed.calls;
}

/* The observer sees successful reads only and changes nothing about them, overlaid or not. */
static void test_read_observer(void) {
    unsigned char seen[32];
    const int plain = (int)runtime_file_open("/app0/plain.cpk", 0, 0);
    const int overlaid = (int)runtime_file_open("/app0/arch.cpk", 0, 0);
    CHECK(plain >= 0 && overlaid >= 0);

    CHECK(runtime_file_read(plain, seen, 4) == 4 && observed.calls == 0);
    CHECK(runtime_file_pread(plain, seen, 4, 8) == 4 && memcmp(seen, "89AB", 4) == 0 && observed.calls == 0);
    CHECK(runtime_file_lseek(plain, 0, SEEK_SET) == 0);

    runtime_file_set_read_observer(observe);
    CHECK(runtime_file_read(plain, seen, 4) == 4 && memcmp(seen, "0123", 4) == 0);
    CHECK(observed.calls == 1 && !strcmp(observed.guest, "/app0/plain.cpk") && observed.offset == 0 && observed.size == 4);
    CHECK(runtime_file_read(plain, seen, 4) == 4 && memcmp(seen, "4567", 4) == 0);
    CHECK(observed.calls == 2 && observed.offset == 4 && runtime_file_lseek(plain, 0, SEEK_CUR) == 8);

    CHECK(runtime_file_pread(plain, seen, 2, 12) == 2 && memcmp(seen, "CD", 2) == 0);
    CHECK(observed.calls == 3 && observed.offset == 12 && observed.size == 2 && runtime_file_lseek(plain, 0, SEEK_CUR) == 8);

    /* A short read at the end reports what came back. */
    CHECK(runtime_file_lseek(plain, 14, SEEK_SET) == 14 && runtime_file_read(plain, seen, 8) == 2);
    CHECK(observed.calls == 4 && observed.offset == 14 && observed.size == 2);

    const int quiet = observed.calls;
    CHECK(runtime_file_read(plain, seen, 8) == 0);
    CHECK(runtime_file_pread(plain, seen, 8, 100) == 0);
    CHECK(runtime_file_read(9999, seen, 8) < 0 && runtime_file_pread(9999, seen, 8, 0) < 0);
    CHECK(runtime_file_read(-1, seen, 8) < 0 && runtime_file_pread(plain, seen, 8, -1) < 0);
    CHECK(observed.calls == quiet);

    CHECK(runtime_file_read(overlaid, seen, 8) == 8 && memcmp(seen, "0123QQQQ", 8) == 0);
    CHECK(observed.calls == quiet + 1 && !strcmp(observed.guest, "/app0/arch.cpk") && observed.offset == 0 && observed.size == 8);
    CHECK(runtime_file_read(overlaid, seen, 8) == 8 && memcmp(seen, "QQQQCDEF", 8) == 0);
    CHECK(observed.calls == quiet + 2 && observed.offset == 8);
    CHECK(runtime_file_pread(overlaid, seen, 4, 20) == 4 && observed.calls == quiet + 3 && observed.offset == 20);
    CHECK(runtime_file_pread(overlaid, seen, 4, 200) == 0 && observed.calls == quiet + 3);

    runtime_file_set_read_observer(NULL);
    CHECK(runtime_file_pread(plain, seen, 4, 0) == 4 && runtime_file_pread(overlaid, seen, 4, 0) == 4);
    CHECK(observed.calls == quiet + 3);
    runtime_file_close(plain);
    runtime_file_close(overlaid);
}

static int64_t read_all(const char *guest, unsigned char *out, uint64_t size) {
    const int64_t fd = runtime_file_open(guest, 0, 0);
    if (fd < 0) return fd;
    memset(out, 0xEE, size);
    const int64_t got = runtime_file_read((int)fd, out, size);
    runtime_file_close((int)fd);
    return got;
}

int main(void) {
    char app0[512], user[512], host[600];
    snprintf(app0, sizeof(app0), "%s/overlay-tests", DCP_OVERLAY_TEST_ROOT);
    snprintf(user, sizeof(user), "%s/user", app0);
    make_dir(DCP_OVERLAY_TEST_ROOT);
    make_dir(app0);
    make_dir(user);
    snprintf(host, sizeof(host), "%s/arch.cpk", app0);
    write_host_file(host, "0123456789ABCDEF");
    snprintf(host, sizeof(host), "%s/other.cpk", app0);
    write_host_file(host, "0123456789ABCDEF");
    snprintf(host, sizeof(host), "%s/plain.cpk", app0);
    write_host_file(host, "0123456789ABCDEF");
    runtime_file_configure(app0, user);

    unsigned char *patch = malloc(8);
    memset(patch, 'P', 8);
    const RuntimeFilePatch one = {4, 8, patch};
    CHECK(runtime_file_overlay("/app0/arch.cpk", 32, &one, 1) == 0);

    unsigned char seen[40];
    CHECK(read_all("/app0/arch.cpk", seen, 32) == 32);
    CHECK(memcmp(seen, "0123PPPPPPPPCDEF", 16) == 0);
    for (int i = 16; i < 32; ++i) CHECK(seen[i] == 0);

    memset(patch, 'Q', 8);
    CHECK(read_all("/app0/arch.cpk", seen, 32) == 32);
    CHECK(memcmp(seen, "0123QQQQQQQQCDEF", 16) == 0);

    const int64_t fd = runtime_file_open("/app0/arch.cpk", 0, 0);
    CHECK(fd >= 0);
    CHECK(runtime_file_pread((int)fd, seen, 8, 2) == 8 && memcmp(seen, "23QQQQQQ", 8) == 0);
    runtime_file_close((int)fd);
    CHECK(read_all("/app0/arch.cpk", seen, 32) == 32 && memcmp(seen, "0123QQQQQQQQCDEF", 16) == 0);

    CHECK(runtime_file_overlay("/app0/arch.cpk", 32, &one, 1) == -1);
    CHECK(runtime_file_overlay("/APP0/ARCH.CPK", 32, &one, 1) == -1);
    CHECK(read_all("/app0/arch.cpk", seen, 32) == 32 && memcmp(seen, "0123QQQQQQQQCDEF", 16) == 0);

    const RuntimeFilePatch outside = {30, 8, patch};
    CHECK(runtime_file_overlay("/app0/other.cpk", 32, &outside, 1) == -1);
    CHECK(read_all("/app0/other.cpk", seen, 32) == 16 && memcmp(seen, "0123456789ABCDEF", 16) == 0);

    CHECK(runtime_file_overlay("/app0/other.cpk", 32, NULL, 0) == 0);
    CHECK(read_all("/app0/other.cpk", seen, 32) == 32 && memcmp(seen, "0123456789ABCDEF", 16) == 0);
    CHECK(runtime_file_overlay("/app0/third.cpk", 8, NULL, 0) == 0);
    CHECK(runtime_file_overlay("/app0/fourth.cpk", 8, NULL, 0) == 0);
    CHECK(runtime_file_overlay("/app0/fifth.cpk", 8, NULL, 0) == -1);

    test_read_observer();

    printf("runtime overlay tests: %d checks, %d failures\n", checks, failures);
    free(patch);
    return failures ? 1 : 0;
}
