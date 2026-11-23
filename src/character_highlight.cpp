#include "character_highlight.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "character_highlight_core.h"
#include "guest_hook.h"

namespace {

using dcp::highlight::AtlasEntry;
using dcp::highlight::kNoKey;

constexpr int kPlayers = CHARACTER_HIGHLIGHT_PLAYERS;
constexpr uint32_t kUnitSlotOffset = 0xa70;
constexpr uint32_t kUnitContextVirtual = 0xd0;
constexpr uint32_t kGamePartyOffset = 0x80;
constexpr uint32_t kSeatBase = 0xa8;
constexpr uint32_t kSeatSize = 0x23b0;
constexpr uint32_t kSeatOwner = 0x21e0;
constexpr uint32_t kSeatFlags = 0x21e4;
constexpr uint32_t kSeatPad = 0x21dc;
constexpr uint32_t kRecordClass = 0x27;
constexpr uint32_t kRecordColor = 0x28;
constexpr int32_t kOwnerLocal = 0;
constexpr int32_t kOwnerRemote = 1;
constexpr uint32_t kSeatNotControlled = 1;
constexpr unsigned kSelectionRefreshTicks = 60;
constexpr int64_t kNotMatchedAfterMs = 20000;
constexpr size_t kMatchSlots = 32;
constexpr uint32_t kTextureShift = 24;

struct MatchSlot {
    std::atomic<uint32_t> image_plus_one{0};
    std::atomic<uint32_t> atlas{0};
    std::atomic<uint64_t> address{0};
};

using KeyArray = std::array<std::atomic<int>, kPlayers>;

std::atomic<unsigned> g_enabled_mask{0};
KeyArray g_local_key{{kNoKey, kNoKey, kNoKey, kNoKey}};
KeyArray g_remote_key{{kNoKey, kNoKey, kNoKey, kNoKey}};
std::array<std::atomic<int>, kPlayers> g_label{{0, 0, 0, 0}};
std::array<std::atomic<int>, kPlayers> g_pad{{-1, -1, -1, -1}};
std::array<MatchSlot, kMatchSlots> g_matches;
std::vector<AtlasEntry> g_table;
std::atomic<bool> g_table_ready{false};
std::mutex g_mutex;
CharacterHighlightLook g_look = nullptr;
void (*g_forward)(uint64_t) = nullptr;
std::array<unsigned, kPlayers> g_ticks{};
std::array<int, kPlayers> g_reported{{kNoKey, kNoKey, kNoKey, kNoKey}};
std::array<int, kPlayers> g_not_matched{{kNoKey, kNoKey, kNoKey, kNoKey}};
std::array<int64_t, kPlayers> g_pending_since_ms{};
unsigned g_ambiguity_logged = 0;
unsigned g_hashes = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int Label(int klass, int color) {
    return klass | (color << 8);
}

uint32_t PackAtlas(const AtlasEntry& entry) {
    return entry.key() | (static_cast<uint32_t>(entry.texture) << kTextureShift);
}

template <typename T>
T Read(uint64_t address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), sizeof(value));
    return value;
}

bool Plausible(uint64_t address) {
    return address >= 0x10000 && address < 0x800000000000ull && (address & 7) == 0;
}

using GameOfUnit = uint64_t(GUEST_HOOK_ABI*)(uint64_t);

uint64_t SeatOf(uint64_t unit, int slot) {
    const uint64_t function = Read<uint64_t>(Read<uint64_t>(unit) + kUnitContextVirtual);
    if (!Plausible(function)) return 0;
    const uint64_t game = reinterpret_cast<GameOfUnit>(static_cast<uintptr_t>(function))(unit);
    if (!Plausible(game)) return 0;
    const uint64_t party = Read<uint64_t>(game + kGamePartyOffset);
    return Plausible(party) ? party + kSeatBase + static_cast<uint64_t>(slot) * kSeatSize : 0;
}

