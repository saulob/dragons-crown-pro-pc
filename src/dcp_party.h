// Party snapshots for the lobby's optional party.report (docs/ONLINE.md, Party statistics): which classes the human
// members of an online room play. The hook only observes player units; nothing here writes to the game.
#ifndef DCP_PARTY_H
#define DCP_PARTY_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "dcp_cheats.h"

namespace dcp::party {

inline constexpr int kSlots = 4;
inline constexpr uint32_t kClassCount = 6; // 0 Fighter, 1 Amazon, 2 Elf, 3 Dwarf, 4 Sorceress, 5 Wizard
inline constexpr size_t kMinReported = 2;  // a room of one is the idle state of every online player
inline constexpr int64_t kFreshMs = 1500;
inline constexpr int64_t kPollMs = 500;
inline constexpr int64_t kStableMs = 3000;
inline constexpr int64_t kMinGapMs = 60000;

constexpr bool ValidClass(uint32_t class_index) {
    return class_index < kClassCount;
}

constexpr bool ValidSlot(int32_t slot) {
    return slot >= 0 && slot < kSlots;
}

// The game's own conversion of a Matching2 member id to its player slot (-1: not a member id).
constexpr int MemberSlot(uint32_t member_id) {
    const uint32_t index = (member_id & 0xffff) >> 4;
    return index == 0 || (member_id & 0xffff) >= 0x50 ? -1 : static_cast<int>(index) - 1;
}

// Only the owner of a room, signed in to the lobby over a usable link, with Online Play on, reports its party.
constexpr bool CanReport(bool online_enabled, bool link_ready, uint64_t room, uint16_t me, uint16_t owner) {
    return online_enabled && link_ready && room != 0 && me != 0 && me == owner;
}

inline int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The class of each party slot's player unit as the unit's own tick last showed it; written by the game thread's hook,
// read by the reporter.
class UnitTable {
public:
    // A cell is [now_ms:48][level:8][class + 1:8]; level is the game's own, 0-based.
    void Seen(int32_t slot, uint32_t class_index, int64_t now_ms, uint8_t level = 0) {
        if (ValidSlot(slot) && ValidClass(class_index)) {
            cells_[static_cast<size_t>(slot)].store(
                (static_cast<uint64_t>(now_ms) << 16) | (uint64_t{level} << 8) | (class_index + 1),
                std::memory_order_relaxed);
        }
    }

    std::optional<uint8_t> ClassAt(int slot, int64_t now_ms) const {
        if (!ValidSlot(slot)) {
            return std::nullopt;
        }
        const std::optional<uint64_t> cell = Fresh(slot, now_ms);
        return cell ? std::optional<uint8_t>(static_cast<uint8_t>((*cell & 0xff) - 1)) : std::nullopt;
    }

    // The level as the game shows it (1-based).
    std::optional<int> LevelAt(int slot, int64_t now_ms) const {
        const std::optional<uint64_t> cell = Fresh(slot, now_ms);
        return cell ? std::optional<int>(static_cast<int>((*cell >> 8) & 0xff) + 1) : std::nullopt;
    }

    void Clear() {
        for (std::atomic<uint64_t>& cell : cells_) {
            cell.store(0, std::memory_order_relaxed);
        }
    }

private:
    std::optional<uint64_t> Fresh(int slot, int64_t now_ms) const {
        if (!ValidSlot(slot)) {
            return std::nullopt;
        }
        const uint64_t cell = cells_[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
        if (cell == 0 || now_ms - static_cast<int64_t>(cell >> 16) > kFreshMs) {
            return std::nullopt;
        }
        return cell;
    }

    std::array<std::atomic<uint64_t>, kSlots> cells_{};
};

inline UnitTable& SharedUnits() {
    static UnitTable table;
    return table;
}

// `memory` reads the game's memory: U64(address), U32(address) and U8(address). Player units of the human category only.
// Fed by the party hook online and by the cheats' player tick offline (they patch the same entry).
template <typename Memory>
void Observe(const Memory& memory, uint64_t unit, int64_t now_ms, UnitTable& table) {
    if (unit == 0) {
        return;
    }
    const uint64_t owner = memory.U64(unit + cheats::kUnitOwnerOffset);
    if (owner == 0 || (memory.U32(owner + cheats::kOwnerCategoryOffset) & cheats::kHumanPlayerCategory) == 0) {
        return;
    }
    table.Seen(static_cast<int32_t>(memory.U32(unit + cheats::kUnitPlayerSlotOffset)),
               memory.U8(unit + cheats::kUnitClassOffset), now_ms, memory.U8(unit + cheats::kUnitLevelOffset));
}

struct Snapshot {
    std::array<uint8_t, kSlots> classes{}; // ascending
    uint8_t size = 0;

    bool operator==(const Snapshot&) const = default;
};

// The classes of the room's members, or nothing unless every member has a live unit in the slot the game gives its
// member id. `member_ids` are the room's Matching2 member ids.
inline std::optional<Snapshot> BuildSnapshot(const std::vector<uint16_t>& member_ids, const UnitTable& table,
                                             int64_t now_ms) {
    if (member_ids.size() < kMinReported || member_ids.size() > kSlots) {
        return std::nullopt;
    }
    std::array<bool, kSlots> taken{};
    Snapshot snapshot;
    for (const uint16_t id : member_ids) {
        const int slot = MemberSlot(id);
        if (!ValidSlot(slot) || taken[static_cast<size_t>(slot)]) {
            return std::nullopt;
        }
        taken[static_cast<size_t>(slot)] = true;
        const std::optional<uint8_t> class_index = table.ClassAt(slot, now_ms);
        if (!class_index) {
            return std::nullopt;
        }
        snapshot.classes[snapshot.size++] = *class_index;
    }
    std::sort(snapshot.classes.begin(), snapshot.classes.begin() + snapshot.size);
    return snapshot;
}

// Decides when a snapshot is worth sending: the same party for kStableMs, not already sent for this room, and not
// sooner than kMinGapMs after the last one. Volatile; nothing identifies a player.
class Reporter {
public:
    bool Due(int64_t now_ms) {
        if (polled_ && now_ms - last_poll_ms_ < kPollMs) {
            return false;
        }
        polled_ = true;
        last_poll_ms_ = now_ms;
        return true;
    }

    void Idle() {
        candidate_.reset();
    }

    std::optional<Snapshot> Poll(uint64_t room, const std::optional<Snapshot>& current, int64_t now_ms) {
        if (!current) {
            candidate_.reset();
            return std::nullopt;
        }
        if (!candidate_ || *candidate_ != *current) {
            candidate_ = current;
            since_ms_ = now_ms;
            return std::nullopt;
        }
        if (now_ms - since_ms_ < kStableMs) {
            return std::nullopt;
        }
        if (sent_ && sent_room_ == room && *sent_ == *current) {
            return std::nullopt;
        }
        if (sent_ && now_ms - sent_ms_ < kMinGapMs) {
            return std::nullopt;
        }
        return current;
    }

    void Sent(uint64_t room, const Snapshot& snapshot, int64_t now_ms) {
        sent_ = snapshot;
        sent_room_ = room;
        sent_ms_ = now_ms;
    }

private:
    bool polled_ = false;
    int64_t last_poll_ms_ = 0;
    std::optional<Snapshot> candidate_;
    int64_t since_ms_ = 0;
    std::optional<Snapshot> sent_;
    uint64_t sent_room_ = 0;
    int64_t sent_ms_ = 0;
};

} // namespace dcp::party

#endif
