#pragma once

namespace dcp {

// Escape cancels pop-ups and the keyboard capture, so a TRG Quit that coincides with an Escape press
// is ignored. The Quit button, Alt+F4 and the window close button quit without Escape.
inline bool LauncherQuitAllowed(bool escape_pressed) { return !escape_pressed; }

} // namespace dcp
