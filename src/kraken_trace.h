// A state-change trace of the Kraken fight in Ghost Ship Cove, written to the runtime log, and a snapshot of its latest
// state for the crash report. It only reads.
#pragma once

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "boss_trace.h"

namespace dcp::krakentrace {

inline constexpr uint32_t kOwnerNameOffset = bosstrace::kOwnerNameOffset;
inline constexpr uint32_t kUnitHpOffset = bosstrace::kUnitHpOffset;
inline constexpr uint32_t kUnitOwnerOffset = bosstrace::kUnitOwnerOffset;
inline constexpr uint32_t kOwnerUnitOffset = 0x9b8;
// A tentacle's weak reference to its mast and the mast's broken flag, as laid out in CUSA10454 1.00.
inline constexpr uint32_t kTentacleMastOffset = 0x38d8;
inline constexpr uint32_t kMastBrokenOffset = 0x38c1;
inline constexpr size_t kNameBytes = 24;
inline constexpr size_t kMaxParts = 32;
inline constexpr int64_t kSampleIntervalMs = bosstrace::kSampleIntervalMs;
inline constexpr int64_t kLethalSettleMs = bosstrace::kLethalSettleMs;
inline constexpr unsigned kMaxLines = 120;
inline constexpr size_t kMaxGuardLines = 4;

enum class Part : uint8_t { None, Body, Mouth, DefenseArm, AttackArm, Rock, Tentacle, Mast };
enum class Link : uint8_t { Unknown, None, Present, Broken, Other };
enum class Phase : uint8_t { Idle, Fight, BodyDown, AllDown, Ended };
enum class Event : uint8_t {
    None,
    FightStart,
    PartSeen,
    PartHp,
    PartDefeated,
    PartRemoved,
    LethalCheck,
    LethalLeft,
    MastLink,
    Multiplier,
    NullMastGuard,
    AllDown,
    FightEnd
};

struct PartClass {
    Part part;
    const char* name;
    const char* label;
};

inline constexpr PartClass kPartClasses[] = {
    {Part::Body, "DcKraken_Body", "Body"},
    {Part::Mouth, "DcKraken_Mouth", "Mouth"},
    {Part::DefenseArm, "DcKraken_DefenseArm", "DefenseArm"},
    {Part::AttackArm, "DcKraken_AttackArm", "AttackArm"},
    {Part::Rock, "DcKraken_Rock", "Rock"},
    {Part::Tentacle, "DcKrakenTentacle", "Tentacle"},
    {Part::Mast, "DcKrakenTentacleMast", "Mast"},
};

constexpr const char* PartLabel(Part part) {
    for (const PartClass& entry : kPartClasses) {
        if (entry.part == part) return entry.label;
    }
    return "none";
}

constexpr const char* LinkLabel(Link link) {
    switch (link) {
    case Link::None: return "none";
    case Link::Present: return "present";
    case Link::Broken: return "broken";
    case Link::Other: return "other";
    default: return "unknown";
    }
}

constexpr const char* PhaseLabel(Phase phase) {
    switch (phase) {
    case Phase::Fight: return "fight";
    case Phase::BodyDown: return "body_down";
    case Phase::AllDown: return "all_down";
    case Phase::Ended: return "ended";
    default: return "idle";
    }
}

constexpr const char* EventLabel(Event event) {
    switch (event) {
    case Event::FightStart: return "fight_start";
    case Event::PartSeen: return "part_seen";
    case Event::PartHp: return "part_hp";
    case Event::PartDefeated: return "part_defeated";
    case Event::PartRemoved: return "part_removed";
    case Event::LethalCheck: return "lethal_check";
    case Event::LethalLeft: return "lethal_left";
    case Event::MastLink: return "mast_link";
    case Event::Multiplier: return "multiplier";
    case Event::NullMastGuard: return "null_mast_guard";
    case Event::AllDown: return "all_down";
    case Event::FightEnd: return "fight_end";
    default: return "none";
    }
}

// What the crash report prints about the fight. Plain atomics, written by the hooks and read by the fault handler, which
// takes no lock and allocates nothing.
struct Snapshot {
    std::atomic<uint32_t> engaged{0};
    std::atomic<uint32_t> event{0};
    std::atomic<uint32_t> phase{0};
    std::atomic<uint32_t> part{0};
    std::atomic<uint32_t> link{0};
    std::atomic<uint32_t> tentacles{0};
    std::atomic<uint32_t> masts{0};
    std::atomic<uint32_t> without_mast{0};
    std::atomic<int32_t> body_percent{-1};
    std::atomic<uint32_t> multiplier{1};
    std::atomic<uint32_t> seconds{0};
};

class KrakenTrace {
public:
    using Sink = void (*)(const char* line);

