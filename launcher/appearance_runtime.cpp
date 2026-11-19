#include "appearance_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <map>

#include "character_art.h"
#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "fighter_bare_head.h"
#include "game_files.h"
#include "save_discovery.h"
#include "utf8.h"

namespace dcp {
namespace {

const CharacterView* SavedCharacter(const std::vector<DiscoveredSave>& saves, const CustomKey& key, const SaveModel** model) {
    for (const DiscoveredSave& save : saves) {
        if (save.title_id != key.title_id || save.dir_name != key.save_dir || !save.model.valid()) continue;
        const CharacterView* character = save.model.Character(key.data_slot, key.character_slot);
        if (character && character->present && character->klass == key.klass) {
            *model = &save.model;
            return character;
        }
    }
    return nullptr;
}

// The bytes the game compares, as appearance_same_identity does.
std::vector<uint8_t> ComparedIdentity(const std::array<uint8_t, kCharIdentityBytes>& identity) {
    std::vector<uint8_t> compared(identity.begin(), identity.begin() + 6);
    compared.insert(compared.end(), identity.begin() + 8, identity.end());
    return compared;
}

bool Identifiable(const std::array<uint8_t, kCharIdentityBytes>& identity) {
    const std::vector<uint8_t> compared = ComparedIdentity(identity);
    const auto all = [&](uint8_t value) { return std::all_of(compared.begin(), compared.end(), [&](uint8_t b) { return b == value; }); };
    return !all(0) && !all(0xCC);
}

std::string Label(const CustomInstanceKey& key) {
    return "DATA " + std::to_string(key.data_slot + 1) + " slot " + std::to_string(key.character_slot + 1) + " " +
           std::string(ClassName(key.klass)) + " colour " + std::to_string(key.base);
}

void CopyText(char* out, size_t size, const std::string& text) {
    std::snprintf(out, size, "%s", text.c_str());
}

} // namespace

std::string CustomInstanceKey::Text() const {
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(look_hash));
    return Label(*this) + " look " + hash;
}

uint64_t CustomLookHash(const CustomLook& look) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ull; };
    mix(look.enabled);
    mix(look.base);
    for (const PartLook& part : look.parts) {
        for (const char c : part.key) mix(uint8_t(c));
        mix(0);
        mix(part.hidden);
        mix(part.color.has_value());
        if (part.color) {
            mix(part.color->r);
            mix(part.color->g);
            mix(part.color->b);
        }
    }
    return hash;
}

namespace {

std::string InstanceName(std::string native, const CustomInstanceKey& key) {
    if (native.size() < 4) return {};
    native.resize(native.size() - 4);
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "_D%dC%02d_%08X.FTX", key.data_slot + 1, key.character_slot + 1,
                  static_cast<unsigned>(key.look_hash ^ key.look_hash >> 32));
    return native + suffix;
}

} // namespace

std::string CustomInstanceEntry(const CustomInstanceKey& key) {
    return InstanceName(NativeColorAtlas(key.klass, key.base), key);
}

std::string CustomWeaponEntry(const CustomInstanceKey& key, size_t weapon) {
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(key.klass);
    if (weapon >= weapons.size()) return {};
    return InstanceName(WeaponAtlasName(key.klass, weapons[weapon]), key);
}

std::vector<CustomInstance> PlanCustomInstances(const CustomStore& store, const std::vector<DiscoveredSave>& saves,
                                                std::string_view title_id, std::vector<std::string>& notes) {
    std::vector<CustomInstance> planned;
    for (const auto& [key, look] : store.entries()) {
        if (key.title_id != title_id || !look.active()) continue;
        const SaveModel* model = nullptr;
        const CharacterView* character = SavedCharacter(saves, key, &model);
        if (!character || !character->color_valid()) {
            notes.push_back("no saved character for " + key.Text());
            continue;
        }
        CustomInstance instance;
        instance.key = key;
        instance.instance = {key.data_slot, key.character_slot, key.klass, int(character->color), CustomLookHash(look)};
        instance.name = character->name;
        std::memcpy(instance.identity.data(), model->bytes().data() + character->record_offset + kCharIdentity,
                    kCharIdentityBytes);
        if (!Identifiable(instance.identity)) {
            notes.push_back(key.Text() + " has no character identity; left native");
            continue;
        }
        instance.look = look;
        instance.entry = CustomInstanceEntry(instance.instance);
        for (size_t w = 0; w < WeaponTexturesOf(key.klass).size(); ++w) {
            if (WeaponChanged(key.klass, w, look)) instance.weapon_entries.emplace_back(w, CustomWeaponEntry(instance.instance, w));
        }
        planned.push_back(std::move(instance));
    }
    // A character saved twice (two save folders) is one character to the game: one look, or none.
    std::map<std::vector<uint8_t>, std::vector<size_t>> by_identity;
    for (size_t i = 0; i < planned.size(); ++i) by_identity[ComparedIdentity(planned[i].identity)].push_back(i);
    std::vector<bool> dropped(planned.size(), false);
    for (const auto& [identity, indices] : by_identity) {
        for (size_t n = 1; n < indices.size(); ++n) {
            const CustomInstance& first = planned[indices.front()];
            const CustomInstance& other = planned[indices[n]];
            const bool same = first.look == other.look && first.instance.base == other.instance.base &&
                              first.instance.klass == other.instance.klass;
            dropped[indices[n]] = true;
            if (!same) {
                dropped[indices.front()] = true;
                notes.push_back(first.key.Text() + " and " + other.key.Text() +
                                " are the same character to the game with different looks; left native");
            }
        }
    }
    std::vector<CustomInstance> result;
    for (size_t i = 0; i < planned.size(); ++i) {
        if (!dropped[i]) result.push_back(std::move(planned[i]));
    }
    return result;
}

