#include "game_install.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <chrono>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "boot_image.h"
#include "pkg_extract.h"
#include "utf8.h"

namespace dcp {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSpaceMargin = 256ull << 20;
constexpr int kRenameAttempts = 10;

InstallResult Failure(InstallStatus status, std::string detail = {}) {
    InstallResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

fs::path WithSuffix(const fs::path& dir, const char* suffix) {
    return dir.parent_path() / (dir.filename().native() + PathFromUtf8(suffix).native());
}

bool IsWithin(const fs::path& child, const fs::path& parent) {
    const fs::path relative = child.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}

fs::path Normal(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    return (ec ? path : absolute).lexically_normal();
}

// The antivirus scanner can hold a freshly written folder for a moment.
bool RenameWithRetry(const fs::path& from, const fs::path& to) {
    for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
        std::error_code ec;
        fs::rename(from, to, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    return false;
}

// Hard links need both paths on one volume: the drive on Windows, the device of the nearest existing folder elsewhere.
bool SameVolume(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return a.root_name() == b.root_name();
#else
    const auto device = [](fs::path path) {
        std::error_code ec;
        while (!path.empty() && !fs::exists(path, ec)) {
            path = path.parent_path();
        }
        struct stat info;
        return !path.empty() && stat(path.c_str(), &info) == 0 ? std::optional<dev_t>(info.st_dev) : std::nullopt;
    };
    const auto first = device(a), second = device(b);
    return first && second && *first == *second;
#endif
}

// An existing folder is only replaced when it is empty or already holds what the installer writes there.
bool IsReplaceable(const fs::path& dir, const char* own_file) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return true;
    }
    return fs::is_directory(dir, ec) && (fs::is_empty(dir, ec) || fs::exists(dir / own_file, ec));
}

class StagingGuard {
public:
    explicit StagingGuard(fs::path dir) : dir_(std::move(dir)) {}
    ~StagingGuard() {
        if (armed_) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
        }
    }
    void Release() { armed_ = false; }

private:
    fs::path dir_;
    bool armed_ = true;
};

struct SourceFile {
    fs::path relative;
    uint64_t size;
};

// Skipped as scripts/game_view.py skips them: extraction leftovers and an earlier preparation's marker.
bool IsSkipped(const fs::path& name) {
    const std::string text = Utf8(name);
    return text == ".pkg_extracted" || text == kPreparedMarker || (text.size() > 5 && text.ends_with(".part"));
}

std::string JsonQuote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

struct CopyContext {
    const InstallHooks* hooks;
    uint64_t done, total;
    bool cancelled;
};

enum class Transfer { kLinked, kCopied, kCancelled, kFailed };

#ifdef _WIN32
DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                            HANDLE, HANDLE, LPVOID data) {
    auto* context = static_cast<CopyContext*>(data);
    if (context->hooks->progress &&
        !context->hooks->progress(context->done + uint64_t(transferred.QuadPart), context->total)) {
        context->cancelled = true;
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}

// A hard link costs no space; another drive gets a copy.
Transfer TransferFile(const fs::path& from, const fs::path& to, CopyContext& copy, std::string& error) {
    if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) {
        return Transfer::kLinked;
    }
    if (CopyFileExW(from.c_str(), to.c_str(), CopyProgress, &copy, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
        return Transfer::kCopied;
    }
    error = "error " + std::to_string(GetLastError());
    return copy.cancelled ? Transfer::kCancelled : Transfer::kFailed;
}
#else
Transfer TransferFile(const fs::path& from, const fs::path& to, CopyContext& copy, std::string& error) {
    std::error_code ec;
    fs::create_hard_link(from, to, ec);
    if (!ec) {
        return Transfer::kLinked;
    }
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        error = "cannot open the file";
        return Transfer::kFailed;
    }
    std::vector<char> block(1u << 20);
    uint64_t transferred = 0;
    while (in.read(block.data(), std::streamsize(block.size())).gcount() > 0) {
        out.write(block.data(), in.gcount());
        transferred += uint64_t(in.gcount());
        if (!out) {
            error = "cannot write the file";
            return Transfer::kFailed;
        }
        if (copy.hooks->progress && !copy.hooks->progress(copy.done + transferred, copy.total)) {
            copy.cancelled = true;
            return Transfer::kCancelled;
        }
    }
    if (!in.eof() || !out.flush()) {
        error = "cannot copy the file";
        return Transfer::kFailed;
    }
    return Transfer::kCopied;
}
#endif