bool ControlledHere(uint64_t seat, int slot) {
    return Read<int32_t>(seat + kSeatOwner) == kOwnerLocal && (slot == 0 || (Read<uint32_t>(seat + kSeatFlags) & kSeatNotControlled) == 0);
}

const char* ClassLabel(int label) {
    const char* name = dcp::highlight::ClassName(label & 0xff);
    return name ? name : "unknown";
}

int LocalRank(int slot) {
    int rank = 0;
    for (int other = 0; other < slot; ++other) rank += g_local_key[other].load() != kNoKey;
    return rank;
}

bool KeyMatched(int key) {
    for (const MatchSlot& slot : g_matches) {
        if (slot.image_plus_one.load(std::memory_order_relaxed) &&
            static_cast<int>(slot.atlas.load(std::memory_order_relaxed) & dcp::highlight::kKeyMaskValue) == key) {
            return true;
        }
    }
    return false;
}

void EvaluateLocked() {
    if (!g_table_ready.load()) return;
    for (int slot = 0; slot < kPlayers; ++slot) {
        const int key = g_local_key[slot].load();
        if (!(g_enabled_mask.load() & (1u << slot)) || key == kNoKey || g_reported[slot] == key) continue;
        if (KeyMatched(key)) {
            g_reported[slot] = key;
            const int label = g_label[slot].load();
            std::printf("Character Highlight: local atlas matched slot=%d class=%s color=%d\n", slot, ClassLabel(label), label >> 8);
        }
    }
}

void RestartPendingLocked(int slot) {
    g_pending_since_ms[slot] = NowMs();
    g_reported[slot] = kNoKey;
    g_not_matched[slot] = kNoKey;
}

void SetLocal(int slot, int key, int label) {
    std::scoped_lock lock{g_mutex};
    g_label[slot] = label;
    if (g_local_key[slot].exchange(key) == key) return;
    if (key != kNoKey) {
        std::printf("Character Highlight: local player=%d slot=%d class=%s color=%d\n", LocalRank(slot), slot, ClassLabel(label), label >> 8);
    } else {
        g_enabled_mask.fetch_and(~(1u << slot));
    }
    RestartPendingLocked(slot);
    EvaluateLocked();
}

void RefreshSlot(uint64_t unit, int slot) {
    const uint64_t seat = SeatOf(unit, slot);
    if (!seat) return;
    const int32_t owner = Read<int32_t>(seat + kSeatOwner);
    const int klass = Read<uint8_t>(seat + kRecordClass);
    const int color = Read<uint8_t>(seat + kRecordColor);
    if (klass >= dcp::highlight::kClassCount || color > dcp::highlight::kMaxColor) return;
    const bool local = ControlledHere(seat, slot);
    const bool remote = owner == kOwnerRemote;
    int key = kNoKey;
    if (local || remote) {
        const uint32_t look = g_look ? g_look(slot, seat, klass, color, owner) : CHARACTER_HIGHLIGHT_NATIVE_LOOK;
        key = static_cast<int>(look == CHARACTER_HIGHLIGHT_NATIVE_LOOK ? dcp::highlight::BaseKey(klass, color) : look);
    }
    g_remote_key[slot] = remote ? key : kNoKey;
    g_pad[slot] = local ? Read<int32_t>(seat + kSeatPad) : -1;
    SetLocal(slot, local ? key : kNoKey, Label(klass, color));
}

void ReportMissingMatch(int slot) {
    std::scoped_lock lock{g_mutex};
    const int key = g_local_key[slot].load();
    if (key == kNoKey || g_reported[slot] == key || g_not_matched[slot] == key) return;
    if (NowMs() - g_pending_since_ms[slot] < kNotMatchedAfterMs) return;
    g_not_matched[slot] = key;
    const int label = g_label[slot].load();
    std::printf("Character Highlight: local atlas not matched slot=%d class=%s color=%d\n", slot, ClassLabel(label), label >> 8);
}

