// The in-game cheat menu's model: what it lists and says, and when the game gets its input back.
#pragma once

#include <array>
#include <cstddef>

#include "dcp_cheats.h"
#include "dcp_menu_input.h"

namespace dcp::cheats {

inline constexpr char kMenuTitle[] = "CHEATS";
inline constexpr char kMenuCloseHint[] = "F10 Close";
inline constexpr char kMenuNote[] = "Not saved to the launcher";

enum class RowKind { Toggle, Action, MovementSpeed, Experience, DamageTaken };

// A toggle row shows Off / On for its cheat; an action row shows its verb and runs once per press; a setting row shows
// its multiplier and moves to the next one per press.
struct MenuRow {
    RowKind kind;
    Cheat cheat;
    Action action;
    const char* label;
    const char* verb;
};

inline constexpr size_t kMenuRowCount = kCheatCount + kActionCount + 3;

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
    rows[next++] = {RowKind::Experience, Cheat{}, Action{}, kExperienceMultiplierName, nullptr};
    rows[next] = {RowKind::DamageTaken, Cheat{}, Action{}, kDamageTakenName, nullptr};
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

enum class MenuKeyAction { None, Open, Close, OnlineHint };

// A held key repeats; only the first press of it counts. `busy` is another menu or a game dialog on screen.
constexpr MenuKeyAction MenuKeyPress(bool repeat, bool online_locked, bool open, bool busy) {
    if (repeat) {
        return MenuKeyAction::None;
    }
    if (open) {
        return MenuKeyAction::Close;
    }
    if (online_locked) {
        return MenuKeyAction::OnlineHint;
    }
    return busy ? MenuKeyAction::None : MenuKeyAction::Open;
}

// Left and right on a row of the Options menu's Cheats page: a toggle goes Off or On, a multiplier moves to its next
// value on the right only, and an action runs on a press alone.
constexpr bool StepActivates(RowKind kind, int direction, bool on) {
    switch (kind) {
    case RowKind::Toggle: return direction != 0 && (direction > 0) != on;
    case RowKind::MovementSpeed:
    case RowKind::Experience:
    case RowKind::DamageTaken: return direction > 0;
    case RowKind::Action: return false;
    }
    return false;
}

// The rows act only when the build has the cheat hooks and Online Play has not locked them. The Options menu's Cheats
// page asks the same question as the F10 menu.
constexpr bool RowsUsable(bool available, bool online_locked) {
    return available && !online_locked;
}

// The status of a row that cannot act. Online Play shows none: the page says why once.
constexpr const char* UnusableRowState(bool online_locked) {
    return online_locked ? nullptr : "Unavailable";
}

using menu::CloseGate;
using menu::InputReleased;
using menu::kReleaseTimeoutSeconds;

} // namespace dcp::cheats