InstallStatus FromCheck(const GameCheck& check) {
    switch (check.state) {
    case GameFileState::kValid: return InstallStatus::kInstalled;
    case GameFileState::kMissing: return InstallStatus::kIncomplete;
    case GameFileState::kReadError: return InstallStatus::kReadFailed;
    case GameFileState::kDifferentVersion:
        return check.detail.starts_with("param.sfo TITLE_ID") || check.detail.starts_with("param.sfo CONTENT_ID")
                   ? InstallStatus::kWrongGame
                   : InstallStatus::kWrongVersion;
    }
    return InstallStatus::kIncomplete;
}

// The folders the installer writes: the prepared game and the boot image, each with a staging and a backup folder.
struct Layout {
    explicit Layout(const GamePaths& target)
        : app0(Normal(target.app0)), image(Normal(target.image_dir)), app0_stage(WithSuffix(app0, ".install-temp")),
          app0_backup(WithSuffix(app0, ".install-backup")), image_stage(WithSuffix(image, ".install-temp")),
          image_backup(WithSuffix(image, ".install-backup")) {}

    bool Overlaps(const fs::path& source) const {
        for (const fs::path& own : {app0, app0_stage, app0_backup, image, image_stage, image_backup}) {
            if (source == own || IsWithin(source, own) || IsWithin(own, source)) {
                return true;
            }
        }
        return false;
    }

    fs::path app0, image, app0_stage, app0_backup, image_stage, image_backup;
};

// A source inside, or holding, the folders of any supported build is refused before it is read.
bool OverlapsAnyBuild(const fs::path& source, const GameRoots& roots, std::span<const GameBuild> builds) {
    for (const GameBuild& build : builds) {
        if (Layout(BuildPaths(roots, build)).Overlaps(source)) {
            return true;
        }
    }
    return false;
}

// Creates the parent folders, checks the free space and clears old staging folders.
InstallResult PrepareStaging(const Layout& layout, uint64_t needed) {
    std::error_code ec;
    for (const fs::path& dir : {layout.app0.parent_path(), layout.image.parent_path()}) {
        fs::create_directories(dir, ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(dir) + ": " + ec.message());
        }
    }
    const fs::space_info space = fs::space(layout.app0.parent_path(), ec);
    if (!ec && needed && space.available < needed + kSpaceMargin) {
        return Failure(InstallStatus::kNoSpace,
                       "needs " + std::to_string(needed) + " bytes, " + std::to_string(space.available) + " available");
    }
    for (const fs::path& dir : {layout.app0_stage, layout.image_stage}) {
        fs::remove_all(dir, ec);
        if (ec || fs::exists(dir, ec)) {
            return Failure(InstallStatus::kWriteFailed, "cannot clear " + Utf8(dir));
        }
    }
    return {};
}

// The marker scripts/game_view.py writes, so the development tools see an installed game as prepared. A marker that is
// a link into an installed game is removed, not written through.
bool WriteMarker(const fs::path& app0, const fs::path& source, size_t linked, size_t copied, const GameBuild& build,
                 const char* scope) {
    std::error_code ec;
    fs::remove(app0 / kPreparedMarker, ec);
    std::ofstream marker(app0 / kPreparedMarker, std::ios::binary | std::ios::trunc);
    marker << "{\n  \"layers\": [\n    " << JsonQuote(Utf8(source)) << "\n  ],\n  \"description\": \""
           << build.title_id << ' ' << build.version << "\",\n  \"link\": " << linked << ",\n  \"copy\": " << copied
           << ",\n  \"replaced\": 0,\n  \"version\": \"" << build.version << "\",\n  \"scope\": \"" << scope << "\"\n}";
    return bool(marker.flush());
}

InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard);

