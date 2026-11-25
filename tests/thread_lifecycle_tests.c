/* DCP-YYM3TL: the Inn's ally loading runs on a detached guest thread. The game's owner joins it (which cannot wait for a
 * detached thread), then cancels it, then frees the object. A cancel that returns while the worker still runs lets the
 * worker use the freed object when the player returns to the Inn and Report adventurers rebuilds it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
/* The runtime's stack pool is not under test here: the host stack is enough. */
void *runtime_stack_acquire(size_t size) { (void)size; return NULL; }
void runtime_stack_release(void *stack, size_t size) { (void)stack; (void)size; }
#endif
void runtime_thread_keys_cleanup(void) {}

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

typedef void *(ABI *ThreadEntry)(void *);
typedef int32_t (ABI *ThreadCreate)(void **thread, void **attr, ThreadEntry entry, void *argument, const char *name);
typedef int32_t (ABI *ThreadJoin)(void *thread, void **result);
typedef int32_t (ABI *ThreadCancel)(void *thread);
typedef int32_t (ABI *AttrInit)(void **attr);
typedef int32_t (ABI *AttrSetDetach)(void **attr, int detached);

static ThreadCreate thread_create;
static ThreadJoin thread_join;
static ThreadCancel thread_cancel;
static AttrInit attr_init;
static AttrSetDetach attr_set_detach;

static atomic_int worker_started, worker_finished, owner_destroyed, report_rebuilt, worker_used_after_destroy;

/* The slow-host loader: it outlives the 2 s grace the runtime used to give a cancel. */
static void *ABI loading_worker(void *argument) {
    (void)argument;
    atomic_store(&worker_started, 1);
    usleep(4000 * 1000);
    if (atomic_load(&owner_destroyed) && atomic_load(&report_rebuilt))
        atomic_store(&worker_used_after_destroy, 1);
    atomic_store(&worker_finished, 1);
    return NULL;
}

static int wait_for(atomic_int *flag, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (atomic_load(flag)) return 1;
        usleep(10 * 1000);
    }
    return atomic_load(flag) != 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("DCP-YYM3TL online Inn report-adventurers lifecycle");

    thread_create = (ThreadCreate)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    thread_join = (ThreadJoin)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    thread_cancel = (ThreadCancel)runtime_thread_resolve("qBDmpCyGssE#libkernel");
    attr_init = (AttrInit)runtime_thread_resolve("nsYoNRywwNg#libkernel");
    attr_set_detach = (AttrSetDetach)runtime_thread_resolve("-Wreprtu0Qs#libkernel");
    CHECK(thread_create && thread_join && thread_cancel && attr_init && attr_set_detach);

    void *attr = NULL;
    CHECK(attr_init(&attr) == 0 && attr_set_detach(&attr, 1) == 0);
    void *loader = NULL;
    CHECK(thread_create(&loader, &attr, loading_worker, NULL, "ally-loader") == 0);
    CHECK(wait_for(&worker_started, 2000));

    /* The game's owner: join (fails for the detached loader), then cancel, then free. */
    CHECK(thread_join(loader, NULL) != 0);
    CHECK(atomic_load(&worker_finished) == 0);
    CHECK(thread_cancel(loader) == 0);
    CHECK(atomic_load(&worker_finished) == 1);

    atomic_store(&owner_destroyed, 1);
    atomic_store(&report_rebuilt, 1); /* back in the Inn, Report adventurers rebuilds the state */
    CHECK(wait_for(&worker_finished, 3000));
    CHECK(atomic_load(&worker_used_after_destroy) == 0);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
