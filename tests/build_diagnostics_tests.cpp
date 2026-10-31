// Unsupported-build diagnostics: SELF parsing, signature scan, relations, classification, the report and the install
// path, on synthetic data. The local game fixtures under game\prepared add the known-build self-test when present.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "build_diagnostics.h"
#include "build_scan.h"
#include "dcp_resolution.h"
#include "game_files.h"
#include "game_install.h"
#include "game_profile.h"
#include "utf8.h"

namespace fs = std::filesystem;
using namespace dcp;
using Bytes = std::vector<uint8_t>;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                       \
        }                                                                                          \
    } while (0)

struct Segment {
    uint32_t type = 1, flags = 5;
    uint64_t vaddr = 0, memsz = 0;
    Bytes data;
};

template <typename T>
void Put(Bytes& out, size_t at, T value) {
    if (out.size() < at + sizeof(T)) {
        out.resize(at + sizeof(T));
    }
    std::memcpy(out.data() + at, &value, sizeof(T));
}

Bytes MakeSelf(const std::vector<Segment>& segments, uint64_t entry = 0x40) {
    const uint16_t count = static_cast<uint16_t>(segments.size());
    const uint64_t base = 32 + uint64_t{count} * 32;
    const uint64_t headers = base + 64;
    uint64_t data_at = (headers + 56 * segments.size() + 15) & ~uint64_t{15};
    Bytes file(data_at, 0);
    const uint8_t magic[4] = {'O', 0x15, '=', 0x1d};
    std::memcpy(file.data(), magic, 4);
    Put<uint16_t>(file, 24, count);
    const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(file.data() + base, ident, sizeof(ident));
    Put<uint16_t>(file, base + 16, 2);
    Put<uint16_t>(file, base + 18, 62);
    Put<uint64_t>(file, base + 24, entry);
    Put<uint64_t>(file, base + 32, 64);
    Put<uint16_t>(file, base + 54, 56);
    Put<uint16_t>(file, base + 56, static_cast<uint16_t>(segments.size()));
    for (size_t i = 0; i < segments.size(); ++i) {
        const Segment& s = segments[i];
        const uint64_t ph = headers + i * 56;
        Put<uint32_t>(file, ph, s.type);
        Put<uint32_t>(file, ph + 4, s.flags);
        Put<uint64_t>(file, ph + 8, data_at);
        Put<uint64_t>(file, ph + 16, s.vaddr);
        Put<uint64_t>(file, ph + 32, s.data.size());
        Put<uint64_t>(file, ph + 40, s.memsz);
        Put<uint64_t>(file, ph + 48, 0x4000);
        const uint64_t entry_at = 32 + i * 32;
        Put<uint64_t>(file, entry_at, 0x800 | (uint64_t{i} << 20));
        Put<uint64_t>(file, entry_at + 8, data_at);
        Put<uint64_t>(file, entry_at + 16, s.data.size());
        Put<uint64_t>(file, entry_at + 24, s.data.size());
        file.resize(data_at + s.data.size());
        std::memcpy(file.data() + data_at, s.data.data(), s.data.size());
        data_at = (data_at + s.data.size() + 15) & ~uint64_t{15};
        file.resize(data_at, 0);
    }
    return file;
}

Bytes Text(size_t size, uint8_t fill = 0xcc) {
    return Bytes(size, fill);
}

scan::ExecutableInfo Parse(const Bytes& file) {
    return scan::ParseExecutable(file);
}

scan::MappedImage MapOne(const Bytes& file, bool* ok = nullptr) {
    scan::MappedImage image;
    std::string error;
    const scan::ExecutableInfo info = Parse(file);
    const bool mapped = scan::MapExecutable(file, info, 0, image, error);
    if (ok) {
        *ok = mapped;
    }
    return image;
}

size_t Count(const scan::MappedImage& image, std::string_view pattern) {
    size_t total = 0;
    scan::FindPattern(image, pattern, 4, total);
    return total;
}

void TestPatterns() {
    Bytes code = Text(0x2000);
    const uint8_t marker[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x12, 0x34, 0x56, 0x78, 0x9a};
    std::memcpy(code.data() + 0x100, marker, sizeof(marker));
    std::vector<Segment> segments(2);
    segments[0].data = code;
    segments[0].memsz = code.size();
    segments[1].type = 1;
    segments[1].flags = 6;
    segments[1].vaddr = 0x4000;
    segments[1].data = Text(0x100, 0xcc);
    std::memcpy(segments[1].data.data() + 0x10, marker, sizeof(marker));
    segments[1].memsz = 0x100;
    bool ok = false;
    const scan::MappedImage image = MapOne(MakeSelf(segments), &ok);
    CHECK(ok);
    CHECK(image.executable.size() == 1);

    size_t total = 0;
    const std::vector<uint64_t> unique = scan::FindPattern(image, "55 48 89 e5 41 57", 4, total);
    CHECK(total == 1 && unique.size() == 1 && unique[0] == 0x100);
    CHECK(Count(image, "55 48 89 e5 ?? 57 ?? ?? ?? 78 9a") == 1);
    CHECK(Count(image, "55 48 89 e5 41 58") == 0);
    CHECK(Count(image, "34 56 78 9a") == 1);
    CHECK(Count(image, "?? ?? ??") == 0);
    CHECK(Count(image, "5") == 0);
    CHECK(Count(image, "zz 12") == 0);
    CHECK(Count(image, "") == 0);

    std::memcpy(code.data() + 0x300, marker, sizeof(marker));
    segments[0].data = code;
    const scan::MappedImage twice = MapOne(MakeSelf(segments));
    const std::vector<uint64_t> both = scan::FindPattern(twice, "55 48 89 e5 41 57", 1, total);
    CHECK(total == 2 && both.size() == 1);

    Bytes tail = Text(0x2000);
    std::memcpy(tail.data() + tail.size() - 3, marker, 3);
    segments[0].data = tail;
    segments[0].memsz = tail.size();
    const scan::MappedImage edge = MapOne(MakeSelf(segments));
    CHECK(Count(edge, "55 48 89 e5 41 57") == 0);
    CHECK(Count(edge, "55 48 89") == 1);
}