    // The game image bounds a class-name pointer must lie in before the name is read. `mast_layout` says the image's
    // tentacle layout is the one the handle offsets describe.
    void Configure(uint64_t image_begin, uint64_t image_end, bool mast_layout, Sink sink) {
        image_begin_ = image_begin;
        image_end_ = image_end;
        mast_layout_ = mast_layout;
        sink_ = sink;
        Reset();
    }

    void Reset() {
        parts_ = {};
        engaged_ = false;
        phase_ = Phase::Idle;
        round_ = -1;
        room_ = -1;
        multiplier_ = 1;
        dropped_ = 0;
        guarded_ = {};
        guard_lines_ = 0;
        lines_ = 0;
        start_ms_ = 0;
        last_sample_ms_ = INT64_MIN;
        ClearSnapshot();
    }

    template <typename Memory>
    Part Identify(const Memory& memory, uint64_t owner) const {
        if (owner == 0 || image_end_ == 0) {
            return Part::None;
        }
        const uint64_t name = memory.U64(owner + kOwnerNameOffset);
        return ReadClass(memory, name);
    }

    bool Tracking() const { return engaged_; }

    // A hit on a Kraken part, before the game lowers its HP. The hit check calls this too, with `lethal_check` set.
    template <typename Memory>
    void Observe(const Memory& memory, Part part, uint64_t owner, uint64_t unit, int32_t amount, int multiplier,
                 bool lethal_check, const bosstrace::Context& context, int64_t now) {
        if (part == Part::None || owner == 0 || unit == 0) {
            return;
        }
        Tracked* tracked = Find(owner);
        const int32_t hp = memory.S32(unit + kUnitHpOffset);
        if (tracked == nullptr) {
            Engage(context, multiplier, now);
            tracked = Add(memory, part, owner, unit, hp);
            if (tracked == nullptr) {
                return;
            }
            Seen(*tracked, hp, amount, now);
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
        if (lethal_check && hp > 0 && tracked->lethal_since_ms == 0 &&
            static_cast<int64_t>(hp) <= static_cast<int64_t>(amount) * multiplier) {
            tracked->lethal_since_ms = now == 0 ? 1 : now;
            Line(now, Event::LethalCheck, tracked, "hp=%d hit=%d dmgx=%d native_lethal=%d", hp, amount, multiplier,
                 hp <= amount ? 1 : 0);
        }
    }

    bool SampleDue(int64_t now) {
        if (!engaged_) {
            return false;
        }
        if (last_sample_ms_ != INT64_MIN && now - last_sample_ms_ < kSampleIntervalMs) {
            return false;
        }
        last_sample_ms_ = now;
        return true;
    }

    // From the player's tick, at most every kSampleIntervalMs.
    template <typename Memory>
    void Sample(const Memory& memory, const bosstrace::Context& context, int multiplier, int64_t now) {
        if (!engaged_) {
            return;
        }
        if (round_ < 0 && context.valid) {
            round_ = context.round;
            room_ = context.room;
        }
        if (!context.valid || context.round != round_) {
            End(now);
            return;
        }
        if (multiplier != multiplier_) {
            multiplier_ = multiplier;
            Line(now, Event::Multiplier, nullptr, "dmgx=%d", multiplier);
        }
        for (size_t i = 0; i < parts_.size(); ++i) {
            if (parts_[i].owner != 0 && !parts_[i].removed) {
                Poll(memory, parts_[i], now);
            }
        }
        const bool any_alive = AliveCount(Part::None) > 0;
        if (!any_alive && phase_ != Phase::AllDown) {
            phase_ = Phase::AllDown;
            Line(now, Event::AllDown, nullptr, "parts=%d", TrackedCount());
        } else if (any_alive && phase_ == Phase::AllDown) {
            phase_ = BodyDown() ? Phase::BodyDown : Phase::Fight;
        }
    }

    // True while a tentacle's null mast is still worth a line: at most kMaxGuardLines, one per tentacle.
    bool GuardDue(uint64_t tentacle) const {
        if (sink_ == nullptr || guard_lines_ >= kMaxGuardLines) {
            return false;
        }
        for (size_t i = 0; i < guard_lines_; ++i) {
            if (guarded_[i] == tentacle) {
                return false;
            }
        }
        return true;
    }

    // The fix answered a tentacle's null mast handle as a mast that is not broken.
    void NullMastGuard(uint64_t tentacle, const bosstrace::Context& context, int multiplier, int64_t now) {
        if (!GuardDue(tentacle)) {
            return;
        }
        guarded_[guard_lines_++] = tentacle;
        Engage(context, multiplier, now);
        Tracked* tracked = Find(tentacle);
        if (tracked != nullptr) {
            tracked->link = Link::None;
            Line(now, Event::NullMastGuard, tracked, "mast=none action=not_broken");
        } else {
            Line(now, Event::NullMastGuard, nullptr, "part=Tentacle mast=none action=not_broken");
        }
    }

    // The latest state for the crash report; false when no Kraken fight has been seen. Reads only the snapshot.
    bool CrashContext(char* out, size_t size) const {
        if (size == 0 || snapshot_.engaged.load(std::memory_order_relaxed) == 0) {
            return false;
        }
        const int32_t body = snapshot_.body_percent.load(std::memory_order_relaxed);
        char body_text[8] = "-";
        if (body >= 0) {
            std::snprintf(body_text, sizeof(body_text), "%d%%", body);
        }
        std::snprintf(out, size,
                      "Kraken phase=%s event=%s part=%s link=%s tentacles=%u masts=%u without_mast=%u body_hp=%s "
                      "dmgx=%u t=%us",
                      PhaseLabel(static_cast<Phase>(snapshot_.phase.load(std::memory_order_relaxed))),
                      EventLabel(static_cast<Event>(snapshot_.event.load(std::memory_order_relaxed))),
                      PartLabel(static_cast<Part>(snapshot_.part.load(std::memory_order_relaxed))),
                      LinkLabel(static_cast<Link>(snapshot_.link.load(std::memory_order_relaxed))),
                      snapshot_.tentacles.load(std::memory_order_relaxed), snapshot_.masts.load(std::memory_order_relaxed),
                      snapshot_.without_mast.load(std::memory_order_relaxed), body_text,
                      snapshot_.multiplier.load(std::memory_order_relaxed),
                      snapshot_.seconds.load(std::memory_order_relaxed));
        return true;
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
        Link link = Link::Unknown;
        uint8_t id = 0;
        bool defeated = false;
        bool removed = false;
    };

    template <typename Memory>
    Part ReadClass(const Memory& memory, uint64_t name) const {
        if (name < image_begin_ || name > image_end_ || image_end_ - name < kNameBytes) {
            return Part::None;
        }
        char text[kNameBytes];
        for (size_t at = 0; at < kNameBytes; at += 8) {
            const uint64_t word = memory.U64(name + at);
            std::memcpy(text + at, &word, 8);
        }
        text[kNameBytes - 1] = '\0';
        for (const PartClass& entry : kPartClasses) {
            if (std::strcmp(text, entry.name) == 0) {
                return entry.part;
            }
        }
        return Part::None;
    }

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
        for (size_t i = 0; i < parts_.size(); ++i) {
            Tracked& tracked = parts_[i];
            if (tracked.owner == 0 || tracked.removed) {
                tracked = {};
                tracked.part = part;
                tracked.owner = owner;
                tracked.unit = unit;
                tracked.name = memory.U64(owner + kOwnerNameOffset);
                tracked.max_hp = hp > 0 ? hp : 1;
                tracked.last_hp = hp;
                tracked.bucket = 10;
                tracked.defeated = unit != 0 && hp <= 0;
                tracked.id = static_cast<uint8_t>(i);
                return &tracked;
            }
        }
        ++dropped_;
        return nullptr;
    }

