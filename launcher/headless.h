// The executable's headless commands, for front ends: --install <package or extracted game folder> and --check-game.
// They print to the console and never open a window, a dialog or the game.
#ifndef DCP_HEADLESS_H
#define DCP_HEADLESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The exit code of a headless command in argv (0 ready, 1 failed or not ready, 2 invalid arguments); -1 when argv holds
 * none and the program should go on. */
int dcp_headless_run(int argc, char **argv);

#ifdef __cplusplus
}

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "game_files.h"
#include "game_install.h"

namespace dcp {

inline constexpr int kHeadlessOk = 0;
inline constexpr int kHeadlessFailed = 1;
inline constexpr int kHeadlessUsage = 2;

enum class HeadlessKind { kNone, kInstall, kCheckGame, kInvalid };

struct HeadlessCommand {
    HeadlessKind kind = HeadlessKind::kNone;
    std::string source;
    std::string error;
};

// The arguments after the program name. kNone when neither command is named.
HeadlessCommand ParseHeadlessArguments(std::span<const std::string> args);

struct HeadlessOutput {
    std::function<void(std::string_view line)> out;
    std::function<void(std::string_view line)> err;
};

// Where the active build's folders are; false with the reason when they cannot be located.
using HeadlessPaths = std::function<bool(GamePaths& paths, std::string& error)>;

int InstallHeadless(const std::filesystem::path& source, const GameRoots& roots, const HeadlessOutput& output,
                    std::span<const GameBuild> builds = SupportedBuilds());
// Read-only: nothing is written, repaired or built.
int CheckGameHeadless(const GamePaths& paths, const HeadlessOutput& output,
                      std::span<const GameBuild> builds = SupportedBuilds());

// -1 when `args` holds no headless command.
int RunHeadless(std::span<const std::string> args, const HeadlessOutput& output, const HeadlessPaths& locate,
                std::span<const GameBuild> builds = SupportedBuilds());

} // namespace dcp
#endif

#endif
