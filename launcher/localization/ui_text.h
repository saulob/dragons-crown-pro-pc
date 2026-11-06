// The launcher's own text in Portuguese (Brazil), keyed by the English string the launcher draws.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dcp {

// The text of the active language for an English UI string. English text with no translation is returned as is.
const char* Tr(const char* english);
std::string Tr(const std::string& english);
// Tr for text built at runtime: a row with one %s ("Request sent to %s.") also matches that text with any value in its
// place. The row is never used as a format string, so the value can hold anything a player typed.
std::string TrValue(const std::string& english);

namespace localization {

// The language the launcher's own text is shown in, set from text_language at start and as the choice changes.
void SetActiveLanguage(std::string_view key);
bool PortugueseBrazilSelected();

struct UiTextRow {
    std::string english;
    std::string text;
};

struct UiTextTable {
    std::vector<UiTextRow> rows;
    std::vector<std::string> errors;
};

// Rows are `english<TAB>text`; both columns use \n, \s (space), \t and \\ escapes.
UiTextTable ParseUiTextTable(std::string_view tsv);

const UiTextTable& PortugueseBrazilUiText();

} // namespace localization
} // namespace dcp
