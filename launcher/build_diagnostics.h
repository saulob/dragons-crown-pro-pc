// Diagnostics for a Dragon's Crown Pro build the port does not support: the selected game is identified as the game,
// refused, and everything needed to add its support is written to the log. Strictly read-only, and a result never
// makes a build supported.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pkg_extract.h"

namespace dcp {

struct GameBuild;

struct SourceEntry {
    std::string path; // relative, '/' separated
    uint64_t size = 0;
};

// A read-only view of the game the player selected: an extracted folder or a package.
class GameSource {
public:
    virtual ~GameSource() = default;
    virtual std::string Kind() const = 0;
    virtual std::vector<SourceEntry> Files() const = 0;
    // The file's bytes; false when it is absent, larger than `max_bytes` or unreadable.
    virtual bool Read(const std::string& path, uint64_t max_bytes, std::vector<uint8_t>& out) const = 0;
    virtual bool ReadRange(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>& out) const = 0;
};

std::unique_ptr<GameSource> OpenFolderSource(const std::filesystem::path& folder);
// `package` must have loaded its files.
std::unique_ptr<GameSource> OpenPackageSource(Package& package);

struct UnsupportedReport {
    bool dragons_crown = false;       // the selection is Dragon's Crown Pro
    std::vector<std::string> evidence; // why it was or was not classified so
    std::vector<std::string> lines;   // the delimited log block; empty unless dragons_crown
    std::string rejection;            // identified as Dragon's Crown Pro but its executable cannot be analyzed: why
};

inline constexpr char kDiagnosticsBegin[] = "Unsupported Dragon's Crown Pro build diagnostics begin";
inline constexpr char kDiagnosticsEnd[] = "Unsupported Dragon's Crown Pro build diagnostics end";

// Classifies the selection and, when it is Dragon's Crown Pro, scans its executable and describes it. `sfo` is the
// selection's param.sfo (empty when it has none). Nothing is written.
UnsupportedReport DiagnoseUnsupportedBuild(const GameSource& source, const std::string& sfo);

// True when the source carries an eboot.bin that is not `build`'s executable; false when it carries none.
bool ExecutableDiffers(const GameSource& source, const GameBuild& build);

// The name of an unsupported build whose executable is known, empty otherwise.
std::string KnownUnsupportedBuild(const std::string& eboot_sha256);

} // namespace dcp
