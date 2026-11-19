#include "appearance_remote.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "character_art.h"

namespace dcp {
namespace {

namespace an = appearance_net;

std::string Upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

uint64_t StoredSize(const CpkEntry& entry) {
    return entry.compressed() ? entry.extract_size : entry.size;
}

void CopyText(char* out, size_t size, const std::string& text) {
    std::snprintf(out, size, "%s", text.c_str());
}

} // namespace

std::string RemoteAtlasEntry(CharacterClass klass, int base, int index) {
    return "PLAYER/" + Upper(ClassName(klass)) + "0" + std::to_string(base) + "_NET" + std::to_string(index + 1) + ".FTX";
}

std::string RemoteWeaponEntry(CharacterClass klass, int base, size_t weapon, int index) {
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
    if (weapon >= weapons.size()) return {};
    return "PLAYER/" + Upper(ClassName(klass)) + "0" + std::to_string(base) + "_" + std::string(weapons[weapon].name) + "_NET" +
           std::to_string(index + 1) + ".FTX";
}

std::optional<std::vector<RemoteTextureSet>> PlanRemoteTextures(const CpkArchive& archive, std::string* error) {
    std::vector<RemoteTextureSet> plan;
    for (int k = 0; k < an::kClassCount; ++k) {
        const CharacterClass klass = CharacterClass(k);
        std::vector<RemoteEntry> weapons;
        const std::span<const WeaponTexture> textures = WeaponTexturesOf(klass);
        for (size_t w = 0; w < textures.size(); ++w) {
            const CpkEntry* weapon = archive.Find(WeaponAtlasName(klass, textures[w]));
            if (!weapon) {
                if (error) *error = WeaponAtlasName(klass, textures[w]) + " is not in the archive";
                return std::nullopt;
            }
            weapons.push_back({{}, StoredSize(*weapon), 0});
        }
        for (int base = 0; base < an::kBaseCount; ++base) {
            const CpkEntry* atlas = archive.Find(NativeColorAtlas(klass, base));
            if (!atlas) {
                if (error) *error = NativeColorAtlas(klass, base) + " is not in the archive";
                return std::nullopt;
            }
            for (int i = 0; i < kRemoteLooksPerPreset; ++i) {
                RemoteTextureSet set{klass, base, i, {RemoteAtlasEntry(klass, base, i), StoredSize(*atlas), 0}, weapons};
                for (size_t w = 0; w < set.weapons.size(); ++w) set.weapons[w].name = RemoteWeaponEntry(klass, base, w, i);
                plan.push_back(std::move(set));
            }
        }
    }
    return plan;
}

std::optional<an::Recipe> RecipeOfLook(CharacterClass klass, int base, const CustomLook& look) {
    if (!look.active() || base < 0 || base >= an::kBaseCount) return std::nullopt;
    an::Recipe recipe;
    recipe.klass = uint8_t(klass);
    recipe.base = uint8_t(base);
    for (const PartLook& part : look.parts) {
        const int id = an::PartId(int(klass), part.key);
        if (id < 0 || (!part.hidden && !part.color)) continue;
        an::NetPart net{uint8_t(id), part.hidden, std::nullopt};
        if (part.color) net.color = std::array<uint8_t, 3>{part.color->r, part.color->g, part.color->b};
        recipe.parts.push_back(net);
    }
    std::optional<an::Recipe> normal = an::Normalized(std::move(recipe));
    if (!normal || normal->native()) return std::nullopt;
    return normal;
}

CustomLook LookOfRecipe(const an::Recipe& recipe) {
    CustomLook look{true, recipe.base, {}};
    for (const an::NetPart& part : recipe.parts) {
        PartLook& edit = look.Edit(an::PartKey(recipe.klass, part.id));
        edit.hidden = part.hidden;
        if (part.color) edit.color = Rgb{(*part.color)[0], (*part.color)[1], (*part.color)[2]};
    }
    return look;
}

RemoteLooks::RemoteLooks(std::vector<RemoteTextureSet> textures, Host host) : host_(std::move(host)) {
    for (RemoteTextureSet& set : textures) textures_.push_back(Texture{std::move(set)});
    if (host_.threaded) worker_ = std::thread([this] { Worker(); });
}

RemoteLooks::~RemoteLooks() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void RemoteLooks::Log(const std::string& line) const {
    if (host_.log) host_.log(line);
}

void RemoteLooks::Set(int slot, std::span<const uint8_t> bytes) {
    if (slot < 0 || slot >= kRemoteSeats) return;
    an::Refusal why = an::Refusal::kNone;
    const std::optional<an::Recipe> recipe = an::Decode(bytes, &why);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recipe || recipe->native()) {
        if (!recipe) Log("Appearance runtime: remote look for slot " + std::to_string(slot) + " refused (" + an::RefusalText(why) + ")");
        ReleaseLocked(slot);
        seats_[size_t(slot)] = Seat{};
        return;
    }
    const uint64_t hash = an::Hash(*recipe);
    Seat& seat = seats_[size_t(slot)];
    if (seat.recipe && seat.hash == hash) return;
    ReleaseLocked(slot);
    seat = Seat{recipe, hash, -1, true};
    AssignLocked(slot);
}