void TestExecutableParsing() {
    std::vector<Segment> segments(2);
    segments[0].data = Text(0x1000);
    segments[0].memsz = 0x1000;
    segments[1].flags = 6;
    segments[1].vaddr = 0x2000;
    segments[1].data = Text(0x200, 1);
    segments[1].memsz = 0x300;
    const Bytes file = MakeSelf(segments, 0x80);
    const scan::ExecutableInfo info = Parse(file);
    CHECK(info.valid && info.mappable);
    CHECK(info.machine == 62 && info.elf_type == 2 && info.entry == 0x80);
    CHECK(info.headers.size() == 2 && info.segments.size() == 2);
    CHECK(info.headers[1].vaddr == 0x2000 && info.headers[1].memsz == 0x300 && info.headers[1].flags == 6);
    CHECK(scan::MappedSize(info) == 0x2300);
    CHECK(info.file_size == file.size());

    uint64_t offset = 0;
    CHECK(scan::FileOffsetOf(info, 0x10, offset) && offset == info.segments[0].offset + 0x10);
    CHECK(scan::FileOffsetOf(info, 0x2000 + 0x1ff, offset) && offset == info.segments[1].offset + 0x1ff);
    CHECK(!scan::FileOffsetOf(info, 0x2200, offset));
    CHECK(!scan::FileOffsetOf(info, 0x1800, offset));

    bool ok = false;
    const scan::MappedImage image = MapOne(file, &ok);
    CHECK(ok && image.bytes.size() == 0x2300);
    CHECK(image.bytes[0x2000] == 1 && image.bytes[0x2200] == 0);

    scan::MappedImage image2;
    std::string error;
    CHECK(scan::MapExecutable(file, info, 0, image2, error));
    const uint64_t second_base = (image2.bytes.size() + scan::kModuleAlignment - 1) & ~(scan::kModuleAlignment - 1);
    CHECK(second_base == 0x10000);
    CHECK(scan::MapExecutable(file, info, second_base, image2, error));
    CHECK(image2.bytes.size() == second_base + 0x2300);
    CHECK(image2.executable.size() == 2 && image2.executable[1].begin == second_base);
    CHECK(!scan::MapExecutable(file, info, 0x1000, image2, error));

    Bytes bad = file;
    bad[0] = 'X';
    CHECK(!Parse(bad).valid);
    bad = file;
    const uint64_t elf = 32 + 2 * 32;
    Put<uint16_t>(bad, elf + 18, 40);
    CHECK(!Parse(bad).valid);
    bad = file;
    Put<uint16_t>(bad, elf + 56, 0);
    CHECK(!Parse(bad).valid);
    bad = file;
    Put<uint64_t>(bad, elf + 32, 0xffffffffffffff00ull);
    CHECK(!Parse(bad).valid);
    bad = file;
    Put<uint16_t>(bad, 24, 0xffff);
    CHECK(!Parse(bad).valid);
    bad = file;
    Put<uint64_t>(bad, elf + 64 + 16, 0xfffffffffffff000ull);
    Put<uint64_t>(bad, elf + 64 + 40, 0x2000);
    {
        const scan::ExecutableInfo huge = Parse(bad);
        CHECK(!huge.mappable);
        scan::MappedImage none;
        CHECK(!scan::MapExecutable(bad, huge, 0, none, error) && none.bytes.empty());
    }
    bad = file;
    Put<uint64_t>(bad, 32 + 8, bad.size() + 100);
    CHECK(!Parse(bad).mappable);
    bad = file;
    Put<uint64_t>(bad, 32 + 16, 0x10);
    CHECK(!Parse(bad).mappable);
    bad = file;
    Put<uint64_t>(bad, 32, 0x800 | 0x2);
    CHECK(!Parse(bad).mappable);

    for (size_t length = 0; length < 512 && length < file.size(); ++length) {
        const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
        const scan::ExecutableInfo truncated = Parse(cut);
        CHECK(!truncated.mappable);
    }
    CHECK(!Parse({}).valid);

    size_t mappable = 0;
    for (size_t position = 0; position < 512; ++position) {
        for (const uint8_t value : {uint8_t{0x00}, uint8_t{0xff}, uint8_t{0x80}}) {
            Bytes mutated = file;
            mutated[position] = value;
            const scan::ExecutableInfo mutant = Parse(mutated);
            if (mutant.mappable) {
                scan::MappedImage mapped;
                ++mappable;
                CHECK(scan::MapExecutable(mutated, mutant, 0, mapped, error) ? mapped.bytes.size() <= scan::kMaxImageSize : true);
            }
        }
    }
    CHECK(mappable > 0);
}

const scan::SiteSpec* SiteNamed(std::string_view feature, std::string_view name) {
    for (const scan::SiteSpec& site : scan::Sites()) {
        if (feature == site.feature && name == site.name) {
            return &site;
        }
    }
    return nullptr;
}

// Writes a site's pattern into `code` so that its anchor is at `site_at`, wildcards as zero.
void Embed(Bytes& code, const scan::SiteSpec& site, size_t site_at) {
    std::istringstream words(site.pattern);
    std::string word;
    size_t at = site_at - site.anchor;
    while (words >> word) {
        code[at++] = word == "??" ? 0 : static_cast<uint8_t>(std::stoi(word, nullptr, 16));
    }
}

void Branch(Bytes& code, size_t from, size_t to) {
    Put<int32_t>(code, from + 1, static_cast<int32_t>(static_cast<int64_t>(to) - static_cast<int64_t>(from) - 5));
}

const scan::SiteResult* ResultOf(const scan::ScanResult& result, std::string_view feature, std::string_view name) {
    for (const scan::SiteResult& site : result.sites) {
        if (feature == site.spec->feature && name == site.spec->name) {
            return &site;
        }
    }
    return nullptr;
}

const scan::RelationResult* RelationOf(const scan::ScanResult& result, std::string_view from, std::string_view to) {
    for (const scan::RelationResult& relation : result.relations) {
        if (from == relation.spec->from && to == relation.spec->to) {
            return &relation;
        }
    }
    return nullptr;
}

void TestSitesAndRelations() {
    for (const scan::SiteSpec& site : scan::Sites()) {
        CHECK(site.expected >= 1);
        CHECK(site.anchor < static_cast<size_t>(std::count(site.pattern, site.pattern + std::strlen(site.pattern), ' ')) + 1);
    }
    const scan::SiteSpec* submit = SiteNamed("True ultrawide", "submit");
    const scan::SiteSpec* fills[3] = {SiteNamed("True ultrawide", "fill call 1"), SiteNamed("True ultrawide", "fill call 2"),
                                      SiteNamed("True ultrawide", "fill call 3")};
    CHECK(submit && fills[0] && fills[1] && fills[2]);

    Bytes code = Text(0x4000);
    const size_t submit_at = 0x400, fill_at[3] = {0x1000, 0x1400, 0x1800};
    Embed(code, *submit, submit_at);
    for (int i = 0; i < 3; ++i) {
        Embed(code, *fills[i], fill_at[i]);
        Branch(code, fill_at[i], submit_at);
    }
    std::vector<Segment> segments(1);
    segments[0].data = code;
    segments[0].memsz = code.size();
    scan::MappedImage image = MapOne(MakeSelf(segments));
    scan::ScanResult result = scan::ScanImage(image);
    const scan::SiteResult* found = ResultOf(result, "True ultrawide", "submit");
    CHECK(found && found->status == scan::SiteStatus::kUnique && found->matches.front() == submit_at);
    for (int i = 0; i < 3; ++i) {
        const scan::SiteResult* fill = ResultOf(result, "True ultrawide", fills[i]->name);
        CHECK(fill && fill->status == scan::SiteStatus::kUnique && fill->matches.front() == fill_at[i]);
        const scan::RelationResult* relation = RelationOf(result, fills[i]->name, "submit");
        CHECK(relation && relation->status == scan::RelationStatus::kHolds);
    }
    const scan::SiteResult* missing = ResultOf(result, "True ultrawide", "queue");
    CHECK(missing && missing->status == scan::SiteStatus::kMissing && missing->matches.empty());
    const scan::RelationResult* unverifiable = RelationOf(result, "part queue call", "queue");
    CHECK(unverifiable && unverifiable->status == scan::RelationStatus::kUnverifiable);
    CHECK(!result.lzcnt.complete && result.lzcnt.count == 0);

    Branch(code, fill_at[1], submit_at + 0x40);
    segments[0].data = code;
    result = scan::ScanImage(MapOne(MakeSelf(segments)));
    const scan::RelationResult* broken = RelationOf(result, "fill call 2", "submit");
    CHECK(broken && broken->status == scan::RelationStatus::kBroken);
    const scan::RelationResult* intact = RelationOf(result, "fill call 1", "submit");
    CHECK(intact && intact->status == scan::RelationStatus::kHolds);

    Embed(code, *submit, 0x2400);
    segments[0].data = code;
    result = scan::ScanImage(MapOne(MakeSelf(segments)));
    found = ResultOf(result, "True ultrawide", "submit");
    CHECK(found && found->status == scan::SiteStatus::kAmbiguous && found->count == 2 && found->matches.size() == 2);
    const scan::RelationResult* unresolved = RelationOf(result, "fill call 1", "submit");
    CHECK(unresolved && unresolved->status == scan::RelationStatus::kUnverifiable);

    const scan::SiteSpec* fades = SiteNamed("True ultrawide", "fade jumps");
    CHECK(fades && fades->expected == 2);
    Bytes grouped = Text(0x4000);
    Embed(grouped, *fades, 0x800);
    Embed(grouped, *fades, 0x1800);
    segments[0].data = grouped;
    result = scan::ScanImage(MapOne(MakeSelf(segments)));
    const scan::SiteResult* pair = ResultOf(result, "True ultrawide", "fade jumps");
    CHECK(pair && pair->status == scan::SiteStatus::kGroup && pair->count == 2);
    Embed(grouped, *fades, 0x2800);
    segments[0].data = grouped;
    result = scan::ScanImage(MapOne(MakeSelf(segments)));
    pair = ResultOf(result, "True ultrawide", "fade jumps");
    CHECK(pair && pair->status == scan::SiteStatus::kAmbiguous && pair->count == 3);

    segments[0].data = Bytes();
    CHECK(scan::ScanImage(scan::MappedImage{}).sites.size() == scan::Sites().size());
}

