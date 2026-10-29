// SPDX-License-Identifier: GPL-2.0-or-later
// The port's overlay (Dear ImGui): the F10 cheat menu, the game's save list and text dialogs, loading notes, the frame
// counter, the Ping and notices, drawn by the presenter into the swapchain image after the game frame.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "dcp_png.h"
#include "video_core/renderer_vulkan/vk_common.h"

union SDL_Event;

namespace Vulkan {
class Instance;
}

namespace DcpOverlay {

/// Present thread, once: the ImGui context and its Vulkan backend.
void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count);

/// Window thread, for every SDL event: true when the overlay consumed it.
bool HandleEvent(const SDL_Event& event);

/// Start-up loading screen (pipeline cache warm-up): `total` 0 shows no count.
void SetLoading(bool active, u32 done, u32 total);
/// Background cache warm-up: a small corner note while the game runs; `total` 0 hides it.
void SetBackgroundLoading(u32 done, u32 total);

/// A short message at the top of the screen for two seconds (any thread).
void ShowNotice(const std::string& text);

/// Whether anything is drawn this frame.
bool Visible();

/// Counts a presented game frame (the frame rate in the window title).
void LogPresent();
u64 PresentedFrames();

/// Present thread: draws into `view` (layout ColorAttachmentOptimal).
void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent);

/// The window's display content scale (DPI): the cheat menu grows with it.
void SetDisplayScale(float scale);

/// The cheat menu or a game dialog is shown: the game's input is held neutral.
bool CapturesInput();

/// The runtime's buttons (PS4 bits) that move and confirm the save list, polled every frame.
void SetChoiceInput(uint32_t (*buttons)());
/// Reads the save list's buttons now (also done every presented frame).
void PollChoiceInput();

/// Window thread: the game's text dialog (sceImeDialog). Shown as a box on screen until
/// `active` is false; typing goes to the window (sdl_window), the box only displays it.
void SetTextEntry(bool active, const std::string& prompt, const std::string& text);

/// The game's save list: shown until the player chooses (arrows/D-pad, Enter/Cross) or
/// cancels (Esc/Circle). `pictures`: one per item (empty = none), drawn beside its text.
/// PollChoice: -1 while open, -2 cancelled, else the chosen index.
void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<DcpPng::Image> pictures, int focus);
int PollChoice();

} // namespace DcpOverlay