    void Seen(const Tracked& tracked, int32_t hp, int32_t amount, int64_t now) {
        Line(now, Event::PartSeen, &tracked, "hp=%d hit=%d tentacles=%d masts=%d", hp, amount,
             AliveCount(Part::Tentacle), AliveCount(Part::Mast));
    }

    void Engage(const bosstrace::Context& context, int multiplier, int64_t now) {
        if (engaged_) {
            return;
        }
        engaged_ = true;
        phase_ = Phase::Fight;
        round_ = context.valid ? context.round : -1;
        room_ = context.valid ? context.room : -1;
        multiplier_ = multiplier;
        start_ms_ = now;
        last_sample_ms_ = INT64_MIN;
        snapshot_.engaged.store(1, std::memory_order_relaxed);
        Line(now, Event::FightStart, nullptr, "round=%d room=%d dmgx=%d", round_, room_, multiplier);
    }

    template <typename Memory>
    void Poll(const Memory& memory, Tracked& tracked, int64_t now) {
        if (memory.U64(tracked.owner + kOwnerNameOffset) != tracked.name ||
            (tracked.unit != 0 && memory.U64(tracked.unit + kUnitOwnerOffset) != tracked.owner)) {
            tracked.removed = true;
            Line(now, Event::PartRemoved, &tracked, "last_hp=%d/%d hits=%d biggest_hit=%d link=%s", tracked.last_hp,
                 tracked.max_hp, tracked.hits, tracked.biggest, LinkLabel(tracked.link));
            return;
        }
        if (tracked.unit != 0) {
            PollHp(memory, tracked, now);
        }
        if (tracked.part == Part::Tentacle && mast_layout_ && !tracked.defeated) {
            PollMast(memory, tracked, now);
        }
        if (tracked.part == Part::Mast && mast_layout_) {
            const Link link = memory.U8(tracked.owner + kMastBrokenOffset) != 0 ? Link::Broken : Link::Present;
            if (link != tracked.link) {
                tracked.link = link;
                Line(now, Event::MastLink, &tracked, "mast=%s", LinkLabel(link));
            }
        }
    }