namespace {

bool HidesFighterHelmet(const CustomInstance& instance) {
    const PartLook* helmet = instance.look.Find("helmet");
    return instance.instance.klass == CharacterClass::kFighter && instance.look.active() && helmet && helmet->hidden;
}

// With a Fighter's helmet hidden, the off-head helmets of the shared model draw from copies in every Fighter atlas, so the
// model and the native atlases are served with them; the hidden helmet's own atlas leaves its copies empty.
std::optional<std::vector<std::pair<std::string, std::vector<uint8_t>>>> FighterHelmetFiles(const CpkArchive& archive,
                                                                                            std::string* error) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    const std::string model_name = GameModelName(CharacterClass::kFighter);
    const CpkEntry* model_entry = archive.Find(model_name);
    std::optional<std::vector<uint8_t>> model = model_entry ? FighterModelWithHelmetCopies(archive.Read(*model_entry, error)) : std::nullopt;
    if (!model) {
        if (error && error->empty()) *error = model_name + " is not the model the helmet table was made for";
        return std::nullopt;
    }
    files.emplace_back(model_name, std::move(*model));
    for (int color = 0; color < kNativeColorCount; ++color) {
        const std::string atlas = NativeColorAtlas(CharacterClass::kFighter, color);
        const CpkEntry* entry = archive.Find(atlas);
        std::vector<uint8_t> pack = entry ? archive.Read(*entry, error) : std::vector<uint8_t>{};
        if (pack.empty() || !AddHelmetCopies(pack)) {
            if (error && error->empty()) *error = atlas + " cannot take the helmet copies";
            return std::nullopt;
        }
        files.emplace_back(atlas, std::move(pack));
    }
    return files;
}

} // namespace

