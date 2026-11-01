// Xbox 360 disc images (XDVDFS), for "install from your own disc" pages.
// Handles plain XISO and XGD2/XGD3 "redump" images. From King Kong Recompiled.
#pragma once

#include <cstdint>
#include <string>

namespace trg {

class Task;

// The title ID in the image's default.xex (its execution info header), or 0
// when the file is not an Xbox 360 disc image or has no readable default.xex.
uint32_t ReadXbox360TitleId(const std::string& utf8_image);

// Copies every file on the disc into `out_dir` on a Task, reporting progress
// in bytes and honouring Cancel. Returns "" on success, else an error message.
// For example:
//   task.Start("Installing", [=](trg::Task& t) { return trg::ExtractXbox360Disc(t, iso, game_dir); });
std::string ExtractXbox360Disc(Task& task, const std::string& utf8_image, const std::string& utf8_out_dir);

}  // namespace trg