    template <typename Memory>
    void PollHp(const Memory& memory, Tracked& tracked, int64_t now) {
        const int32_t hp = memory.S32(tracked.unit + kUnitHpOffset);
        const int32_t bucket =
            hp <= 0 ? 0 : (hp >= tracked.max_hp ? 10 : static_cast<int32_t>(static_cast<int64_t>(hp) * 10 / tracked.max_hp));
        if (hp <= 0) {
            tracked.lethal_since_ms = 0;
            if (!tracked.defeated) {
                tracked.defeated = true;
                if (tracked.part == Part::Body) {
                    phase_ = Phase::BodyDown;
                }
                Line(now, Event::PartDefeated, &tracked, "hp=%d/%d hits=%d biggest_hit=%d", hp, tracked.max_hp,
                     tracked.hits, tracked.biggest);
            }
        } else {
            tracked.defeated = false;
            if (bucket != tracked.bucket && (tracked.part == Part::Body || tracked.part == Part::Mouth)) {
                Line(now, Event::PartHp, &tracked, "hp=%d/%d pct=%d", hp, tracked.max_hp, bucket * 10);
            }
            if (tracked.lethal_since_ms != 0 && now - tracked.lethal_since_ms >= kLethalSettleMs) {
                Line(now, Event::LethalLeft, &tracked, "hp=%d/%d", hp, tracked.max_hp);
                tracked.lethal_since_ms = 0;
            }
        }
        tracked.last_hp = hp;
        tracked.bucket = bucket;
    }