void TestLzcnt() {
    Bytes code = Text(0x1000);
    const uint8_t one[] = {0xf3, 0x48, 0x0f, 0xbd, 0xc8};
    std::memcpy(code.data() + 0x10, one, sizeof(one));
    const uint8_t prefixed[] = {0x66, 0xf3, 0x0f, 0xbd, 0xe8};
    std::memcpy(code.data() + 0x40, prefixed, sizeof(prefixed));
    const uint8_t memory[] = {0xf3, 0x49, 0x0f, 0xbd, 0x84, 0x1f, 0xa8, 0x03, 0x00, 0x00};
    std::memcpy(code.data() + 0x80, memory, sizeof(memory));
    std::vector<Segment> segments(1);
    segments[0].data = code;
    segments[0].memsz = code.size();
    const scan::ScanResult result = scan::ScanImage(MapOne(MakeSelf(segments)));
    CHECK(result.lzcnt.count == 3 && result.lzcnt.first == 0x10 && result.lzcnt.last == 0x80 && !result.lzcnt.complete);
}

// A minimal param.sfo with string fields.
std::string MakeSfo(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string keys, values;
    std::string table;
    auto put32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [](std::string& s, uint16_t v) { s.append(reinterpret_cast<const char*>(&v), 2); };
    for (const auto& [key, value] : fields) {
        put16(table, static_cast<uint16_t>(keys.size()));
        put16(table, 0x0204);
        put32(table, static_cast<uint32_t>(value.size() + 1));
        put32(table, static_cast<uint32_t>(value.size() + 1));
        put32(table, static_cast<uint32_t>(values.size()));
        keys += key;
        keys.push_back('\0');
        values += value;
        values.push_back('\0');
    }
    std::string sfo(1, '\0');
    sfo += "PSF";
    put32(sfo, 0x101);
    const uint32_t key_table = static_cast<uint32_t>(20 + table.size());
    put32(sfo, key_table);
    put32(sfo, static_cast<uint32_t>(key_table + keys.size()));
    put32(sfo, static_cast<uint32_t>(fields.size()));
    return sfo + table + keys + values;
}

void WriteFile(const fs::path& path, const Bytes& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void WriteFile(const fs::path& path, const std::string& data) {
    WriteFile(path, Bytes(data.begin(), data.end()));
}

Bytes SyntheticExecutable(uint8_t fill = 0xcc) {
    std::vector<Segment> segments(2);
    segments[0].data = Text(0x3000, fill);
    segments[0].memsz = 0x3000;
    segments[1].flags = 6;
    segments[1].vaddr = 0x4000;
    segments[1].data = Text(0x400, 7);
    segments[1].memsz = 0x800;
    return MakeSelf(segments, 0x1230);
}

struct GameFolder {
    fs::path root;
    explicit GameFolder(fs::path where) : root(std::move(where)) { fs::remove_all(root); }
    ~GameFolder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void Populate(const fs::path& app0, const std::vector<std::pair<std::string, std::string>>& sfo, const char* archive) {
    WriteFile(app0 / "sce_sys" / "param.sfo", MakeSfo(sfo));
    WriteFile(app0 / "eboot.bin", SyntheticExecutable());
    for (const char* module : kBundledModules) {
        WriteFile(app0 / "sce_module" / module, SyntheticExecutable(0x90));
    }
    WriteFile(app0 / "sce_discmap.plt", std::string(64, 'p'));
    if (archive) {
        WriteFile(app0 / archive, std::string(4096, 'c'));
    }
}

std::map<std::string, std::string> Snapshot(const fs::path& root) {
    std::map<std::string, std::string> state;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            const std::string key = Utf8(entry.path().lexically_relative(root));
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            state[key] = std::to_string(content.size()) + ":" + Sha256Hex(entry.path()) + ":" +
                         std::to_string(fs::last_write_time(entry.path()).time_since_epoch().count());
        }
    }
    return state;
}

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

const GameBuild* FindBuildOfTitle(const std::string& title_id) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (title_id == build.title_id) return &build;
    }
    return nullptr;
}

const std::vector<std::pair<std::string, std::string>> kUnknownDragonsCrown = {
    {"APP_VER", "01.07"}, {"CATEGORY", "gd"},  {"CONTENT_ID", "EP9999-CUSA99998_00-DRAGONSCROWNEU01"},
    {"TITLE", "Dragon's Crown Pro"}, {"TITLE_ID", "CUSA99998"}, {"VERSION", "01.00"}};

