#include "hp_override_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "game_files.h"
#include "utf8.h"

namespace dcp {
namespace {

const CharacterView* SavedCharacter(const std::vector<DiscoveredSave>& saves, const CustomKey& key,
                                    const SaveModel** save_model) {
    for (const DiscoveredSave& save : saves) {
        if (save.title_id != key.title_id || save.dir_name != key.save_dir) continue;
        const CharacterView* character = save.model.Character(key.data_slot, key.character_slot);
        if (character && character->present && character->klass == key.klass) {
            if (save_model) *save_model = &save.model;
            return character;
        }
    }
    return nullptr;
}

std::vector<uint8_t> ComparedIdentity(const uint8_t* id) {
    std::vector<uint8_t> out(id, id + kCharIdentityBytes);
    out[6] = 0;
    out[7] = 0;
    return out;
}

bool Identifiable(const uint8_t* id) {
    const std::vector<uint8_t> compared = ComparedIdentity(id);
    const auto all = [&](uint8_t v) { return std::ranges::all_of(compared, [&](uint8_t b) { return b == v; }); };
    return !all(0) && !all(0xCC);
}

bool SameOverride(const CharacterOverrideEntry& a, const CharacterOverrideEntry& b) {
    return a.klass == b.klass && a.present == b.present && a.max_hp == b.max_hp && a.strength == b.strength &&
           a.constitution == b.constitution && a.dexterity == b.dexterity && a.intelligence == b.intelligence &&
           a.magic_resistance == b.magic_resistance && a.luck == b.luck;
}

} // namespace

std::vector<CharacterOverrideEntry> PlanCharacterOverrides(const CharacterOverrideStore& store,
                                                           const std::vector<DiscoveredSave>& saves,
                                                           std::string_view title_id, std::vector<std::string>& notes) {
    struct Planned {
        CustomKey key;
        CharacterOverrideEntry entry{};
    };
    std::vector<Planned> planned;
    for (const auto& [key, override] : store.entries()) {
        if (key.title_id != title_id || !override.Any()) continue;
        const SaveModel* model = nullptr;
        const CharacterView* character = SavedCharacter(saves, key, &model);
        if (!character) {
            notes.push_back("no saved character for " + key.Text());
            continue;
        }
        Planned item;
        item.key = key;
        item.entry.klass = uint8_t(key.klass);
        item.entry.present = override.present;
        item.entry.max_hp = override.max_hp;
        item.entry.strength = override.strength;
        item.entry.constitution = override.constitution;
        item.entry.dexterity = override.dexterity;
        item.entry.intelligence = override.intelligence;
        item.entry.magic_resistance = override.magic_resistance;
        item.entry.luck = override.luck;
        std::memcpy(item.entry.identity, model->bytes().data() + character->record_offset + kCharIdentity,
                    kCharIdentityBytes);
        if (!Identifiable(item.entry.identity)) {
            notes.push_back(key.Text() + " has no character identity; left native");
            continue;
        }
        planned.push_back(std::move(item));
    }
    std::map<std::vector<uint8_t>, std::vector<size_t>> by_identity;
    for (size_t i = 0; i < planned.size(); ++i) by_identity[ComparedIdentity(planned[i].entry.identity)].push_back(i);
    std::vector<bool> dropped(planned.size(), false);
    for (const auto& [identity, indices] : by_identity) {
        for (size_t n = 1; n < indices.size(); ++n) {
            const Planned& first = planned[indices.front()];
            const Planned& other = planned[indices[n]];
            const bool same = SameOverride(first.entry, other.entry);
            dropped[indices[n]] = true;
            if (!same) {
                dropped[indices.front()] = true;
                notes.push_back(first.key.Text() + " and " + other.key.Text() +
                                " are the same character with different overrides; left native");
            }
        }
    }
    std::vector<CharacterOverrideEntry> result;
    for (size_t i = 0; i < planned.size(); ++i) {
        if (!dropped[i]) result.push_back(planned[i].entry);
    }
    return result;
}

} // namespace dcp

extern "C" size_t dcp_character_overrides(const char* image_dir, const char* user_dir,
                                          const CharacterOverrideEntry** entries, char* message, size_t message_size) {
    using namespace dcp;
    static std::vector<CharacterOverrideEntry> retained;
    retained.clear();
    if (entries) *entries = nullptr;
    if (message && message_size) *message = '\0';
    const GameBuild* build = BuildOfImageDir(image_dir);
    if (!build || !user_dir) return 0;
    const std::filesystem::path user = PathFromUtf8(user_dir);
    CharacterOverrideStore store(CharacterOverrideStorePath(user));
    std::vector<std::string> notes;
    if (store.Load()) {
        retained = PlanCharacterOverrides(store, DiscoverSaves(user, build->title_id), build->title_id, notes);
    } else {
        notes.push_back(store.error());
    }
    if (message && message_size) {
        std::string text;
        for (const std::string& note : notes) text += (text.empty() ? "" : "; ") + note;
        std::snprintf(message, message_size, "%s", text.c_str());
    }
    if (entries && !retained.empty()) *entries = retained.data();
    return retained.size();
}
