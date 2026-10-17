#include "runtime_cheats.h"

#include <atomic>

#include "dcp_cheats.h"

namespace {

using dcp::cheats::Cheat;

dcp::cheats::SessionCheats g_session;
dcp::cheats::RefillRequests g_refills;
dcp::cheats::ActionRequests g_actions;
dcp::cheats::SessionExperience g_experience;
std::atomic<unsigned> g_saved{0};
std::atomic<int> g_saved_experience{dcp::cheats::kExperienceMultiplierOff};
std::atomic<int> g_available{0};

bool Valid(int cheat) {
    return cheat >= 0 && static_cast<size_t>(cheat) < dcp::cheats::kCheatCount;
}

bool ValidAction(int action) {
    return action >= 0 && static_cast<size_t>(action) < dcp::cheats::kActionCount;
}

bool SavedValue(Cheat cheat) {
    return (g_saved.load(std::memory_order_relaxed) & dcp::cheats::Bit(cheat)) != 0;
}

// The port has no online session. This is where one would switch the cheats off.
bool OnlineSessionActive() {
    return false;
}

} // namespace

extern "C" void runtime_cheats_start(unsigned saved_mask, int experience_multiplier) {
    g_session.Reset();
    g_experience.Reset();
    g_refills.Clear();
    g_actions.Clear();
    g_saved.store(saved_mask, std::memory_order_relaxed);
    g_saved_experience.store(dcp::cheats::ListedExperienceMultiplier(experience_multiplier), std::memory_order_relaxed);
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
    if (!Valid(cheat)) {
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
    for (const dcp::cheats::CheatInfo& info : dcp::cheats::kCheats) {
        if (g_refills.Pending(info.cheat)) {
            mask |= dcp::cheats::Bit(info.cheat);
        }
    }
    return mask;
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
    return ValidAction(action) && g_actions.Pending(static_cast<dcp::cheats::Action>(action)) ? 1 : 0;
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
