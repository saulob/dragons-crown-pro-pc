#include "build_diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <istream>
#include <set>
#include <streambuf>
#include <string_view>
#include <system_error>

#include "build_scan.h"
#include "cpk_patch.h"
#include "crypto.h"
#include "feature_sites.h"
#include "game_files.h"
#include "game_patches.h"
#include "utf8.h"

namespace dcp {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr uint64_t kMaxExecutable = 128ull << 20;
constexpr uint64_t kMaxModule = 64ull << 20;
constexpr uint64_t kMaxSfo = 1ull << 20;
constexpr uint64_t kHashLimit = 32ull << 20;
constexpr size_t kMaxSourceFiles = 5000;
constexpr size_t kMaxListedFiles = 60;
constexpr size_t kMaxPartialSites = 40;
constexpr size_t kMinPartialRun = 6;

struct KnownUnsupported {
    const char* sha256;
    const char* label;
};
constexpr std::array<KnownUnsupported, 0> kKnownUnsupported{};

struct FeatureScope {
    const char* feature;
    const char* enabled_in;
};
constexpr FeatureScope kScopes[] = {
    {"Render resolution", "every supported build with verified sites"},
    {"True ultrawide", "every supported build with verified sites"},
    {"System type", "every supported build with verified sites"},
    {"Skip offers", "every supported build"},
    {"Startup save", "the CUSA07001 builds"},
    {"LZCNT compatibility", "every supported build"},
};

constexpr size_t kValidatorCount = 3;

class FolderSource final : public GameSource {
public:
    explicit FolderSource(fs::path root) : root_(std::move(root)) {}

    std::string Kind() const override { return "game folder"; }

    std::vector<SourceEntry> Files() const override {
        std::vector<SourceEntry> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && files.size() < kMaxSourceFiles; it.increment(ec)) {
            std::error_code item_ec;
            if (!it->is_regular_file(item_ec) || item_ec) {
                continue;
            }
            std::string relative = Utf8(it->path().lexically_relative(root_));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files.push_back({std::move(relative), it->file_size(item_ec)});
        }
        return files;
    }

    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        out.clear();
        std::error_code ec;
        const fs::path file = LocalPath(path);
        const uint64_t size = fs::file_size(file, ec);
        if (ec || size > max_bytes) {
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        out.resize(static_cast<size_t>(size));
        if (!in || !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
            out.clear();
            return false;
        }
        return true;
    }

    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        out.clear();
        std::ifstream in(LocalPath(path), std::ios::binary);
        out.resize(length);
        if (!in || !in.seekg(static_cast<std::streamoff>(offset)) ||
            !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length))) {
            out.clear();
            return false;
        }
        return true;
    }

    fs::path LocalPath(const std::string& path) const { return root_ / PathFromUtf8(path); }

private:
    fs::path root_;
};

class PackageSource final : public GameSource {
public:
    explicit PackageSource(Package& package) : package_(package), files_(package.Files()) {}

    std::string Kind() const override { return "package"; }

    std::vector<SourceEntry> Files() const override {
        std::vector<SourceEntry> files;
        for (const PackageFile& file : files_) {
            if (files.size() >= kMaxSourceFiles) {
                break;
            }
            files.push_back({file.path, file.size});
        }
        return files;
    }

    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        return package_.ReadFile(path, max_bytes, out).ok();
    }

    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        return package_.ReadFileRange(path, offset, length, out).ok();
    }


private:
    Package& package_;
    std::vector<PackageFile> files_;
};

class Lines {
public:
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) {
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        const int length = std::vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        std::string text(length > 0 ? static_cast<size_t>(length) : 0, '\0');
        if (length > 0) {
            std::vsnprintf(text.data(), text.size() + 1, format, args);
        }
        va_end(args);
        lines_.push_back(std::move(text));
    }
    void Add(const std::string& text) { lines_.push_back(text); }
    std::vector<std::string>& Take() { return lines_; }

