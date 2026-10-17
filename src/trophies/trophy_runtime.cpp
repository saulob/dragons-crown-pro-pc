#include "trophies/trophy_runtime.h"

#include "trophies/trophy_service.h"
#include "utf8.h"

namespace {

dcp::trophies::TrophyService& Service() {
    static dcp::trophies::TrophyService service;
    return service;
}

} // namespace

extern "C" {

void trophy_runtime_configure(const char *app0, const char *user_dir) {
    Service().Configure(dcp::PathFromUtf8(app0 ? app0 : ""), dcp::PathFromUtf8(user_dir ? user_dir : "user"));
}

int32_t trophy_runtime_create_context(int32_t *context, int32_t user, uint32_t label, uint64_t options) {
    return Service().CreateContext(context, user, label, options);
}

int32_t trophy_runtime_create_handle(int32_t *handle) {
    return Service().CreateHandle(handle);
}

int32_t trophy_runtime_register_context(int32_t context, int32_t handle, uint64_t options) {
    return Service().RegisterContext(context, handle, options);
}

int32_t trophy_runtime_destroy_handle(int32_t handle) {
    return Service().DestroyHandle(handle);
}

int32_t trophy_runtime_destroy_context(int32_t context) {
    return Service().DestroyContext(context);
}

int32_t trophy_runtime_unlock(int32_t context, int32_t handle, int32_t trophy_id, int32_t *platinum_id) {
    return Service().UnlockTrophy(context, handle, trophy_id, platinum_id);
}

int32_t trophy_runtime_game_info(int32_t context, int32_t handle, void *details, void *data) {
    return Service().GetGameInfo(context, handle, details, data);
}

int32_t trophy_runtime_trophy_info(int32_t context, int32_t handle, int32_t trophy_id, void *details, void *data) {
    return Service().GetTrophyInfo(context, handle, trophy_id, details, data);
}

int32_t trophy_runtime_unlock_state(int32_t context, int32_t handle, uint32_t *flags, uint32_t *count) {
    return Service().GetUnlockState(context, handle, reinterpret_cast<dcp::trophies::FlagArray *>(flags), count);
}

}