    // Follows a tentacle's handle to the mast it holds and tracks that mast too.
    template <typename Memory>
    void PollMast(const Memory& memory, Tracked& tentacle, int64_t now) {
        const uint64_t mast = memory.U64(tentacle.owner + kTentacleMastOffset);
        Link link = Link::None;
        if (mast != 0) {
            const Part held = Identify(memory, mast);
            link = held != Part::Mast ? Link::Other : (memory.U8(mast + kMastBrokenOffset) != 0 ? Link::Broken : Link::Present);
            if (held == Part::Mast && Find(mast) == nullptr) {
                const uint64_t unit = memory.U64(mast + kOwnerUnitOffset);
                const bool valid = unit != 0 && memory.U64(unit + kUnitOwnerOffset) == mast;
                Tracked* added = Add(memory, Part::Mast, mast, valid ? unit : 0, valid ? memory.S32(unit + kUnitHpOffset) : 0);
                if (added != nullptr) {
                    Line(now, Event::PartSeen, added, "hp=%d via=tentacle tentacles=%d masts=%d", added->last_hp,
                         AliveCount(Part::Tentacle), AliveCount(Part::Mast));
                }
            }
        }
        if (link != tentacle.link) {
            tentacle.link = link;
            Line(now, Event::MastLink, &tentacle, "mast=%s", LinkLabel(link));
        }
    }

    int AliveCount(Part part) const {
        int count = 0;
        for (const Tracked& tracked : parts_) {
            if (tracked.owner != 0 && !tracked.removed && !tracked.defeated && (part == Part::None || tracked.part == part)) {
                ++count;
            }
        }
        return count;
    }

    int TrackedCount() const {
        int count = 0;
        for (const Tracked& tracked : parts_) {
            count += tracked.owner != 0 ? 1 : 0;
        }
        return count;
    }

    int WithoutMast() const {
        int count = 0;
        for (const Tracked& tracked : parts_) {
            if (tracked.part == Part::Tentacle && tracked.owner != 0 && !tracked.removed && !tracked.defeated &&
                tracked.link == Link::None) {
                ++count;
            }
        }
        return count;
    }

    bool BodyDown() const {
        for (const Tracked& tracked : parts_) {
            if (tracked.part == Part::Body && tracked.owner != 0 && (tracked.defeated || tracked.removed)) {
                return true;
            }
        }
        return false;
    }

    int BodyPercent() const {
        for (const Tracked& tracked : parts_) {
            if (tracked.part == Part::Body && tracked.owner != 0 && !tracked.removed) {
                return tracked.defeated ? 0 : tracked.bucket * 10;
            }
        }
        return -1;
    }

    void End(int64_t now) {
        engaged_ = false;
        phase_ = Phase::Ended;
        Line(now, Event::FightEnd, nullptr, "round=%d room=%d parts_seen=%d down_or_gone=%d dropped=%u", round_, room_,
             TrackedCount(), TrackedCount() - AliveCount(Part::None), dropped_);
        parts_ = {};
        guarded_ = {};
        guard_lines_ = 0;
        round_ = -1;
        room_ = -1;
        dropped_ = 0;
    }

