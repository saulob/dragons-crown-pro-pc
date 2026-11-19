#include "appearance_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <map>
#include <memory>

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

bool HidesFighterHelmet(CharacterClass klass, const CustomLook& look) {
    const PartLook* helmet = look.Find("helmet");
    return klass == CharacterClass::kFighter && look.active() && helmet && helmet->hidden;
}

namespace {

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

std::vector<uint8_t> NetRecipe(const CustomInstance& instance) {
    const std::optional<appearance_net::Recipe> recipe = RecipeOfLook(instance.instance.klass, instance.instance.base, instance.look);
    return recipe ? appearance_net::Encode(*recipe) : std::vector<uint8_t>{};
}

} // namespace

std::optional<LookTextures> BuildLookTextures(std::span<const uint8_t> native_atlas, const EntryReader& read,
                                              const std::vector<RegionMap>& maps, CharacterClass klass, const CustomLook& look,
                                              bool helmet_copies, std::vector<std::string>& notes, std::string* error) {
    std::string why;
    std::optional<std::vector<uint8_t>> atlas = native_atlas.empty() ? std::nullopt : CustomizeAtlas(native_atlas, maps, klass, look, &why);
    if (!atlas) {
        if (error) *error = why.empty() ? "the native atlas is unreadable" : why;
        return std::nullopt;
    }
    if (helmet_copies && klass == CharacterClass::kFighter && !HidesFighterHelmet(klass, look) && !AddHelmetCopies(*atlas)) {
        if (error) *error = "the helmet copies could not be added";
        return std::nullopt;
    }
    LookTextures textures{std::move(*atlas), {}};
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
    for (size_t w = 0; w < weapons.size() && w < APPEARANCE_WEAPON_COUNT; ++w) {
        if (!WeaponChanged(klass, w, look)) continue;
        const std::string native = WeaponAtlasName(klass, weapons[w]);
        const std::vector<uint8_t> pack = read(native);
        why.clear();
        std::optional<std::vector<uint8_t>> weapon = pack.empty() ? std::nullopt : CustomizeWeapon(pack, klass, w, look, &why);
        if (!weapon) {
            notes.push_back(native + " could not be customized" + (why.empty() ? "" : ": " + why));
            continue;
        }
        textures.weapons.emplace_back(w, std::move(*weapon));
    }
    return textures;
}

