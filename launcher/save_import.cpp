#include "save_import.h"

#include <fstream>
#include <iterator>
#include <system_error>

#include "save_backup.h"
#include "save_data.h"

namespace dcp {
namespace {

struct TitleEntry {
    std::string_view id;
    std::string_view platform;
    std::string_view region;
};

// The PS Vita and PS3 Dragon's Crown title ids, with their platforms and regions.
constexpr TitleEntry kKnownTitles[] = {
    {"PCSE00019", "Vita", "USA"}, {"PCSB00408", "Vita", "Europe"}, {"PCSG00187", "Vita", "Asia"},
    {"VLJM35040", "Vita", "Japan"}, {"VCAS34032", "Vita", "Asia"}, {"VCAS34037", "Vita", "Asia"},
    {"VCAS34029", "Vita", "Asia"}, {"VCKS64028", "Vita", "Korea"}, {"BLUS30767", "PS3", "USA"},
    {"NPUB31235", "PS3", "USA"},   {"NPEB01836", "PS3", "Europe"}, {"BLJM61053", "PS3", "Japan"},
    {"BLAS50638", "PS3", "Asia"},
};

const TitleEntry* FindTitle(std::string_view title_id) {
    for (const TitleEntry& entry : kKnownTitles) {
        if (title_id == entry.id) {
            return &entry;
        }
    }
    return nullptr;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Finds SAVE0.DAT in a folder, looking one level down for a savedata/<title>/<dir>/SAVE0.DAT layout.
std::filesystem::path FindSave0(const std::filesystem::path& folder) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end;
         !ec && it != end && it.depth() <= 3; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == "SAVE0.DAT") {
            return it->path();
        }
    }
    return {};
}

std::string TitleIdFromPath(const std::filesystem::path& path) {
    std::filesystem::path current = path;
    while (!current.empty()) {
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        const std::string name = parent.filename().string();
        for (const TitleEntry& entry : kKnownTitles) {
            if (name == entry.id) {
                return name;
            }
        }
        current = parent;
    }
    return {};
}

} // namespace

const char* ImportPlatformOf(std::string_view title_id) {
    const TitleEntry* entry = FindTitle(title_id);
    return entry ? entry->platform.data() : "";
}

std::string ImportRegionOf(std::string_view title_id) {
    const TitleEntry* entry = FindTitle(title_id);
    return entry ? std::string(entry->region) : std::string();
}

ImportResult InspectImport(const std::filesystem::path& source) {
    ImportResult result;
    std::error_code ec;
    std::filesystem::path save0 = source;
    if (std::filesystem::is_directory(source, ec)) {
        save0 = FindSave0(source);
        if (save0.empty()) {
            result.errors.push_back("no SAVE0.DAT found in the selected folder");
            return result;
        }
    }
    if (!ReadFileBytes(save0, result.bytes)) {
        result.errors.push_back("could not read the save");
        return result;
    }
    const std::string title_id = TitleIdFromPath(save0);
    if (const TitleEntry* entry = FindTitle(title_id)) {
        result.platform = entry->platform;
        result.region = entry->region;
    }
    std::vector<std::string> problems;
    if (!ValidateSave(result.bytes, problems)) {
        result.errors = problems;
        return result;
    }
    const SaveModel model = SaveModel::FromBytes(result.bytes);
    if (!model.valid()) {
        result.errors = model.errors();
        return result;
    }
    result.valid = true;
    result.data_slots = static_cast<int>(kDataSlotCount);
    for (const DataSlotView& slot : model.slots()) {
        for (const CharacterView& character : slot.characters) {
            if (character.present) {
                ++result.characters;
            }
        }
    }
    std::string detected = result.platform.empty() ? "Dragon's Crown" : std::string(result.platform) + " Dragon's Crown";
    if (!result.region.empty()) {
        detected += " (" + result.region + ")";
    }
    result.detected = detected;
    return result;
}

std::optional<std::string> CommitImport(const ImportResult& import, const std::filesystem::path& dest_save0_path) {
    if (!import.valid) {
        return "the import was not inspected successfully";
    }
    return WriteSaveAtomically(dest_save0_path, import.bytes, ValidateSave);
}

} // namespace dcp
