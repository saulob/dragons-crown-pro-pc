#include "save_import.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_ps3.h"
#include "save_ps3_layout.h"
#include "save_sfo.h"

namespace dcp {
namespace {

struct TitleEntry {
    std::string_view id;
    std::string_view platform;
    std::string_view region;
};

constexpr TitleEntry kKnownTitles[] = {
    {"PCSE00019", "Vita", "USA"},   {"PCSB00408", "Vita", "Europe"}, {"PCSG00187", "Vita", "Asia"},
    {"VLJM35040", "Vita", "Japan"}, {"VCAS34032", "Vita", "Asia"},   {"VCAS34037", "Vita", "Asia"},
    {"VCAS34029", "Vita", "Asia"},  {"VCKS64028", "Vita", "Korea"},  {"BLUS30767", "PS3", "USA"},
    {"NPUB31235", "PS3", "USA"},    {"NPEB01836", "PS3", "Europe"},  {"BLES01950", "PS3", "Europe"},
    {"BLJM61053", "PS3", "Japan"},  {"BLJM61041", "PS3", "Japan"},   {"BLAS50638", "PS3", "Asia"},
    {"BCAS20298", "PS3", "Asia"},   {"BCAS20290", "PS3", "Asia"},    {"BCAS20288", "PS3", "Asia"},
};

constexpr size_t kMaxSaveBytes = 16u << 20;
constexpr size_t kMaxSfoBytes = 64u << 10;
constexpr size_t kMaxPfdBytes = 64u << 10;
constexpr size_t kMaxFolderVisits = 5000;
constexpr int kMaxFolderDepth = 4;
constexpr double kEncryptedEntropy = 7.5;
constexpr size_t kPs3EncryptedSize = kSaveFileSize + 8;

struct Candidate {
    std::filesystem::path file;
    std::string entry;
    std::string title_from_sfo;
    std::string title_from_path;
    bool has_sfo = false;
    bool has_pfd = false;
    bool has_sce_sys = false;
    std::string save_directory;
    std::vector<uint8_t> pfd;
    std::vector<uint8_t> bytes;
};

std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return result;
}

const TitleEntry* FindTitle(std::string_view title_id) {
    for (const TitleEntry& entry : kKnownTitles) {
        if (title_id == entry.id) return &entry;
    }
    return nullptr;
}

// A folder or entry name that starts with a known Title ID ("BLUS30767-AUTO_0-", "PCSE00019_SLOT0").
std::string TitleOfName(std::string_view name) {
    if (name.size() < 9) return {};
    const std::string_view head = name.substr(0, 9);
    if (FindTitle(head) || ProTitleInfo(head)) return std::string(head);
    return {};
}

enum class Read { kOk, kUnreadable, kTooLarge };

Read ReadLimited(const std::filesystem::path& path, size_t limit, std::vector<uint8_t>& out) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return Read::kUnreadable;
    if (size > limit) return Read::kTooLarge;
    std::ifstream in(path, std::ios::binary);
    if (!in) return Read::kUnreadable;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return Read::kOk;
}

std::string TitleOfAncestors(const std::filesystem::path& file) {
    for (std::filesystem::path current = file.parent_path(); !current.empty(); current = current.parent_path()) {
        if (const std::string title = TitleOfName(current.filename().string()); !title.empty()) return title;
        if (current == current.parent_path()) break;
    }
    return {};
}

// The files a save folder keeps beside SAVE0.DAT, by lower-case name; PARAM.SFO may sit in sce_sys on Vita.
void ReadSiblings(const std::filesystem::path& dir, Candidate& candidate) {
    std::error_code ec;
    std::vector<uint8_t> sfo;
    const auto look = [&](const std::filesystem::path& folder, bool in_sce_sys) {
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = Lower(it->path().filename().string());
            if (name == "param.sfo" && sfo.empty()) {
                candidate.has_sfo = ReadLimited(it->path(), kMaxSfoBytes, sfo) == Read::kOk;
            } else if (name == "param.pfd") {
                candidate.has_pfd = true;
                if (ReadLimited(it->path(), kMaxPfdBytes, candidate.pfd) != Read::kOk) candidate.pfd.clear();
            } else if (in_sce_sys && (name == "sealedkey" || name == "keystone")) {
                candidate.has_sce_sys = true;
            }
        }
        ec.clear();
    };
    look(dir, false);
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (Lower(it->path().filename().string()) == "sce_sys" && it->is_directory(ec)) {
            candidate.has_sce_sys = true;
            look(it->path(), true);
        }
    }
    if (const std::optional<SfoInfo> info = ParseParamSfo(sfo)) {
        candidate.title_from_sfo = TitleIdOfSfo(*info);
        if (const auto found = info->text.find("SAVEDATA_DIRECTORY"); found != info->text.end()) candidate.save_directory = found->second;
    }
}

