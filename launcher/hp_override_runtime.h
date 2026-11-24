#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "hp_override.h"
#include "hp_override_hooks.h"
#include "save_discovery.h"

namespace dcp {

// Resolves persisted HP overrides for a title against its discovered saves, keyed by the game's 18-byte character
// identity. Conflicting overrides for the same identity are dropped safely with a diagnostic note.
std::vector<HpOverrideEntry> PlanHpOverrides(const HpOverrideStore& store, const std::vector<DiscoveredSave>& saves,
                                             std::string_view title_id, std::vector<std::string>& notes);

} // namespace dcp

extern "C" size_t dcp_character_hp_overrides(const char* image_dir, const char* user_dir,
                                             const HpOverrideEntry** entries, char* message, size_t message_size);
