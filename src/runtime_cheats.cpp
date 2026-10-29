#include "runtime_cheats.h"

#include <atomic>
#include <cstdio>

#include "dcp_cheats.h"

namespace {

using dcp::cheats::Cheat;

dcp::cheats::SessionCheats g_session;
dcp::cheats::RefillRequests g_refills;
dcp::cheats::ActionRequests g_actions;
dcp::cheats::SessionExperience g_experience;
dcp::cheats::SessionMovementSpeed g_movement;
std::atomic<unsigned> g_saved{0};
std::atomic<int> g_saved_experience{dcp::cheats::kExperienceMultiplierOff};
std::atomic<int> g_saved_movement{dcp::cheats::kMovementSpeedOff};
std::atomic<int> g_available{0};
std::atomic<bool> g_online{false};
int (*g_slot_human)(int slot) = nullptr;

bool Valid(int cheat) {
    return cheat >= 0 && static_cast<size_t>(cheat) < dcp::cheats::kCheatCount;
}

bool ValidAction(int action) {
    return action >= 0 && static_cast<size_t>(action) < dcp::cheats::kActionCount;
}

bool SavedValue(Cheat cheat) {
    return (g_saved.load(std::memory_order_relaxed) & dcp::cheats::Bit(cheat)) != 0;
}

bool OnlineSessionActive() {
    return g_online.load(std::memory_order_relaxed);
}

void ClearSession() {
    g_session.Reset();
    g_experience.Reset();
    g_movement.Reset();
    g_refills.Clear();
    g_actions.Clear();
}

} // namespace

extern "C" void runtime_cheats_set_online(int online) {
    g_online.store(online != 0, std::memory_order_relaxed);
    if (online) {
        runtime_cheats_start(0, dcp::cheats::kExperienceMultiplierOff, dcp::cheats::kMovementSpeedOff);
    }
}

extern "C" int runtime_cheats_online_locked(void) {
    return OnlineSessionActive() ? 1 : 0;
}

extern "C" void runtime_cheats_start(unsigned saved_mask, int experience_multiplier, int movement_speed) {
    ClearSession();
    if (OnlineSessionActive()) {
        saved_mask = 0;
        experience_multiplier = dcp::cheats::kExperienceMultiplierOff;
        movement_speed = dcp::cheats::kMovementSpeedOff;
    }
    g_saved.store(saved_mask, std::memory_order_relaxed);
    g_saved_experience.store(dcp::cheats::ListedExperienceMultiplier(experience_multiplier), std::memory_order_relaxed);
    g_saved_movement.store(dcp::cheats::ListedMovementSpeed(movement_speed), std::memory_order_relaxed);
    for (const dcp::cheats::CheatInfo& info : dcp::cheats::kCheats) {
        if (dcp::cheats::RefillsOnEnable(info.cheat) && (saved_mask & dcp::cheats::Bit(info.cheat)) != 0) {
            g_refills.Request(info.cheat);
        }
    }
}

extern "C" int runtime_cheat_experience_multiplier(void) {
    if (OnlineSessionActive()) {
        return dcp::cheats::kExperienceMultiplierOff;
    }
    return g_experience.Effective(g_saved_experience.load(std::memory_order_relaxed));
}

extern "C" int runtime_cheat_experience_cycle(void) {
    if (OnlineSessionActive()) {
        return dcp::cheats::kExperienceMultiplierOff;
    }
    return g_experience.Cycle(g_saved_experience.load(std::memory_order_relaxed));
}

extern "C" int runtime_cheat_movement_speed(void) {
    if (OnlineSessionActive()) {
        return dcp::cheats::kMovementSpeedOff;
    }
    return g_movement.Effective(g_saved_movement.load(std::memory_order_relaxed));
}

extern "C" int runtime_cheat_movement_cycle(void) {
    if (OnlineSessionActive()) {
        return dcp::cheats::kMovementSpeedOff;
    }
    return g_movement.Cycle(g_saved_movement.load(std::memory_order_relaxed));
}

extern "C" void runtime_cheats_set_slot_human(int (*is_human)(int slot)) {
    g_slot_human = is_human;
}

extern "C" int runtime_cheat_slot_human(int slot) {
    return g_slot_human ? g_slot_human(slot) : (slot == 0 ? 1 : 0);
}

extern "C" int runtime_cheats_available(void) {
    return g_available.load(std::memory_order_relaxed);
}

extern "C" void runtime_cheats_set_available(int available) {
    g_available.store(available, std::memory_order_relaxed);
}

extern "C" int runtime_cheat_enabled(int cheat) {
    if (!Valid(cheat) || OnlineSessionActive()) {
        return 0;
    }
    return g_session.Effective(static_cast<Cheat>(cheat), SavedValue(static_cast<Cheat>(cheat))) ? 1 : 0;
}

extern "C" void runtime_cheat_toggle(int cheat) {
    if (!Valid(cheat) || OnlineSessionActive()) {
        return;
    }
    const Cheat which = static_cast<Cheat>(cheat);
    const bool enabled = g_session.Toggle(which, SavedValue(which));
    if (!dcp::cheats::RefillsOnEnable(which)) {
        return;
    }
    if (enabled) {
        g_refills.Request(which);
    } else {
        g_refills.Cancel(which);
    }
}

extern "C" unsigned runtime_cheat_refills_pending(void) {
    unsigned mask = 0;
    if (OnlineSessionActive()) {
        return mask;
    }
    for (const dcp::cheats::CheatInfo& info : dcp::cheats::kCheats) {
        if (g_refills.Pending(info.cheat)) {
            mask |= dcp::cheats::Bit(info.cheat);
        }
    }
    return mask;
}

extern "C" void runtime_cheats_log_enabled(void) {
    if (OnlineSessionActive()) {
        std::printf("Cheats: disabled for Online Play\n");
        return;
    }
    uint32_t mask = 0;
    for (const dcp::cheats::CheatInfo& info : dcp::cheats::kCheats) {
        if (runtime_cheat_enabled(static_cast<int>(info.cheat))) {
            mask |= dcp::cheats::Bit(info.cheat);
        }
    }
    std::printf("Cheats: enabled: %s\n", dcp::cheats::EnabledCheatsText(mask).c_str());
    std::printf("Cheats: Experience Multiplier %s\n",
                dcp::cheats::ExperienceMultiplierLabel(runtime_cheat_experience_multiplier()));
    std::printf("Cheats: Movement Speed %s\n", dcp::cheats::MovementSpeedLabel(runtime_cheat_movement_speed()));
}

extern "C" int runtime_cheat_take_refill(int cheat) {
    if (!Valid(cheat) || OnlineSessionActive()) {
        return 0;
    }
    return g_refills.Take(static_cast<Cheat>(cheat)) ? 1 : 0;
}

extern "C" void runtime_cheat_action(int action) {
    if (ValidAction(action) && !OnlineSessionActive()) {
        g_actions.Request(static_cast<dcp::cheats::Action>(action));
    }
}

extern "C" int runtime_cheat_action_pending(int action) {
    if (!ValidAction(action) || OnlineSessionActive()) {
        return 0;
    }
    return g_actions.Pending(static_cast<dcp::cheats::Action>(action)) ? 1 : 0;
}

extern "C" int runtime_cheat_take_action(int action) {
    if (!ValidAction(action) || OnlineSessionActive()) {
        return 0;
    }
    return g_actions.Take(static_cast<dcp::cheats::Action>(action)) ? 1 : 0;
}

extern "C" void runtime_cheat_clear_actions(void) {
    g_actions.Clear();
}