ImportBlock CollectFile(const std::filesystem::path& file, Candidate& candidate) {
    candidate.file = file;
    const Read read = ReadLimited(file, kMaxSaveBytes, candidate.bytes);
    if (read != Read::kOk) return read == Read::kTooLarge ? ImportBlock::kTooLarge : ImportBlock::kUnreadable;
    candidate.title_from_path = TitleOfAncestors(file);
    ReadSiblings(file.parent_path(), candidate);
    if (const std::string folder = file.parent_path().filename().string(); !folder.empty()) candidate.save_directory = folder;
    return ImportBlock::kNone;
}

ImportBlock CollectFolder(const std::filesystem::path& folder, std::vector<Candidate>& found) {
    std::error_code ec;
    size_t visited = 0;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && it.depth() <= kMaxFolderDepth; it.increment(ec)) {
        if (++visited > kMaxFolderVisits) return ImportBlock::kAmbiguous;
        if (it->is_regular_file(ec) && Lower(it->path().filename().string()) == "save0.dat") files.push_back(it->path());
    }
    for (const std::filesystem::path& file : files) {
        Candidate candidate;
        if (const ImportBlock block = CollectFile(file, candidate); block != ImportBlock::kNone) return block;
        found.push_back(std::move(candidate));
    }
    return ImportBlock::kNone;
}

ImportBlock BlockOfPs3(Ps3Status status) {
    switch (status) {
    case Ps3Status::kUnknownTitle: return ImportBlock::kPs3UnknownTitle;
    case Ps3Status::kHashMismatch: return ImportBlock::kPs3HashMismatch;
    case Ps3Status::kCryptoFailed:
    case Ps3Status::kBadPlaintext: return ImportBlock::kPs3Undecryptable;
    default: return ImportBlock::kPs3PfdInvalid;
    }
}

void FillFromPs3(ImportResult& result, const Ps3Plaintext& plain) {
    result.data_slots = plain.slots;
    result.characters = plain.characters;
}

// A PS3 source is unwrapped and converted in memory; only a converted save that passes the Pro checks is kept.
void DescribePs3(ImportResult& result, const Candidate& candidate) {
    result.platform = "PS3";
    result.conversion_required = true;
    const std::string directory = !candidate.save_directory.empty() ? candidate.save_directory : result.title_id;
    std::vector<uint8_t> plain;
    if (InspectPs3Plaintext(result.bytes).valid) {
        result.format = "PS3 layout, big-endian";
        plain = std::move(result.bytes);
    } else if (!candidate.has_pfd) {
        result.encrypted = true;
        result.pfd_state = "Missing";
        result.format = "PS3 container, encrypted";
        result.block = ImportBlock::kPs3NeedsPfd;
        result.bytes.clear();
        return;
    } else {
        result.encrypted = true;
        result.format = "PS3 container, encrypted";
        Ps3Pfd header;
        result.pfd_state = ParsePfd(candidate.pfd, header) == Ps3Status::kOk ? "Valid" : "Invalid";
        Ps3Status status = DecryptPs3Save0(candidate.pfd, result.bytes, directory, plain);
        if (status == Ps3Status::kOk && !InspectPs3Plaintext(plain).valid) status = Ps3Status::kBadPlaintext;
        if (status != Ps3Status::kOk) {
            result.decryption = status == Ps3Status::kUnknownTitle ? "Not supported" : "";
            result.errors.push_back(Ps3StatusText(status));
            result.block = BlockOfPs3(status);
            result.bytes.clear();
            return;
        }
        result.decryption = "Supported";
        result.format = "PS3 layout, big-endian (decrypted)";
    }

    result.ps3_layout_version = uint32_t(plain[8]) << 24 | uint32_t(plain[9]) << 16 | uint32_t(plain[10]) << 8 | plain[11];
    std::vector<uint8_t> pro;
    std::vector<std::string> problems;
    Ps3ConvertReport report;
    if (!ConvertPs3ToPro(plain, pro, report) || !ValidateSave(pro, problems)) {
        FillFromPs3(result, InspectPs3Plaintext(plain));
        result.block = ImportBlock::kPs3UnsupportedLayout;
        result.bytes.clear();
        return;
    }
    result.bytes = std::move(pro);
}