void LogAmbiguity(unsigned slots) {
    std::scoped_lock lock{g_mutex};
    if (g_ambiguity_logged & (1u << slots)) return;
    g_ambiguity_logged |= 1u << slots;
    std::string list;
    for (int slot = 0; slot < kPlayers; ++slot) {
        if (slots & (1u << slot)) list += (list.empty() ? "" : ",") + std::to_string(slot);
    }
    std::printf("Character Highlight: shared atlas ambiguous slots=%s\n", list.c_str());
}

void AddEntries(const std::vector<AtlasEntry>& entries) {
    std::scoped_lock lock{g_mutex};
    g_table.insert(g_table.end(), entries.begin(), entries.end());
    g_table_ready = !g_table.empty();
}

} // namespace

extern "C" void character_highlight_set_look_resolver(CharacterHighlightLook look) {
    g_look = look;
}

extern "C" uint32_t character_highlight_custom_key(const char* entry) {
    return dcp::highlight::CustomKey(entry ? entry : "");
}

extern "C" int character_highlight_add_custom_atlas(const char* entry, int klass, int color, const uint8_t* ftx, size_t size) {
    auto entries = dcp::highlight::EntriesFor(klass, color, ftx, size);
    if (!entries || !entry) return 0;
    const uint32_t key = dcp::highlight::CustomKey(entry);
    for (AtlasEntry& texture : *entries) texture.custom = key;
    AddEntries(*entries);
    return static_cast<int>(entries->size());
}

extern "C" int character_highlight_local_slot(int local_index) {
    int rank = 0;
    for (int slot = 0; slot < kPlayers; ++slot) {
        if (g_local_key[slot].load() == kNoKey) continue;
        if (rank++ == local_index) return slot;
    }
    return -1;
}

static int FlipSlot(int slot) {
    const unsigned bit = 1u << slot;
    const bool on = (g_enabled_mask.fetch_xor(bit) & bit) == 0;
    std::printf("Character Highlight: local player=%d slot=%d %s\n", LocalRank(slot), slot, on ? "on" : "off");
    if (on) {
        std::scoped_lock lock{g_mutex};
        RestartPendingLocked(slot);
        EvaluateLocked();
    }
    return slot;
}

extern "C" int character_highlight_toggle_local(int local_index) {
    const int slot = local_index < 0 ? -1 : character_highlight_local_slot(local_index);
    return slot < 0 ? -1 : FlipSlot(slot);
}

extern "C" int character_highlight_toggle_pad(int pad) {
    if (pad < 0) return -1;
    for (int slot = 0; slot < kPlayers; ++slot) {
        if (g_local_key[slot].load() != kNoKey && g_pad[slot].load() == pad) return FlipSlot(slot);
    }
    return -1;
}

extern "C" int character_highlight_player_enabled(int slot) {
    return slot >= 0 && slot < kPlayers && (g_enabled_mask.load() & (1u << slot)) ? 1 : 0;
}

extern "C" int character_highlight_load_table(const char* path) {
    std::ifstream in(path ? path : "", std::ios::binary);
    std::stringstream text;
    if (!in || !(text << in.rdbuf())) {
        std::printf("Character Highlight: atlas signature table unavailable\n");
        return -1;
    }
    auto entries = dcp::highlight::ParseTable(text.str());
    if (!entries || entries->empty()) {
        std::printf("Character Highlight: atlas signature table unreadable\n");
        return -1;
    }
    const size_t count = entries->size();
    AddEntries(*entries);
    std::printf("Character Highlight: atlas signature table ready entries=%zu\n", count);
    return static_cast<int>(count);
}

extern "C" void character_highlight_set_forward(void (*forward)(uint64_t unit)) {
    g_forward = forward;
}