std::optional<CustomOverlay> BuildCustomOverlay(const std::filesystem::path& archive_path,
                                                const std::vector<CustomInstance>& instances, CharacterArt& art,
                                                std::vector<std::string>& notes, const std::vector<RemoteTextureSet>* remote) {
    CpkArchive archive;
    std::string error;
    if ((instances.empty() && !remote) || !archive.Open(archive_path, &error)) {
        if (!error.empty()) notes.push_back(error);
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> added, replaced;
    const bool hides = std::any_of(instances.begin(), instances.end(), [](const CustomInstance& instance) {
        return HidesFighterHelmet(instance.instance.klass, instance.look);
    });
    if (hides || remote) {
        if (auto files = FighterHelmetFiles(archive, &error)) {
            replaced = std::move(*files);
            notes.push_back("Fighter helmet copies served with " + GameModelName(CharacterClass::kFighter));
        } else {
            notes.push_back("Fighter helmet copies not served: " + error);
            error.clear();
        }
    }
    const EntryReader read = [&](const std::string& entry) {
        const CpkEntry* found = archive.Find(entry);
        return found ? archive.Read(*found, &error) : std::vector<uint8_t>{};
    };
    std::vector<AppearanceInstance> runtime;
    std::map<std::string, std::vector<uint8_t>> base_atlases;
    for (const CustomInstance& instance : instances) {
        const CharacterClass klass = instance.instance.klass;
        const std::string atlas = NativeColorAtlas(klass, instance.instance.base);
        auto [found, fresh] = base_atlases.try_emplace(atlas);
        if (fresh) found->second = read(atlas);
        const std::vector<RegionMap>* maps = art.Regions(klass);
        std::vector<std::string> weapon_notes;
        std::optional<LookTextures> textures;
        if (maps && !instance.entry.empty())
            textures = BuildLookTextures(found->second, read, *maps, klass, instance.look, !replaced.empty(), weapon_notes, &error);
        for (const std::string& note : weapon_notes) notes.push_back(instance.instance.Text() + ": " + note);
        if (!textures) {
            notes.push_back(instance.instance.Text() + ": " + atlas + " could not be customized" + (error.empty() ? "" : ": " + error));
            error.clear();
            continue;
        }
        added.emplace_back(instance.entry, std::move(textures->atlas));
        AppearanceInstance served{};
        std::memcpy(served.identity, instance.identity.data(), kCharIdentityBytes);
        served.klass = uint8_t(klass);
        served.color = uint8_t(instance.instance.base);
        CopyText(served.entry, sizeof(served.entry), instance.entry);
        CopyText(served.label, sizeof(served.label), Label(instance.instance));
        const std::vector<uint8_t> recipe = NetRecipe(instance);
        if (recipe.size() <= sizeof(served.recipe)) {
            std::memcpy(served.recipe, recipe.data(), recipe.size());
            served.recipe_size = uint8_t(recipe.size());
        }
        for (auto& [weapon, data] : textures->weapons) {
            const auto planned = std::find_if(instance.weapon_entries.begin(), instance.weapon_entries.end(),
                                              [&](const auto& entry) { return entry.first == weapon; });
            if (planned == instance.weapon_entries.end()) continue;
            const std::string native = WeaponAtlasName(klass, WeaponTexturesOf(klass)[weapon]);
            added.emplace_back(planned->second, std::move(data));
            CopyText(served.weapons[weapon].native, sizeof(served.weapons[weapon].native), native);
            CopyText(served.weapons[weapon].entry, sizeof(served.weapons[weapon].entry), planned->second);
            notes.push_back("custom weapon texture " + planned->second + " replaces " + native + " while it is the one drawn");
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
    std::vector<std::pair<std::string, uint64_t>> reserved;
    if (remote) {
        for (const RemoteTextureSet& set : *remote) {
            reserved.emplace_back(set.atlas.name, set.atlas.size);
            for (const RemoteEntry& weapon : set.weapons) reserved.emplace_back(weapon.name, weapon.size);
        }
    }
    if (added.empty() && reserved.empty()) return std::nullopt;
    std::error_code ec;
    const uint64_t length = std::filesystem::file_size(archive_path, ec);
    if (ec) {
        notes.push_back("cannot read the size of " + Utf8(archive_path.filename()));
        return std::nullopt;
    }
    std::optional<ArchiveOverlay> overlay = OverlayChangedEntries(archive_path, length, replaced, added, &error, reserved);
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
    std::vector<RemoteTextureSet> placed;
    if (remote) {
        placed = *remote;
        const auto place = [&](RemoteEntry& entry) {
            const CpkEntry* listed = served.Find(entry.name);
            const auto at = std::find_if(overlay->reserved.begin(), overlay->reserved.end(),
                                         [&](const ReservedEntry& r) { return r.path == entry.name; });
            if (!listed || at == overlay->reserved.end() || listed->offset != at->offset || listed->size != entry.size) return false;
            entry.offset = at->offset;
            return true;
        };
        for (RemoteTextureSet& set : placed) {
            bool ok = place(set.atlas);
            for (RemoteEntry& weapon : set.weapons) ok = ok && place(weapon);
            if (!ok) {
                notes.push_back("the overlaid archive does not list " + set.atlas.name);
                return std::nullopt;
            }
        }
        notes.push_back(std::to_string(placed.size()) + " texture sets reserved for other players' looks");
    }
    return CustomOverlay{std::move(*overlay), std::move(runtime), std::move(placed), !replaced.empty()};
}

} // namespace dcp

namespace {

// Other players' looks: set up with the overlay, started once the overlay is registered. The archive and the region maps
// belong to the worker thread.
struct RemoteRuntime {
    std::filesystem::path archive;
    dcp::CharacterArtSources sources;
    std::vector<dcp::RemoteTextureSet> textures;
    bool helmet_copies = false;
    const uint8_t* placeholder = nullptr; // what a reserved texture reads before and after a look
    std::unique_ptr<dcp::CpkArchive> cpk;
    std::unique_ptr<dcp::CharacterArt> art;
    std::unique_ptr<dcp::RemoteLooks> looks;
};

RemoteRuntime& Remote() {
    static RemoteRuntime remote;
    return remote;
}

std::atomic<dcp::RemoteLooks*> g_looks{nullptr};

uint64_t SteadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void LogLine(const std::string& line) {
    std::printf("%s\n", line.c_str());
}

bool OpenRemoteSources(RemoteRuntime& remote, std::string* error) {
    if (!remote.cpk) {
        auto cpk = std::make_unique<dcp::CpkArchive>();
        if (!cpk->Open(remote.archive, error)) return false;
        remote.cpk = std::move(cpk);
    }
    if (!remote.art) remote.art = std::make_unique<dcp::CharacterArt>(remote.sources);
    return true;
}

std::optional<dcp::LookTextures> BuildRemote(dcp::CharacterClass klass, int base, const dcp::CustomLook& look, std::string* error) {
    using namespace dcp;
    RemoteRuntime& remote = Remote();
    if (!OpenRemoteSources(remote, error)) return std::nullopt;
    const std::vector<RegionMap>* maps = remote.art->Regions(klass);
    if (!maps) {
        if (error) *error = "the " + std::string(ClassName(klass)) + " regions are unreadable";
        return std::nullopt;
    }
    std::string read_error;
    const EntryReader read = [&](const std::string& entry) {
        const CpkEntry* found = remote.cpk->Find(entry);
        return found ? remote.cpk->Read(*found, &read_error) : std::vector<uint8_t>{};
    };
    std::vector<std::string> notes;
    std::optional<LookTextures> textures =
        BuildLookTextures(read(NativeColorAtlas(klass, base)), read, *maps, klass, look, remote.helmet_copies, notes, error);
    for (const std::string& note : notes) LogLine("Appearance runtime: remote " + note);
    return textures;
}

// The region maps of every class, read from the Editor's cache or learned once, before the first look arrives.
void PrepareRemote() {
    using namespace dcp;
    RemoteRuntime& remote = Remote();
    std::string error;
    if (!OpenRemoteSources(remote, &error)) {
        LogLine("Appearance runtime: other players' looks unavailable: " + error);
        return;
    }
    const uint64_t started = SteadyMs();
    int ready = 0;
    for (int k = 0; k < appearance_net::kClassCount; ++k) ready += remote.art->Regions(CharacterClass(k)) != nullptr;
    LogLine("Appearance runtime: region maps of " + std::to_string(ready) + " classes ready for other players' looks in " +
            std::to_string(SteadyMs() - started) + " ms");
}

} // namespace

extern "C" const char* dcp_custom_appearance(const char* image_dir, const char* app0, const char* user_dir, int online,
                                             uint64_t* length, DcpFilePatch** patches, size_t* count,
                                             const AppearanceInstance** instances, size_t* instance_count, char* message,
                                             size_t message_size) {
    using namespace dcp;
    static std::list<CustomOverlay> retained; // the runtime borrows the overlay and the instances for the process
    static std::vector<DcpFilePatch> table;
    static std::vector<uint8_t> unwritten;     // what a remote texture holds before its first look
    if (message_size) *message = '\0';
    const GameBuild* build = BuildOfImageDir(image_dir);
    if (!build || !app0 || !user_dir) return nullptr;
    const std::filesystem::path user = PathFromUtf8(user_dir);
    CustomStore store(CustomStorePath(user));
    std::vector<std::string> notes;
    std::vector<CustomInstance> planned;
    if (store.Load()) planned = PlanCustomInstances(store, DiscoverSaves(user, build->title_id), build->title_id, notes);
    else notes.push_back(store.error());
    const std::filesystem::path archive = PathFromUtf8(app0) / "DRACRO.CPK";
    CharacterArtSources sources = ArtSourcesOf(PathFromUtf8(app0), build->regional_archive);
    sources.cache_dir = EditorCacheDir(user);
    std::optional<std::vector<RemoteTextureSet>> remote_plan;
    if (online) {
        CpkArchive cpk;
        std::string error;
        if (cpk.Open(archive, &error)) remote_plan = PlanRemoteTextures(cpk, &error);
        if (!remote_plan) notes.push_back("other players' looks unavailable: " + error);
    }
    CharacterArt art(sources);
    std::optional<CustomOverlay> overlay = planned.empty() && !remote_plan
                                               ? std::nullopt
                                               : BuildCustomOverlay(archive, planned, art, notes, remote_plan ? &*remote_plan : nullptr);
    std::string text;
    for (const std::string& note : notes) text += (text.empty() ? "" : "; ") + note;
    std::snprintf(message, message_size, "%s", text.c_str());
    if (!overlay) return nullptr;
    retained.push_back(std::move(*overlay));
    const CustomOverlay& kept = retained.back();
    table.clear();
    for (const auto& [offset, data] : kept.overlay.patches) table.push_back({offset, data.size(), data.data()});
    uint64_t largest = 0;
    for (const ReservedEntry& entry : kept.overlay.reserved) largest = std::max(largest, entry.size);
    unwritten.assign(size_t(largest), 0);
    for (const ReservedEntry& entry : kept.overlay.reserved) table.push_back({entry.offset, entry.size, unwritten.data()});
    if (!kept.remote.empty()) {
        RemoteRuntime& remote = Remote();
        remote.archive = archive;
        remote.sources = std::move(sources);
        remote.textures = kept.remote;
        remote.helmet_copies = kept.helmet_copies;
        remote.placeholder = unwritten.data();
    }
    *length = kept.overlay.length;
    *patches = table.data();
    *count = table.size();
    *instances = kept.runtime.data();
    *instance_count = kept.runtime.size();
    return "DRACRO.CPK";
}

extern "C" int dcp_remote_appearance_start(DcpOverlayReplace replace) {
    using namespace dcp;
    RemoteRuntime& remote = Remote();
    if (!replace || remote.textures.empty() || remote.looks) return 0;
    RemoteLooks::Host host;
    host.build = BuildRemote;
    host.serve = [replace, placeholder = remote.placeholder](const RemoteEntry& entry, const uint8_t* data) {
        return replace("/app0/DRACRO.CPK", entry.offset, entry.size, data ? data : placeholder) == 0;
    };
    host.log = LogLine;
    host.now_ms = SteadyMs;
    host.prepare = PrepareRemote;
    remote.looks = std::make_unique<RemoteLooks>(remote.textures, std::move(host));
    g_looks.store(remote.looks.get(), std::memory_order_release);
    return 1;
}

extern "C" const AppearanceInstance* dcp_remote_appearance_of(int slot, int klass, int color) {
    const dcp::RemoteLooks* looks = g_looks.load(std::memory_order_acquire);
    return looks ? looks->Of(slot, klass, color) : nullptr;
}

extern "C" const AppearanceInstance* dcp_remote_appearance_serial(uint16_t serial) {
    const dcp::RemoteLooks* looks = g_looks.load(std::memory_order_acquire);
    return looks ? looks->BySerial(serial) : nullptr;
}

extern "C" void dcp_remote_appearance_set(int slot, const uint8_t* recipe, size_t size) {
    if (dcp::RemoteLooks* looks = g_looks.load(std::memory_order_acquire)) looks->Set(slot, std::span<const uint8_t>(recipe, size));
}

extern "C" void dcp_remote_appearance_clear(int slot) {
    if (dcp::RemoteLooks* looks = g_looks.load(std::memory_order_acquire)) looks->Clear(slot);
}
