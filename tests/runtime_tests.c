/* Focused Linux tests of the runtime's memory and fiber layers: the guest address range, mapping and protection
 * through the PS4 kernel entry points, and libSceFiber's stack switching. No game files, no window. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xmmintrin.h>

#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define GUEST_PAGE 16384
#define PROT_CPU_RW 3
#define PROT_CPU_RWX 7
#define GUEST_MAP_FIXED 0x10

static int checks, failures;
#define CHECK(condition) do { __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED); if (!(condition)) { __atomic_add_fetch(&failures, 1, __ATOMIC_RELAXED); printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

void runtime_thread_keys_cleanup(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef int32_t (ABI *MapFlexible)(void **address, uint64_t size, int prot, int flags, const char *name);
typedef int32_t (ABI *Unmap)(void *address, uint64_t size);
typedef int32_t (ABI *Protect)(const void *address, uint64_t size, int prot);
typedef int32_t (ABI *AllocateDirect)(int64_t low, int64_t high, uint64_t size, uint64_t alignment, int type, int64_t *out);
typedef int32_t (ABI *MapDirect)(void **address, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment);

/* The /proc/self/maps line holding `address`: its permissions ("rw-p"), or an empty string. */
static void permissions_at(uintptr_t address, char out[8]) {
    out[0] = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    while (maps && fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3 && address >= start && address < end) {
            snprintf(out, 8, "%s", perms);
            break;
        }
    }
    if (maps) fclose(maps);
}

