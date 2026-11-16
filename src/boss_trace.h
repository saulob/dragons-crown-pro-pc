// A state-change trace of the Chimera fight in the Labyrinth of Chaos and the Tower of Mirages, written to the runtime
// log with the cheat hooks' own observations. It only reads; every value it prints is the game's.
#pragma once

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dcp::bosstrace {

inline constexpr uint32_t kOwnerNameOffset = 0xb8;     // the class name every unit's owner object carries
inline constexpr uint32_t kGameRoundOffset = 0x88;     // game object -> the round state
inline constexpr uint32_t kRoundFloorOffset = 0xa8;    // zero-based floor
inline constexpr uint32_t kRoundRoomOffset = 0xb4;     // rooms entered on this floor
inline constexpr uint32_t kRoundIndexOffset = 0x108;   // the round table's slot
inline constexpr uint32_t kGameSettingsOffset = 0x120; // game object -> the difficulty record
inline constexpr uint32_t kSettingsUltimateOffset = 0x89;
inline constexpr uint32_t kUnitHpOffset = 0xb0;
inline constexpr uint32_t kUnitOwnerOffset = 0x1e0;
inline constexpr int32_t kRandomDungeonRound = 0x11;
inline constexpr int32_t kTowerBossFloor = 63; // floor 64 as the player counts it
inline constexpr size_t kMaxParts = 4;
inline constexpr int64_t kSampleIntervalMs = 250;
inline constexpr int64_t kLethalSettleMs = 1000;
inline constexpr unsigned kMaxLines = 160;

enum class Part : uint8_t { None, Chimera, Body, Goat, Snake };

constexpr const char* PartName(Part part) {
    switch (part) {
    case Part::Chimera: return "Chimera";
    case Part::Body: return "ChimeraBody";
    case Part::Goat: return "ChimeraGoat";
    case Part::Snake: return "ChimeraSnake";
    default: return "?";
    }
}

struct Context {
    bool valid = false;
    int32_t round = -1;
    int32_t floor = -1; // zero-based
    int32_t room = -1;
    bool ultimate = false;
};

template <typename Memory>
Context ReadContext(const Memory& memory, uint64_t game) {
    Context context;
    if (game == 0) {
        return context;
    }
    const uint64_t round = memory.U64(game + kGameRoundOffset);
    if (round == 0) {
        return context;
    }
    const uint64_t settings = memory.U64(game + kGameSettingsOffset);
    context.valid = true;
    context.round = memory.S32(round + kRoundIndexOffset);
    context.floor = memory.S32(round + kRoundFloorOffset);
    context.room = memory.S32(round + kRoundRoomOffset);
    context.ultimate = settings != 0 && memory.U8(settings + kSettingsUltimateOffset) != 0;
    return context;
}

class ChimeraTrace {
public:
    using Sink = void (*)(const char* line);

    // The game image bounds a class-name pointer must lie in before the name is read.
    void Configure(uint64_t image_begin, uint64_t image_end, Sink sink) {
        image_begin_ = image_begin;
        image_end_ = image_end;
        sink_ = sink;
        Reset();
    }

    void Reset() {
        parts_ = {};
        engaged_ = false;
        on_floor_ = false;
        all_down_ = false;
        room_ = -1;
        floor_ = -1;
        lines_ = 0;
        last_sample_ms_ = INT64_MIN;
        multiplier_ = 1;
    }

    template <typename Memory>
    Part Identify(const Memory& memory, uint64_t owner) const {
        if (owner == 0 || image_end_ == 0) {
            return Part::None;
        }
        const uint64_t name = memory.U64(owner + kOwnerNameOffset);
        if (name < image_begin_ || name > image_end_ || image_end_ - name < 16) {
            return Part::None;
        }
        char text[16];
        const uint64_t head = memory.U64(name), tail = memory.U64(name + 8);
        std::memcpy(text, &head, 8);
        std::memcpy(text + 8, &tail, 8);
        text[15] = '\0';
        if (std::memcmp(text, "DcChimera", 9) != 0) {
            return Part::None;
        }
        const char* suffix = text + 9;
        if (*suffix == '\0') return Part::Chimera;
        if (std::strcmp(suffix, "Body") == 0) return Part::Body;
        if (std::strcmp(suffix, "Goat") == 0) return Part::Goat;
        if (std::strcmp(suffix, "Snake") == 0) return Part::Snake;
        return Part::None;
    }

    bool Tracking() const { return engaged_ || on_floor_; }

    // A hit on a Chimera part, before the game lowers its HP. The hit check calls this too, with `lethal_check` set,
    // before the game decides whether the hit is lethal.
    template <typename Memory>
    void Observe(const Memory& memory, Part part, uint64_t owner, uint64_t unit, int32_t amount, int multiplier,
                 bool lethal_check, const Context& context, int64_t now) {
        if (part == Part::None || unit == 0 || !context.valid || context.round != kRandomDungeonRound) {
            return;
        }
        Tracked* tracked = Find(owner);
        const int32_t hp = memory.S32(unit + kUnitHpOffset);
        if (tracked == nullptr) {
            tracked = Add(memory, part, owner, unit, hp);
            if (tracked == nullptr) {
                return;
            }
            Engage(context, multiplier, now);
            Line(now, "%s first sight: hp=%d hit=%d damage multiplier=%dx", PartName(part), hp, amount, multiplier);
        }
        if (amount <= 0) {
            return;
        }
        if (!lethal_check) {
            ++tracked->hits;
            if (amount > tracked->biggest) {
                tracked->biggest = amount;
            }
        }
        if (lethal_check && hp > 0 && tracked->lethal_since_ms == 0) {
            const bool native = hp <= amount;
            const bool multiplied = static_cast<int64_t>(hp) <= static_cast<int64_t>(amount) * multiplier;
            if (multiplied) {
                tracked->lethal_since_ms = now == 0 ? 1 : now;
                Line(now, "%s lethal check: hp=%d hit=%d damage multiplier=%dx native lethal=%d", PartName(part), hp,
                     amount, multiplier, native ? 1 : 0);
            }
        }
    }

    bool SampleDue(int64_t now) {
        if (last_sample_ms_ != INT64_MIN && now - last_sample_ms_ < kSampleIntervalMs) {
            return false;
        }
        last_sample_ms_ = now;
        return true;
    }

    // From the player's tick, at most every kSampleIntervalMs.
    template <typename Memory>
    void Sample(const Memory& memory, const Context& context, int multiplier, int64_t now) {
        if (!context.valid) {
            return;
        }
        const bool in_dungeon = context.round == kRandomDungeonRound;
        const bool on_floor = in_dungeon && context.floor == kTowerBossFloor;
        if (on_floor && (!on_floor_ || context.room != room_)) {
            if (!on_floor_ && !engaged_) {
                start_ms_ = now;
            }
            Line(now, "Tower floor %d: room %d, ultimate=%d, damage multiplier=%dx", context.floor + 1, context.room,
                 context.ultimate ? 1 : 0, multiplier);
        }
        room_ = on_floor ? context.room : -1;
        if (!in_dungeon || (engaged_ && context.floor != floor_)) {
            End(context, now);
            on_floor_ = on_floor;
            return;
        }
        if (on_floor_ && !on_floor) {
            End(context, now);
        }
        on_floor_ = on_floor;
        if (!engaged_) {
            return;
        }
        if (multiplier != multiplier_) {
            Line(now, "damage multiplier %dx -> %dx", multiplier_, multiplier);
            multiplier_ = multiplier;
        }
        bool alive = false;
        for (Tracked& tracked : parts_) {
            if (tracked.owner != 0 && !tracked.removed) {
                alive = Poll(memory, tracked, now) || alive;
            }
        }
        if (!alive && !all_down_) {
            all_down_ = true;
            Line(now, "all tracked Chimera parts are down or gone");
        } else if (alive) {
            all_down_ = false;
        }
    }

private:
    struct Tracked {
        Part part = Part::None;
        uint64_t owner = 0;
        uint64_t unit = 0;
        uint64_t name = 0;
        int32_t max_hp = 1;
        int32_t last_hp = 0;
        int32_t bucket = 10;
        int32_t hits = 0;
        int32_t biggest = 0;
        int64_t lethal_since_ms = 0;
        bool defeated = false;
        bool removed = false;
    };

    Tracked* Find(uint64_t owner) {
        for (Tracked& tracked : parts_) {
            if (tracked.owner == owner && !tracked.removed) {
                return &tracked;
            }
        }
        return nullptr;
    }

    template <typename Memory>
    Tracked* Add(const Memory& memory, Part part, uint64_t owner, uint64_t unit, int32_t hp) {
        for (Tracked& tracked : parts_) {
            if (tracked.owner == 0 || tracked.removed) {
                tracked = {};
                tracked.part = part;
                tracked.owner = owner;
                tracked.unit = unit;
                tracked.name = memory.U64(owner + kOwnerNameOffset);
                tracked.max_hp = hp > 0 ? hp : 1;
                tracked.last_hp = hp;
                tracked.bucket = 10;
                tracked.defeated = hp <= 0;
                return &tracked;
            }
        }
        return nullptr;
    }

    void Engage(const Context& context, int multiplier, int64_t now) {
        if (engaged_) {
            return;
        }
        engaged_ = true;
        all_down_ = false;
        floor_ = context.floor;
        multiplier_ = multiplier;
        if (!on_floor_) {
            start_ms_ = now;
        }
        Line(now, "Chimera fight started: floor %d, room %d, round %d, ultimate=%d, damage multiplier=%dx",
             context.floor + 1, context.room, context.round, context.ultimate ? 1 : 0, multiplier);
    }

    template <typename Memory>
    bool Poll(const Memory& memory, Tracked& tracked, int64_t now) {
        if (memory.U64(tracked.owner + kOwnerNameOffset) != tracked.name ||
            memory.U64(tracked.unit + kUnitOwnerOffset) != tracked.owner) {
            tracked.removed = true;
            Line(now, "%s removed: last hp=%d/%d hits=%d biggest hit=%d", PartName(tracked.part), tracked.last_hp,
                 tracked.max_hp, tracked.hits, tracked.biggest);
            return false;
        }
        const int32_t hp = memory.S32(tracked.unit + kUnitHpOffset);
        const int32_t bucket =
            hp <= 0 ? 0 : (hp >= tracked.max_hp ? 10 : static_cast<int32_t>(static_cast<int64_t>(hp) * 10 / tracked.max_hp));
        if (hp <= 0) {
            tracked.lethal_since_ms = 0;
            if (!tracked.defeated) {
                tracked.defeated = true;
                Line(now, "%s defeated: hp=%d/%d hits=%d biggest hit=%d", PartName(tracked.part), hp, tracked.max_hp,
                     tracked.hits, tracked.biggest);
            }
        } else {
            if (tracked.defeated) {
                tracked.defeated = false;
                Line(now, "%s hp above 0 again: hp=%d/%d", PartName(tracked.part), hp, tracked.max_hp);
            }
            if (bucket != tracked.bucket) {
                const int32_t steps = bucket > tracked.bucket ? bucket - tracked.bucket : tracked.bucket - bucket;
                Line(now, "%s hp %d/%d (%d%%)%s", PartName(tracked.part), hp, tracked.max_hp, bucket * 10,
                     steps >= 3 ? ", jumped several steps" : "");
            }
            if (tracked.lethal_since_ms != 0 && now - tracked.lethal_since_ms >= kLethalSettleMs) {
                Line(now, "%s lethal check left hp=%d/%d", PartName(tracked.part), hp, tracked.max_hp);
                tracked.lethal_since_ms = 0;
            }
        }
        tracked.last_hp = hp;
        tracked.bucket = bucket;
        return hp > 0;
    }

    void End(const Context& context, int64_t now) {
        if (engaged_) {
            int defeated = 0, total = 0;
            for (const Tracked& tracked : parts_) {
                if (tracked.owner != 0) {
                    ++total;
                    defeated += tracked.defeated || tracked.removed ? 1 : 0;
                }
            }
            Line(now, "Chimera fight ended: round %d, floor %d, parts seen=%d, down or gone=%d", context.round,
                 context.floor + 1, total, defeated);
        } else if (on_floor_) {
            Line(now, "left Tower floor %d: round %d, floor %d", kTowerBossFloor + 1, context.round, context.floor + 1);
        }
        parts_ = {};
        engaged_ = false;
        all_down_ = false;
        floor_ = -1;
    }

#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    void Line(int64_t now, const char* format, ...) {
        if (sink_ == nullptr || lines_ > kMaxLines) {
            return;
        }
        char text[200];
        const int length = std::snprintf(text, sizeof(text), "Boss trace +%lld.%lld s: ",
                                         static_cast<long long>((now - start_ms_) / 1000),
                                         static_cast<long long>((now - start_ms_) % 1000 / 100));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(text)) {
            return;
        }
        if (++lines_ > kMaxLines) {
            std::snprintf(text + length, sizeof(text) - static_cast<size_t>(length), "line limit reached");
        } else {
            va_list args;
            va_start(args, format);
            std::vsnprintf(text + length, sizeof(text) - static_cast<size_t>(length), format, args);
            va_end(args);
        }
        sink_(text);
    }

    uint64_t image_begin_ = 0;
    uint64_t image_end_ = 0;
    Sink sink_ = nullptr;
    std::array<Tracked, kMaxParts> parts_{};
    bool engaged_ = false;
    bool on_floor_ = false;
    bool all_down_ = false;
    int32_t room_ = -1;
    int32_t floor_ = -1;
    int multiplier_ = 1;
    unsigned lines_ = 0;
    int64_t start_ms_ = 0;
    int64_t last_sample_ms_ = INT64_MIN;
};

} // namespace dcp::bosstrace
