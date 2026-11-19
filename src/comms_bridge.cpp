#include "comms_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "comms_text.h"
#include "dcp_comms.h"

namespace {

constexpr uint32_t kUnitOwner = 0x1e0;
constexpr uint32_t kUnitSlot = 0xa70;
constexpr uint32_t kOwnerCategory = 0x2a0c;
constexpr uint32_t kHumanCategory = 1;
constexpr int64_t kRunningMs = 500;
constexpr int64_t kShowWindowMs = 4000;
constexpr int64_t kRetryMs = 250;
constexpr size_t kTextUnits = 32;

// The greeting balloon function and the call in it that copies the message text into the balloon string.
constexpr int16_t kShowPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                     0xec, 0xd8, 0x02, 0x00, 0x00, 0x4c, 0x8b, 0x25, -1,   -1,   -1,   -1,   0x49, 0x89,
                                     0xfd, 0x49, 0x8b, 0x04, 0x24};
constexpr uint8_t kTextSite[] = {0x4c, 0x0f, 0x47, 0x75, 0x88, 0x4c, 0x89, 0xf6, 0xe8};
constexpr uint64_t kSiteWindow = 0x600;
constexpr uint8_t kCall = 0xe8;

struct State {
    CommsBridgeShowFn show = nullptr;
    uint64_t (*clock)() = nullptr;
    std::atomic<bool> installed{false};
    std::atomic<uint64_t> pending[dcp::comms::kSlots]{};
    std::atomic<int64_t> seen_ms[dcp::comms::kSlots]{};
    int64_t retry_ms[dcp::comms::kSlots]{};
    uint64_t showing_owner = 0;
    bool consumed = false;
    uint16_t text[kTextUnits]{};
};

State g;