void TestClassificationAndReport() {
    GameFolder folder(fs::temp_directory_path() / "dcp_diag_unknown");
    Populate(folder.root, kUnknownDragonsCrown, "DRACRO_EU.CPK");
    const auto before = Snapshot(folder.root);
    const std::string sfo = MakeSfo(kUnknownDragonsCrown);

    const std::unique_ptr<GameSource> source = OpenFolderSource(folder.root);
    const UnsupportedReport report = DiagnoseUnsupportedBuild(*source, sfo);
    CHECK(report.dragons_crown);
    CHECK(report.lines.size() > 20);
    CHECK(report.lines.front() == kDiagnosticsBegin && report.lines.back() == kDiagnosticsEnd);
    CHECK(Contains(report.lines, "TITLE_ID: CUSA99998"));
    CHECK(Contains(report.lines, "CONTENT_ID: EP9999-CUSA99998_00-DRAGONSCROWNEU01"));
    CHECK(Contains(report.lines, "region (from CONTENT_ID prefix): Europe"));
    CHECK(Contains(report.lines, "APP_VER: 01.07"));
    CHECK(Contains(report.lines, "VERSION: 01.00"));
    CHECK(Contains(report.lines, "SHA-256: " + Sha256Hex(folder.root / "eboot.bin")));
    CHECK(Contains(report.lines, "size: " + std::to_string(fs::file_size(folder.root / "eboot.bin")) + " bytes"));
    CHECK(Contains(report.lines, "entry 0x1230"));
    CHECK(Contains(report.lines, "executable load segments: 1"));
    CHECK(Contains(report.lines, "ph[1]: type 0x1 rw- vaddr 0x4000 memsz 0x800"));
    CHECK(Contains(report.lines, "DRACRO_EU.CPK: 4096 bytes"));
    CHECK(Contains(report.lines, "not a readable CPK"));
    CHECK(Contains(report.lines, "Mapping incomplete"));
    CHECK(Contains(report.lines, "Closest structural profile"));
    CHECK(Contains(report.lines, "closest profile: none"));
    CHECK(Contains(report.lines, "no executable patch or hook was applied"));
    CHECK(!Contains(report.lines, Utf8(folder.root)));
    CHECK(!Contains(report.lines, Utf8(fs::temp_directory_path())));
    CHECK(Snapshot(folder.root) == before);

    const std::string japanese = "\xE3\x83\x89\xE3\x83\xA9\xE3\x82\xB4\xE3\x83\xB3\xE3\x82\xBA\xE3\x82\xAF\xE3\x83\xA9\xE3\x82\xA6\xE3\x83\xB3";
    const std::string jp_sfo = MakeSfo({{"APP_VER", "01.09"}, {"CONTENT_ID", "JP9999-CUSA77777_00-SOMETHING00000001"},
                                        {"TITLE", japanese}, {"TITLE_ID", "CUSA77777"}, {"VERSION", "01.00"}});
    CHECK(DiagnoseUnsupportedBuild(*source, jp_sfo).dragons_crown);
    const std::string known_title = MakeSfo({{"APP_VER", "09.99"}, {"CONTENT_ID", "UP2611-CUSA10454_00-DRAGONSCROWNPRO1"},
                                            {"TITLE", "x"}, {"TITLE_ID", "CUSA10454"}, {"VERSION", "01.03"}});
    CHECK(DiagnoseUnsupportedBuild(*source, known_title).dragons_crown);

    const std::string other = MakeSfo({{"APP_VER", "01.00"}, {"CONTENT_ID", "UP0000-CUSA00001_00-OTHERGAME0000001"},
                                       {"TITLE", "Other Game"}, {"TITLE_ID", "CUSA00001"}, {"VERSION", "01.00"}});
    const UnsupportedReport unrelated_archive = DiagnoseUnsupportedBuild(*source, other);
    CHECK(!unrelated_archive.dragons_crown && unrelated_archive.lines.empty());
    CHECK(Contains(unrelated_archive.evidence, "does not name"));
    CHECK(!DiagnoseUnsupportedBuild(*source, std::string()).dragons_crown);
    CHECK(!DiagnoseUnsupportedBuild(*source, "garbage").dragons_crown);

    GameFolder unrelated(fs::temp_directory_path() / "dcp_diag_unrelated");
    Populate(unrelated.root, {{"APP_VER", "01.00"}, {"CONTENT_ID", "UP0000-CUSA00001_00-OTHERGAME0000001"},
                              {"TITLE", "Other Game"}, {"TITLE_ID", "CUSA00001"}, {"VERSION", "01.00"}}, nullptr);
    const UnsupportedReport unrelated_report = DiagnoseUnsupportedBuild(*OpenFolderSource(unrelated.root), other);
    CHECK(!unrelated_report.dragons_crown && unrelated_report.lines.empty());
    CHECK(Contains(unrelated_report.evidence, "no DRACRO*.CPK archive"));
    CHECK(!DiagnoseUnsupportedBuild(*OpenFolderSource(unrelated.root), sfo).dragons_crown);

    GameFolder broken(fs::temp_directory_path() / "dcp_diag_broken");
    Populate(broken.root, kUnknownDragonsCrown, "DRACRO.CPK");
    WriteFile(broken.root / "eboot.bin", std::string(300, 'z'));
    const UnsupportedReport malformed = DiagnoseUnsupportedBuild(*OpenFolderSource(broken.root), sfo);
    CHECK(!malformed.dragons_crown && Contains(malformed.evidence, "not a readable PS4 executable"));
    Bytes damaged = SyntheticExecutable();
    damaged.resize(damaged.size() / 2);
    WriteFile(broken.root / "eboot.bin", damaged);
    const UnsupportedReport truncated = DiagnoseUnsupportedBuild(*OpenFolderSource(broken.root), sfo);
    CHECK(!truncated.dragons_crown || Contains(truncated.lines, "Mapping incomplete"));
    WriteFile(broken.root / "eboot.bin", SyntheticExecutable());
    fs::remove(broken.root / "sce_module" / "libc.prx");
    WriteFile(broken.root / "sce_module" / "libSceFios2.prx", std::string(100, 'q'));
    const UnsupportedReport modules = DiagnoseUnsupportedBuild(*OpenFolderSource(broken.root), sfo);
    CHECK(modules.dragons_crown && Contains(modules.lines, "Executable mapping: incomplete"));
    CHECK(Contains(modules.lines, "sce_module/libc.prx: missing or unreadable"));
}

void TestSfoRobustness() {
    const std::string sfo = MakeSfo(kUnknownDragonsCrown);
    CHECK(SfoFields(sfo).size() == kUnknownDragonsCrown.size());
    for (size_t length = 0; length < sfo.size(); ++length) {
        const std::string cut = sfo.substr(0, length);
        const std::vector<SfoField> fields = SfoFields(cut);
        CHECK(fields.size() <= kUnknownDragonsCrown.size());
        SfoString(cut, "TITLE_ID");
    }
    std::string hostile = sfo;
    for (size_t at = 8; at < 20; ++at) {
        for (const char value : {'\xff', '\x00', '\x7f'}) {
            std::string mutated = hostile;
            mutated[at] = value;
            SfoFields(mutated);
            SfoString(mutated, "TITLE_ID");
        }
    }
}