void RemoteLooks::Clear(int slot) {
    if (slot < 0 || slot >= kRemoteSeats) return;
    std::lock_guard<std::mutex> lock(mutex_);
    ReleaseLocked(slot);
    seats_[size_t(slot)] = Seat{};
    RetryWaitingLocked();
}

void RemoteLooks::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int slot = 0; slot < kRemoteSeats; ++slot) {
        ReleaseLocked(slot);
        seats_[size_t(slot)] = Seat{};
    }
}

void RemoteLooks::ReleaseLocked(int slot) {
    Seat& seat = seats_[size_t(slot)];
    published_[size_t(slot)].store(nullptr, std::memory_order_release);
    if (seat.texture >= 0) {
        Texture& texture = textures_[size_t(seat.texture)];
        if (--texture.users <= 0) {
            texture.users = 0;
            texture.released_ms = host_.now_ms();
        }
    }
    seat.texture = -1;
}

// The same look shares its texture; a new one takes a texture never used, else the one released longest ago, once quiet.
void RemoteLooks::AssignLocked(int slot) {
    Seat& seat = seats_[size_t(slot)];
    const CharacterClass klass = CharacterClass(seat.recipe->klass);
    const int base = seat.recipe->base;
    const uint64_t now = host_.now_ms();
    int chosen = -1;
    for (size_t i = 0; i < textures_.size(); ++i) {
        const Texture& texture = textures_[i];
        if (texture.set.klass == klass && texture.set.base == base && texture.hash == seat.hash && (texture.ready || texture.building)) {
            chosen = int(i);
            break;
        }
    }
    if (chosen >= 0) {
        Texture& shared = textures_[size_t(chosen)];
        ++shared.users;
        seat.texture = chosen;
        seat.waiting = false;
        if (shared.ready) PublishLocked(slot);
        return;
    }
    for (size_t i = 0; i < textures_.size(); ++i) {
        const Texture& texture = textures_[i];
        if (texture.set.klass != klass || texture.set.base != base || texture.users > 0 || texture.building) continue;
        if (texture.used && now - texture.released_ms < kRemoteReuseQuietMs) continue;
        const bool better = chosen < 0 || (!texture.used && textures_[size_t(chosen)].used) ||
                            (texture.used == textures_[size_t(chosen)].used && texture.released_ms < textures_[size_t(chosen)].released_ms);
        if (better) chosen = int(i);
    }
    if (chosen < 0) {
        if (!seat.waiting)
            Log("Appearance runtime: slot " + std::to_string(slot) + " waits for a free " + std::string(ClassName(klass)) + " colour " +
                std::to_string(base) + " texture");
        seat.waiting = true;
        return;
    }
    Texture& texture = textures_[size_t(chosen)];
    texture.hash = seat.hash;
    texture.users = 1;
    texture.used = true;
    texture.building = true;
    texture.ready = false;
    seat.texture = chosen;
    seat.waiting = false;
    jobs_.push_back(Job{chosen, *seat.recipe, seat.hash});
    wake_.notify_all();
}

