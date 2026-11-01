// The launcher in its own window, for games that start without Dear ImGui
// (the SMS port): SDL2 + OpenGL 3.3, closed again before the game opens its
// own window. Built when TRG_LAUNCHER_STANDALONE is on.
#pragma once

#include <functional>
#include <string>

#include "trg/launcher.h"

namespace trg {

struct WindowOptions {
  std::string title = "Launcher";
  int width = 1180, height = 800;          // shrunk to fit the screen
  int min_width = 900, min_height = 600;
  bool load_system_fonts = true;           // when config().fonts.regular is not set
  bool vsync = true;
};

struct StandaloneHooks {
  // The window, GL context and ImGui context exist: load textures, set
  // branding.icon, add fonts.
  std::function<void(Launcher&)> on_start;
  // Before anything is torn down: free textures.
  std::function<void(Launcher&)> on_stop;
  // After each frame is drawn, before it is shown (framebuffer size in pixels).
  std::function<void(Launcher&, int width, int height)> after_render;
};

// Runs the launcher until Play or Quit. When no window can be opened, returns
// kPlay so the game still starts.
Result RunStandalone(Launcher& launcher, const WindowOptions& options = {}, const StandaloneHooks& hooks = {});

// An OpenGL texture from RGBA8 pixels, for header art and icons. Only valid
// while the standalone window is open (create it in on_start).
ImTextureID CreateTextureRGBA(int width, int height, const void* rgba);
void DestroyTexture(ImTextureID texture);

}  // namespace trg