void TestInstallPath() {
    GameFolder base(fs::temp_directory_path() / "dcp_diag_install");
    const fs::path game = base.root / "game";
    Populate(game, kUnknownDragonsCrown, "DRACRO_EU.CPK");
    const GameRoots roots{base.root / "prepared", base.root / "image"};
    const auto before = Snapshot(game);

    const InstallResult result = InstallFromFolder(game, roots);
    CHECK(result.status == InstallStatus::kUnsupportedBuild && !result.ok());
    CHECK(!result.diagnostics.empty() && result.diagnostics.front() == kDiagnosticsBegin);
    CHECK(!fs::exists(roots.prepared) && !fs::exists(roots.image));
    CHECK(Snapshot(game) == before);
    const InstallMessage message = DescribeInstall(InstallStatus::kUnsupportedBuild);
    CHECK(message.title == "Dragon's Crown Pro version not supported yet");
    CHECK(message.text.find("latest log") != std::string::npos);
    CHECK(message.text.find("GitHub") != std::string::npos);
    CHECK(message.text.find("0x") == std::string::npos);

    GameFolder other_base(fs::temp_directory_path() / "dcp_diag_install_other");
    Populate(other_base.root / "game", {{"APP_VER", "01.00"}, {"CONTENT_ID", "UP0000-CUSA00001_00-OTHERGAME0000001"},
                                       {"TITLE", "Other Game"}, {"TITLE_ID", "CUSA00001"}, {"VERSION", "01.00"}}, "DRACRO_X.CPK");
    const InstallResult other = InstallFromFolder(other_base.root / "game", {other_base.root / "prepared", other_base.root / "image"});
    CHECK(other.status == InstallStatus::kWrongGame && other.diagnostics.empty());

    GameFolder missing_base(fs::temp_directory_path() / "dcp_diag_install_missing");
    Populate(missing_base.root / "game", kUnknownDragonsCrown, "DRACRO_EU.CPK");
    fs::remove(missing_base.root / "game" / "eboot.bin");
    const InstallResult missing = InstallFromFolder(missing_base.root / "game", {missing_base.root / "p", missing_base.root / "i"});
    CHECK(missing.status == InstallStatus::kNotGameFolder && missing.diagnostics.empty());

    const std::string install_source = [] {
        std::ifstream in(fs::path(DCP_SOURCE_DIR) / "launcher" / "game_install.cpp", std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    const size_t first_use = install_source.find("DiagnoseUnsupportedBuild(");
    CHECK(first_use != std::string::npos && install_source.find("DiagnoseUnsupportedBuild(", first_use + 1) == std::string::npos);
    const size_t guard = install_source.rfind("check.state == GameFileState::kDifferentVersion", first_use);
    CHECK(guard != std::string::npos && first_use - guard < 200);
}

// ---- identity does not depend on the executable ----

namespace cpk_build {

void Be16(std::string& o, uint32_t v) { o += char(v >> 8); o += char(v); }
void Be32(std::string& o, uint32_t v) { for (int i = 3; i >= 0; --i) o += char(v >> (8 * i)); }
void Be64(std::string& o, uint64_t v) { for (int i = 7; i >= 0; --i) o += char(v >> (8 * i)); }
void Le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += char(v >> (8 * i)); }
void Le64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o += char(v >> (8 * i)); }

struct Column {
    const char* name;
    int type;
    int storage;
};
struct Cell {
    uint64_t number = 0;
    std::string text;
};
using Row = std::vector<std::pair<std::string, Cell>>;

std::string Utf(const std::string& name, const std::vector<Column>& columns, const std::vector<Row>& rows) {
    std::string strings("<NULL>", 7);
    const auto intern = [&](const std::string& text) {
        const uint32_t off = uint32_t(strings.size());
        strings += text;
        strings += char(0);
        return off;
    };
    const auto cell = [](const Row& row, const char* key) {
        for (const auto& [k, v] : row) {
            if (k == key) return v;
        }
        return Cell{};
    };
    const uint32_t name_off = intern(name);
    std::string cols;
    for (const Column& c : columns) {
        cols += char(c.storage | c.type);
        Be32(cols, intern(c.name));
        if (c.storage == 0x30) {
            const uint64_t v = cell(rows.at(0), c.name).number;
            if (c.type == 2) Be16(cols, uint32_t(v)); else if (c.type == 4) Be32(cols, uint32_t(v)); else Be64(cols, v);
        }
    }
    std::string rowbuf;
    uint32_t row_len = 0;
    for (const Row& row : rows) {
        const size_t start = rowbuf.size();
        for (const Column& c : columns) {
            if (c.storage != 0x50) continue;
            const Cell v = cell(row, c.name);
            if (c.type == 0xA) Be32(rowbuf, intern(v.text));
            else if (c.type == 2) Be16(rowbuf, uint32_t(v.number));
            else if (c.type == 4) Be32(rowbuf, uint32_t(v.number));
            else Be64(rowbuf, v.number);
        }
        row_len = uint32_t(rowbuf.size() - start);
    }
    const uint32_t rows_off = 32 + uint32_t(cols.size()) - 8;
    const uint32_t strings_off = rows_off + uint32_t(rowbuf.size());
    std::string packet("@UTF");
    Be32(packet, 0);
    Be32(packet, rows_off);
    Be32(packet, strings_off);
    Be32(packet, strings_off + uint32_t(strings.size()));
    Be32(packet, name_off);
    Be16(packet, uint32_t(columns.size()));
    Be16(packet, row_len);
    Be32(packet, uint32_t(rows.size()));
    packet += cols + rowbuf + strings;
    const uint32_t size = uint32_t(packet.size() - 8);
    for (int i = 0; i < 4; ++i) packet[4 + i] = char(size >> (8 * (3 - i)));
    return packet;
}

std::string Crypt(std::string packet) {
    uint32_t m = 0x5F;
    for (char& b : packet) {
        b = char(uint8_t(b) ^ uint8_t(m));
        m = (m * 0x15) & 0xFF;
    }
    return packet;
}

std::string Section(const char* magic, const std::string& packet) {
    std::string out(magic, 4);
    Le32(out, 0xFF);
    Le64(out, packet.size());
    return out + packet;
}

Cell N(uint64_t n) { return {n, {}}; }
Cell S(std::string s) { return {0, std::move(s)}; }

std::string BuildCpk(const std::vector<std::string>& paths) {
    const uint64_t toc_offset = 0x800, content_offset = 0x6000, align = 0x800;
    std::string content;
    std::vector<Row> rows;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string data = "data";
        while ((content_offset + content.size()) % align) content += char(0);
        const size_t slash = paths[i].rfind('/');
        rows.push_back({{"DirName", S(paths[i].substr(0, slash))},
                        {"FileName", S(paths[i].substr(slash + 1))},
                        {"FileSize", N(data.size())},
                        {"ExtractSize", N(data.size())},
                        {"FileOffset", N(content_offset + content.size() - toc_offset)},
                        {"ID", N(i)},
                        {"UserString", S("<NULL>")}});
        content += data;
    }
    const std::string toc = Utf("CpkTocInfo",
                                {{"DirName", 0xA, 0x50}, {"FileName", 0xA, 0x50}, {"FileSize", 4, 0x50}, {"ExtractSize", 4, 0x50},
                                 {"FileOffset", 6, 0x50}, {"ID", 4, 0x50}, {"UserString", 0xA, 0x50}},
                                rows);
    const std::string header = Utf("CpkHeader",
                                   {{"TocOffset", 6, 0x50}, {"ContentOffset", 6, 0x50}, {"Files", 4, 0x50}, {"Align", 2, 0x50},
                                    {"EnableTocCrc", 2, 0x50}, {"EnableFileCrc", 2, 0x50}, {"Version", 2, 0x30}},
                                   {{{"TocOffset", N(toc_offset)}, {"ContentOffset", N(content_offset)}, {"Files", N(paths.size())},
                                     {"Align", N(align)}, {"EnableTocCrc", N(1)}, {"EnableFileCrc", N(0)}, {"Version", N(7)}}});
    std::string blob = Section("CPK ", Crypt(header));
    blob.resize(toc_offset, char(0));
    blob += Section("TOC ", toc);
    blob.resize(content_offset, char(0));
    return blob + content;
}

} // namespace cpk_build

const char* const kStems[] = {"Archdemon", "Doppelganger", "Dragonlisk", "DoomBeetle", "Kraken",   "Medusa",
                              "Minotaur",  "Myconid",      "Owlbear",    "Wyvern",     "GiantSpider", "Cyclops",
                              "Chimera",   "Gazer",        "Harpy",      "PlantWorm",  "SaberTooth", "Vampire"};
const char* const kSheets[] = {"TreasureArt", "Temple", "ChallengeQuest", "AbilityList", "ItemList"};

std::string GameCpk(size_t monsters = 18, size_t sheets = 5) {
    std::vector<std::string> paths{"stage/Interface00.ftx", "snd/bgm.acb"};
    for (size_t i = 0; i < monsters; ++i) paths.push_back(std::string("character/") + kStems[i] + "00.mcb");
    for (size_t i = 0; i < sheets; ++i) paths.push_back(std::string("msgsheet/") + kSheets[i] + ".fms");
    return cpk_build::BuildCpk(paths);
}

