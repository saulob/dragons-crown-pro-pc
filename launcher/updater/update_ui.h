#pragma once

#include "trg/launcher.h"

#include "updater/update_controller.h"

namespace trg::update {

enum class UpdateUiAction { kNone, kDownload, kRetry, kRestart };

UpdateUiAction DrawUpdateBanner(const UpdateSnapshot& snapshot, const trg::Theme& theme, const trg::Fonts& fonts);

}  // namespace trg::update