int64_t NowMs() {
    if (g.clock) {
        return static_cast<int64_t>(g.clock());
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool MatchesAt(const uint8_t* image, const int16_t* pattern, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (pattern[i] >= 0 && image[i] != static_cast<uint8_t>(pattern[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" {

int comms_bridge_locate(const uint8_t* image, uint64_t size, CommsBridgeSites* sites) {
    constexpr size_t kLength = sizeof(kShowPrologue) / sizeof(kShowPrologue[0]);
    if (!image || !sites || size < kSiteWindow + kLength) {
        return 0;
    }
    uint64_t show = 0;
    size_t found = 0;
    for (uint64_t at = 0; at + kSiteWindow < size; ++at) {
        if (image[at] == 0x55 && MatchesAt(image + at, kShowPrologue, kLength)) {
            show = at;
            ++found;
        }
    }
    if (found != 1) {
        return 0;
    }
    uint64_t call = 0;
    found = 0;
    for (uint64_t at = show; at + sizeof(kTextSite) + 4 <= show + kSiteWindow; ++at) {
        if (std::memcmp(image + at, kTextSite, sizeof(kTextSite)) == 0) {
            call = at + sizeof(kTextSite) - 1;
            ++found;
        }
    }
    if (found != 1) {
        return 0;
    }
    size_t callers = 0;
    for (uint64_t at = 0; at + 5 <= size; ++at) {
        if (image[at] != kCall) {
            continue;
        }
        int32_t rel;
        std::memcpy(&rel, image + at + 1, sizeof(rel));
        if (static_cast<int64_t>(at) + 5 + rel == static_cast<int64_t>(show)) {
            ++callers;
        }
    }
    if (callers != 1) {
        return 0;
    }
    sites->show = static_cast<uint32_t>(show);
    sites->text_call = static_cast<uint32_t>(call);
    return 1;
}

size_t comms_bridge_stub_bytes(void) {
    return guest_hook_stub_size();
}

void GUEST_HOOK_ABI comms_bridge_text_hook(GuestRegs* regs) {
    if (g.showing_owner == 0 || regs->r13 != g.showing_owner) {
        return;
    }
    regs->rsi = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g.text));
    g.consumed = true;
}

void comms_bridge_attach(CommsBridgeShowFn show, uint64_t (*clock)(void)) {
    g.show = show;
    g.clock = clock;
    g.installed.store(show != nullptr);
}

int comms_bridge_install(uint8_t* image, uint64_t size, uint64_t entry, const CommsBridgeSites* sites, uint8_t* stubs,
                         size_t stubs_size) {
    if (!image || !sites || sites->show >= size || sites->text_call <= sites->show ||
        sites->text_call - sites->show >= kSiteWindow || sites->text_call + 5ull > size || image[sites->text_call] != kCall) {
        return 0;
    }
    GuestHook hook{"comms text", sites->text_call, 5, {}, comms_bridge_text_hook};
    std::memcpy(hook.original, image + sites->text_call, 5);
    const GuestImageId id{size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        std::printf("Comms Wheel: not installed, %s\n", guest_hook_status_text(status));
        return 0;
    }
    comms_bridge_attach(reinterpret_cast<CommsBridgeShowFn>(reinterpret_cast<uintptr_t>(image) + sites->show), nullptr);
    std::printf("Comms Wheel: native greeting balloon at 0x%x, text hook at 0x%x\n", sites->show, sites->text_call);
    return 1;
}

int comms_bridge_active(void) {
    return g.installed.load() ? 1 : 0;
}

void comms_bridge_show(int slot, int id) {
    if (!g.installed.load() || slot < 0 || slot >= dcp::comms::kSlots || !dcp::comms::ValidId(id)) {
        return;
    }
    g.pending[slot].store((static_cast<uint64_t>(NowMs()) << 8) | static_cast<uint64_t>(id + 1));
}

int comms_bridge_unit_running(int slot) {
    if (!g.installed.load() || slot < 0 || slot >= dcp::comms::kSlots) {
        return 0;
    }
    const int64_t seen = g.seen_ms[slot].load();
    return seen != 0 && NowMs() - seen <= kRunningMs ? 1 : 0;
}

void comms_bridge_unit_tick(uint64_t unit) {
    if (!g.installed.load() || unit == 0) {
        return;
    }
    uint64_t owner;
    std::memcpy(&owner, reinterpret_cast<const void*>(static_cast<uintptr_t>(unit + kUnitOwner)), sizeof(owner));
    if (owner == 0) {
        return;
    }
    uint32_t category;
    std::memcpy(&category, reinterpret_cast<const void*>(static_cast<uintptr_t>(owner + kOwnerCategory)), sizeof(category));
    int32_t slot;
    std::memcpy(&slot, reinterpret_cast<const void*>(static_cast<uintptr_t>(unit + kUnitSlot)), sizeof(slot));
    if ((category & kHumanCategory) == 0 || slot < 0 || slot >= dcp::comms::kSlots) {
        return;
    }
    const int64_t now = NowMs();
    g.seen_ms[slot].store(now);
    const uint64_t queued = g.pending[slot].load();
    if (queued == 0 || now < g.retry_ms[slot]) {
        return;
    }
    const int id = static_cast<int>(queued & 0xff) - 1;
    uint64_t expected = queued;
    if (now - static_cast<int64_t>(queued >> 8) > kShowWindowMs || !dcp::comms::ValidId(id)) {
        g.pending[slot].compare_exchange_strong(expected, 0);
        std::printf("Comms Wheel: %s for slot %d was not shown, the game's balloon refused it\n", dcp::comms::IdKey(id), slot);
        return;
    }
    if (comms_text_game_utf16(id, g.text, kTextUnits) == 0) {
        g.pending[slot].store(0);
        return;
    }
    g.consumed = false;
    g.showing_owner = owner;
    g.show(owner);
    g.showing_owner = 0;
    if (g.consumed) {
        g.pending[slot].compare_exchange_strong(expected, 0);
        std::printf("Comms Wheel: %s shown on slot %d\n", dcp::comms::IdKey(id), slot);
    } else {
        g.retry_ms[slot] = now + kRetryMs;
    }
}

} // extern "C"