std::string GenericCpk() {
    return cpk_build::BuildCpk({"data/a.bin", "data/b.bin", "movie/intro.usm", "character/Hero00.mcb", "msgsheet/Menu.fms"});
}

Bytes FingerprintExecutable() {
    Bytes code = Text(0x4000);
    const scan::SiteSpec* submit = SiteNamed("True ultrawide", "submit");
    const scan::SiteSpec* fills[3] = {SiteNamed("True ultrawide", "fill call 1"), SiteNamed("True ultrawide", "fill call 2"),
                                      SiteNamed("True ultrawide", "fill call 3")};
    Embed(code, *submit, 0x400);
    for (int i = 0; i < 3; ++i) {
        Embed(code, *fills[i], 0x1000 + 0x400 * i);
        Branch(code, 0x1000 + 0x400 * i, 0x400);
    }
    std::vector<Segment> segments(1);
    segments[0].data = code;
    segments[0].memsz = code.size();
    return MakeSelf(segments, 0x40);
}

Bytes LzcntExecutable() {
    Bytes code = Text(0x1000);
    const uint8_t one[] = {0xf3, 0x48, 0x0f, 0xbd, 0xc8};
    std::memcpy(code.data() + 0x10, one, sizeof(one));
    const uint8_t two[] = {0xf3, 0x49, 0x0f, 0xbd, 0x84, 0x1f, 0xa8, 0x03, 0x00, 0x00};
    std::memcpy(code.data() + 0x80, two, sizeof(two));
    std::vector<Segment> segments(1);
    segments[0].data = code;
    segments[0].memsz = code.size();
    return MakeSelf(segments, 0x40);
}

