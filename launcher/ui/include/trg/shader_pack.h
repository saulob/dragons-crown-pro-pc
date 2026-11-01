// Shader packs for ReXGlue ports (from King Kong Recompiled).
//
// ReXGlue saves every shader and pipeline a game has used in its cache
// (<cache>/shaders/shareable/<TITLE>.xsh and <TITLE>.<path>.d3d12.xpso) and
// prepares all of them each time the game starts. A shader pack is those
// files, collected by playing the game, published as release assets with a
// small manifest:
//
//   shader-pack.txt:   version=3
//                      555307D3.xsh
//                      555307D3.rtv.d3d12.xpso
//
// Installing merges the pack into the player's cache by hash, keeping anything
// already there, so a first play-through doesn't pause for new effects. The
// pack holds Xbox 360 shader code and pipeline descriptions, not GPU-specific
// binaries: every player's own driver still compiles them.
#pragma once

#include <string>

#include "trg/platform.h"

namespace trg {

class Ui;

// Installed pack version (0 when none), from <cache_dir>/shaders/shader-pack.txt.
int InstalledShaderPackVersion(const std::string& utf8_cache_dir);

// Merges one storage file (.xsh or .xpso) into another, adding only records it
// lacks; copies it when `into` doesn't exist yet. Returns how many records were
// added, or -1 when the files come from a different runtime version.
int MergeShaderStorageFile(const std::string& utf8_from, const std::string& utf8_into);

// Task job: downloads <base_url>shader-pack.txt and the files it lists, then
// merges them into the cache. `message` (read after the Task finishes) says
// what happened in words for the player.
std::string InstallShaderPack(Task& task, const std::string& base_url, const std::string& utf8_cache_dir,
                              std::string* message);

// "Shader pack: [Download shader pack]" row, or "[Check for a newer pack]"
// once one is installed, with progress and the result underneath.
struct ShaderPackRowState {
  Task task;
  std::string message;
  int installed = -1;  // -1: not read yet
};
void ShaderPackRow(Ui& ui, ShaderPackRowState& state, const std::string& base_url, const std::string& utf8_cache_dir,
                   const char* help = nullptr);

}  // namespace trg