// Builds the boot image from the staged game, checks both and swaps them in. Until the swap the current installation
// is untouched; a failed swap puts it back.
InstallResult BuildAndActivate(const Layout& layout, StagingGuard& app0_guard, StagingGuard& image_guard,
                               const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build,
                               uint64_t total) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    phase("Preparing the game...");
    if (const std::string error = BuildBootImage(layout.app0_stage, layout.image_stage); !error.empty()) {
        return Failure(InstallStatus::kPrepareFailed, error);
    }

    phase("Verifying the installation...");
    const GameCheck installed = CheckGameFiles({layout.app0_stage, layout.image_stage}, builds);
    if (installed.state != GameFileState::kValid || installed.build != &build) {
        return Failure(InstallStatus::kPrepareFailed, installed.detail);
    }
    if (hooks.progress && !hooks.progress(total, total)) {
        return Failure(InstallStatus::kCancelled);
    }
    phase("Finishing...");
    return Activate(layout, app0_guard, &image_guard);
}

// Puts the staged folders in place of the installed ones; without an image the installed image is removed, since it
// was built from another installation. A failed step puts the installed folders back.
InstallResult Activate(const Layout& layout, StagingGuard& app0_guard, StagingGuard* image_guard) {
    std::error_code ec;
    const fs::path &app0 = layout.app0, &image = layout.image;
    const bool had_app0 = fs::exists(app0, ec), had_image = fs::exists(image, ec);
    fs::remove_all(layout.app0_backup, ec);
    fs::remove_all(layout.image_backup, ec);
    if (had_app0 && !RenameWithRetry(app0, layout.app0_backup)) {
        return Failure(InstallStatus::kWriteFailed, "cannot move aside " + Utf8(app0));
    }
    if (!RenameWithRetry(layout.app0_stage, app0)) {
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(app0));
    }
    const auto restore_app0 = [&] {
        RenameWithRetry(app0, layout.app0_stage);
        if (had_app0) {
            RenameWithRetry(layout.app0_backup, app0);
        }
    };
    if (had_image && !RenameWithRetry(image, layout.image_backup)) {
        restore_app0();
        return Failure(InstallStatus::kWriteFailed, "cannot move aside " + Utf8(image));
    }
    if (image_guard && !RenameWithRetry(layout.image_stage, image)) {
        if (had_image) {
            RenameWithRetry(layout.image_backup, image);
        }
        restore_app0();
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(image));
    }
    app0_guard.Release();
    if (image_guard) {
        image_guard->Release();
    }
    fs::remove_all(layout.app0_backup, ec);
    fs::remove_all(layout.image_backup, ec);
    return {};
}

// Lays the files of `from` into `to` as links (copies across volumes); the installed game is not changed.
InstallResult LinkTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    std::vector<SourceFile> files;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            const uint64_t size = it->file_size(ec);
            files.push_back({it->path().lexically_relative(from), size});
            total += size;
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(from) + ": " + ec.message());
    }
    const InstallHooks no_progress;
    CopyContext copy{&no_progress, 0, total, false};
    for (const SourceFile& file : files) {
        fs::create_directories((to / file.relative).parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to / file.relative.parent_path()));
        }
        std::string error;
        if (TransferFile(from / file.relative, to / file.relative, copy, error) == Transfer::kFailed) {
            return Failure(InstallStatus::kWriteFailed, "cannot copy " + Utf8(file.relative) + " (" + error + ")");
        }
        copy.done += file.size;
    }
    return {};
}

// Lays the files of an unpacked update over the staged game. A staged file is replaced, never written through: it may
// be a link into the installed game.
InstallResult OverlayUpdate(const fs::path& update, const fs::path& staged) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(update, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path());
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(update) + ": " + ec.message());
    }
    for (const fs::path& from : files) {
        const fs::path to = staged / from.lexically_relative(update);
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to.parent_path()));
        }
        fs::remove(to, ec);
        fs::rename(from, to, ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot place " + Utf8(to));
        }
    }
    return {};
}

InstallStatus FromPackage(PackageResult::Code code) {
    switch (code) {
    case PackageResult::kOk: return InstallStatus::kInstalled;
    case PackageResult::kNotPackage: return InstallStatus::kNotPackage;
    case PackageResult::kUnsupported: return InstallStatus::kUnsupportedPackage;
    case PackageResult::kIncomplete: return InstallStatus::kIncomplete;
    case PackageResult::kReadFailed: return InstallStatus::kReadFailed;
    case PackageResult::kWriteFailed: return InstallStatus::kWriteFailed;
    case PackageResult::kCancelled: return InstallStatus::kCancelled;
    }
    return InstallStatus::kReadFailed;
}