struct MemorySource final : GameSource {
    std::map<std::string, Bytes> files;
    std::string Kind() const override { return "package"; }
    std::vector<SourceEntry> Files() const override {
        std::vector<SourceEntry> list;
        for (const auto& [path, data] : files) list.push_back({path, data.size()});
        return list;
    }
    bool Read(const std::string& path, uint64_t max_bytes, Bytes& out) const override {
        const auto found = files.find(path);
        if (found == files.end() || found->second.size() > max_bytes) return false;
        out = found->second;
        return true;
    }
    bool ReadRange(const std::string& path, uint64_t offset, size_t length, Bytes& out) const override {
        const auto found = files.find(path);
        if (found == files.end() || offset > found->second.size() || length > found->second.size() - offset) return false;
        out.assign(found->second.begin() + static_cast<std::ptrdiff_t>(offset),
                   found->second.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return true;
    }
    fs::path LocalPath(const std::string&) const override { return {}; }
    void Put(const std::string& path, const std::string& data) { files[path] = Bytes(data.begin(), data.end()); }
};

MemorySource PackageLike(const std::string& archive_name, const std::string& archive, const Bytes& eboot) {
    MemorySource source;
    source.files["eboot.bin"] = eboot;
    for (const char* module : kBundledModules) source.files[std::string("sce_module/") + module] = SyntheticExecutable(0x90);
    source.Put("sce_discmap.plt", "plt");
    if (!archive_name.empty()) source.Put(archive_name, archive);
    return source;
}

const std::vector<std::pair<std::string, std::string>> kEuropeSfo = {
    {"APP_VER", "01.00"}, {"CATEGORY", "gd"}, {"CONTENT_ID", "EP2611-CUSA10487_00-DRAGONSCROWNPRO1"},
    {"TITLE", "Dragon's Crown Pro"}, {"TITLE_ID", "CUSA10487"}, {"VERSION", "01.00"}};
const std::vector<std::pair<std::string, std::string>> kBlandSfo = {
    {"APP_VER", "01.00"}, {"CONTENT_ID", "EP0000-CUSA10487_00-PRODUCT0000000001"}, {"TITLE", "Product"},
    {"TITLE_ID", "CUSA10487"}, {"VERSION", "01.00"}};

bool Reported(const UnsupportedReport& report) {
    return report.dragons_crown && !report.lines.empty() && report.lines.front() == kDiagnosticsBegin &&
           report.lines.back() == kDiagnosticsEnd;
}

void TestIdentityIndependentOfExecutable() {
    const std::string europe = MakeSfo(kEuropeSfo), bland = MakeSfo(kBlandSfo);
    const Bytes unfamiliar = SyntheticExecutable();

    for (const GameBuild& build : SupportedBuilds()) {
        CHECK(std::string(build.title_id) != "CUSA10487");
    }

    // Unknown TITLE_ID, metadata, resources, executable fingerprints that still match.
    {
        MemorySource source = PackageLike("DRACRO.CPK", GameCpk(), FingerprintExecutable());
        const UnsupportedReport report = DiagnoseUnsupportedBuild(source, europe);
        CHECK(Reported(report));
        CHECK(Contains(report.lines, "[unique] submit"));
        CHECK(Contains(report.lines, "TITLE_ID: CUSA10487"));
        CHECK(Contains(report.lines, "an archive holds the Dragon's Crown resource set"));
    }
    // Zero fingerprints: the report still comes, with the mapping incomplete.
    {
        MemorySource source = PackageLike("DRACRO.CPK", GameCpk(), unfamiliar);
        const UnsupportedReport report = DiagnoseUnsupportedBuild(source, europe);
        CHECK(Reported(report));
        CHECK(Contains(report.lines, "Mapping incomplete"));
        CHECK(Contains(report.lines, "Build-specific features: 0/8 mapped"));
        CHECK(Contains(report.lines, "[missing] submit"));
        CHECK(Contains(report.lines, "LZCNT instructions: 0"));
    }
    // A different LZCNT digest and count.
    {
        MemorySource source = PackageLike("DRACRO.CPK", GameCpk(), LzcntExecutable());
        const UnsupportedReport report = DiagnoseUnsupportedBuild(source, europe);
        CHECK(Reported(report));
        CHECK(Contains(report.lines, "LZCNT instructions: 2 (expected 62)"));
        CHECK(Contains(report.lines, "LZCNT compatibility: 0/1 sites mapped"));
    }
    // Strong resources alone: the archive keeps any name and param.sfo may be absent, unusable or bland.
    for (const std::string& sfo : {std::string(), std::string("garbage"), bland}) {
        MemorySource source = PackageLike("GAME_EU.cpk", GameCpk(), unfamiliar);
        const UnsupportedReport report = DiagnoseUnsupportedBuild(source, sfo);
        CHECK(Reported(report));
        CHECK(Contains(report.lines, "GAME_EU.cpk: CPK with"));
    }
    // Dragon's Crown metadata with a named archive that holds nothing recognizable.
    {
        MemorySource source = PackageLike("DRACRO_EU.CPK", "not a cpk", unfamiliar);
        CHECK(Reported(DiagnoseUnsupportedBuild(source, europe)));
        MemorySource generic = PackageLike("DRACRO_EU.CPK", GenericCpk(), unfamiliar);
        CHECK(Reported(DiagnoseUnsupportedBuild(generic, europe)));
    }

    // Unrelated games.
    {
        MemorySource no_archive = PackageLike("", "", unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(no_archive, bland).dragons_crown);
        CHECK(!DiagnoseUnsupportedBuild(no_archive, europe).dragons_crown);
        MemorySource fake = PackageLike("DRACRO.CPK", "fake contents", unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(fake, bland).dragons_crown);
        CHECK(!DiagnoseUnsupportedBuild(fake, std::string()).dragons_crown);
        MemorySource generic = PackageLike("DRACRO.CPK", GenericCpk(), unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(generic, bland).dragons_crown);
        MemorySource other = PackageLike("data.cpk", GenericCpk(), unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(other, bland).dragons_crown);
        MemorySource weak = PackageLike("DRACRO.CPK", GameCpk(11, 5), unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(weak, bland).dragons_crown);
        MemorySource few_sheets = PackageLike("DRACRO.CPK", GameCpk(18, 2), unfamiliar);
        CHECK(!DiagnoseUnsupportedBuild(few_sheets, bland).dragons_crown);
        MemorySource random_exe = PackageLike("DRACRO.CPK", GameCpk(), Bytes(500, 0x41));
        CHECK(!DiagnoseUnsupportedBuild(random_exe, europe).dragons_crown);
    }

    // Malformed archives never crash or classify.
    {
        const std::string cpk = GameCpk();
        for (size_t length = 0; length < cpk.size(); length += 61) {
            MemorySource source = PackageLike("DRACRO.CPK", cpk.substr(0, length), unfamiliar);
            const UnsupportedReport report = DiagnoseUnsupportedBuild(source, bland);
            CHECK(!report.dragons_crown || length > 0x800);
        }
        for (size_t at = 0; at < 0x900; at += 3) {
            for (const char value : {char(0xff), char(0x00), char(0x7f)}) {
                std::string hostile = cpk;
                hostile[at] = value;
                MemorySource source = PackageLike("DRACRO.CPK", hostile, unfamiliar);
                DiagnoseUnsupportedBuild(source, bland);
            }
        }
    }
}

void TestEuropeanInstall() {
    GameFolder base(fs::temp_directory_path() / "dcp_diag_cusa10487");
    const fs::path game = base.root / "game";
    WriteFile(game / "sce_sys" / "param.sfo", MakeSfo(kEuropeSfo));
    WriteFile(game / "eboot.bin", SyntheticExecutable());
    for (const char* module : kBundledModules) WriteFile(game / "sce_module" / module, SyntheticExecutable(0x90));
    WriteFile(game / "sce_discmap.plt", std::string(64, 'p'));
    WriteFile(game / "DRACRO.CPK", GameCpk());
    const GameRoots roots{base.root / "prepared", base.root / "image"};
    const auto before = Snapshot(game);

    const InstallResult result = InstallFromFolder(game, roots);
    CHECK(result.status == InstallStatus::kUnsupportedBuild && !result.ok());
    CHECK(Contains(result.diagnostics, "TITLE_ID: CUSA10487"));
    CHECK(Contains(result.diagnostics, "Dragon's Crown monster resources"));
    CHECK(Contains(result.diagnostics, "no executable patch or hook was applied"));
    CHECK(!fs::exists(roots.prepared) && !fs::exists(roots.image));
    CHECK(Snapshot(game) == before);
    CHECK(FindBuildOfTitle("CUSA10487") == nullptr);
    CHECK(DescribeInstall(result.status).title == "Dragon's Crown Pro version not supported yet");
}

// ---- known-build self-test against the local game fixtures ----

struct Fixture {
    const char* folder;
    const char* label_prefix;
    int reference;
};

const Fixture kFixtures[] = {
    {"v1.03", "CUSA10454 1.00", 0},        {"CUSA07001-1.00", "CUSA07001 1.00", 1}, {"CUSA07001-1.02", "CUSA07001 1.02", 2},
    {"CUSA07001-1.03", "CUSA07001 1.03", 3}, {"CUSA10209-1.00", "CUSA10209 1.00", 4},
};

Bytes ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string ReadText(const char* relative) {
    const Bytes bytes = ReadBytes(fs::path(DCP_SOURCE_DIR) / relative);
    return std::string(bytes.begin(), bytes.end());
}

// Production values: feature -> site -> offset, for one reference build, read from the source tables.
using Expected = std::map<std::pair<std::string, std::string>, uint64_t>;

uint64_t Hex(const std::string& text) {
    return std::stoull(text, nullptr, 16);
}

Expected ProductionTables(const Fixture& fixture) {
    Expected expected;
    const std::string key(fixture.label_prefix);
    const std::string dashed = [&] {
        std::string copy = key;
        const size_t space = copy.find(' ');
        copy[space] = '-';
        return copy;
    }();
    const std::regex number("0x[0-9a-f]+");
    const auto numbers = [&](const std::string& text) {
        std::vector<uint64_t> values;
        for (std::sregex_iterator it(text.begin(), text.end(), number), end; it != end; ++it) {
            values.push_back(Hex(it->str()));
        }
        return values;
    };
    const auto row = [&](const std::string& source, const std::string& label, const char* tail) {
        const size_t at = source.find("\"" + label);
        if (at == std::string::npos) {
            return std::vector<uint64_t>();
        }
        const size_t end = source.find(tail, at);
        return numbers(source.substr(at, end - at));
    };

    const std::string ultrawide = ReadText("src/ultrawide_hooks.c");
    const std::string records = ultrawide.substr(ultrawide.find("ultrawide_records["));
    const char* names[] = {"submit", "fill call 1", "fill call 2", "fill call 3", "frame commit", "queue", "part queue call",
                           "model draw", "widget draw call", "map draw call", "facility draw call", "worldmap draw"};
    if (const auto v = row(records, key, "}},"); v.size() >= 16) {
        for (size_t i = 0; i < 12; ++i) {
            expected[{"True ultrawide", names[i]}] = v[2 + i];
        }
        expected[{"True ultrawide", "fade jumps"}] = v[14];
    }
    expected[{"True ultrawide", "camera view call"}] = 0x118e9;
    expected[{"True ultrawide", "view build"}] = 0x110b0;
    const uint64_t ortho[] = {0x10686, 0x106bb, 0x10699, 0x106d5, 0x106f7, 0x10703};
    for (size_t i = 0; i < 6; ++i) {
        expected[{"True ultrawide", "pass ortho load " + std::to_string(i + 1)}] = ortho[i];
    }

    const std::string system_type = ReadText("src/system_type_hook.c");
    {
        const size_t at = system_type.find("\"" + key);
        const size_t tail = system_type.find("}}, ", at);
        const auto sites = numbers(system_type.substr(at, tail - at));
        const auto set_text = numbers(system_type.substr(tail, 20));
        if (sites.size() > 3 && !set_text.empty()) {
            expected[{"System type", "card site 1"}] = sites[2];
            expected[{"System type", "card site 2"}] = sites[3];
            expected[{"System type", "set text"}] = set_text[0];
        }
    }

    const std::string skip = ReadText("src/skip_offers.c");
    if (const auto v = row(skip, dashed, "}},"); v.size() >= 5) {
        expected[{"Skip offers", "story window"}] = v[0];
        expected[{"Skip offers", "tutorial window"}] = v[3];
    }

    if (std::string(fixture.label_prefix).starts_with("CUSA07001")) {
        const std::string startup = ReadText("src/startup_save_hook.c");
        if (const auto v = row(startup, key, "ACCEPT_PROLOGUE}"); v.size() >= 5) {
            expected[{"Startup save", "step tail"}] = v[2];
            expected[{"Startup save", "block owner"}] = v[3];
            expected[{"Startup save", "accept"}] = v[4];
        }
    }
    if (fixture.reference == 0) {
        const std::string cheats = ReadText("src/cheat_hooks.cpp");
        const std::regex hook("\\{\"([^\"]+)\", (0x[0-9a-f]+), (\\d+), \\{");
        for (std::sregex_iterator it(cheats.begin(), cheats.end(), hook), end; it != end; ++it) {
            expected[{"Cheats", "hook: " + (*it)[1].str()}] = Hex((*it)[2].str());
        }
        const std::regex function("constexpr uint64_t k(\\w+)Offset = (0x[0-9a-f]+);");
        for (std::sregex_iterator it(cheats.begin(), cheats.end(), function), end; it != end; ++it) {
            const std::string name = (*it)[1].str();
            if (cheats.find("k" + name + "Prologue[]") != std::string::npos) {
                expected[{"Cheats", "function: " + name}] = Hex((*it)[2].str());
            }
        }
        expected[{"Japanese NPC voices", "NPC cue site"}] = 0x68bc9;
        expected[{"Japanese NPC voices", "resource acquire"}] = 0x57cbb0;
    }
    return expected;
}

void TestKnownBuilds() {
    const fs::path root = fs::path(DCP_SOURCE_DIR) / "game" / "prepared";
    size_t tested = 0;
    for (const Fixture& fixture : kFixtures) {
        const fs::path app0 = root / fixture.folder;
        if (!fs::is_regular_file(app0 / "eboot.bin")) {
            std::printf("known-build self-test skipped for %s (no local fixture)\n", fixture.folder);
            continue;
        }
        ++tested;
        const std::unique_ptr<GameSource> source = OpenFolderSource(app0);
        const Bytes eboot = ReadBytes(app0 / "eboot.bin");
        scan::MappedImage image;
        std::string error;
        const scan::ExecutableInfo info = scan::ParseExecutable(eboot);
        CHECK(info.valid && info.mappable && scan::MapExecutable(eboot, info, 0, image, error));
        for (const char* module : kBundledModules) {
            const Bytes bytes = ReadBytes(app0 / "sce_module" / module);
            const scan::ExecutableInfo module_info = scan::ParseExecutable(bytes);
            const uint64_t base = (image.bytes.size() + scan::kModuleAlignment - 1) & ~(scan::kModuleAlignment - 1);
            CHECK(scan::MapExecutable(bytes, module_info, base, image, error));
        }
        const scan::ReferenceBuild& reference = scan::kReferenceBuilds[fixture.reference];
        CHECK(image.bytes.size() == reference.image_size && info.entry == reference.entry);

        const scan::ScanResult result = scan::ScanImage(image);
        const Expected expected = ProductionTables(fixture);
        size_t recovered = 0, unique = 0, ambiguous = 0, missing = 0, mismatches = 0;
        for (const auto& [name, rva] : expected) {
            const scan::SiteResult* site = ResultOf(result, name.first, name.second);
            CHECK(site != nullptr);
            if (!site) {
                continue;
            }
            const bool found = !site->matches.empty() && site->matches.front() == rva;
            recovered += found ? 1 : 0;
            unique += site->status == scan::SiteStatus::kUnique || site->status == scan::SiteStatus::kGroup ? 1 : 0;
            ambiguous += site->status == scan::SiteStatus::kAmbiguous ? 1 : 0;
            missing += site->status == scan::SiteStatus::kMissing ? 1 : 0;
            if (!found) {
                ++mismatches;
                std::printf("  mismatch %s / %s: production 0x%llx, recovered %zu matches\n", name.first.c_str(),
                            name.second.c_str(), static_cast<unsigned long long>(rva), site->count);
            }
        }
        const GameBuild* profile = nullptr;
        for (const GameBuild& build : SupportedBuilds()) {
            if (std::string(build.label).starts_with(fixture.label_prefix)) {
                profile = &build;
            }
        }
        CHECK(profile != nullptr);
        size_t resolution_expected = 0, resolution_recovered = 0;
        if (profile) {
            DcpResolutionSite sites[DCP_CUSA10209_102_SIGNATURE_COUNT];
            DcpResolutionSignatureResult found[DCP_CUSA10209_102_SIGNATURE_COUNT] = {};
            const int all = dcp_find_resolution_sites(image.bytes.data(), image.bytes.size(), dcp_cusa10209_102_signatures,
                                                      DCP_CUSA10209_102_SIGNATURE_COUNT, sites, found);
            CHECK(all == 1);
            std::set<uint32_t> recovered_offsets;
            for (const DcpResolutionSite& site : sites) {
                recovered_offsets.insert(site.offset);
            }
            for (const DcpResolutionSite& site : profile->resolution_sites) {
                ++resolution_expected;
                resolution_recovered += recovered_offsets.contains(site.offset) ? 1 : 0;
            }
            CHECK(resolution_expected == resolution_recovered);
        }
        const std::string lzcnt = ReadText("src/lzcnt_compat.c");
        const size_t section = lzcnt.find(std::string("{\"") + fixture.label_prefix);
        const size_t section_end = lzcnt.find("}},", section);
        const std::regex offset("0x[0-9a-f]+");
        std::vector<uint64_t> lzcnt_offsets;
        const std::string block = lzcnt.substr(section, section_end - section);
        for (std::sregex_iterator it(block.begin(), block.end(), offset), end; it != end; ++it) {
            lzcnt_offsets.push_back(Hex(it->str()));
        }
        CHECK(lzcnt_offsets.size() == 63);
        CHECK(result.lzcnt.complete && result.lzcnt.count == lzcnt_offsets.size() - 2 + 1);

        const GameCheck tree = CheckGameTree(app0);
        CHECK(tree.state == GameFileState::kValid && tree.build == profile);
        CHECK(Reported(DiagnoseUnsupportedBuild(*source, std::string())));
        std::printf("%s: expected %zu build-dependent sites (+%zu render sites, 61 LZCNT sites), recovered %zu, unique %zu, "
                    "ambiguous %zu, missing %zu, mismatches %zu; render sites %zu/%zu; relations %zu/%zu hold; LZCNT %zu\n",
                    fixture.label_prefix, expected.size(), resolution_expected, recovered, unique, ambiguous, missing, mismatches,
                    resolution_recovered, resolution_expected,
                    static_cast<size_t>(std::count_if(result.relations.begin(), result.relations.end(), [](const auto& r) {
                        return r.status == scan::RelationStatus::kHolds;
                    })),
                    result.relations.size(), result.lzcnt.count);
        CHECK(mismatches == 0 && ambiguous == 0 && missing == 0);
        CHECK(std::all_of(result.relations.begin(), result.relations.end(),
                          [](const auto& r) { return r.status == scan::RelationStatus::kHolds; }));
        CHECK(KnownUnsupportedBuild(Sha256Hex(app0 / "eboot.bin")).empty());
        for (const scan::SiteResult& site : result.sites) {
            const uint32_t want = site.spec->reference[fixture.reference];
            if (want != 0) {
                CHECK(!site.matches.empty() && site.matches.front() == want);
            }
        }
    }
    if (tested == 0) {
        std::printf("known-build self-test skipped: no local game fixtures\n");
    }
}

} // namespace

int main() {
    TestPatterns();
    TestExecutableParsing();
    TestSitesAndRelations();
    TestLzcnt();
    TestSfoRobustness();
    TestClassificationAndReport();
    TestInstallPath();
    TestIdentityIndependentOfExecutable();
    TestEuropeanInstall();
    TestKnownBuilds();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
