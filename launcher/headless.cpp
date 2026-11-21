#include "headless.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

#include "dcp_launcher.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace dcp {
namespace {

constexpr char kUsage[] = "Usage: dragons_crown_pro --install <PKG or extracted game folder> | dragons_crown_pro --check-game";

bool IsHeadlessOption(const std::string& argument) { return argument == "--install" || argument == "--check-game"; }

HeadlessCommand Invalid(std::string error) {
    HeadlessCommand command;
    command.kind = HeadlessKind::kInvalid;
    command.error = std::move(error);
    return command;
}

std::string WithoutEllipsis(std::string text) {
    while (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string Megabytes(uint64_t bytes) { return std::to_string(bytes >> 20); }

// Reports every tenth of a phase; each install has its own state.
struct ProgressLines {
    const HeadlessOutput* output;
    int last_step = -1;
};

std::string BuildName(const InstallResult& result) {
    if (result.build) {
        return result.build->label;
    }
    return result.detail;
}

void ReportFailure(const InstallResult& result, const HeadlessOutput& output) {
    const InstallMessage message = DescribeInstall(result.status);
    if (result.status == InstallStatus::kUnsupportedBuild) {
        output.err("Install: failed: " + message.title);
        for (const std::string& line : result.diagnostics) {
            output.err(line);
        }
        return;
    }
    output.err("Install: failed: " + message.title + ". " + message.text);
    if (!result.detail.empty()) {
        output.err("Install: " + result.detail);
    }
}

} // namespace

HeadlessCommand ParseHeadlessArguments(std::span<const std::string> args) {
    HeadlessCommand command;
    size_t named = args.size();
    for (size_t i = 0; i < args.size(); ++i) {
        if (IsHeadlessOption(args[i])) {
            named = i;
            break;
        }
    }
    if (named == args.size()) {
        return command;
    }
    if (named != 0) {
        return Invalid(args[named] + " cannot be combined with other arguments");
    }
    if (args[0] == "--check-game") {
        if (args.size() > 1) {
            return Invalid("--check-game takes no other arguments, got " + args[1]);
        }
        command.kind = HeadlessKind::kCheckGame;
        return command;
    }
    if (args.size() < 2) {
        return Invalid("--install needs the PKG or the extracted game folder");
    }
    if (args.size() > 2) {
        return Invalid("--install takes one source, got an extra argument " + args[2]);
    }
    if (args[1].empty() || args[1].starts_with("--")) {
        return Invalid("--install needs the PKG or the extracted game folder, got " + (args[1].empty() ? "nothing" : args[1]));
    }
    command.kind = HeadlessKind::kInstall;
    command.source = args[1];
    return command;
}

int InstallHeadless(const fs::path& source, const GameRoots& roots, const HeadlessOutput& output,
                    std::span<const GameBuild> builds) {
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        output.err("Install: failed: " + Utf8(source) + " does not exist");
        return kHeadlessFailed;
    }
    const auto progress = std::make_shared<ProgressLines>(ProgressLines{&output});
    InstallHooks hooks;
    hooks.phase = [progress](const char* phase) {
        progress->last_step = -1;
        progress->output->out("Install: " + WithoutEllipsis(phase));
    };
    hooks.progress = [progress](uint64_t done, uint64_t total) {
        if (total == 0) {
            return true;
        }
        const int step = static_cast<int>(done * 10 / total);
        if (step > progress->last_step) {
            progress->last_step = step;
            if (step > 0) {
                progress->output->out("Install: " + std::to_string(step * 10) + "% " + Megabytes(done) + " / " +
                                      Megabytes(total) + " MB");
            }
        }
        return true;
    };
    const InstallResult result = InstallGame(source, roots, hooks, builds);
    if (!result.ok()) {
        ReportFailure(result, output);
        return kHeadlessFailed;
    }
    for (const std::string& line : result.site_report) {
        output.out(line);
    }
    const char* note = result.status == InstallStatus::kUpToDate    ? " (already installed)"
                       : result.status == InstallStatus::kActivated ? " (activated the installed build)"
                                                                    : "";
    output.out("Install: ready " + BuildName(result) + note);
    return kHeadlessOk;
}

int CheckGameHeadless(const GamePaths& paths, const HeadlessOutput& output, std::span<const GameBuild> builds) {
    const GameCheck check = CheckGameFiles(paths, builds, true);
    if (check.state != GameFileState::kValid || !check.build) {
        output.out("Game files: not ready");
        if (check.build) {
            output.out(std::string("Build: ") + check.build->label);
        }
        output.out(std::string("State: ") + StateName(check.state));
        if (!check.detail.empty()) {
            output.out("Reason: " + check.detail);
        }
        return kHeadlessFailed;
    }
    output.out("Game files: ready");
    output.out(std::string("Build: ") + check.build->label);
    output.out(std::string("Title ID: ") + check.build->title_id);
    output.out(std::string("Version: ") + check.build->version);
    return kHeadlessOk;
}

int RunHeadless(std::span<const std::string> args, const HeadlessOutput& output, const HeadlessPaths& locate,
                std::span<const GameBuild> builds) {
    const HeadlessCommand command = ParseHeadlessArguments(args);
    if (command.kind == HeadlessKind::kNone) {
        return -1;
    }
    if (command.kind == HeadlessKind::kInvalid) {
        output.err(command.error);
        output.err(kUsage);
        return kHeadlessUsage;
    }
    if (command.kind == HeadlessKind::kInstall) {
        if (const char* pinned = std::getenv(kPinnedBuildVariable); pinned && *pinned) {
            output.err(std::string("Install: failed: ") + kPinnedBuildVariable + " pins the game build, so installing is off");
            return kHeadlessFailed;
        }
    }
    GamePaths paths;
    std::string error;
    if (!locate(paths, error)) {
        const bool install = command.kind == HeadlessKind::kInstall;
        (install ? output.err : output.out)(std::string(install ? "Install: failed" : "Game files: not ready") +
                                            ": cannot locate the game folders" + (error.empty() ? "" : ": " + error));
        return kHeadlessFailed;
    }
    if (command.kind == HeadlessKind::kCheckGame) {
        return CheckGameHeadless(paths, output, builds);
    }
    return InstallHeadless(PathFromUtf8(command.source), RootsOf(paths), output, builds);
}

} // namespace dcp

