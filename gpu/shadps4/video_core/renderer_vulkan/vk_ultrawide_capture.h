// SPDX-License-Identifier: GPL-2.0-or-later
// DEBUG_SAULO: temporary F9 ultrawide frame capture. One press records the next whole frame, flip to flip: every
// draw with its target, viewport, scissors, shaders, textures, blend state and screen footprint, matched to the guest
// pass and render command that emitted it (src/ultrawide_capture.c), a picture at every guest pass boundary and
// pictures of the draws that reach the ultrawide side areas or cover much of the target. Written under
// <logs>/ultrawide-capture/capture-NNN; the Rasterizer::Capture* members are in vk_ultrawide_capture.cpp.

#pragma once

#include <atomic>

#include "dcpgpu.h"

namespace UltrawideCapture {

/// Armed or recording: every draw is recorded on the GPU command thread, without the draw pipeline or the
/// texture set memo, so each one is seen with its packet and textures.
inline std::atomic<bool> active{false};
inline bool Active() {
    return active.load(std::memory_order_relaxed);
}

void Configure(const char* logs_dir, const char* ultrawide_line, bool guest_hooks);
/// Window thread: F9 went down.
void Press(bool repeat);
bool GuestWanted();
void Queued(const DcpCaptureQueued& queued);
void Executed(const DcpCaptureExecuted& executed);

} // namespace UltrawideCapture