extern "C" void character_highlight_observe_local_unit(uint64_t unit) {
    if (!Plausible(unit)) return;
    const int slot = Read<int32_t>(unit + kUnitSlotOffset);
    if (slot < 0 || slot >= kPlayers) return;
    const unsigned tick = g_ticks[slot]++;
    if (tick % kSelectionRefreshTicks == 0) RefreshSlot(unit, slot);
    if ((g_enabled_mask.load() & (1u << slot)) && g_table_ready.load() && tick % kSelectionRefreshTicks == 1) {
        ReportMissingMatch(slot);
    }
}

extern "C" void character_highlight_observe_unit(uint64_t unit) {
    if (g_forward) g_forward(unit);
    character_highlight_observe_local_unit(unit);
}

extern "C" void character_highlight_image_registered(uint32_t image, uint64_t guest_address, uint64_t guest_size, uint32_t width,
                                                     uint32_t height) {
    if (!g_table_ready.load() || guest_size < dcp::highlight::kMinAtlasBytes || guest_address == 0) return;
    std::scoped_lock lock{g_mutex};
    uint64_t signature = 0;
    uint32_t signed_size = 0;
    for (const AtlasEntry& entry : g_table) {
        if (entry.texels.width != width || entry.texels.height != height || entry.texels.data_size > guest_size) continue;
        if (signed_size != entry.texels.data_size) {
            signed_size = entry.texels.data_size;
            ++g_hashes;
            signature = dcp::highlight::SampleSignature(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(guest_address)),
                                                        signed_size);
        }
        if (entry.signature != signature) continue;
        for (MatchSlot& slot : g_matches) {
            if (slot.image_plus_one.load() == 0) {
                slot.atlas.store(PackAtlas(entry));
                slot.address.store(guest_address);
                slot.image_plus_one.store(image + 1);
                break;
            }
        }
    }
    EvaluateLocked();
}

extern "C" unsigned character_highlight_hash_count(void) {
    std::scoped_lock lock{g_mutex};
    return g_hashes;
}

extern "C" void character_highlight_image_unregistered(uint32_t image) {
    for (MatchSlot& slot : g_matches) {
        if (slot.image_plus_one.load() == image + 1) {
            slot.image_plus_one.store(0);
            slot.address.store(0);
        }
    }
}

namespace {

int SlotForKey(int key, unsigned enabled) {
    if (key == kNoKey) return -1;
    unsigned local = 0;
    bool remote = false;
    for (int slot = 0; slot < kPlayers; ++slot) {
        if (g_local_key[slot].load(std::memory_order_relaxed) == key) local |= 1u << slot;
        remote |= g_remote_key[slot].load(std::memory_order_relaxed) == key;
    }
    if (local == 0) return -1;
    if (remote || (local & (local - 1))) {
        if (local & enabled) LogAmbiguity(local);
        return -1;
    }
    return (local & enabled) ? __builtin_ctz(local) : -1;
}

int KeyForImage(uint32_t image) {
    for (const MatchSlot& slot : g_matches) {
        if (slot.image_plus_one.load(std::memory_order_relaxed) == image + 1) {
            return static_cast<int>(slot.atlas.load(std::memory_order_relaxed) & dcp::highlight::kKeyMaskValue);
        }
    }
    return kNoKey;
}

} // namespace

extern "C" int character_highlight_slot_for_image(uint32_t image) {
    const unsigned enabled = g_enabled_mask.load(std::memory_order_relaxed);
    if (enabled == 0) return -1;
    return SlotForKey(KeyForImage(image), enabled);
}

extern "C" int character_highlight_slot_for_address(uint64_t guest_address) {
    const unsigned enabled = g_enabled_mask.load(std::memory_order_relaxed);
    if (enabled == 0 || guest_address == 0) return -1;
    for (const MatchSlot& slot : g_matches) {
        if (slot.address.load(std::memory_order_relaxed) == guest_address) {
            return SlotForKey(
                static_cast<int>(slot.atlas.load(std::memory_order_relaxed) & dcp::highlight::kKeyMaskValue),
                enabled);
        }
    }
    return -1;
}
