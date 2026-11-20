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
#include "appearance_remote.h"
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

// A saved or remote look's textures from the native ones `read` gives; with `helmet_copies` (the Fighter model drawing
// its off-head helmets from copies is served) a Fighter atlas whose helmet shows gets the copies. Weapons that cannot be
// customized stay native, with a note.
std::optional<LookTextures> BuildLookTextures(std::span<const uint8_t> native_atlas, const EntryReader& read,
                                              const std::vector<RegionMap>& maps, CharacterClass klass, const CustomLook& look,
                                              bool helmet_copies, std::vector<std::string>& notes, std::string* error);
bool HidesFighterHelmet(CharacterClass klass, const CustomLook& look);

struct CustomOverlay {
    ArchiveOverlay overlay;
    std::vector<AppearanceInstance> runtime; // in the order of the instances built
    std::vector<RemoteTextureSet> remote;    // with their offsets, when online
    bool helmet_copies = false;
};

// Online (`remote` given): the remote textures are reserved and the Fighter model with helmet copies is always served, so
// another player's hidden helmet can be drawn; an overlay is made even with no saved look.
std::optional<CustomOverlay> BuildCustomOverlay(const std::filesystem::path& archive, const std::vector<CustomInstance>& instances,
                                                CharacterArt& art, std::vector<std::string>& notes,
                                                const std::vector<RemoteTextureSet>* remote = nullptr,
                                                const ArchiveOverlay* base = nullptr);

} // namespace dcp
