#include "localization/ui_text.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "game_profile.h"

namespace dcp {
namespace localization {

extern const char kPtBrUiTextTsv[];

namespace {

std::atomic<bool> g_portuguese_brazil{false};

bool DecodeUi(std::string_view escaped, std::string& out) {
    out.clear();
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '\\') {
            out.push_back(escaped[i]);
            continue;
        }
        if (++i >= escaped.size()) {
            return false;
        }
        switch (escaped[i]) {
        case 'n': out.push_back('\n'); break;
        case 's': out.push_back(' '); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        default: return false;
        }
    }
    return true;
}

} // namespace

void SetActiveLanguage(std::string_view key) { g_portuguese_brazil = key == kPortugueseBrazilKey; }

bool PortugueseBrazilSelected() { return g_portuguese_brazil; }

UiTextTable ParseUiTextTable(std::string_view tsv) {
    UiTextTable table;
    size_t start = 0;
    uint32_t number = 0;
    while (start < tsv.size()) {
        size_t end = tsv.find('\n', start);
        if (end == std::string_view::npos) {
            end = tsv.size();
        }
        const std::string_view line = tsv.substr(start, end - start);
        start = end + 1;
        ++number;
        if (line.empty()) {
            continue;
        }
        const size_t tab = line.find('\t');
        UiTextRow row;
        if (tab == std::string_view::npos || line.find('\t', tab + 1) != std::string_view::npos ||
            !DecodeUi(line.substr(0, tab), row.english) || !DecodeUi(line.substr(tab + 1), row.text)) {
            table.errors.push_back("row " + std::to_string(number) + ": expected english and text columns");
            continue;
        }
        if (row.english.empty() || row.text.empty()) {
            table.errors.push_back("row " + std::to_string(number) + ": empty english or text");
            continue;
        }
        table.rows.push_back(std::move(row));
    }
    return table;
}

const UiTextTable& PortugueseBrazilUiText() {
    static const UiTextTable table = ParseUiTextTable(kPtBrUiTextTsv);
    return table;
}

} // namespace localization

namespace {

const std::unordered_map<std::string_view, const std::string*>& PortugueseBrazilIndex() {
    static const auto index = [] {
        std::unordered_map<std::string_view, const std::string*> map;
        for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) {
            map.emplace(row.english, &row.text);
        }
        return map;
    }();
    return index;
}

struct ValueRow {
    std::string_view before, after;
    std::string_view text_before, text_after;
};

constexpr std::string_view kValueMark = "%s";

const std::vector<ValueRow>& PortugueseBrazilValueRows() {
    static const auto rows = [] {
        std::vector<ValueRow> found;
        for (const localization::UiTextRow& row : localization::PortugueseBrazilUiText().rows) {
            const std::string_view english = row.english, text = row.text;
            const size_t at = english.find(kValueMark), text_at = text.find(kValueMark);
            if (at != std::string_view::npos && at == english.rfind(kValueMark) && text_at != std::string_view::npos &&
                text_at == text.rfind(kValueMark)) {
                found.push_back({english.substr(0, at), english.substr(at + kValueMark.size()), text.substr(0, text_at),
                                 text.substr(text_at + kValueMark.size())});
            }
        }
        // "Cannot write the update file %s." before "Cannot write %s.".
        std::stable_sort(found.begin(), found.end(), [](const ValueRow& a, const ValueRow& b) {
            return a.before.size() + a.after.size() > b.before.size() + b.after.size();
        });
        return found;
    }();
    return rows;
}

} // namespace

const char* Tr(const char* english) {
    if (english == nullptr || !localization::PortugueseBrazilSelected()) {
        return english;
    }
    const auto& index = PortugueseBrazilIndex();
    const auto found = index.find(english);
    return found == index.end() ? english : found->second->c_str();
}

std::string Tr(const std::string& english) {
    if (!localization::PortugueseBrazilSelected()) {
        return english;
    }
    const auto& index = PortugueseBrazilIndex();
    const auto found = index.find(english);
    return found == index.end() ? english : *found->second;
}

std::string TrValue(const std::string& english) {
    if (!localization::PortugueseBrazilSelected()) {
        return english;
    }
    const auto& index = PortugueseBrazilIndex();
    if (const auto found = index.find(english); found != index.end()) {
        return *found->second;
    }
    const std::string_view text = english;
    for (const ValueRow& row : PortugueseBrazilValueRows()) {
        if (text.size() > row.before.size() + row.after.size() && text.starts_with(row.before) && text.ends_with(row.after)) {
            const std::string_view value = text.substr(row.before.size(), text.size() - row.before.size() - row.after.size());
            return std::string(row.text_before).append(value).append(row.text_after);
        }
    }
    return english;
}

} // namespace dcp
