// The input rules the in-game menus (Cheats, Options) share: when the game gets its input back after a menu closes,
// the L3 + R3 chord, and stepping through rows, pages and choices. Logic only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dcp::menu {

// A button that is never reported released (a controller unplugged as the menu closes) cannot block the game forever.
inline constexpr double kReleaseTimeoutSeconds = 1.0;

// Whether the menu may release the game's input: nothing it received is still held, or the wait ran out.
inline bool InputReleased(bool anything_held, double waited_seconds, double timeout_seconds) {
    return !anything_held || waited_seconds >= timeout_seconds;
}

// Closing the menu (B, Escape, its key or chord) starts a wait, and the game gets its input back only after what was
// pressed in the menu is released, so the closing press never also reaches the game.
class CloseGate {
public:
    void Request() { requested_ = true; }
    bool requested() const { return requested_; }

    // Once per frame. True when the menu may go and the game may read its input again.
    bool Step(bool anything_held, double now) {
        if (!requested_) {
            return false;
        }
        if (since_ < 0.0) {
            since_ = now;
        }
        return InputReleased(anything_held, now - since_, kReleaseTimeoutSeconds);
    }

private:
    bool requested_ = false;
    double since_ = -1.0;
};

// True once per press of every button in `mask` together. After a press, all of them must be let go before the next
// one counts, so a held chord never repeats and a half-release does not re-arm it.
class Chord {
public:
    explicit Chord(uint32_t mask) : mask_(mask) {}

    bool Poll(uint32_t buttons) {
        const uint32_t held = buttons & mask_;
        const bool pressed = armed_ && held == mask_;
        if (pressed) {
            armed_ = false;
        } else if (held == 0) {
            armed_ = true;
        }
        return pressed;
    }

private:
    uint32_t mask_;
    bool armed_ = true;
};

enum class ToggleAction { None, Open, Close };

// A menu's key or chord: a held key repeats and only the first press counts. `busy` is another menu or a game dialog
// on screen, which the menu never opens over.
constexpr ToggleAction TogglePress(bool repeat, bool open, bool busy) {
    if (repeat) {
        return ToggleAction::None;
    }
    if (open) {
        return ToggleAction::Close;
    }
    return busy ? ToggleAction::None : ToggleAction::Open;
}

// The index one step away, wrapping around.
constexpr int WrapIndex(int index, int count, int step) {
    return count <= 0 ? 0 : ((index + step) % count + count) % count;
}

// One step from `current` (-1 or 1), stopping at the ends. A current value that is not a choice moves to the first.
constexpr int StepChoice(int current, int count, int step) {
    if (count <= 0) {
        return -1;
    }
    if (current < 0 || current >= count) {
        return 0;
    }
    return std::clamp(current + step, 0, count - 1);
}

// The next choice, wrapping around, so a toggle flips.
constexpr int NextChoiceWrapping(int current, int count) {
    if (count <= 0) {
        return -1;
    }
    return current < 0 || current + 1 >= count ? 0 : current + 1;
}

// The first visible row of a list of `count` rows, `visible` of them shown from `first`, that keeps `selected` shown.
constexpr int ScrollToShow(int selected, int first, int visible, int count) {
    if (visible <= 0 || count <= visible) {
        return 0;
    }
    if (selected < first) {
        first = selected;
    } else if (selected >= first + visible) {
        first = selected - visible + 1;
    }
    return std::clamp(first, 0, count - visible);
}

} // namespace dcp::menu