void Describe(ImportResult& result, const Candidate& candidate) {
    result.file = candidate.file;
    result.entry = candidate.entry;
    result.has_param_sfo = candidate.has_sfo;
    result.has_param_pfd = candidate.has_pfd;
    result.bytes = candidate.bytes;
    result.title_id = !candidate.title_from_sfo.empty() ? candidate.title_from_sfo : candidate.title_from_path;
    if (const TitleEntry* known = FindTitle(result.title_id)) {
        result.platform = known->platform;
        result.region = known->region;
    } else if (const TitleInfo* pro = ProTitleInfo(result.title_id)) {
        result.platform = pro->platform;
        result.region = pro->region;
    } else if (candidate.has_pfd) {
        result.platform = "PS3";
    } else if (candidate.has_sce_sys) {
        result.platform = "Vita";
    }

    std::vector<std::string> problems;
    const bool pro_layout = ValidateSave(result.bytes, problems);
    const bool encrypted = !pro_layout && ByteEntropy(result.bytes) > kEncryptedEntropy;
    const bool ps3_shaped = !pro_layout && (result.bytes.size() == kPs3EncryptedSize && encrypted);
    if (result.platform == "PS3" || ps3_shaped || InspectPs3Plaintext(result.bytes).valid) {
        if (pro_layout) {
            result.block = ImportBlock::kPs3UnsupportedLayout;
            result.conversion_required = true;
            result.format = "PS3 layout, big-endian";
            result.bytes.clear();
            return;
        }
        DescribePs3(result, candidate);
    } else {
        const auto refuse = [&](ImportBlock block, const char* format) {
            result.block = block;
            result.format = format;
            result.errors = problems;
        };
        if (result.platform == "Vita" && encrypted) {
            result.encrypted = true;
            refuse(ImportBlock::kEncryptedVita, "PS Vita container, encrypted");
        } else if (!pro_layout) {
            refuse(ImportBlock::kInvalidSave, "Not a Dragon's Crown Pro save");
        } else {
            result.format = "Dragon's Crown Pro layout";
        }
    }
    if (result.block != ImportBlock::kNone) return;

    const SaveModel model = SaveModel::FromBytes(result.bytes);
    result.valid = model.valid();
    if (!result.valid) {
        result.block = ImportBlock::kInvalidSave;
        result.errors = model.errors();
        return;
    }
    result.data_slots = int(kDataSlotCount);
    for (const DataSlotView& slot : model.slots()) {
        for (const CharacterView& character : slot.characters) result.characters += character.present ? 1 : 0;
    }
    result.detected = (result.platform.empty() ? std::string("Dragon's Crown") : result.platform + " Dragon's Crown") +
                      (result.region.empty() ? std::string() : " (" + result.region + ")");
}

bool SameBytes(const std::vector<Candidate>& found) {
    return std::all_of(found.begin(), found.end(), [&](const Candidate& other) { return other.bytes == found.front().bytes; });
}

ImportResult Finish(ImportResult result, std::vector<Candidate>& found) {
    result.candidates = int(found.size());
    if (found.empty()) {
        result.block = ImportBlock::kNoSave;
        return result;
    }
    if (!SameBytes(found)) {
        result.block = ImportBlock::kAmbiguous;
        return result;
    }
    Describe(result, found.front());
    return result;
}

std::string DirectoryOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

ImportBlock BlockOfZip(ZipError error) {
    switch (error) {
    case ZipError::kUnsafePath: return ImportBlock::kArchiveUnsafe;
    case ZipError::kTooLarge:
    case ZipError::kTooManyEntries: return ImportBlock::kArchiveLimits;
    case ZipError::kEncrypted: return ImportBlock::kArchiveEncrypted;
    default: return ImportBlock::kArchiveInvalid;
    }
}

bool HasPrefix(const std::vector<uint8_t>& bytes, std::string_view magic) {
    return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

} // namespace

double ByteEntropy(std::span<const uint8_t> bytes) {
    if (bytes.empty()) return 0.0;
    std::array<size_t, 256> counts{};
    for (const uint8_t byte : bytes) ++counts[byte];
    double entropy = 0.0;
    for (const size_t count : counts) {
        if (count == 0) continue;
        const double p = double(count) / double(bytes.size());
        entropy -= p * std::log2(p);
    }
    return entropy;
}

const char* ImportPlatformOf(std::string_view title_id) {
    const TitleEntry* entry = FindTitle(title_id);
    return entry ? entry->platform.data() : "";
}