private:
    std::vector<std::string> lines_;
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Hex(uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string Sha256Of(const Bytes& data) {
    uint8_t digest[32];
    if (!crypto::Sha256(data.data(), data.size(), digest)) {
        return {};
    }
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

std::string Shown(const std::string& value) {
    return value.empty() ? "(missing)" : value;
}

std::string RegionOf(const std::string& content_id) {
    static const std::pair<const char*, const char*> kPrefixes[] = {
        {"UP", "USA"}, {"EP", "Europe"}, {"JP", "Japan"}, {"HP", "Asia/Hong Kong"}, {"KP", "Korea"}, {"IP", "Internal"}};
    for (const auto& [prefix, region] : kPrefixes) {
        if (content_id.size() >= 2 && content_id.compare(0, 2, prefix) == 0) {
            return region;
        }
    }
    return "unknown";
}

bool HasPrefixCpk(const std::string& path) {
    const std::string name = Lower(path);
    return name.find('/') == std::string::npos && name.starts_with("dracro") && name.ends_with(".cpk");
}

bool TitleNamesGame(const std::vector<SfoField>& fields) {
    constexpr char kJapaneseTitle[] = "\xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4\xE3\x83\xB3\xE3\x82\xBA\xE3\x82\xAF\xE3\x83\xA9\xE3\x82\xA6\xE3\x83\xB3";
    for (const SfoField& field : fields) {
        if (!field.key.starts_with("TITLE") || field.key == "TITLE_ID") {
            continue;
        }
        const std::string value = Lower(field.value);
        if ((value.find("dragon") != std::string::npos && value.find("crown") != std::string::npos) ||
            field.value.find(kJapaneseTitle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool KnownTitleId(const std::string& title_id) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (title_id == build.title_id) {
            return true;
        }
    }
    return false;
}

constexpr const char* kMonsterStems[] = {"archdemon", "doppelganger", "dragonlisk", "doombeetle", "kraken",      "medusa",
                                         "minotaur",  "myconid",      "owlbear",    "wyvern",     "giantspider", "cyclops",
                                         "chimera",   "gazer",        "harpy",      "plantworm",  "sabertooth",  "vampire"};
constexpr const char* kMessageSheets[] = {"msgsheet/treasureart.fms", "msgsheet/temple.fms", "msgsheet/challengequest.fms",
                                          "msgsheet/abilitylist.fms", "msgsheet/itemlist.fms"};
constexpr size_t kMinMonsterStems = 12;
constexpr size_t kMinMessageSheets = 3;
constexpr uint64_t kProbeChunk = 256u << 10;
constexpr size_t kMaxProbedArchives = 8;

struct ArchiveEvidence {
    std::string path;
    bool valid = false;
    size_t entries = 0, monsters = 0, sheets = 0;
    bool Strong() const { return valid && monsters >= kMinMonsterStems && sheets >= kMinMessageSheets; }
};

struct Classification {
    bool title_names = false, content_names = false, known_title = false;
    bool named_archive = false, modules = false;
    bool strong_archive = false;
    bool update_executable = false;
    std::vector<ArchiveEvidence> archives;
    std::vector<std::string> evidence;

    bool Metadata() const { return title_names || content_names || known_title; }
    bool Structure() const { return named_archive || update_executable; }
    bool Identified() const { return strong_archive || (Metadata() && Structure()); }
};

class RangeBuffer final : public std::streambuf {
public:
    RangeBuffer(const GameSource& source, std::string path, uint64_t size)
        : source_(source), path_(std::move(path)), size_(static_cast<off_type>(size)) {}

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        const off_type start = base_ + (gptr() - eback());
        if (start >= size_) {
            return traits_type::eof();
        }
        const size_t length = static_cast<size_t>(std::min<off_type>(static_cast<off_type>(kProbeChunk), size_ - start));
        if (!source_.ReadRange(path_, static_cast<uint64_t>(start), length, chunk_) || chunk_.size() != length) {
            return traits_type::eof();
        }
        base_ = start;
        char* data = reinterpret_cast<char*>(chunk_.data());
        setg(data, data, data + length);
        return traits_type::to_int_type(*gptr());
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir dir, std::ios_base::openmode) override {
        const off_type current = base_ + (gptr() - eback());
        const off_type target = dir == std::ios_base::beg ? offset : dir == std::ios_base::cur ? current + offset : size_ + offset;
        if (target < 0 || target > size_) {
            return pos_type(off_type(-1));
        }
        base_ = target;
        setg(nullptr, nullptr, nullptr);
        return pos_type(target);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }

private:
    const GameSource& source_;
    std::string path_;
    off_type size_;
    off_type base_ = 0;
    Bytes chunk_;
};

struct Executables {
    Bytes eboot;
    scan::ExecutableInfo eboot_info;
    scan::MappedImage image;
    struct Module {
        std::string path;
        Bytes bytes;
        scan::ExecutableInfo info;
        bool present = false;
    };
    std::vector<Module> modules;
};

void LoadExecutables(const GameSource& source, Executables& exe) {
    source.Read("eboot.bin", kMaxExecutable, exe.eboot);
    exe.eboot_info = scan::ParseExecutable(exe.eboot);
    std::string error;
    if (exe.eboot_info.valid && exe.eboot_info.mappable) {
        scan::MapExecutable(exe.eboot, exe.eboot_info, 0, exe.image, error);
        exe.image.entry = exe.eboot_info.entry;
    }
    for (const char* name : kBundledModules) {
        Executables::Module module;
        module.path = std::string("sce_module/") + name;
        module.present = source.Read(module.path, kMaxModule, module.bytes);
        module.info = scan::ParseExecutable(module.bytes);
        scan::ModuleMap map;
        map.name = name;
        if (!module.present) {
            map.error = "missing or unreadable";
        } else if (exe.image.bytes.empty()) {
            map.error = "the executable did not map";
        } else {
            const uint64_t base = (exe.image.bytes.size() + scan::kModuleAlignment - 1) & ~(scan::kModuleAlignment - 1);
            map.base = base;
            map.size = scan::MappedSize(module.info);
            map.mapped = scan::MapExecutable(module.bytes, module.info, base, exe.image, map.error);
        }
        exe.image.modules.push_back(std::move(map));
        exe.modules.push_back(std::move(module));
    }
}

const char* PermissionName(uint32_t flags) {
    static char names[8][4] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
    return names[flags & 7];
}

void DescribeIdentity(Lines& out, const std::string& sfo, const Classification& cls, const GameSource& source) {
    out.Add("Game identification:");
    out.Add("  source: %s", source.Kind().c_str());
    for (const std::string& line : cls.evidence) {
        out.Add("  evidence: %s", line.c_str());
    }
    const std::string content_id = SfoString(sfo, "CONTENT_ID");
    out.Add("  TITLE: %s", Shown(SfoString(sfo, "TITLE")).c_str());
    out.Add("  TITLE_ID: %s", Shown(SfoString(sfo, "TITLE_ID")).c_str());
    out.Add("  CONTENT_ID: %s", Shown(content_id).c_str());
    out.Add("  region (from CONTENT_ID prefix): %s", RegionOf(content_id).c_str());
    out.Add("  APP_VER: %s", Shown(SfoString(sfo, "APP_VER")).c_str());
    out.Add("  VERSION: %s", Shown(SfoString(sfo, "VERSION")).c_str());
    out.Add("  CATEGORY: %s", Shown(SfoString(sfo, "CATEGORY")).c_str());
    for (const GameBuild& build : SupportedBuilds()) {
        if (SfoString(sfo, "TITLE_ID") == build.title_id) {
            const PackageLayer& layer = FinishedLayer(build);
            out.Add("  supported build of this TITLE_ID: %s (APP_VER %s, VERSION %s, eboot SHA-256 %s)", build.label,
                    layer.app_ver, layer.sfo_version, build.eboot_sha256);
        }
    }
    std::string others;
    for (const SfoField& field : SfoFields(sfo)) {
        if (field.key == "TITLE" || field.key == "TITLE_ID" || field.key == "CONTENT_ID" || field.key == "APP_VER" ||
            field.key == "VERSION" || field.key == "CATEGORY" || field.key.starts_with("TITLE_")) {
            continue;
        }
        others += (others.empty() ? "" : ", ") + field.key + "=" + field.value;
    }
    out.Add("  other param.sfo fields: %s", others.empty() ? "(none)" : others.c_str());
}

void DescribeExecutable(Lines& out, const Executables& exe) {
    const scan::ExecutableInfo& info = exe.eboot_info;
    out.Add("Executable (eboot.bin):");
    out.Add("  size: %llu bytes", static_cast<unsigned long long>(info.file_size));
    out.Add("  SHA-256: %s", Shown(Sha256Of(exe.eboot)).c_str());
    const std::string known = KnownUnsupportedBuild(Sha256Of(exe.eboot));
    if (!known.empty()) {
        out.Add("  known unsupported build: %s", known.c_str());
    }
    out.Add("  format: PS4 SELF, ident %02x %02x %02x %02x, header 0x%x, meta 0x%x", info.ident[0], info.ident[1],
            info.ident[2], info.ident[3], info.header_size, info.meta_size);
    out.Add("  ELF: type 0x%x, machine %u, entry %s, %zu program headers, %zu SELF segments", info.elf_type, info.machine,
            Hex(info.entry).c_str(), info.headers.size(), info.segments.size());
    if (!info.valid) {
        out.Add("  structure: %s", info.error.c_str());
        return;
    }
    out.Add("  image base: 0 (an image offset is the segment vaddr); mapped size %s", Hex(scan::MappedSize(info)).c_str());
    size_t executable = 0;
    for (size_t i = 0; i < info.headers.size(); ++i) {
        const scan::ProgramHeader& h = info.headers[i];
        if (h.type == 1 && (h.flags & 1)) {
            ++executable;
        }
        out.Add("  ph[%zu]: type 0x%x %s vaddr %s memsz %s filesz %s elf-offset %s align %s", i, h.type,
                PermissionName(h.flags), Hex(h.vaddr).c_str(), Hex(h.memsz).c_str(), Hex(h.filesz).c_str(),
                Hex(h.offset).c_str(), Hex(h.align).c_str());
    }
    out.Add("  executable load segments: %zu", executable);
    for (size_t i = 0; i < info.segments.size(); ++i) {
        const scan::SelfSegment& s = info.segments[i];
        if (s.flags & 0x800) {
            out.Add("  self segment[%zu]: flags %s file-offset %s size %s -> ph[%llu]", i, Hex(s.flags).c_str(),
                    Hex(s.offset).c_str(), Hex(s.size).c_str(), static_cast<unsigned long long>((s.flags >> 20) & 4095));
        }
    }
    for (const scan::ProgramHeader& h : info.headers) {
        uint64_t file_offset = 0;
        if ((h.type == 1 || h.type == 0x61000010) && h.filesz && scan::FileOffsetOf(info, h.vaddr, file_offset)) {
            out.Add("  rva %s..%s = file offset %s (rva + %s)", Hex(h.vaddr).c_str(), Hex(h.vaddr + h.filesz).c_str(),
                    Hex(file_offset).c_str(), file_offset >= h.vaddr ? Hex(file_offset - h.vaddr).c_str() : "negative");
        }
    }
    if (!info.mappable) {
        out.Add("  mapping: %s", info.error.c_str());
    }
}

void DescribeModules(Lines& out, const Executables& exe) {
    out.Add("Bundled modules (linked after the executable):");
    for (size_t i = 0; i < exe.modules.size(); ++i) {
        const Executables::Module& module = exe.modules[i];
        const scan::ModuleMap& map = exe.image.modules[i];
        if (!module.present) {
            out.Add("  %s: missing or unreadable", module.path.c_str());
            continue;
        }
        out.Add("  %s: %zu bytes, SHA-256 %s, %s", module.path.c_str(), module.bytes.size(), Shown(Sha256Of(module.bytes)).c_str(),
                module.info.valid ? "valid SELF" : module.info.error.c_str());
        out.Add("    image: base %s size %s %s", Hex(map.base).c_str(), Hex(map.size).c_str(),
                map.mapped ? "mapped" : ("not mapped: " + map.error).c_str());
    }
    out.Add("  linked image size: %s, entry %s", Hex(exe.image.bytes.size()).c_str(), Hex(exe.image.entry).c_str());
    for (const scan::ReferenceBuild& reference : scan::kReferenceBuilds) {
        if (reference.image_size == exe.image.bytes.size() && reference.entry == exe.image.entry) {
            out.Add("  image size and entry equal %s", reference.label);
        }
    }
}

struct CpkSummary {
    size_t entries = 0;
    size_t interface_entries = 0;
    std::vector<std::string> system_messages;
    std::string spider;
};

bool SummarizeCpk(const GameSource& source, const SourceEntry& file, CpkSummary& summary, std::string& error) {
    RangeBuffer buffer(source, file.path, file.size);
    std::istream stream(&buffer);
    CpkArchive archive;
    if (!archive.OpenStream(stream, &error)) {
        return false;
    }
    summary.entries = archive.entries().size();
    for (const CpkEntry& entry : archive.entries()) {
        const std::string path = Lower(entry.path);
        if (path.starts_with("stage/interface")) {
            ++summary.interface_entries;
        } else if (path.starts_with("msgsheet/systemmsg") && summary.system_messages.size() < 16) {
            summary.system_messages.push_back(entry.path);
        }
    }
    if (const CpkEntry* spider = archive.Find(kSpiderEntry)) {
        summary.spider = std::to_string(spider->extract_size) + " bytes";
    }
    return true;
}

void DescribeFiles(Lines& out, const GameSource& source, const std::vector<SourceEntry>& files) {
    out.Add("Game files the port relies on:");
    std::map<std::string, uint64_t> sizes;
    for (const SourceEntry& file : files) {
        sizes[file.path] = file.size;
    }
    std::set<std::string> wanted(std::begin(kSharedGameFiles), std::end(kSharedGameFiles));
    wanted.insert("sce_discmap_patch.plt");
    for (const SourceEntry& file : files) {
        if (HasPrefixCpk(file.path)) {
            wanted.insert(file.path);
        }
    }
    for (const std::string& name : wanted) {
        const auto found = sizes.find(name);
        if (found == sizes.end()) {
            out.Add("  %s: absent", name.c_str());
            continue;
        }
        std::string line = "  " + name + ": " + std::to_string(found->second) + " bytes";
        Bytes data;
        if (found->second > 0 && found->second <= kHashLimit && source.Read(name, kHashLimit, data)) {
            line += ", SHA-256 " + Sha256Of(data);
        }
        if (HasPrefixCpk(name)) {
            CpkSummary summary;
            std::string error;
            if (SummarizeCpk(source, {name, found->second}, summary, error)) {
                line += ", CPK with " + std::to_string(summary.entries) + " entries, " +
                        std::to_string(summary.interface_entries) + " interface entries";
                if (!summary.spider.empty()) {
                    line += ", " + std::string(kSpiderEntry) + " " + summary.spider;
                }
                for (const std::string& sheet : summary.system_messages) {
                    line += ", " + sheet;
                }
            } else {
                line += ", not a readable CPK (" + error + ")";
            }
        }
        out.Add(line);
    }
    size_t listed = 0;
    std::map<std::string, size_t> folders;
    for (const SourceEntry& file : files) {
        const size_t slash = file.path.find('/');
        if (slash == std::string::npos) {
            if (listed++ < kMaxListedFiles && !wanted.contains(file.path)) {
                out.Add("  other root file %s: %llu bytes", file.path.c_str(), static_cast<unsigned long long>(file.size));
            }
        } else {
            ++folders[file.path.substr(0, slash)];
        }
    }
    for (const auto& [folder, count] : folders) {
        out.Add("  folder %s/: %zu files", folder.c_str(), count);
    }
}

std::string FingerprintText(const scan::MappedImage& image, const scan::SiteSpec& spec, uint64_t site) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(scan::SolidFingerprint(image, spec.pattern, site - spec.anchor)));
    return text;
}

// Longest runs of solid bytes of a pattern, as hex text and the byte offset each starts at.
std::vector<std::pair<size_t, std::string>> SolidRuns(std::string_view pattern) {
    std::vector<std::pair<size_t, std::string>> runs;
    std::string current;
    size_t start = 0, index = 0;
    const auto close = [&] {
        if (current.size() >= kMinPartialRun * 3 - 1) {
            runs.emplace_back(start, current);
        }
        current.clear();
    };
    for (size_t i = 0; i < pattern.size(); i += 3, ++index) {
        const std::string_view token = pattern.substr(i, 2);
        if (token == "??") {
            close();
            continue;
        }
        if (current.empty()) {
            start = index;
        } else {
            current += ' ';
        }
        current += token;
    }
    close();
    std::sort(runs.begin(), runs.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
    if (runs.size() > 2) {
        runs.resize(2);
    }
    std::sort(runs.begin(), runs.end());
    return runs;
}

void DescribePartial(Lines& out, const scan::MappedImage& image, const scan::SiteSpec& spec) {
    const auto runs = SolidRuns(spec.pattern);
    if (runs.empty()) {
        return;
    }
    std::vector<std::vector<uint64_t>> hits;
    std::string line = "      partial:";
    for (const auto& [offset, text] : runs) {
        size_t total = 0;
        hits.push_back(scan::FindPattern(image, text, 3, total));
        line += " run@" + std::to_string(offset) + " x" + std::to_string(total);
        for (const uint64_t hit : hits.back()) {
            line += " " + Hex(hit);
        }
    }
    out.Add(line);
    if (runs.size() == 2) {
        for (const uint64_t first : hits[0]) {
            for (const uint64_t second : hits[1]) {
                if (second > first && second - first == runs[1].first - runs[0].first) {
                    out.Add("      consistent candidate (not a mapping): site at %s",
                            Hex(first - runs[0].first + spec.anchor).c_str());
                }
            }
        }
    }
}

std::string ProfileScope(bool GameBuild::*flag) {
    size_t enabled = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        enabled += build.*flag ? 1 : 0;
    }
    return " (enabled today in: " + std::to_string(enabled) + " of " + std::to_string(SupportedBuilds().size()) +
           " supported builds)";
}

std::string ScopeText(const std::string& feature) {
    if (feature == "Cheats") {
        return ProfileScope(&GameBuild::cheats);
    }
    if (feature == "Japanese NPC voices") {
        return ProfileScope(&GameBuild::japanese_npc_voices);
    }
    for (const FeatureScope& item : kScopes) {
        if (feature == item.feature) {
            return std::string(" (enabled today in: ") + item.enabled_in + ")";
        }
    }
    return {};
}

struct FeatureTotals {
    size_t sites = 0, mapped = 0, ambiguous = 0, missing = 0, relation_failures = 0;
    std::vector<std::string> unresolved;
};

void DescribeSites(Lines& out, const scan::MappedImage& image, const scan::ScanResult& result,
                   std::map<std::string, FeatureTotals>& totals, std::vector<std::string>& order) {
    const auto feature_of = [&](const std::string& name) -> FeatureTotals& {
        if (!totals.contains(name)) {
            order.push_back(name);
        }
        return totals[name];
    };
    std::string last;
    size_t partials = 0;
    for (const scan::SiteResult& site : result.sites) {
        const std::string feature = site.spec->feature;
        FeatureTotals& total = feature_of(feature);
        if (feature != last) {
            out.Add("  %s%s", feature.c_str(), ScopeText(feature).c_str());
            last = feature;
        }
        ++total.sites;
        std::string line = std::string("    [") + scan::SiteStatusName(site.status) + "] " + site.spec->name;
        if (site.status == scan::SiteStatus::kUnique || site.status == scan::SiteStatus::kGroup) {
            ++total.mapped;
            for (const uint64_t match : site.matches) {
                line += " " + Hex(match) + " fp " + FingerprintText(image, *site.spec, match);
            }
            if (site.status == scan::SiteStatus::kGroup) {
                line += " (" + std::to_string(site.count) + " identical copies, ordered by address)";
            }
            out.Add(line);
            continue;
        }
        total.unresolved.push_back(site.spec->name);
        if (site.status == scan::SiteStatus::kAmbiguous) {
            ++total.ambiguous;
            line += ": " + std::to_string(site.count) + " matches, expected " + std::to_string(site.spec->expected) + ":";
            for (const uint64_t match : site.matches) {
                line += " " + Hex(match);
            }
            out.Add(line);
        } else {
            ++total.missing;
            out.Add(line + ": no match");
            if (partials++ < kMaxPartialSites) {
                DescribePartial(out, image, *site.spec);
            }
        }
    }
    out.Add("  Render resolution%s", ScopeText("Render resolution").c_str());
    FeatureTotals& resolution = feature_of("Render resolution");
    resolution.sites = result.resolution.expected;
    resolution.mapped = result.resolution.unique;
    resolution.ambiguous = result.resolution.ambiguous;
    resolution.missing = result.resolution.missing;
    if (!result.resolution.complete) {
        resolution.unresolved.push_back("render size instructions");
    }
    out.Add("    render size instructions: %zu of %zu unique (%zu ambiguous, %zu missing)", result.resolution.unique,
            result.resolution.expected, result.resolution.ambiguous, result.resolution.missing);
    out.Add("  LZCNT compatibility%s", ScopeText("LZCNT compatibility").c_str());
    FeatureTotals& lzcnt = feature_of("LZCNT compatibility");
    lzcnt.sites = 1;
    lzcnt.mapped = result.lzcnt.complete ? 1 : 0;
    lzcnt.missing = result.lzcnt.count == 0 ? 1 : 0;
    lzcnt.ambiguous = result.lzcnt.count != 0 && !result.lzcnt.complete ? 1 : 0;
    if (!result.lzcnt.complete) {
        lzcnt.unresolved.push_back("LZCNT instruction set");
    }
    out.Add("    LZCNT instructions: %zu (expected %zu), digest %016llx (expected %016llx), first %s last %s", result.lzcnt.count,
            scan::kLzcntInstructions, static_cast<unsigned long long>(result.lzcnt.digest),
            static_cast<unsigned long long>(scan::kLzcntDigest), Hex(result.lzcnt.first).c_str(),
            Hex(result.lzcnt.last).c_str());
}

void DescribeRelations(Lines& out, const scan::ScanResult& result, std::map<std::string, FeatureTotals>& totals) {
    out.Add("Relationship validation:");
    for (const scan::RelationResult& relation : result.relations) {
        const char* verb = relation.spec->kind == scan::RelationKind::kCallsSite ? "calls" : "shares its call target with";
        const char* state = relation.status == scan::RelationStatus::kHolds    ? "holds"
                            : relation.status == scan::RelationStatus::kBroken ? "BROKEN"
                                                                               : "unverifiable";
        out.Add("  %s: %s %s %s: %s%s%s", relation.spec->feature, relation.spec->from, verb, relation.spec->to, state,
                relation.detail.empty() ? "" : " - ", relation.detail.c_str());
        if (relation.status == scan::RelationStatus::kBroken) {
            ++totals[relation.spec->feature].relation_failures;
        }
    }
}

void DescribeProfiles(Lines& out, const scan::ScanResult& result) {
    out.Add("Closest structural profile (diagnostic only; not a compatibility claim):");
    size_t best = 0, best_same = 0, best_spacing = 0;
    for (size_t r = 0; r < scan::kReferenceBuildCount; ++r) {
        size_t applicable = 0, same = 0, spacing = 0, pairs = 0;
        const scan::SiteResult* previous = nullptr;
        for (const scan::SiteResult& site : result.sites) {
            const uint32_t reference = site.spec->reference[r];
            if (reference != 0) {
                ++applicable;
                if (!site.matches.empty() && site.matches.front() == reference) {
                    ++same;
                }
            }
            if (previous && std::string_view(previous->spec->feature) == site.spec->feature &&
                previous->spec->reference[r] != 0 && reference != 0 && !previous->matches.empty() && !site.matches.empty()) {
                ++pairs;
                if (site.matches.front() - previous->matches.front() == reference - previous->spec->reference[r]) {
                    ++spacing;
                }
            }
            previous = &site;
        }
        out.Add("  %s: %zu of %zu sites at the same offset, %zu of %zu neighbouring sites at the same spacing",
                scan::kReferenceBuilds[r].label, same, applicable, spacing, pairs);
        if (same > best_same || (same == best_same && spacing > best_spacing)) {
            best = r;
            best_same = same;
            best_spacing = spacing;
        }
    }
    if (best_same == 0 && best_spacing == 0) {
        out.Add("  closest profile: none; no site is at a supported build's offset");
    } else {
        out.Add("  closest profile: %s; it is not inherited and proves nothing about compatibility",
                scan::kReferenceBuilds[best].label);
    }
}

void DescribeLimits(Lines& out, const std::string& sfo, const std::vector<SourceEntry>& files) {
    std::set<std::string> present;
    for (const SourceEntry& file : files) {
        present.insert(file.path);
    }
    std::vector<std::string> absent;
    for (const char* name : kSharedGameFiles) {
        if (std::string_view(name) != "sce_sys/param.sfo" && !present.contains(name)) {
            absent.push_back(name);
        }
    }
    if (absent.empty()) {
        return;
    }
    const bool update = SfoString(sfo, "CATEGORY") == "gp";
    out.Add("Package limits:");
    for (const std::string& name : absent) {
        out.Add("  %s is not in this package", name.c_str());
    }
    out.Add(update ? "  this is an update (CATEGORY gp): its missing files belong to the base build, so sites that live in them "
                     "cannot be evaluated here and are listed as missing"
                   : "  the package is incomplete: sites that live in the missing files are listed as missing");
}

// The loader's own locators run on the scanned image; accepted means the feature would resolve on this executable.
size_t DescribeValidators(Lines& out, const scan::MappedImage& image) {
    out.Add("Runtime validators (the loader's locators, run on this image; accepted means the feature would resolve):");
    if (image.bytes.empty()) {
        out.Add("  not run: the executable did not map");
        return 0;
    }
    std::vector<DcpExecRange> ranges;
    for (const scan::Range& range : image.executable) {
        ranges.push_back({range.begin, range.end});
    }
    const DcpImageView view{image.bytes.data(), image.bytes.size(), ranges.data(), ranges.size()};
    size_t accepted = 0;
    const auto verdict = [&](const char* feature, bool ok, const DcpLocateReport& report, const std::string& detail) {
        accepted += ok ? 1 : 0;
        out.Add("  %s: %s, %zu of %zu checks%s%s", feature, ok ? "accepted" : "REJECTED", report.found, report.expected,
                ok ? detail.c_str() : ": ", ok ? "" : report.failure);
    };

    DcpLocateReport report{};
    DcpCheatSites cheats{};
    const bool cheats_ok = dcp_locate_cheat_sites(&view, &cheats, &report) != 0;
    verdict("Cheats", cheats_ok, report,
            ", " + std::to_string(DCP_CHEAT_HOOKS) + " hooks, " + std::to_string(DCP_CHEAT_FUNCTIONS) + " functions");
    if (cheats_ok) {
        std::string layout;
        for (int field = 0; field < DCP_LAYOUT_COUNT; ++field) {
            layout += (layout.empty() ? "" : ", ") + std::string(dcp_layout_field_name(static_cast<DcpLayoutField>(field))) +
                      " " + Hex(cheats.layout[field]);
        }
        out.Add("    layout: %s", layout.c_str());
    }

    report = {};
    DcpVoiceSites voices{};
    const bool voices_ok = dcp_locate_voice_sites(&view, &voices, &report) != 0;
    verdict("Japanese NPC voices", voices_ok, report,
            ", cue " + Hex(voices.npc_cue) + ", resource acquire " + Hex(voices.resource_acquire));

    report = {};
    DcpUltrawideSites ultrawide{};
    const bool ultrawide_ok = dcp_locate_ultrawide_sites(&view, &ultrawide, &report) != 0;
    verdict("True ultrawide", ultrawide_ok, report,
            ", submit " + Hex(ultrawide.submit) + ", view build " + Hex(ultrawide.view_build));
    return accepted;
}

bool IsRootArchive(const std::string& path) {
    return path.find('/') == std::string::npos && Lower(path).ends_with(".cpk");
}

ArchiveEvidence ProbeArchive(const GameSource& source, const SourceEntry& file) {
    ArchiveEvidence evidence;
    evidence.path = file.path;
    RangeBuffer buffer(source, file.path, file.size);
    std::istream stream(&buffer);
    CpkArchive archive;
    if (!archive.OpenStream(stream)) {
        return evidence;
    }
    evidence.valid = true;
    evidence.entries = archive.entries().size();
    std::vector<bool> stems(std::size(kMonsterStems)), sheets(std::size(kMessageSheets));
    for (const CpkEntry& entry : archive.entries()) {
        const std::string path = Lower(entry.path);
        if (path.starts_with("character/")) {
            const std::string_view name = std::string_view(path).substr(10);
            for (size_t i = 0; i < stems.size(); ++i) {
                stems[i] = stems[i] || name.starts_with(kMonsterStems[i]);
            }
        }
        for (size_t i = 0; i < sheets.size(); ++i) {
            sheets[i] = sheets[i] || path == kMessageSheets[i];
        }
    }
    evidence.monsters = static_cast<size_t>(std::count(stems.begin(), stems.end(), true));
    evidence.sheets = static_cast<size_t>(std::count(sheets.begin(), sheets.end(), true));
    return evidence;
}

Classification Classify(const GameSource& source, const std::string& sfo, const std::vector<SourceEntry>& files) {
    Classification cls;
    const std::string title_id = SfoString(sfo, "TITLE_ID");
    cls.known_title = KnownTitleId(title_id);
    cls.content_names = Lower(SfoString(sfo, "CONTENT_ID")).find("dragonscrown") != std::string::npos;
    cls.title_names = TitleNamesGame(SfoFields(sfo));
    if (cls.known_title) {
        cls.evidence.push_back("TITLE_ID " + title_id + " belongs to a supported Dragon's Crown Pro title");
    }
    if (cls.content_names) {
        cls.evidence.push_back("CONTENT_ID names DRAGONSCROWN");
    }
    if (cls.title_names) {
        cls.evidence.push_back("TITLE names Dragon's Crown");
    }
    if (!cls.Metadata()) {
        cls.evidence.push_back("param.sfo does not name Dragon's Crown Pro");
    }
    cls.named_archive = std::any_of(files.begin(), files.end(), [](const SourceEntry& f) { return HasPrefixCpk(f.path); });
    std::set<std::string> present;
    for (const SourceEntry& file : files) {
        present.insert(file.path);
    }
    cls.modules = present.contains("sce_discmap.plt") &&
                  std::all_of(std::begin(kBundledModules), std::end(kBundledModules),
                              [&](const char* name) { return present.contains(std::string("sce_module/") + name); });
    cls.update_executable = SfoString(sfo, "CATEGORY") == "gp" && present.contains("eboot.bin");
    cls.evidence.push_back(cls.named_archive ? "a DRACRO*.CPK archive is present" : "no DRACRO*.CPK archive");
    if (cls.update_executable && !cls.named_archive) {
        cls.evidence.push_back("an update package (CATEGORY gp) carries eboot.bin");
    }
    if (cls.modules) {
        cls.evidence.push_back("the bundled modules and sce_discmap.plt are present");
    }
    for (const SourceEntry& file : files) {
        if (!IsRootArchive(file.path) || file.size == 0 || cls.archives.size() >= kMaxProbedArchives) {
            continue;
        }
        cls.archives.push_back(ProbeArchive(source, file));
        const ArchiveEvidence& archive = cls.archives.back();
        if (archive.valid) {
            cls.evidence.push_back(archive.path + ": CPK with " + std::to_string(archive.entries) + " entries, " +
                                   std::to_string(archive.monsters) + "/" + std::to_string(std::size(kMonsterStems)) +
                                   " Dragon's Crown monster resources, " + std::to_string(archive.sheets) + "/" +
                                   std::to_string(std::size(kMessageSheets)) + " message sheets");
        }
        cls.strong_archive = cls.strong_archive || archive.Strong();
    }
    if (cls.strong_archive) {
        cls.evidence.push_back("an archive holds the Dragon's Crown resource set");
    }
    return cls;
}

} // namespace

