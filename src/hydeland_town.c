#include "hydeland_town.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic uint64_t mask_table;
static _Atomic(HydelandTownObserver) observers[HYDELAND_TOWN_OBSERVERS];
static atomic_int installed;

static uint64_t guest_u64(uint64_t at) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint32_t guest_u32(uint64_t at) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint8_t guest_u8(uint64_t at) {
    return *(const uint8_t *)(uintptr_t)at;
}

uint64_t hydeland_town_manager(uint64_t town) {
    if (!town) return 0;
    const uint64_t context = guest_u64(town + HYDELAND_TOWN_CONTEXT);
    return context ? guest_u64(context + HYDELAND_CONTEXT_MANAGER) : 0;
}

int hydeland_town_accepts_input(uint64_t manager) {
    const uint64_t table = atomic_load(&mask_table);
    if (!manager || !table) return 0;
    const uint64_t mode = guest_u64(manager + HYDELAND_MANAGER_MODE);
    const uint64_t scene = guest_u64(manager + HYDELAND_MANAGER_SCENE);
    const uint64_t queue = guest_u64(manager + HYDELAND_MANAGER_QUEUE);
    if (!mode || !scene || !queue) return 0;
    return guest_u8(mode + HYDELAND_OBJECT_STATE) == HYDELAND_MODE_TOWN &&
           (guest_u32(mode + HYDELAND_MODE_INPUT) & guest_u32(table + HYDELAND_MASK_INPUT)) != 0 &&
           guest_u8(scene + HYDELAND_OBJECT_STATE) == HYDELAND_SCENE_PLAY &&
           guest_u8(queue + HYDELAND_OBJECT_STATE) == 0 &&
           guest_u64(queue + HYDELAND_QUEUE_END) == guest_u64(queue + HYDELAND_QUEUE_BEGIN);
}

int hydeland_town_is_hydeland(uint64_t manager) {
    const uint64_t scene = manager ? guest_u64(manager + HYDELAND_MANAGER_SCENE) : 0;
    return scene && guest_u32(scene + HYDELAND_AREA_TYPE) == HYDELAND_AREA_TOWN &&
           guest_u32(scene + HYDELAND_AREA_LOCATION) == HYDELAND_AREA_STREET;
}

void hydeland_town_set_mask_table(uint64_t table) {
    atomic_store(&mask_table, table);
}

static int contains(const uint8_t *image, uint32_t from, uint32_t length, const uint8_t *needle, size_t needle_size) {
    for (uint32_t i = 0; i + needle_size <= length; ++i)
        if (!memcmp(image + from + i, needle, needle_size)) return 1;
    return 0;
}

/* The prompt function opens with the four checks hydeland_town_accepts_input repeats, and loads the mask from a table. */
int hydeland_town_prompt_checks_match(const uint8_t *image, uint64_t size, uint32_t function) {
    static const uint8_t mode[] = {0x41, 0x80, 0x7c, 0x24, 0x5b, 0x03};
    static const uint8_t mask[] = {0x8b, 0x40, 0x04, 0x41, 0x85, 0x84, 0x24, 0x14, 0x05, 0x00, 0x00};
    static const uint8_t scene[] = {0x41, 0x80, 0x7e, 0x5b, 0x02};
    static const uint8_t queue[] = {0x4c, 0x8b, 0xbb, 0x08, 0x01, 0x00, 0x00};
    if ((uint64_t)function + HYDELAND_PROMPT_CHECKS_SPAN > size) return 0;
    const uint8_t *lea = image + function + HYDELAND_PROMPT_MASK_LEA;
    return lea[0] == 0x48 && lea[1] == 0x8d && lea[2] == 0x05 &&
           contains(image, function, HYDELAND_PROMPT_CHECKS_SPAN, mode, sizeof(mode)) &&
           contains(image, function, HYDELAND_PROMPT_CHECKS_SPAN, mask, sizeof(mask)) &&
           contains(image, function, HYDELAND_PROMPT_CHECKS_SPAN, scene, sizeof(scene)) &&
           contains(image, function, HYDELAND_PROMPT_CHECKS_SPAN, queue, sizeof(queue));
}

uint64_t hydeland_town_mask_table(const uint8_t *image, uint32_t prompt_function) {
    const uint8_t *lea = image + prompt_function + HYDELAND_PROMPT_MASK_LEA;
    int32_t displacement;
    memcpy(&displacement, lea + 3, sizeof(displacement));
    return (uint64_t)(uintptr_t)(lea + 7 + displacement);
}

void hydeland_town_observe(HydelandTownSlot slot, HydelandTownObserver observer) {
    if (slot < HYDELAND_TOWN_OBSERVERS) atomic_store(&observers[slot], observer);
}

void GUEST_HOOK_ABI hydeland_town_hook(GuestRegs *regs) {
    for (int slot = 0; slot < HYDELAND_TOWN_OBSERVERS; ++slot) {
        const HydelandTownObserver observer = atomic_load(&observers[slot]);
        if (observer) observer(regs);
    }
}

int hydeland_town_hook_at(const uint8_t *image, uint64_t size, uint32_t call, GuestHook *hook) {
    if ((uint64_t)call + 5 > size || image[call] != 0xe8) return 0;
    *hook = (GuestHook){"hydeland update", call, 5, {0}, hydeland_town_hook};
    memcpy(hook->original, image + call, 5);
    return 1;
}

int hydeland_town_installed(void) {
    return atomic_load(&installed);
}

void hydeland_town_set_installed(int value) {
    atomic_store(&installed, value != 0);
}
