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

    printf("runtime overlay tests: %d checks, %d failures\n", checks, failures);
    free(patch);
    return failures ? 1 : 0;
}
