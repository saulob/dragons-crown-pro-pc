// The window's explanation of why the game cannot start, drawn with Dear ImGui: no system dialog, no Vulkan.
#pragma once

#include <string>

#include "trg/launcher.h"

namespace dcp {

class ProblemDialog {
public:
    void Show(std::string cause, std::string advice);
    bool open() const { return open_; }

    // Call every frame between BeginFrame and EndFrame; the dialog is modal while it is open.
    void Draw(const trg::Fonts& fonts, const trg::Theme& theme);

private:
    std::string cause_;
    std::string advice_;
    bool open_ = false;
    bool opened_ = false;
};

} // namespace dcp