// The build the launcher runs, when its files still pass the readiness check.
const GameBuild* ActiveValidBuild(const GameRoots& roots, std::span<const GameBuild> builds) {
    const GameBuild* active = LastPreparedBuild(roots, builds);
    if (!active) {
        return nullptr;
    }
    const GameCheck check = CheckGameFiles(BuildPaths(roots, *active), builds);
    return check.state == GameFileState::kValid && check.build == active ? active : nullptr;
}

// Makes a complete build the newest prepared one, which is how ActiveBuildPaths picks the active build. Only the small
// marker is written.
bool ActivatePrepared(const fs::path& app0) {
    std::error_code ec;
    fs::last_write_time(app0 / kPreparedMarker, fs::file_time_type::clock::now(), ec);
    return !ec;
}

// A selected build that needs no copying. The active build itself is up to date. A complete folder of any other build
// that passes the readiness check becomes the active one, whether it is older or newer. Nothing is returned when the
// build has to be installed.
std::optional<InstallResult> ReuseInstalledBuild(const GameRoots& roots, std::span<const GameBuild> builds,
                                                 const GameBuild& build, bool replace_active) {
    if (ActiveValidBuild(roots, builds) == &build) {
        if (replace_active) {
            return std::nullopt;
        }
        return Failure(InstallStatus::kUpToDate, build.label);
    }
    const GamePaths target = BuildPaths(roots, build);
    const GameCheck prepared = CheckGameFiles(target, builds);
    if (prepared.state != GameFileState::kValid || prepared.build != &build) {
        return std::nullopt;
    }
    if (!ActivatePrepared(target.app0)) {
        return Failure(InstallStatus::kWriteFailed, "cannot activate " + Utf8(target.app0));
    }
    InstallResult result;
    result.status = InstallStatus::kActivated;
    result.build = &build;
    return result;
}