void RemoteLooks::RetryWaitingLocked() {
    for (int slot = 0; slot < kRemoteSeats; ++slot) {
        const Seat& seat = seats_[size_t(slot)];
        if (seat.recipe && seat.waiting && seat.texture < 0) AssignLocked(slot);
    }
}

void RemoteLooks::PublishLocked(int slot) {
    const Seat& seat = seats_[size_t(slot)];
    const Texture& texture = textures_[size_t(seat.texture)];
    if (serial_ >= 0x7fff) {
        Log("Appearance runtime: remote look limit reached for this session; slot " + std::to_string(slot) + " drawn native");
        return;
    }
    const CharacterClass klass = CharacterClass(seat.recipe->klass);
    auto instance = std::make_unique<AppearanceInstance>();
    instance->klass = seat.recipe->klass;
    instance->color = seat.recipe->base;
    instance->remote = ++serial_;
    CopyText(instance->entry, sizeof(instance->entry), texture.set.atlas.name);
    CopyText(instance->label, sizeof(instance->label),
             "Player " + std::to_string(slot + 1) + " " + std::string(ClassName(klass)) + " colour " +
                 std::to_string(seat.recipe->base) + " look " + an::ShortHash(seat.hash));
    const CustomLook look = LookOfRecipe(*seat.recipe);
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
    for (size_t w = 0; w < weapons.size() && w < APPEARANCE_WEAPON_COUNT && w < texture.set.weapons.size(); ++w) {
        if (!WeaponChanged(klass, w, look)) continue;
        CopyText(instance->weapons[w].native, sizeof(instance->weapons[w].native), WeaponAtlasName(klass, weapons[w]));
        CopyText(instance->weapons[w].entry, sizeof(instance->weapons[w].entry), texture.set.weapons[w].name);
    }
    const std::vector<uint8_t> bytes = an::Encode(*seat.recipe);
    std::memcpy(instance->recipe, bytes.data(), std::min(bytes.size(), sizeof(instance->recipe)));
    instance->recipe_size = uint8_t(std::min(bytes.size(), sizeof(instance->recipe)));
    published_[size_t(slot)].store(instance.get(), std::memory_order_release);
    Log("Appearance runtime: remote slot " + std::to_string(slot) + " bound " + instance->entry + " (" + instance->label + ")");
    snapshots_.push_back(std::move(instance));
}

const AppearanceInstance* RemoteLooks::Of(int slot, int klass, int color) const {
    if (slot < 0 || slot >= kRemoteSeats) return nullptr;
    const AppearanceInstance* instance = published_[size_t(slot)].load(std::memory_order_acquire);
    return instance && instance->klass == klass && instance->color == color ? instance : nullptr;
}

const AppearanceInstance* RemoteLooks::BySerial(uint16_t serial) const {
    for (const auto& published : published_) {
        const AppearanceInstance* instance = published.load(std::memory_order_acquire);
        if (instance && instance->remote == serial) return instance;
    }
    return nullptr;
}

bool RemoteLooks::RunOne(std::unique_lock<std::mutex>& lock) {
    if (jobs_.empty()) return false;
    const Job job = jobs_.front();
    jobs_.pop_front();
    running_ = true;
    const CharacterClass klass = CharacterClass(job.recipe.klass);
    lock.unlock();
    std::string error;
    std::optional<LookTextures> textures;
    if (host_.build) textures = host_.build(klass, job.recipe.base, LookOfRecipe(job.recipe), &error);
    else error = "no texture builder";
    Finish(job, std::move(textures), error);
    lock.lock();
    running_ = false;
    ++builds_;
    idle_.notify_all();
    return true;
}

void RemoteLooks::DropTextureLocked(int index) {
    Texture& texture = textures_[size_t(index)];
    texture.building = false;
    texture.ready = false;
    texture.hash = 0;
    texture.users = 0;
    texture.released_ms = host_.now_ms();
    for (Seat& seat : seats_)
        if (seat.texture == index) seat.texture = -1;
}