std::optional<CustomOverlay> BuildCustomOverlay(const std::filesystem::path& archive_path,
                                                const std::vector<CustomInstance>& instances, CharacterArt& art,
                                                std::vector<std::string>& notes) {
    CpkArchive archive;
    std::string error;
    if (instances.empty() || !archive.Open(archive_path, &error)) {
        if (!error.empty()) notes.push_back(error);
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> added, replaced;
    if (std::any_of(instances.begin(), instances.end(), HidesFighterHelmet)) {
        if (auto files = FighterHelmetFiles(archive, &error)) {
            replaced = std::move(*files);
            notes.push_back("Fighter helmet copies served with " + GameModelName(CharacterClass::kFighter));
        } else {
            notes.push_back("Fighter helmet copies not served: " + error);
            error.clear();
        }
    }
    std::vector<AppearanceInstance> runtime;
    std::map<std::string, std::vector<uint8_t>> base_atlases;
    for (const CustomInstance& instance : instances) {
        const CharacterClass klass = instance.instance.klass;
        const std::string atlas = NativeColorAtlas(klass, instance.instance.base);
        auto [found, fresh] = base_atlases.try_emplace(atlas);
        if (fresh) {
            const CpkEntry* entry = archive.Find(atlas);
            if (entry) found->second = archive.Read(*entry, &error);
        }
        const std::vector<RegionMap>* maps = art.Regions(klass);
        std::optional<std::vector<uint8_t>> custom;
        if (maps && !found->second.empty()) custom = CustomizeAtlas(found->second, *maps, klass, instance.look, &error);
        if (!custom || instance.entry.empty()) {
            notes.push_back(instance.instance.Text() + ": " + atlas + " could not be customized" +
                            (error.empty() ? "" : ": " + error));
            continue;
        }
        if (!replaced.empty() && klass == CharacterClass::kFighter && !HidesFighterHelmet(instance) && !AddHelmetCopies(*custom)) {
            notes.push_back(instance.instance.Text() + ": the helmet copies could not be added");
            continue;
        }
        added.emplace_back(instance.entry, std::move(*custom));
        AppearanceInstance served{};
        std::memcpy(served.identity, instance.identity.data(), kCharIdentityBytes);
        served.klass = uint8_t(klass);
        served.color = uint8_t(instance.instance.base);
        CopyText(served.entry, sizeof(served.entry), instance.entry);
        CopyText(served.label, sizeof(served.label), Label(instance.instance));
        for (const auto& [weapon, entry] : instance.weapon_entries) {
            const std::string native = WeaponAtlasName(klass, WeaponTexturesOf(klass)[weapon]);
            const CpkEntry* found_weapon = archive.Find(native);
            std::optional<std::vector<uint8_t>> custom_weapon;
            if (found_weapon) custom_weapon = CustomizeWeapon(archive.Read(*found_weapon, &error), klass, weapon, instance.look, &error);
            if (!custom_weapon || weapon >= std::size(served.weapons)) {
                notes.push_back(instance.instance.Text() + ": " + native + " could not be customized" + (error.empty() ? "" : ": " + error));
                continue;
            }
            added.emplace_back(entry, std::move(*custom_weapon));
            CopyText(served.weapons[weapon].native, sizeof(served.weapons[weapon].native), native);
            CopyText(served.weapons[weapon].entry, sizeof(served.weapons[weapon].entry), entry);
            notes.push_back("custom weapon texture " + entry + " replaces " + native + " while it is the one drawn");
        }
        runtime.push_back(served);
        notes.push_back("custom texture instance " + instance.entry + " created for " + instance.instance.Text() + " (" +
                        instance.name + ")");
        std::string overridden;
        for (const PartLook& part : instance.look.parts) {
            if (!part.color && !part.hidden) continue;
            overridden += (overridden.empty() ? "" : ", ") + PartLabel(klass, part.key);
        }
        notes.push_back("base " + std::to_string(instance.instance.base) + " + explicit overrides " + overridden); // DEBUG_SAULO
        for (const PartLook& part : instance.look.parts) {
            const std::string label = PartLabel(klass, part.key);
            if (part.hidden) notes.push_back("hidden " + label);
            else if (part.color) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%02X%02X%02X", part.color->r, part.color->g, part.color->b);
                notes.push_back("applied " + label + " #" + hex);
            }
        }
    }
    if (added.empty()) return std::nullopt;
    std::error_code ec;
    const uint64_t length = std::filesystem::file_size(archive_path, ec);
    if (ec) {
        notes.push_back("cannot read the size of " + Utf8(archive_path.filename()));
        return std::nullopt;
    }
    std::optional<ArchiveOverlay> overlay = OverlayChangedEntries(archive_path, length, replaced, added, &error);
    if (!overlay) {
        notes.push_back("overlay failed: " + error);
        return std::nullopt;
    }
    CpkArchive served;
    if (!OpenOverlaid(archive_path, *overlay, served, &error)) {
        notes.push_back("the overlaid archive cannot be read back: " + error);
        return std::nullopt;
    }
    for (const auto* files : {&added, &replaced}) {
        for (const auto& [path, data] : *files) {
            const CpkEntry* entry = served.Find(path);
            if (!entry || entry->size != data.size() || ReadOverlaid(archive_path, *overlay, entry->offset, entry->size) != data) {
                notes.push_back("the overlaid archive does not hold " + path);
                return std::nullopt;
            }
        }
    }
    return CustomOverlay{std::move(*overlay), std::move(runtime)};
}

} // namespace dcp

extern "C" const char* dcp_custom_appearance(const char* image_dir, const char* app0, const char* user_dir, uint64_t* length,
                                             DcpFilePatch** patches, size_t* count, const AppearanceInstance** instances,
                                             size_t* instance_count, char* message, size_t message_size) {
    using namespace dcp;
    static std::list<CustomOverlay> retained; // the runtime borrows the overlay and the instances for the process
    static std::vector<DcpFilePatch> table;
    if (message_size) *message = '\0';
    const GameBuild* build = BuildOfImageDir(image_dir);
    if (!build || !app0 || !user_dir) return nullptr;
    const std::filesystem::path user = PathFromUtf8(user_dir);
    CustomStore store(CustomStorePath(user));
    std::vector<std::string> notes;
    if (!store.Load()) {
        std::snprintf(message, message_size, "%s", store.error().c_str());
        return nullptr;
    }
    const std::vector<CustomInstance> planned =
        PlanCustomInstances(store, DiscoverSaves(user, build->title_id), build->title_id, notes);
    const std::filesystem::path archive = PathFromUtf8(app0) / "DRACRO.CPK";
    CharacterArtSources sources = ArtSourcesOf(PathFromUtf8(app0), build->regional_archive);
    sources.cache_dir = EditorCacheDir(user);
    CharacterArt art(std::move(sources));
    std::optional<CustomOverlay> overlay = planned.empty() ? std::nullopt : BuildCustomOverlay(archive, planned, art, notes);
    std::string text;
    for (const std::string& note : notes) text += (text.empty() ? "" : "; ") + note;
    std::snprintf(message, message_size, "%s", text.c_str());
    if (!overlay) return nullptr;
    retained.push_back(std::move(*overlay));
    table.clear();
    for (const auto& [offset, data] : retained.back().overlay.patches) table.push_back({offset, data.size(), data.data()});
    *length = retained.back().overlay.length;
    *patches = table.data();
    *count = table.size();
    *instances = retained.back().runtime.data();
    *instance_count = retained.back().runtime.size();
    return "DRACRO.CPK";
}
