// A bounds-checked reader of PARAM.SFO, enough to name the title a PlayStation save belongs to.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace dcp {

struct SfoInfo {
    std::map<std::string, std::string> text; // string entries by key
};

std::optional<SfoInfo> ParseParamSfo(std::span<const uint8_t> bytes);

// The Title ID an SFO names: SAVEDATA_DIRECTORY on PS3 ("BLUS30767-AUTO_0-"), PARENT_DIRECTORY on Vita ("/PCSE00019").
std::string TitleIdOfSfo(const SfoInfo& sfo);

} // namespace dcp