    void ClearSnapshot() {
        snapshot_.engaged.store(0, std::memory_order_relaxed);
        snapshot_.event.store(0, std::memory_order_relaxed);
        snapshot_.phase.store(0, std::memory_order_relaxed);
        snapshot_.part.store(0, std::memory_order_relaxed);
        snapshot_.link.store(0, std::memory_order_relaxed);
        snapshot_.tentacles.store(0, std::memory_order_relaxed);
        snapshot_.masts.store(0, std::memory_order_relaxed);
        snapshot_.without_mast.store(0, std::memory_order_relaxed);
        snapshot_.body_percent.store(-1, std::memory_order_relaxed);
        snapshot_.multiplier.store(1, std::memory_order_relaxed);
        snapshot_.seconds.store(0, std::memory_order_relaxed);
    }

    void Publish(Event event, const Tracked* about, int64_t now) {
        const auto count = [](int value) { return static_cast<uint32_t>(value < 0 ? 0 : value); };
        snapshot_.event.store(static_cast<uint32_t>(event), std::memory_order_relaxed);
        snapshot_.phase.store(static_cast<uint32_t>(phase_), std::memory_order_relaxed);
        snapshot_.part.store(about != nullptr ? static_cast<uint32_t>(about->part) : 0, std::memory_order_relaxed);
        snapshot_.link.store(about != nullptr ? static_cast<uint32_t>(about->link) : 0, std::memory_order_relaxed);
        snapshot_.tentacles.store(count(AliveCount(Part::Tentacle)), std::memory_order_relaxed);
        snapshot_.masts.store(count(AliveCount(Part::Mast)), std::memory_order_relaxed);
        snapshot_.without_mast.store(count(WithoutMast()), std::memory_order_relaxed);
        snapshot_.body_percent.store(BodyPercent(), std::memory_order_relaxed);
        snapshot_.multiplier.store(count(multiplier_), std::memory_order_relaxed);
        snapshot_.seconds.store(count(static_cast<int>((now - start_ms_) / 1000)), std::memory_order_relaxed);
    }

#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    void Line(int64_t now, Event event, const Tracked* about, const char* format, ...) {
        Publish(event, about, now);
        if (sink_ == nullptr || lines_ > kMaxLines) {
            return;
        }
        char text[220];
        int length = std::snprintf(text, sizeof(text), "Kraken telemetry: t=%lld.%lld event=%s",
                                   static_cast<long long>((now - start_ms_) / 1000),
                                   static_cast<long long>((now - start_ms_) % 1000 / 100), EventLabel(event));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(text)) {
            return;
        }
        if (about != nullptr) {
            const int added = std::snprintf(text + length, sizeof(text) - static_cast<size_t>(length), " part=%s id=%u",
                                            PartLabel(about->part), static_cast<unsigned>(about->id));
            if (added < 0 || static_cast<size_t>(added) >= sizeof(text) - static_cast<size_t>(length)) {
                return;
            }
            length += added;
        }
        const int space = std::snprintf(text + length, sizeof(text) - static_cast<size_t>(length), " phase=%s ",
                                        PhaseLabel(phase_));
        if (space < 0 || static_cast<size_t>(space) >= sizeof(text) - static_cast<size_t>(length)) {
            return;
        }
        length += space;
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
    bool mast_layout_ = false;
    Sink sink_ = nullptr;
    std::array<Tracked, kMaxParts> parts_{};
    std::array<uint64_t, kMaxGuardLines> guarded_{};
    size_t guard_lines_ = 0;
    bool engaged_ = false;
    Phase phase_ = Phase::Idle;
    int32_t round_ = -1;
    int32_t room_ = -1;
    int multiplier_ = 1;
    unsigned dropped_ = 0;
    unsigned lines_ = 0;
    int64_t start_ms_ = 0;
    int64_t last_sample_ms_ = INT64_MIN;
    Snapshot snapshot_;
};

} // namespace dcp::krakentrace
