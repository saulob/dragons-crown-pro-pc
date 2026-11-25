// The project's tracked translation sheets (localization/en/sheets and localization/pt-BR/sheets) as the tests read them.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "net/json.h"

namespace project_sheets {

inline std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The text under `key` of every entry of a sheet file, by record index; an entry's shared indices each get it.
inline std::map<size_t, std::string> Entries(const std::filesystem::path& file, const char* key) {
    std::map<size_t, std::string> entries;
    const std::optional<dcp::net::Json> json = dcp::net::Json::parse(ReadFile(file), 8);
    if (!json || !(*json)["entries"].is_array()) return entries;
    for (const dcp::net::Json& item : (*json)["entries"].items()) {
        const auto add = [&](const dcp::net::Json& index) {
            if (index.is_number() && index.as_int() >= 0) entries[size_t(index.as_int())] = item[key].as_string();
        };
        if (item["index"].is_array()) {
            for (const dcp::net::Json& index : item["index"].items()) add(index);
        } else {
            add(item["index"]);
        }
    }
    return entries;
}

// A sheet: the USA slot's English records, and the Portuguese entries by record index.
struct Sheet {
    std::vector<std::string> english;
    std::map<size_t, std::string> portuguese;
};

inline Sheet Load(const std::filesystem::path& source, const char* sheet) {
    Sheet loaded;
    const std::string file = std::string(sheet) + ".json";
    const std::filesystem::path english = source / "localization" / "en" / "sheets" / file;
    const std::optional<dcp::net::Json> json = dcp::net::Json::parse(ReadFile(english), 8);
    if (json && (*json)["records"].is_number()) loaded.english.resize(size_t((*json)["records"].as_int()));
    for (const auto& [index, text] : Entries(english, "en")) {
        if (index < loaded.english.size()) loaded.english[index] = text;
    }
    loaded.portuguese = Entries(source / "localization" / "pt-BR" / "sheets" / file, "pt_br");
    return loaded;
}

// The Portuguese text of a record, or the English one where the translation has none.
inline std::string Text(const Sheet& sheet, size_t index) {
    const auto found = sheet.portuguese.find(index);
    if (found != sheet.portuguese.end()) return found->second;
    return index < sheet.english.size() ? sheet.english[index] : std::string();
}

inline bool Present(const std::filesystem::path& source, std::initializer_list<const char*> sheets) {
    for (const char* sheet : sheets) {
        const std::string file = std::string(sheet) + ".json";
        if (!std::filesystem::is_regular_file(source / "localization" / "en" / "sheets" / file) ||
            !std::filesystem::is_regular_file(source / "localization" / "pt-BR" / "sheets" / file)) {
            return false;
        }
    }
    return true;
}

} // namespace project_sheets
