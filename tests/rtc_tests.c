/* Focused tests of libSceRtc's sceRtcGetWin32FileTime, resolved the way the loader binds the game's imports. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

#define RTC_INVALID_POINTER ((int32_t)0x80B50002)
#define RTC_INVALID_VALUE ((int32_t)0x80B50003)
#define RTC_INVALID_YEAR ((int32_t)0x80B50008)
#define RTC_INVALID_MONTH ((int32_t)0x80B50009)
#define RTC_INVALID_DAY ((int32_t)0x80B5000A)
#define RTC_INVALID_MICROSECOND ((int32_t)0x80B5000E)

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } DateTime;
typedef int32_t (ABI *GetWin32FileTime)(const DateTime *date, uint64_t *filetime);

static uint64_t filetime_of(GetWin32FileTime get, DateTime d, int32_t *result) {
    uint64_t out = 0;
    *result = get(&d, &out);
    return out;
}

int main(void) {
    runtime_start(1);
    const char *nid = "jfRO0uTjtzA#libSceRtc";
    GetWin32FileTime get = (GetWin32FileTime)runtime_resolve(nid, 0);
    CHECK(get != NULL && !strcmp(runtime_import_name(nid), "sceRtcGetWin32FileTime"));
    /* The loader traps an import only when runtime_resolve returns 0; this must be libSceRtc's own function. */
    CHECK(get != NULL && (uintptr_t)get == runtime_rtc_resolve(nid) && runtime_services_resolve(nid) == 0);
    if (!get) return 1;

    int32_t r = -1;
    CHECK(filetime_of(get, (DateTime){1601, 1, 1, 0, 0, 0, 0}, &r) == 0 && r == 0);
    CHECK(filetime_of(get, (DateTime){1970, 1, 1, 0, 0, 0, 0}, &r) == UINT64_C(116444736000000000) && r == 0);
    CHECK(filetime_of(get, (DateTime){2000, 2, 29, 23, 59, 59, 0}, &r) == UINT64_C(125963423990000000) && r == 0);
    CHECK(filetime_of(get, (DateTime){2026, 10, 29, 12, 34, 56, 789012}, &r) == UINT64_C(134377508967890120) && r == 0);
    CHECK(filetime_of(get, (DateTime){9999, 12, 31, 23, 59, 59, 999999}, &r) == UINT64_C(2650467743999999990) && r == 0);

    /* Exactly 8 little-endian bytes written; the struct is not changed. */
    unsigned char out[16];
    memset(out, 0xcc, sizeof(out));
    const DateTime date = {2026, 10, 29, 12, 34, 56, 0};
    DateTime copy = date;
    CHECK(get(&copy, (uint64_t *)(out + 4)) == 0);
    const uint64_t expected = UINT64_C(134377508960000000);
    for (int i = 0; i < 8; ++i) CHECK(out[4 + i] == (unsigned char)(expected >> (8 * i)));
    CHECK(out[3] == 0xcc && out[12] == 0xcc && !memcmp(&copy, &date, sizeof(date)));

    /* Errors leave the output alone (the game pre-zeroes it and ignores the result). */
    uint64_t kept = 7;
    CHECK(get(NULL, &kept) == RTC_INVALID_POINTER && kept == 7);
    CHECK(get(&date, NULL) == RTC_INVALID_POINTER);
    DateTime bad = {0, 1, 1, 0, 0, 0, 0};
    CHECK(get(&bad, &kept) == RTC_INVALID_YEAR && kept == 7);
    bad = (DateTime){2026, 13, 1, 0, 0, 0, 0};
    CHECK(get(&bad, &kept) == RTC_INVALID_MONTH);
    bad = (DateTime){2026, 2, 29, 0, 0, 0, 0};
    CHECK(get(&bad, &kept) == RTC_INVALID_DAY);
    bad = (DateTime){2026, 1, 1, 0, 0, 0, 1000000};
    CHECK(get(&bad, &kept) == RTC_INVALID_MICROSECOND);
    bad = (DateTime){1600, 12, 31, 23, 59, 59, 0};
    CHECK(get(&bad, &kept) == RTC_INVALID_VALUE && kept == 7);

    /* Later dates give larger values. */
    uint64_t previous = 0;
    const DateTime ordered[] = {{1601, 1, 1, 0, 0, 0, 1}, {1999, 12, 31, 23, 59, 59, 999999}, {2000, 1, 1, 0, 0, 0, 0},
                                {2026, 10, 29, 0, 0, 0, 0}, {2026, 10, 29, 0, 0, 0, 1}, {2026, 10, 29, 0, 0, 1, 0}};
    for (size_t i = 0; i < sizeof(ordered) / sizeof(*ordered); ++i) {
        const uint64_t value = filetime_of(get, ordered[i], &r);
        CHECK(r == 0 && value > previous);
        previous = value;
    }
    printf("rtc tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