std::unique_ptr<GameSource> OpenFolderSource(const fs::path& folder) {
    return std::make_unique<FolderSource>(folder);
}

std::unique_ptr<GameSource> OpenPackageSource(Package& package) {
    return std::make_unique<PackageSource>(package);
}

std::string KnownUnsupportedBuild(const std::string& eboot_sha256) {
    for (const KnownUnsupported& known : kKnownUnsupported) {
        if (eboot_sha256 == known.sha256) {
            return known.label;
        }
    }
    return {};
}

bool ExecutableDiffers(const GameSource& source, const GameBuild& build) {
    Bytes eboot;
    return source.Read("eboot.bin", kMaxExecutable, eboot) && Sha256Of(eboot) != build.eboot_sha256;
}

UnsupportedReport DiagnoseUnsupportedBuild(const GameSource& source, const std::string& sfo) {
    UnsupportedReport report;
    const std::vector<SourceEntry> files = source.Files();
    Classification cls = Classify(source, sfo, files);
    if (!cls.Identified()) {
        report.evidence = std::move(cls.evidence);
        return report;
    }

    Executables exe;
    LoadExecutables(source, exe);
    if (!exe.eboot_info.valid) {
        report.rejection = exe.eboot.empty() ? "eboot.bin is not in the source or is unreadable"
                                             : "eboot.bin is not a readable PS4 executable (" + exe.eboot_info.error + "), " +
                                                   std::to_string(exe.eboot.size()) + " bytes, SHA-256 " + Sha256Of(exe.eboot);
        cls.evidence.push_back(report.rejection);
        report.evidence = std::move(cls.evidence);
        return report;
    }
    const scan::ScanResult result = scan::ScanImage(exe.image);

    Lines out;
    out.Add(kDiagnosticsBegin);
    out.Add("The game was recognized as Dragon's Crown Pro, but this exact build is not supported; nothing was installed.");
    DescribeIdentity(out, sfo, cls, source);
    DescribeExecutable(out, exe);
    DescribeModules(out, exe);
    DescribeLimits(out, sfo, files);
    DescribeFiles(out, source, files);

    out.Add("Build-specific feature mapping (read-only scan; a located site is not a verified one):");
    std::map<std::string, FeatureTotals> totals;
    std::vector<std::string> order;
    DescribeSites(out, exe.image, result, totals, order);
    DescribeRelations(out, result, totals);
    DescribeProfiles(out, result);
    const size_t validators = DescribeValidators(out, exe.image);

    size_t mapped_features = 0, ambiguous = 0, missing = 0;
    std::vector<std::string> unresolved;
    for (const std::string& name : order) {
        const FeatureTotals& total = totals[name];
        const bool complete = total.mapped == total.sites && total.relation_failures == 0;
        mapped_features += complete ? 1 : 0;
        ambiguous += total.ambiguous;
        missing += total.missing;
        out.Add("  %s: %zu/%zu sites mapped, %zu ambiguous, %zu missing, %zu broken relations: %s", name.c_str(), total.mapped,
                total.sites, total.ambiguous, total.missing, total.relation_failures, complete ? "complete" : "incomplete");
        for (const std::string& site : total.unresolved) {
            unresolved.push_back(name + ": " + site);
        }
    }
    bool modules_mapped = true;
    for (const scan::ModuleMap& module : exe.image.modules) {
        modules_mapped = modules_mapped && module.mapped;
    }
    const bool identity_complete = !SfoString(sfo, "TITLE_ID").empty() && !SfoString(sfo, "CONTENT_ID").empty() &&
                                   !SfoString(sfo, "APP_VER").empty() && !SfoString(sfo, "VERSION").empty();
    const bool executable_complete = exe.eboot_info.mappable && modules_mapped;
    const bool all_complete = identity_complete && executable_complete && mapped_features == order.size() && missing == 0 &&
                              ambiguous == 0 && validators == kValidatorCount;
    out.Add("Overall diagnostic result:");
    out.Add("  Identity: %s", identity_complete ? "complete" : "incomplete");
    out.Add("  Executable mapping: %s", executable_complete ? "complete" : "incomplete");
    out.Add("  Build-specific features: %zu/%zu mapped", mapped_features, order.size());
    out.Add("  Runtime validators: %zu/%zu accepted", validators, kValidatorCount);
    out.Add("  Ambiguous sites: %zu", ambiguous);
    out.Add("  Missing sites: %zu", missing);
    if (!unresolved.empty()) {
        out.Add("  Unresolved:");
        for (const std::string& item : unresolved) {
            out.Add("    %s", item.c_str());
        }
    }
    out.Add(all_complete ? "  Mapping appears complete" : "  Mapping incomplete");
    out.Add("This build stays unsupported; no executable patch or hook was applied and no game file was changed.");
    out.Add(kDiagnosticsEnd);

    report.dragons_crown = true;
    report.evidence = std::move(cls.evidence);
    report.lines = std::move(out.Take());
    return report;
}

} // namespace dcp