namespace {

#ifdef _WIN32
// The executable has no console of its own: print to the one it was started from when nothing is redirected.
void AttachParentConsole() {
    const auto usable = [](HANDLE handle) { return handle && handle != INVALID_HANDLE_VALUE; };
    const bool out = usable(GetStdHandle(STD_OUTPUT_HANDLE)), err = usable(GetStdHandle(STD_ERROR_HANDLE));
    if ((out && err) || !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    if (!out) {
        std::freopen("CONOUT$", "w", stdout);
    }
    if (!err) {
        std::freopen("CONOUT$", "w", stderr);
    }
}
#endif

bool LocateActivePaths(dcp::GamePaths& paths, std::string& error) {
    static DcpPaths resolved;
    const DcpPathRequest request = {nullptr, nullptr, nullptr, nullptr, nullptr};
    if (!dcp_resolve_paths(&request, &resolved)) {
        error = resolved.error;
        return false;
    }
    paths.app0 = dcp::PathFromUtf8(resolved.app0);
    paths.image_dir = dcp::PathFromUtf8(resolved.image_dir);
    return true;
}

} // namespace

extern "C" int dcp_headless_run(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    if (dcp::ParseHeadlessArguments(args).kind == dcp::HeadlessKind::kNone) {
        return -1;
    }
#ifdef _WIN32
    AttachParentConsole();
#endif
    const auto line = [](FILE* stream) {
        return [stream](std::string_view text) {
            std::fwrite(text.data(), 1, text.size(), stream);
            std::fputc('\n', stream);
            std::fflush(stream);
        };
    };
    return dcp::RunHeadless(args, {line(stdout), line(stderr)}, LocateActivePaths);
}