static void test_reservation_failure(void) {
    pid_t child = fork();
    if (child == 0) {
        freopen("/dev/null", "w", stderr);
        void *taken = mmap((void *)(USER_MIN + 0x100000000), 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (taken == MAP_FAILED) _exit(99);
        runtime_memory_reserve();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 21);
}

static void test_reservation(void) {
    runtime_memory_reserve();
    char perms[8];
    permissions_at(USER_MIN, perms);
    CHECK(!strcmp(perms, "---p"));
    permissions_at(USER_MAX - 4096, perms);
    CHECK(!strcmp(perms, "---p"));
    permissions_at(USER_MAX, perms);
    CHECK(strcmp(perms, "---p") != 0);
    /* A hint inside the range, as drivers give them, is not honoured. */
    void *hinted = mmap((void *)(USER_MIN + 0x20000000), 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(hinted != MAP_FAILED && ((uintptr_t)hinted < USER_MIN || (uintptr_t)hinted >= USER_MAX));
    if (hinted != MAP_FAILED) munmap(hinted, 1 << 20);
    void *fixed = mmap((void *)(USER_MIN + 0x20000000), 1 << 20, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(fixed == MAP_FAILED && errno == EEXIST);
    void *low = runtime_low_map(1 << 20, PROT_READ | PROT_WRITE);
    CHECK(low && (uintptr_t)low >= UINT64_C(0x800000000) && (uintptr_t)low < USER_MIN);
}

static void test_mapping(void) {
    MapFlexible map = (MapFlexible)runtime_memory_resolve("sceKernelMapNamedFlexibleMemory");
    Unmap unmap = (Unmap)runtime_memory_resolve("sceKernelMunmap");
    Protect protect = (Protect)runtime_memory_resolve("sceKernelMprotect");
    CHECK(map && unmap && protect);
    const uint64_t size = 4 * GUEST_PAGE;
    void *at = NULL;
    CHECK(map(&at, size, PROT_CPU_RW, 0, "test") == 0);
    CHECK((uintptr_t)at >= USER_MIN && (uintptr_t)at + size <= USER_MAX && !((uintptr_t)at % GUEST_PAGE));
    char perms[8];
    permissions_at((uintptr_t)at, perms);
    CHECK(!strcmp(perms, "rw-s"));
    unsigned char *code = at;
    code[0] = 0xb8; code[1] = 42; code[2] = 0; code[3] = 0; code[4] = 0; code[5] = 0xc3; /* mov eax,42; ret */
    CHECK(protect(at, size, PROT_CPU_RWX) == 0);
    permissions_at((uintptr_t)at, perms);
    CHECK(!strcmp(perms, "rwxs"));
    CHECK(((int (*)(void))(void *)code)() == 42);
    CHECK(protect(at, size, 1) == 0);
    permissions_at((uintptr_t)at, perms);
    CHECK(!strcmp(perms, "r--s"));
    CHECK(unmap(at, size) == 0);
    permissions_at((uintptr_t)at, perms);
    CHECK(!strcmp(perms, "---p")); /* still reserved: no host allocation can take the hole */

    void *outside = (void *)0x600000;
    permissions_at((uintptr_t)outside, perms);
    char before[8];
    snprintf(before, sizeof(before), "%s", perms);
    CHECK(map(&outside, size, PROT_CPU_RW, GUEST_MAP_FIXED, "outside") != 0);
    permissions_at(0x600000, perms);
    CHECK(!strcmp(perms, before)); /* the host mapping there is untouched */
    void *edge = (void *)(USER_MAX - GUEST_PAGE);
    CHECK(map(&edge, 2 * GUEST_PAGE, PROT_CPU_RW, GUEST_MAP_FIXED, "edge") != 0);
}

static void test_direct_aliasing(void) {
    AllocateDirect allocate = (AllocateDirect)runtime_memory_resolve("sceKernelAllocateDirectMemory");
    MapDirect map = (MapDirect)runtime_memory_resolve("sceKernelMapDirectMemory");
    Unmap unmap = (Unmap)runtime_memory_resolve("sceKernelMunmap");
    int64_t physical = -1;
    const uint64_t size = 8 * GUEST_PAGE;
    CHECK(allocate(0, INT64_MAX, size, 0, 3, &physical) == 0 && physical >= 0);
    void *first = NULL, *second = NULL;
    CHECK(map(&first, size, PROT_CPU_RW, 0, physical, 0) == 0);
    CHECK(map(&second, size, PROT_CPU_RW, 0, physical, 0) == 0);
    CHECK(first && second && first != second);
    if (first && second) {
        memset(first, 0, size);
        ((uint64_t *)first)[3] = UINT64_C(0x1122334455667788);
        CHECK(((uint64_t *)second)[3] == UINT64_C(0x1122334455667788));
        CHECK(unmap(first, size) == 0 && unmap(second, size) == 0);
    }
}

int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);

/* A flexible mapping split by a partial unmap keeps its backing pages: the right part still maps the pages it did. */
static void test_flexible_split(void) {
    MapFlexible map = (MapFlexible)runtime_memory_resolve("sceKernelMapNamedFlexibleMemory");
    Unmap unmap = (Unmap)runtime_memory_resolve("sceKernelMunmap");
    void *base = NULL;
    CHECK(map(&base, 3 * GUEST_PAGE, PROT_CPU_RW, 0, "split") == 0 && base);
    unsigned char *at = base;
    if (!at) return;
    at[0] = 0xa0;
    at[2 * GUEST_PAGE] = 0xc2;
    CHECK(unmap(at + GUEST_PAGE, GUEST_PAGE) == 0);
    const unsigned char patch = 0x5a;
    CHECK(runtime_memory_write_backing((uintptr_t)at + 2 * GUEST_PAGE + 5, &patch, 1));
    CHECK(at[2 * GUEST_PAGE + 5] == patch && at[5] == 0); /* the write reached the right page, not the left one */
    CHECK(unmap(at + 2 * GUEST_PAGE, GUEST_PAGE) == 0);
    CHECK(at[0] == 0xa0);                                 /* releasing the right page leaves the left one alone */
    void *again = at + 2 * GUEST_PAGE;
    CHECK(map(&again, GUEST_PAGE, PROT_CPU_RW, GUEST_MAP_FIXED, "again") == 0 && *(unsigned char *)again == 0);
    CHECK(at[0] == 0xa0 && unmap(at, GUEST_PAGE) == 0 && unmap(again, GUEST_PAGE) == 0);
}

/* ---- fibers ---- */
typedef int32_t (ABI *FiberInitialize)(void *fiber, const char *name, void (ABI *entry)(uint64_t, uint64_t), uint64_t arg,
                                       void *context, uint64_t size, const void *option, uint32_t version);
typedef int32_t (ABI *FiberRun)(void *fiber, uint64_t arg, uint64_t *returned);
typedef int32_t (ABI *FiberSwitch)(void *fiber, uint64_t arg, uint64_t *received);
typedef int32_t (ABI *FiberReturn)(uint64_t arg, uint64_t *received);
typedef int32_t (ABI *FiberFinalize)(void *fiber);
typedef int32_t (ABI *FiberSelf)(void **fiber);
static FiberInitialize fiber_initialize;
static FiberRun fiber_run;
static FiberSwitch fiber_switch;
static FiberReturn fiber_return;
static FiberFinalize fiber_finalize;
static FiberSelf fiber_self;

static __attribute__((aligned(16))) char fiber_a[256], fiber_b[256], fiber_c[256];
static uint64_t seen_entry_args[2];
static uintptr_t deepest_stack_address;

static uint64_t recurse(int depth) {
    volatile char pad[512];
    memset((void *)pad, depth, sizeof(pad));
    return depth ? recurse(depth - 1) + pad[depth % 512] : 0;
}

static void ABI entry_a(uint64_t initial, uint64_t first_run) {
    seen_entry_args[0] = initial;
    seen_entry_args[1] = first_run;
    char local;
    deepest_stack_address = (uintptr_t)&local;
    __m128 value = _mm_set1_ps(1.5f); /* SSE spills need a 16-byte aligned frame */
    uint64_t received = 0;
    for (int round = 0; round < 3; ++round) {
        fiber_return(0x1000 + (uint64_t)round, &received);
        value = _mm_add_ps(value, _mm_set1_ps((float)received));
    }
    unsigned csr = _mm_getcsr();
    fiber_return(csr == 0x1f80 ? 0x600D : 0xBAD, &received);
    uint64_t sum = recurse(300); /* ~150 KiB of the fiber's stack */
    float lane[4];
    _mm_storeu_ps(lane, value);
    fiber_return(sum + (uint64_t)lane[0], &received);
    for (;;) fiber_return(0xDEAD, &received);
}

static void ABI entry_b(uint64_t initial, uint64_t first_run) {
    (void)initial; (void)first_run;
    void *self = NULL;
    CHECK(fiber_self(&self) == 0 && self == fiber_b);
    uint64_t received = 0;
    /* B hands control to C; C returns to the thread; the thread later resumes B. */
    fiber_switch(fiber_c, 0xC0, &received);
    fiber_return(received + 1, &received);
    for (;;) fiber_return(0xDEAD, &received);
}

static void ABI entry_c(uint64_t initial, uint64_t first_run) {
    (void)initial;
    uint64_t received = 0;
    fiber_return(first_run + 0x100, &received);
    for (;;) fiber_return(0xDEAD, &received);
}

static void *run_on_another_thread(void *unused) {
    (void)unused;
    uint64_t returned = 0;
    CHECK(fiber_run(fiber_a, 7, &returned) == 0); /* a fiber started on the main thread continues here */
    CHECK(returned == 0x600D);
    return NULL;
}

static void test_fibers(void) {
    fiber_initialize = (FiberInitialize)runtime_fiber_resolve("_sceFiberInitializeImpl");
    fiber_run = (FiberRun)runtime_fiber_resolve("sceFiberRun");
    fiber_switch = (FiberSwitch)runtime_fiber_resolve("sceFiberSwitch");
    fiber_return = (FiberReturn)runtime_fiber_resolve("sceFiberReturnToThread");
    fiber_finalize = (FiberFinalize)runtime_fiber_resolve("sceFiberFinalize");
    fiber_self = (FiberSelf)runtime_fiber_resolve("sceFiberGetSelf");
    CHECK(fiber_initialize && fiber_run && fiber_switch && fiber_return && fiber_finalize && fiber_self);

    CHECK(fiber_initialize(fiber_a, "a", entry_a, 0xA11CE, NULL, 0, NULL, 0) == 0);
    CHECK(fiber_initialize(fiber_b, "b", entry_b, 0xB0B, NULL, 0, NULL, 0) == 0);
    CHECK(fiber_initialize(fiber_c, "c", entry_c, 0xC0C, NULL, 0, NULL, 0) == 0);
    CHECK(fiber_run(NULL, 0, NULL) != 0 && fiber_self(&(void *){0}) != 0); /* a plain thread is no fiber */

    unsigned thread_csr = _mm_getcsr();
    uint64_t returned = 0;
    for (int round = 0; round < 3; ++round) {
        CHECK(fiber_run(fiber_a, 10 + (uint64_t)round, &returned) == 0);
        CHECK(returned == 0x1000 + (uint64_t)round);
    }
    CHECK(seen_entry_args[0] == 0xA11CE && seen_entry_args[1] == 10);
    CHECK(deepest_stack_address && deepest_stack_address < USER_MIN); /* fiber stacks sit below 1 TiB */
    CHECK(_mm_getcsr() == thread_csr);

    /* B switches to C, C returns to the thread; the thread then resumes B, which was parked inside its Switch. */
    CHECK(fiber_run(fiber_b, 0, &returned) == 0);
    CHECK(returned == 0x1C0);
    CHECK(fiber_run(fiber_b, 5, &returned) == 0);
    CHECK(returned == 6);
    CHECK(fiber_finalize(fiber_b) == 0 && fiber_finalize(fiber_c) == 0);

    pthread_t other;
    pthread_create(&other, NULL, run_on_another_thread, NULL);
    pthread_join(other, NULL);
    CHECK(fiber_run(fiber_a, 8, &returned) == 0 && returned != 0xDEAD); /* A survived 150 KiB of recursion */
    CHECK(fiber_finalize(fiber_a) == 0);

    /* Many short-lived fibers reuse their stacks. */
    for (int i = 0; i < 200; ++i) {
        static char scratch[256] __attribute__((aligned(16)));
        CHECK(fiber_initialize(scratch, "temp", entry_c, 0, NULL, 0, NULL, 0) == 0);
        CHECK(fiber_run(scratch, 1, &returned) == 0 && returned == 0x101);
        CHECK(fiber_finalize(scratch) == 0);
    }
}

/* ---- thread stacks ---- */
typedef int32_t (ABI *ThreadCreate)(void **thread, void **attr, void *(ABI *entry)(void *), void *argument, const char *name);
typedef int32_t (ABI *ThreadJoin)(void *thread, void **result);
typedef int32_t (ABI *ThreadDetach)(void *thread);
typedef int32_t (ABI *AttrInit)(void **attr);
typedef int32_t (ABI *AttrSetDetach)(void **attr, int state);
static ThreadCreate thread_create;
static ThreadJoin thread_join;
static ThreadDetach thread_detach;
static AttrInit attr_init;
static AttrSetDetach attr_set_detach;
static atomic_int workers_done;
static const uintptr_t VA_BUDGET = 64u << 20; /* a leak of the 1000 stacks below would be over a gigabyte */

static void *ABI worker(void *argument) {
    recurse(600); /* about 300 KiB of the thread's stack */
    atomic_fetch_add(&workers_done, 1);
    return (void *)((uintptr_t)argument + 1);
}

static void *ABI exiting_worker(void *argument) {
    atomic_fetch_add(&workers_done, 1);
    return argument;
}

static uintptr_t address_probe(void) {
    void *probe = runtime_low_map(GUEST_PAGE, PROT_READ);
    return (uintptr_t)probe;
}

static void wait_for_workers(int total) {
    for (int spins = 0; atomic_load(&workers_done) < total && spins < 100000; ++spins) usleep(100);
}

static size_t mapping_count(void) {
    size_t lines = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    for (int c; maps && (c = fgetc(maps)) != EOF;) lines += c == '\n';
    if (maps) fclose(maps);
    return lines;
}

static void *spawn_and_join(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; ++i) {
        void *thread = NULL, *result = NULL;
        CHECK(thread_create(&thread, NULL, worker, (void *)(uintptr_t)i, "child") == 0);
        CHECK(thread_join(thread, &result) == 0 && result == (void *)(uintptr_t)(i + 1));
    }
    return NULL;
}

static void test_thread_stacks(void) {
    thread_create = (ThreadCreate)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    thread_join = (ThreadJoin)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    thread_detach = (ThreadDetach)runtime_thread_resolve("4qGrR6eoP9Y#libkernel");
    attr_init = (AttrInit)runtime_thread_resolve("nsYoNRywwNg#libkernel");
    attr_set_detach = (AttrSetDetach)runtime_thread_resolve("-Wreprtu0Qs#libkernel");
    CHECK(thread_create && thread_join && thread_detach && attr_init && attr_set_detach);

    /* A released stack comes back, guard page included, and its pages were returned. */
    void *first = runtime_stack_acquire(1 << 20);
    char perms[8];
    permissions_at((uintptr_t)first, perms);
    CHECK(first && (uintptr_t)first < USER_MIN && !strcmp(perms, "---p"));
    permissions_at((uintptr_t)first + 65536, perms);
    CHECK(!strcmp(perms, "rw-p"));
    ((char *)first)[1 << 19] = 7;
    runtime_stack_release(first, 1 << 20);
    void *again = runtime_stack_acquire(1 << 20);
    CHECK(again == first);
    runtime_stack_release(again, 1 << 20);

    void *warm = NULL;
    CHECK(thread_create(&warm, NULL, worker, NULL, "warm") == 0 && thread_join(warm, NULL) == 0);
    CHECK(thread_detach(warm) != 0); /* joined: the reaper must never join it again */
    const uintptr_t start = address_probe();
    const size_t maps_before = mapping_count();

    for (int i = 0; i < 1000; ++i) {
        void *thread = NULL, *result = NULL;
        if (thread_create(&thread, NULL, worker, (void *)(uintptr_t)i, "joined")) { CHECK(0); break; }
        CHECK(thread_join(thread, &result) == 0 && result == (void *)(uintptr_t)(i + 1));
    }
    const uintptr_t after_joined = address_probe();
    CHECK(after_joined - start < VA_BUDGET);

    void *attr = NULL;
    CHECK(attr_init(&attr) == 0 && attr_set_detach(&attr, 1) == 0);
    atomic_store(&workers_done, 0);
    for (int i = 0; i < 1000; ++i) {
        void *thread = NULL;
        if (thread_create(&thread, &attr, worker, NULL, "detached")) { CHECK(0); break; }
        wait_for_workers(i + 1);
    }
    const uintptr_t after_detached = address_probe();
    CHECK(atomic_load(&workers_done) == 1000 && after_detached - after_joined < VA_BUDGET);

    atomic_store(&workers_done, 0);
    for (int i = 0; i < 300; ++i) {
        void *thread = NULL;
        if (thread_create(&thread, NULL, exiting_worker, NULL, "late-detach")) { CHECK(0); break; }
        if (i % 2) wait_for_workers(i + 1); /* detached after it has exited, and while it may still run */
        CHECK(thread_detach(thread) == 0);
    }
    for (int i = 0; i < 3; ++i) { void *thread = NULL; thread_create(&thread, &attr, exiting_worker, NULL, "flush"); }
    const uintptr_t after_late = address_probe();
    CHECK(after_late - after_detached < VA_BUDGET);

    pthread_t parents[8];
    for (int i = 0; i < 8; ++i) pthread_create(&parents[i], NULL, spawn_and_join, NULL);
    for (int i = 0; i < 8; ++i) pthread_join(parents[i], NULL);
    CHECK(address_probe() - after_late < VA_BUDGET);
    CHECK(mapping_count() < maps_before + 100);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_reservation_failure();
    test_reservation();
    test_mapping();
    test_direct_aliasing();
    test_flexible_split();
    test_fibers();
    test_thread_stacks();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