std::string ImportRegionOf(std::string_view title_id) {
    const TitleEntry* entry = FindTitle(title_id);
    return entry ? std::string(entry->region) : std::string();
}

const char* ImportBlockText(ImportBlock block) {
    switch (block) {
    case ImportBlock::kNone: return "";
    case ImportBlock::kUnreadable: return "The source could not be read.";
    case ImportBlock::kTooLarge: return "The file is too large to be a Dragon's Crown save.";
    case ImportBlock::kNoSave: return "No SAVE0.DAT was found.";
    case ImportBlock::kAmbiguous:
        return "More than one different SAVE0.DAT was found. Choose the folder or archive of a single save.";
    case ImportBlock::kRarArchive:
        return "RAR archives are not opened by the launcher. Extract it first, then import the extracted folder.";
    case ImportBlock::kArchiveInvalid: return "The archive could not be read as a ZIP file.";
    case ImportBlock::kArchiveUnsafe: return "The archive has an unsafe file path and was not opened.";
    case ImportBlock::kArchiveLimits: return "The archive is too large, or holds too many files, to be a single save.";
    case ImportBlock::kArchiveEncrypted: return "The archive is password protected.";
    case ImportBlock::kPs3NeedsPfd:
        return "A PS3 save needs its PARAM.PFD beside SAVE0.DAT. Choose the save folder or a ZIP of it, not SAVE0.DAT alone.";
    case ImportBlock::kPs3PfdInvalid: return "PARAM.PFD could not be read as the description of a Dragon's Crown PS3 save.";
    case ImportBlock::kPs3UnknownTitle: return "This PS3 save is not from Dragon's Crown, so it is not supported.";
    case ImportBlock::kPs3HashMismatch: return "SAVE0.DAT does not match its PARAM.PFD: the save is damaged or was modified.";
    case ImportBlock::kPs3Undecryptable: return "SAVE0.DAT could not be decrypted into a Dragon's Crown save.";
    case ImportBlock::kPs3UnsupportedLayout:
        return "Unsupported PS3 save layout: only PS3 saves of layout version 36 can be converted, and this save uses a different one.";
    case ImportBlock::kEncryptedVita: return "This PS Vita save is still encrypted. Export a decrypted SAVE0.DAT first.";
    case ImportBlock::kInvalidSave: return "This is not a valid Dragon's Crown Pro SAVE0.DAT.";
    }
    return "";
}

ImportResult InspectZip(std::span<const uint8_t> zip, const std::filesystem::path& name, const ZipLimits& limits) {
    ImportResult result;
    result.source = ImportSource::kZip;
    result.file = name;
    ZipReader reader(zip, limits);
    if (const ZipError error = reader.Open(); error != ZipError::kNone) {
        result.block = BlockOfZip(error);
        return result;
    }
    const std::vector<ZipEntry>& entries = reader.entries();
    std::vector<size_t> saves;
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string& path = entries[i].name;
        if (!entries[i].directory && Lower(path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1)) == "save0.dat") {
            saves.push_back(i);
        }
    }
    result.candidates = int(saves.size());
    if (saves.empty()) {
        result.block = ImportBlock::kNoSave;
        return result;
    }
    const auto same = [&](size_t index) {
        return entries[index].size == entries[saves.front()].size && entries[index].crc == entries[saves.front()].crc;
    };
    if (!std::all_of(saves.begin(), saves.end(), same)) {
        result.block = ImportBlock::kAmbiguous;
        return result;
    }

    const ZipEntry& chosen = entries[saves.front()];
    Candidate candidate;
    candidate.file = name;
    candidate.entry = chosen.name;
    const std::string directory = DirectoryOf(chosen.name);
    for (const char* part : {"", "sce_sys/"}) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].directory || Lower(entries[i].name) != Lower(directory + part + "param.sfo")) continue;
            std::vector<uint8_t> sfo;
            if (entries[i].size <= kMaxSfoBytes && reader.Extract(i, sfo) == ZipError::kNone) {
                candidate.has_sfo = true;
                if (const std::optional<SfoInfo> info = ParseParamSfo(sfo)) candidate.title_from_sfo = TitleIdOfSfo(*info);
            }
        }
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const std::string lower = Lower(entries[i].name);
        if (lower == Lower(directory + "param.pfd")) {
            candidate.has_pfd = true;
            if (entries[i].size > kMaxPfdBytes || reader.Extract(i, candidate.pfd) != ZipError::kNone) candidate.pfd.clear();
        }
        if (lower.rfind(Lower(directory + "sce_sys/"), 0) == 0) candidate.has_sce_sys = true;
    }
    if (directory.size() > 1) {
        const size_t slash = directory.rfind('/', directory.size() - 2);
        candidate.save_directory = directory.substr(slash == std::string::npos ? 0 : slash + 1, directory.size() - 1 - (slash == std::string::npos ? 0 : slash + 1));
    }
    for (size_t at = 0; at < chosen.name.size() && candidate.title_from_path.empty();) {
        const size_t slash = chosen.name.find('/', at);
        if (slash == std::string::npos) break;
        candidate.title_from_path = TitleOfName(std::string_view(chosen.name).substr(at, slash - at));
        at = slash + 1;
    }
    if (candidate.title_from_path.empty()) candidate.title_from_path = TitleOfName(name.stem().string());

    if (const ZipError error = reader.Extract(saves.front(), candidate.bytes); error != ZipError::kNone) {
        result.block = BlockOfZip(error);
        return result;
    }
    std::vector<Candidate> found;
    found.push_back(std::move(candidate));
    return Finish(std::move(result), found);
}