// A package that finishes its build by itself: the base package of a build without updates.
InstallResult InstallBasePackage(const fs::path& source, Package& package, const Layout& layout,
                                 const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (const PackageResult loaded = package.LoadFiles(); !loaded.ok()) {
        return Failure(FromPackage(loaded.code), loaded.detail);
    }
    const uint64_t total = package.total_bytes();
    if (InstallResult prepared = PrepareStaging(layout, total); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    phase("Unpacking the game files...");
    if (const PackageResult extracted = package.Extract(layout.app0_stage, hooks.progress); !extracted.ok()) {
        return Failure(FromPackage(extracted.code), extracted.detail);
    }
    if (!WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    phase("Checking the game files...");
    const GameCheck unpacked = CheckGameTree(layout.app0_stage, builds);
    if (unpacked.state != GameFileState::kValid || unpacked.build != &build) {
        return Failure(FromCheck(unpacked), unpacked.detail);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

// An update laid over the installed base build of its title, staged and checked like any other installation.
InstallResult InstallUpdatePackage(const fs::path& source, Package& package, const Layout& layout, const GameRoots& roots,
                                   const InstallHooks& hooks, std::span<const GameBuild> builds, const GameBuild& build) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const GameBuild* base_build = BaseBuildOf(build, builds);
    if (!base_build) {
        return Failure(InstallStatus::kWrongVersion, std::string("no base package for ") + build.label);
    }
    const GamePaths base = BuildPaths(roots, *base_build);
    if (const GameCheck base_check = CheckGameTree(base.app0, builds);
        base_check.state != GameFileState::kValid || base_check.build != base_build) {
        return Failure(InstallStatus::kUpdateNeedsBase, base_check.detail);
    }
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }
    if (const PackageResult loaded = package.LoadFiles(); !loaded.ok()) {
        return Failure(FromPackage(loaded.code), loaded.detail);
    }
    const uint64_t total = package.total_bytes();
    if (InstallResult prepared = PrepareStaging(layout, 0); !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    const fs::path update_dir = WithSuffix(layout.app0, ".install-update");
    std::error_code ec;
    fs::remove_all(update_dir, ec);
    phase("Unpacking the update...");
    if (const PackageResult extracted = package.Extract(update_dir, hooks.progress); !extracted.ok()) {
        fs::remove_all(update_dir, ec);
        return Failure(FromPackage(extracted.code), extracted.detail);
    }
    phase("Copying the game files...");
    InstallResult staged = StageUpdate(base.app0, update_dir, layout.app0_stage);
    fs::remove_all(update_dir, ec);
    if (!staged.ok()) {
        return staged;
    }
    if (!WriteMarker(layout.app0_stage, source, 0, package.file_count(), build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    phase("Checking the game files...");
    const GameCheck merged = CheckGameTree(layout.app0_stage, builds);
    if (merged.state != GameFileState::kValid || merged.build != &build) {
        return Failure(FromCheck(merged), merged.detail);
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = package.file_count();
    result.build = &build;
    return result;
}

} // namespace

InstallResult StageUpdate(const fs::path& base_app0, const fs::path& update_dir, const fs::path& staged_app0) {
    if (InstallResult linked = LinkTree(base_app0, staged_app0); !linked.ok()) {
        return linked;
    }
    return OverlayUpdate(update_dir, staged_app0);
}

InstallResult InstallGame(const fs::path& source, const GameRoots& roots, const InstallHooks& hooks,
                          std::span<const GameBuild> builds) {
    std::error_code ec;
    return fs::is_directory(source, ec) ? InstallFromFolder(source, roots, hooks, builds)
                                        : InstallFromPackage(source, roots, hooks, builds);
}

InstallResult InstallFromPackage(const fs::path& package_file, const GameRoots& roots, const InstallHooks& hooks,
                                 std::span<const GameBuild> builds) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    const fs::path source = Normal(package_file);

    phase("Checking the package...");
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return Failure(InstallStatus::kNotPackage, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    Package package;
    if (const PackageResult opened = package.Open(source); !opened.ok()) {
        return Failure(FromPackage(opened.code), opened.detail);
    }
    const GameCheck identity = CheckPackageIdentity(package.sfo(), builds);
    if (identity.state != GameFileState::kValid) {
        return Failure(FromCheck(identity), identity.detail);
    }
    const GameBuild& build = *identity.build;
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, hooks.replace_active)) {
        return *reused;
    }
    const Layout layout(BuildPaths(roots, build));
    if (identity.layer > 0) {
        return InstallUpdatePackage(source, package, layout, roots, hooks, builds, build);
    }
    return InstallBasePackage(source, package, layout, hooks, builds, build);
}

InstallResult InstallFromFolder(const fs::path& source_dir, const GameRoots& roots, const InstallHooks& hooks,
                                std::span<const GameBuild> builds) {
    const auto phase = [&](const char* text) {
        if (hooks.phase) {
            hooks.phase(text);
        }
    };
    std::error_code ec;
    const fs::path source = Normal(source_dir);

    phase("Checking the game folder...");
    if (!fs::is_regular_file(source / "eboot.bin", ec) || !fs::is_regular_file(source / "sce_sys" / "param.sfo", ec)) {
        return Failure(InstallStatus::kNotGameFolder, Utf8(source));
    }
    if (OverlapsAnyBuild(source, roots, builds)) {
        return Failure(InstallStatus::kOverlapping, Utf8(source));
    }
    const GameCheck check = CheckGameTree(source, builds);
    if (check.state != GameFileState::kValid) {
        return Failure(FromCheck(check), check.detail);
    }
    const GameBuild& build = *check.build;
    if (std::optional<InstallResult> reused = ReuseInstalledBuild(roots, builds, build, hooks.replace_active)) {
        return *reused;
    }
    const Layout layout(BuildPaths(roots, build));
    if (!IsReplaceable(layout.app0, kPreparedMarker) || !IsReplaceable(layout.image, "boot.bin")) {
        return Failure(InstallStatus::kForeignFolder, Utf8(layout.app0) + " or " + Utf8(layout.image));
    }

    std::vector<SourceFile> files;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (IsSkipped(it->path().filename())) {
            if (it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(ec)) {
            const uint64_t size = it->file_size(ec);
            files.push_back({it->path().lexically_relative(source), size});
            total += size;
        }
    }
    if (ec) {
        return Failure(InstallStatus::kReadFailed, "cannot list " + Utf8(source) + ": " + ec.message());
    }

    // The same drive gets hard links, which need no space.
    if (InstallResult prepared = PrepareStaging(layout, SameVolume(source, layout.app0) ? 0 : total);
        !prepared.ok()) {
        return prepared;
    }
    StagingGuard app0_guard(layout.app0_stage), image_guard(layout.image_stage);

    phase("Copying the game files...");
    CopyContext copy{&hooks, 0, total, false};
    size_t linked = 0;
    for (const SourceFile& file : files) {
        const fs::path from = source / file.relative, to = layout.app0_stage / file.relative;
        fs::create_directories(to.parent_path(), ec);
        if (ec) {
            return Failure(InstallStatus::kWriteFailed, "cannot create " + Utf8(to.parent_path()) + ": " + ec.message());
        }
        std::string error;
        switch (TransferFile(from, to, copy, error)) {
        case Transfer::kLinked: ++linked; break;
        case Transfer::kCopied: break;
        case Transfer::kCancelled: return Failure(InstallStatus::kCancelled);
        case Transfer::kFailed:
            return Failure(InstallStatus::kWriteFailed, "cannot copy " + Utf8(file.relative) + " (" + error + ")");
        }
        copy.done += file.size;
        if (hooks.progress && !hooks.progress(copy.done, total)) {
            return Failure(InstallStatus::kCancelled);
        }
    }
    if (!WriteMarker(layout.app0_stage, source, linked, files.size() - linked, build, "full")) {
        return Failure(InstallStatus::kWriteFailed, "cannot write the preparation marker");
    }
    if (InstallResult activated = BuildAndActivate(layout, app0_guard, image_guard, hooks, builds, build, total);
        !activated.ok()) {
        return activated;
    }

    InstallResult result;
    result.bytes = total;
    result.files = files.size();
    result.linked = linked;
    result.build = &build;
    return result;
}

InstallMessage DescribeInstall(InstallStatus status) {
    switch (status) {
    case InstallStatus::kInstalled:
        return {"Game installed", "Dragon's Crown Pro is ready to play."};
    case InstallStatus::kCancelled:
        return {"Installation cancelled", "The installed game was not changed."};
    case InstallStatus::kNotPackage:
        return {"Not a PS4 package", "Select the Dragon's Crown Pro .pkg file."};
    case InstallStatus::kUnsupportedPackage:
        return {"This package cannot be unpacked",
                "It is encrypted for a PS4 console. Select a decrypted (fake) PKG of the game."};
    case InstallStatus::kNotGameFolder:
        return {"Not a game folder", "Select the folder that contains eboot.bin, sce_module and sce_sys."};
    case InstallStatus::kWrongGame:
        return {"Wrong game", "This is not Dragon's Crown Pro."};
    case InstallStatus::kWrongVersion:
        return {"Different version of Dragon's Crown Pro", "This port supports " + SupportedBuildsText() + "."};
    case InstallStatus::kIncomplete:
        return {"Game files missing",
                "The package is incomplete or some of the game's files are missing. Copy or download it again."};
    case InstallStatus::kReadFailed:
        return {"Could not read the game files",
                "The package could not be read or is damaged. Check the file, then try again."};
    case InstallStatus::kOverlapping:
        return {"Choose another file", "Select your game package, not a file inside the port's own game folder."};
    case InstallStatus::kForeignFolder:
        return {"Could not install the game",
                "The port's game folder holds other files. Move them out and try again."};
    case InstallStatus::kNoSpace:
        return {"Not enough free space", "The game needs about 6 GB. Free up disk space and try again."};
    case InstallStatus::kWriteFailed:
        return {"Could not install the game", "The game files could not be written next to the port."};
    case InstallStatus::kPrepareFailed:
        return {"Could not prepare the game", "The game's code could not be prepared. The log has the details."};
    case InstallStatus::kUpdateNeedsBase:
        return {"Install the base package first", "This update is laid over the game's base package. Install that one first."};
    case InstallStatus::kActivated:
        return {"Game activated", "The selected build is now the active game."};
    case InstallStatus::kUpToDate:
        return {"Already up to date", "This version is already the active game."};
    }
    return {"Could not install the game", "An unexpected error occurred."};
}

} // namespace dcp