// The entry reads `data` from now on (nullptr: the placeholder); the data it read before stays alive a while longer.
bool RemoteLooks::ServeLocked(const RemoteEntry& entry, std::unique_ptr<std::vector<uint8_t>> data) {
    const uint64_t now = host_.now_ms();
    std::erase_if(served_, [&](const Served& old) { return old.retired && now - old.retired_ms >= kRemoteRetireMs; });
    if (host_.serve && !host_.serve(entry, data ? data->data() : nullptr)) return false;
    for (Served& old : served_) {
        if (old.entry == entry.name && !old.retired) {
            old.retired = true;
            old.retired_ms = now;
        }
    }
    if (data) served_.push_back(Served{entry.name, std::move(data), false, 0});
    return true;
}

// A texture no player has worn for kRemoteKeepMs gives its memory back; the look is rebuilt if it comes back.
void RemoteLooks::EvictIdleLocked() {
    const uint64_t now = host_.now_ms();
    for (size_t i = 0; i < textures_.size(); ++i) {
        Texture& texture = textures_[i];
        if (!texture.ready || texture.users > 0 || now - texture.released_ms < kRemoteKeepMs) continue;
        ServeLocked(texture.set.atlas, nullptr);
        for (const RemoteEntry& weapon : texture.set.weapons) ServeLocked(weapon, nullptr);
        Log("Appearance runtime: " + texture.set.atlas.name + " released, unused for " + std::to_string(kRemoteKeepMs / 1000) + " s");
        DropTextureLocked(int(i));
    }
}

// Served before it is published: no hook names the texture until its bytes are in place.
void RemoteLooks::Finish(const Job& job, std::optional<LookTextures> textures, const std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    Texture& texture = textures_[size_t(job.texture)];
    if (texture.hash != job.hash) return;
    const std::string what = std::string(ClassName(texture.set.klass)) + " colour " + std::to_string(texture.set.base) + " look " +
                             an::ShortHash(job.hash);
    std::string failure = textures ? "" : (error.empty() ? "unknown error" : error);
    if (textures && textures->atlas.size() != texture.set.atlas.size)
        failure = texture.set.atlas.name + " is " + std::to_string(texture.set.atlas.size) + " bytes, the look's atlas " +
                  std::to_string(textures->atlas.size());
    if (textures && failure.empty()) {
        for (const auto& [weapon, data] : textures->weapons) {
            if (weapon >= texture.set.weapons.size() || data.size() != texture.set.weapons[weapon].size)
                failure = "a default weapon texture has another size";
        }
    }
    bool served = failure.empty() && ServeLocked(texture.set.atlas, std::make_unique<std::vector<uint8_t>>(std::move(textures->atlas)));
    if (served) {
        for (auto& [weapon, data] : textures->weapons)
            served = served && ServeLocked(texture.set.weapons[weapon], std::make_unique<std::vector<uint8_t>>(std::move(data)));
    }
    if (!served) {
        Log("Appearance runtime: remote " + what + " not built (" + (failure.empty() ? "not served" : failure) + "); drawn native");
        DropTextureLocked(job.texture);
        return;
    }
    texture.building = false;
    texture.ready = true;
    Log("Appearance runtime: remote " + what + " built into " + texture.set.atlas.name);
    for (int slot = 0; slot < kRemoteSeats; ++slot) {
        const Seat& seat = seats_[size_t(slot)];
        if (seat.texture == job.texture && seat.hash == job.hash) PublishLocked(slot);
    }
}

void RemoteLooks::Worker() {
    if (host_.prepare) host_.prepare();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        if (RunOne(lock)) continue;
        EvictIdleLocked();
        RetryWaitingLocked();
        if (!jobs_.empty()) continue;
        wake_.wait_for(lock, std::chrono::seconds(1));
    }
}

void RemoteLooks::Drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!host_.threaded) {
        EvictIdleLocked();
        RetryWaitingLocked();
        while (RunOne(lock)) {
        }
        return;
    }
    idle_.wait(lock, [&] { return jobs_.empty() && !running_; });
}

int RemoteLooks::TextureOf(int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot >= 0 && slot < kRemoteSeats ? seats_[size_t(slot)].texture : -1;
}

size_t RemoteLooks::Builds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return builds_;
}

} // namespace dcp
