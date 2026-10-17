#ifndef DCP_TROPHY_RUNTIME_H
#define DCP_TROPHY_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void trophy_runtime_configure(const char *app0, const char *user_dir);

int32_t trophy_runtime_create_context(int32_t *context, int32_t user, uint32_t label, uint64_t options);
int32_t trophy_runtime_create_handle(int32_t *handle);
int32_t trophy_runtime_register_context(int32_t context, int32_t handle, uint64_t options);
int32_t trophy_runtime_destroy_handle(int32_t handle);
int32_t trophy_runtime_destroy_context(int32_t context);
int32_t trophy_runtime_unlock(int32_t context, int32_t handle, int32_t trophy_id, int32_t *platinum_id);
int32_t trophy_runtime_game_info(int32_t context, int32_t handle, void *details, void *data);
int32_t trophy_runtime_trophy_info(int32_t context, int32_t handle, int32_t trophy_id, void *details, void *data);
int32_t trophy_runtime_unlock_state(int32_t context, int32_t handle, uint32_t *flags, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif
