#include "runtime_integrity.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "runtime.h"

/* launcher/save_integrity.h */
int dcp_save_integrity_mark(const char *user_dir, const char *title_id, const char *save_dir, unsigned bits);
int dcp_save_integrity_flags(const char *user_dir, const char *title_id, const char *save_dir);

#define CHEAT_USED 1u
#define MARK_ATTEMPTS 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint settled;
static unsigned failed_generation, failures;

void runtime_integrity_cheat_used(void) {
    const unsigned marker = runtime_savedata_campaign_marker();
    if (!marker || atomic_load_explicit(&settled, memory_order_acquire) == marker) return;
    if (pthread_mutex_trylock(&lock)) return;
    char title[16], dir[33];
    const unsigned generation = runtime_savedata_campaign(title, sizeof(title), dir, sizeof(dir));
    if (generation == marker && runtime_savedata_campaign_marker() == generation && atomic_load(&settled) != generation) {
        if (dcp_save_integrity_mark(runtime_file_user_dir(), title, dir, CHEAT_USED)) {
            atomic_store_explicit(&settled, generation, memory_order_release);
            printf("Online integrity: gameplay cheats used with %s/%s\n", title, dir);
        } else {
            if (failed_generation != generation) failures = 0;
            failed_generation = generation;
            if (++failures >= MARK_ATTEMPTS) {
                atomic_store_explicit(&settled, generation, memory_order_release);
                printf("Online integrity: could not record cheat use for %s/%s\n", title, dir);
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

int runtime_integrity_hello_flags(void) {
    char title[16], dir[33];
    if (runtime_savedata_campaign(title, sizeof(title), dir, sizeof(dir)))
        return dcp_save_integrity_flags(runtime_file_user_dir(), title, dir);
    const char *installed = runtime_savedata_title();
    return installed ? dcp_save_integrity_flags(runtime_file_user_dir(), installed, NULL) : -1;
}
