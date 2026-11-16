#include "online_prompt.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic(OnlinePromptFn) prompt_function;
static _Atomic uint64_t prompt_owner_vtable;

void online_prompt_configure(OnlinePromptFn prompt, uint64_t owner_vtable) {
    atomic_store(&prompt_owner_vtable, owner_vtable);
    atomic_store(&prompt_function, prompt);
}

int online_prompt_configured(void) {
    return atomic_load(&prompt_function) != NULL;
}

OnlinePromptStep online_prompt_step(OnlinePrompt *prompt) {
    const OnlinePromptFn run = atomic_load(&prompt_function);
    if (!run) {
        online_prompt_drop(prompt);
        return ONLINE_PROMPT_FINISHED;
    }
    const int done = run((uint64_t)(uintptr_t)prompt->owner, ONLINE_PROMPT_SLOT) != 0;
    const int was_open = prompt->open;
    prompt->open = !done && prompt->owner[ONLINE_PROMPT_OWNER_STATES + ONLINE_PROMPT_SLOT] != 0;
    prompt->settled |= done;
    if (done) return ONLINE_PROMPT_FINISHED;
    return prompt->open && !was_open ? ONLINE_PROMPT_SHOWN : ONLINE_PROMPT_RUNNING;
}

OnlinePromptStep online_prompt_start(OnlinePrompt *prompt, uint64_t system_object) {
    memset(prompt->owner, 0, sizeof(prompt->owner));
    const uint64_t vtable = atomic_load(&prompt_owner_vtable);
    memcpy(prompt->owner, &vtable, sizeof(vtable));
    memcpy(prompt->owner + ONLINE_PROMPT_OWNER_SYSOBJ, &system_object, sizeof(system_object));
    const uint32_t changed = ONLINE_PROMPT_SLOT1_CHANGED;
    memcpy(prompt->owner + ONLINE_PROMPT_OWNER_FLAGS, &changed, sizeof(changed));
    prompt->system_object = system_object;
    return online_prompt_step(prompt);
}

void online_prompt_drop(OnlinePrompt *prompt) {
    prompt->open = 0;
    prompt->system_object = 0;
}

static uint64_t guest_u64(uint64_t at) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

int online_native_online(uint64_t manager) {
    const uint64_t game = manager ? guest_u64(manager + ONLINE_MANAGER_GAME) : 0;
    const uint64_t net = game ? guest_u64(game + ONLINE_GAME_NET) : 0;
    const uint64_t vtable = net ? guest_u64(net) : 0;
    const uint64_t method = vtable ? guest_u64(vtable + ONLINE_NET_IS_ONLINE) : 0;
    if (!method) return -1;
    return ((OnlineNetQueryFn)(uintptr_t)method)(net) != 0;
}