ImportResult InspectImport(const std::filesystem::path& source) {
    ImportResult result;
    std::error_code ec;
    result.file = source;
    if (std::filesystem::is_directory(source, ec)) {
        result.source = ImportSource::kFolder;
        std::vector<Candidate> found;
        if (const ImportBlock block = CollectFolder(source, found); block != ImportBlock::kNone) {
            result.block = block;
            result.candidates = int(found.size());
            return result;
        }
        return Finish(std::move(result), found);
    }

    std::vector<uint8_t> head;
    constexpr size_t kMaxArchiveBytes = 96u << 20;
    if (!std::filesystem::is_regular_file(source, ec)) {
        result.block = ImportBlock::kUnreadable;
        return result;
    }
    const uintmax_t size = std::filesystem::file_size(source, ec);
    if (ec) {
        result.block = ImportBlock::kUnreadable;
        return result;
    }
    {
        std::ifstream in(source, std::ios::binary);
        head.resize(8);
        in.read(reinterpret_cast<char*>(head.data()), 8);
        head.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    }
    constexpr char kRarMagic[] = {'R', 'a', 'r', '!', 0x1a, 0x07};
    constexpr char kZipMagic[] = {'P', 'K', 0x03, 0x04};
    constexpr char kEmptyZipMagic[] = {'P', 'K', 0x05, 0x06};
    if (HasPrefix(head, std::string_view(kRarMagic, sizeof(kRarMagic)))) {
        result.source = ImportSource::kRar;
        result.block = ImportBlock::kRarArchive;
        return result;
    }
    if (HasPrefix(head, std::string_view(kZipMagic, sizeof(kZipMagic))) ||
        HasPrefix(head, std::string_view(kEmptyZipMagic, sizeof(kEmptyZipMagic)))) {
        std::vector<uint8_t> archive;
        if (size > kMaxArchiveBytes) {
            result.source = ImportSource::kZip;
            result.block = ImportBlock::kArchiveLimits;
            return result;
        }
        if (const Read read = ReadLimited(source, kMaxArchiveBytes, archive); read != Read::kOk) {
            result.block = ImportBlock::kUnreadable;
            return result;
        }
        return InspectZip(archive, source);
    }

    result.source = ImportSource::kSaveFile;
    std::vector<Candidate> found(1);
    if (const ImportBlock block = CollectFile(source, found.front()); block != ImportBlock::kNone) {
        result.block = block;
        return result;
    }
    return Finish(std::move(result), found);
}

std::optional<std::string> CommitImport(const ImportResult& import, const std::filesystem::path& dest_save0_path,
                                        const std::filesystem::path& backup_folder, std::string_view title_id,
                                        std::string_view dir_name) {
    std::vector<std::string> problems;
    if (!import.valid || import.block != ImportBlock::kNone || !ValidateSave(import.bytes, problems)) {
        return "the import was not inspected successfully";
    }
    std::error_code ec;
    if (std::filesystem::exists(dest_save0_path, ec)) {
        if (!CreateBackup(backup_folder, dest_save0_path, title_id, dir_name)) {
            return "could not back up the destination save before importing";
        }
    }
    if (std::optional<std::string> error = WriteSaveAtomically(dest_save0_path, import.bytes, ValidateSave)) {
        return error;
    }
    std::vector<uint8_t> written;
    if (ReadLimited(dest_save0_path, kMaxSaveBytes, written) != Read::kOk || written != import.bytes) {
        return "the imported save did not read back identically";
    }
    return std::nullopt;
}

} // namespace dcp
