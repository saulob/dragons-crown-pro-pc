// Serving the saved custom looks to the game: one atlas per customized character, added to DRACRO.CPK as it is read
// and named by the appearance hooks only where that character is drawn.
#pragma once

#include <array>
#include <compare>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appearance_custom.h"
#include "appearance_hooks.h"
#include "character_art.h"
#include "cpk_patch.h"
#include "save_data.h"
#include "save_discovery.h"

namespace dcp {

static_assert(kCharIdentityBytes == APPEARANCE_IDENTITY_BYTES);

// One character's custom atlas: a changed look or preset is a different instance.
struct CustomInstanceKey {
    int data_slot = 0;
    int character_slot = 0;
    CharacterClass klass = CharacterClass::kFighter;
    int base = 0;
    uint64_t look_hash = 0;
    auto operator<=>(const CustomInstanceKey&) const = default;
    std::string Text() const;
};

uint64_t CustomLookHash(const CustomLook& look);
// The archive entry of an instance: PLAYER/AMAZON00_D1C02_1A2B3C4D.FTX.
std::string CustomInstanceEntry(const CustomInstanceKey& key);
// The same for its default weapon texture: PLAYER/FIGHTER00_SHIELD01_D1C02_1A2B3C4D.FTX.
std::string CustomWeaponEntry(const CustomInstanceKey& key, size_t weapon);

struct CustomInstance {
    CustomKey key;
    CustomInstanceKey instance;
    std::string name;
    std::array<uint8_t, kCharIdentityBytes> identity{};
    CustomLook look;
    std::string entry;
    std::vector<std::pair<size_t, std::string>> weapon_entries; // weapon index, entry; only for weapons the look changes
};

// The instances for a title: every saved character with an active look, each on its own atlas. Characters the game
// could not tell apart are left native; `notes` says why an entry was not used.
std::vector<CustomInstance> PlanCustomInstances(const CustomStore& store, const std::vector<DiscoveredSave>& saves,
                                                std::string_view title_id, std::vector<std::string>& notes);

struct CustomOverlay {
    ArchiveOverlay overlay;
    std::vector<AppearanceInstance> runtime; // in the order of the instances built
};

std::optional<CustomOverlay> BuildCustomOverlay(const std::filesystem::path& archive, const std::vector<CustomInstance>& instances,
                                                CharacterArt& art, std::vector<std::string>& notes);

} // namespace dcp
