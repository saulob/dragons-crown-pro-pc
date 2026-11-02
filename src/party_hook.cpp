#include "party_hook.h"

#include <cstdio>
#include <cstring>

#include "dcp_party.h"
#include "guest_hook.h"

namespace {

struct HostMemory {
    template <typename T>
    static T Load(uint64_t address) {
        T value;
        std::memcpy(&value, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof(value));
        return value;
    }
    uint64_t U64(uint64_t address) const { return Load<uint64_t>(address); }
    uint32_t U32(uint64_t address) const { return Load<uint32_t>(address); }
    uint8_t U8(uint64_t address) const { return Load<uint8_t>(address); }
};

void GUEST_HOOK_ABI PartyTickHook(GuestRegs* regs) {
    dcp::party::Observe(HostMemory{}, regs->rdi, dcp::party::NowMs(), dcp::party::SharedUnits());
}

// The entry of the player unit's per-tick update: the same site and prologue the cheats hook.
constexpr uint8_t kTickPrologue[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};

bool LayoutMatches(const DcpPartySites& sites, const char** field) {
    using namespace dcp::cheats;
    const struct {
        const char* name;
        uint32_t located, expected;
    } fields[] = {{"unit owner", sites.unit_owner, kUnitOwnerOffset},
                  {"unit player slot", sites.unit_slot, kUnitPlayerSlotOffset},
                  {"unit class", sites.unit_class, kUnitClassOffset},
                  {"owner category", sites.owner_category, kOwnerCategoryOffset}};
    for (const auto& f : fields) {
        if (f.located != f.expected) {
            *field = f.name;
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" size_t party_hook_stub_bytes(void) {
    return guest_hook_stub_size();
}

extern "C" int party_hook_install_located(uint8_t* image, uint64_t size, uint64_t entry, const DcpPartySites* sites,
                                          uint8_t* stubs, size_t stubs_size) {
    const char* field = "layout";
    if (!sites || !LayoutMatches(*sites, &field)) {
        std::printf("Party: not reported, the game's %s displacement differs from the handler's\n", field);
        return 0;
    }
    GuestHook hook{"party tick", sites->tick, sizeof(kTickPrologue), {}, PartyTickHook};
    std::memcpy(hook.original, kTickPrologue, sizeof(kTickPrologue));
    const GuestImageId id{size, entry};
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, &id, &hook, 1, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        std::printf("Party: not reported, hook not installed: %s\n", guest_hook_status_text(status));
        return 0;
    }
    std::printf("Party: observer hook installed at 0x%x\n", sites->tick);
    return 1;
}
