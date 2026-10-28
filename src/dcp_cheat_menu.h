// The in-game cheat menu's model: what it lists and says, and when the game gets its input back.
#pragma once

#include <array>
#include <cstddef>

#include "dcp_cheats.h"

namespace dcp::cheats {

inline constexpr char kMenuTitle[] = "CHEATS";
inline constexpr char kMenuCloseHint[] = "F10 Close";
inline constexpr char kMenuNote[] = "Not saved to the launcher";

enum class RowKind { Toggle, Action, MovementSpeed, Experience };

// A toggle row shows Off / On for its cheat; an action row shows its verb and runs once per press; a setting row shows
// its multiplier and moves to the next one per press.
struct MenuRow {
    RowKind kind;
    Cheat cheat;
    Action action;
    const char* label;
    const char* verb;
};

inline constexpr size_t kMenuRowCount = kCheatCount + kActionCount + 2;

constexpr std::array<MenuRow, kMenuRowCount> MakeMenuRows() {
    std::array<MenuRow, kMenuRowCount> rows{};
    size_t next = 0;
    for (const CheatInfo& info : kCheats) {
        rows[next++] = {RowKind::Toggle, info.cheat, Action{}, info.label, nullptr};
        for (const ActionInfo& action : kActions) {
            if (action.after == info.cheat) {
                rows[next++] = {RowKind::Action, Cheat{}, action.action, action.label, action.verb};
            }
        }
    }
    rows[next++] = {RowKind::MovementSpeed, Cheat{}, Action{}, kMovementSpeedName, nullptr};
    rows[next] = {RowKind::Experience, Cheat{}, Action{}, kExperienceMultiplierName, nullptr};
    return rows;
}

inline constexpr std::array<MenuRow, kMenuRowCount> kMenuRows = MakeMenuRows();

// The panel is kMenuWidthEm wide. It grows with the display's content scale, a step below it (kMenuScaleStep) and never
// under kMenuMinScale, and shrinks to fit kMenuMaxWidthShare of the screen down to the 1x floor.
inline constexpr float kMenuWidthEm = 21.0f;
inline constexpr float kMenuMaxScale = 3.0f;
inline constexpr float kMenuScaleStep = 0.75f;
inline constexpr float kMenuMinScale = 1.25f;
inline constexpr float kMenuMaxWidthShare = 0.45f;

constexpr float MenuScale(float display_scale, float screen_width, float em_pixels) {
    const float display = display_scale < 1.0f ? 1.0f : (display_scale > kMenuMaxScale ? kMenuMaxScale : display_scale);
    const float stepped = display * kMenuScaleStep;
    const float wanted = stepped < kMenuMinScale ? kMenuMinScale : stepped;
    const float fits = kMenuMaxWidthShare * screen_width / (kMenuWidthEm * em_pixels);
    return wanted < fits ? wanted : (fits > 1.0f ? fits : 1.0f);
}

// The scrollable rows only: each row is row_height tall, with item_spacing between them.
constexpr float MenuBodyHeight(size_t row_count, float row_height, float item_spacing) {
    return row_count == 0 ? 0.0f : row_count * row_height + (row_count - 1) * item_spacing;
}

// The row content can be up to a pixel shorter than the calculated height because ImGui truncates layout positions.
constexpr float kMenuScrollbarTolerance = 1.0f;

constexpr bool MenuNeedsScrollbar(float body_height, float child_height) {
    return body_height > child_height + kMenuScrollbarTolerance;
}

// The least scroll movement that keeps [row_top, row_bottom] inside [view_top, view_bottom]: negative scrolls the
// list up, positive scrolls it down, and zero leaves a row that is already fully visible alone.
constexpr float MenuScrollToShowRow(float row_top, float row_bottom, float view_top, float view_bottom) {
    if (row_top < view_top) {
        return row_top - view_top;
    }
    if (row_bottom > view_bottom) {
        return row_bottom - view_bottom;
    }
    return 0.0f;
}

// A button that is never reported released (a controller unplugged as the menu closes) cannot block the game forever.
inline constexpr double kReleaseTimeoutSeconds = 1.0;

// Whether the menu may release the game's input: nothing it received is still held, or the wait ran out.
inline bool InputReleased(bool anything_held, double waited_seconds, double timeout_seconds) {
    return !anything_held || waited_seconds >= timeout_seconds;
}

// Closing the menu (B, Escape, F10) starts a wait, and the game gets its input back only after what was pressed in
// the menu is released, so the closing press never also reaches the game.
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

} // namespace dcp::cheats
