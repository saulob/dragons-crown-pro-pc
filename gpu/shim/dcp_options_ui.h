// SPDX-License-Identifier: GPL-2.0-or-later
// The Options menu's window: drawn and read once per overlay frame. The overlay owns when it opens and closes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace DcpOptionsUi {

struct CheatRowView {
    const char* label;
    const char* help;
    const char* state;
    bool lit;
};

// The Cheats page shows the F10 menu's rows and acts through the same runtime calls; the overlay supplies them.
struct CheatRows {
    size_t count;
    CheatRowView (*view)(size_t row);
    void (*activate)(size_t row, int direction); ///< 0: press, -1 and 1: left and right
    const char* (*unusable_note)();              ///< null when the rows act
    bool (*online_play)();                       ///< true under Online Play: the Online page replaces the Cheats page
};

enum class Result { Stay, Close };

void Reset();
Result Frame(const CheatRows& cheats);

// Controller events, for the L3 + R3 chord: true when the button completes the chord on that controller.
bool TrackChord(uint32_t pad, int sdl_button, bool down);
void ForgetPad(uint32_t pad);

} // namespace DcpOptionsUi
