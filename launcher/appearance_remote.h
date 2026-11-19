// The looks of the other players of an online room, drawn with this client's own game files: each arrives as a recipe
// (appearance_net.h), is built into textures off the game thread with the same code as a saved look, and is served from
// archive entries reserved at launch, so nothing but the recipe ever crosses the network.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "appearance_custom.h"
#include "appearance_hooks.h"
#include "appearance_net.h"
#include "cpk_patch.h"

namespace dcp {

// Textures per class and preset for remote looks (the presets' atlases differ in size): one per other player.
inline constexpr int kRemoteLooksPerPreset = 3;
inline constexpr int kRemoteSeats = 4;
// A texture no player wears any more keeps its look this long before another look may be written into it.
inline constexpr uint64_t kRemoteReuseQuietMs = 10000;
// Replaced texture data stays readable this long for a read the game started before the swap.
inline constexpr uint64_t kRemoteRetireMs = 60000;
// A built look nobody wears is kept this long for a player who comes back, then its memory is given back.
inline constexpr uint64_t kRemoteKeepMs = 300000;

struct RemoteEntry {
    std::string name;
    uint64_t size = 0;
    uint64_t offset = 0; // in the overlaid archive, once reserved
};

struct RemoteTextureSet {
    CharacterClass klass = CharacterClass::kFighter;
    int base = 0;
    int index = 0;
    RemoteEntry atlas;
    std::vector<RemoteEntry> weapons; // WeaponTexturesOf(klass) order
};

std::string RemoteAtlasEntry(CharacterClass klass, int base, int index);                // PLAYER/FIGHTER02_NET1.FTX
std::string RemoteWeaponEntry(CharacterClass klass, int base, size_t weapon, int index); // PLAYER/FIGHTER02_SHIELD01_NET1.FTX
// Sized as each preset's native textures; nothing when the archive lacks one of them.
std::optional<std::vector<RemoteTextureSet>> PlanRemoteTextures(const CpkArchive& archive, std::string* error = nullptr);

// The saved look as it travels, for a character drawn with preset `base`; nothing when the look changes nothing.
std::optional<appearance_net::Recipe> RecipeOfLook(CharacterClass klass, int base, const CustomLook& look);
CustomLook LookOfRecipe(const appearance_net::Recipe& recipe);

// The textures of one look: its atlas, and the default weapons it changes.
struct LookTextures {
    std::vector<uint8_t> atlas;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> weapons;
};

class RemoteLooks {
public:
    struct Host {
        // Runs on the worker; `error` says why when nothing comes back.
        std::function<std::optional<LookTextures>(CharacterClass klass, int base, const CustomLook& look, std::string* error)> build;
        // Makes the game read `data` for the entry from now on (nullptr: the placeholder); the data stays alive until
        // kRemoteRetireMs after it is replaced.
        std::function<bool(const RemoteEntry& entry, const uint8_t* data)> serve;
        std::function<void(const std::string& line)> log;
        std::function<uint64_t()> now_ms;
        // Runs first on the worker (the region maps); a host without a worker runs jobs only in Drain.
        std::function<void()> prepare;
        bool threaded = true;
    };

    RemoteLooks(std::vector<RemoteTextureSet> textures, Host host);
    ~RemoteLooks();
    RemoteLooks(const RemoteLooks&) = delete;
    RemoteLooks& operator=(const RemoteLooks&) = delete;

    // A recipe for party slot 0-3, validated here again; an identical one changes nothing, an invalid one is native.
    void Set(int slot, std::span<const uint8_t> recipe);
    void Clear(int slot);
    void Reset();

    // Lock-free, any thread: the look of the slot while it is built and still drawn with this class and preset.
    const AppearanceInstance* Of(int slot, int klass, int color) const;
    const AppearanceInstance* BySerial(uint16_t serial) const;

    // Until no build is queued or running; without a worker, runs them here.
    void Drain();
    int TextureOf(int slot) const;
    size_t Builds() const;

private:
    struct Texture {
        RemoteTextureSet set;
        uint64_t hash = 0;
        int users = 0;
        bool used = false, building = false, ready = false;
        uint64_t released_ms = 0;
    };
    struct Seat {
        std::optional<appearance_net::Recipe> recipe;
        uint64_t hash = 0;
        int texture = -1;
        bool waiting = false;
    };
    struct Job {
        int texture = -1;
        appearance_net::Recipe recipe;
        uint64_t hash = 0;
    };
    struct Served {
        std::string entry;
        std::unique_ptr<std::vector<uint8_t>> data;
        bool retired = false;
        uint64_t retired_ms = 0;
    };

    void Log(const std::string& line) const;
    void ReleaseLocked(int slot);
    void AssignLocked(int slot);
    void PublishLocked(int slot);
    void RetryWaitingLocked();
    void DropTextureLocked(int index);
    bool ServeLocked(const RemoteEntry& entry, std::unique_ptr<std::vector<uint8_t>> data);
    void EvictIdleLocked();
    bool RunOne(std::unique_lock<std::mutex>& lock);
    void Finish(const Job& job, std::optional<LookTextures> textures, const std::string& error);
    void Worker();

    Host host_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::vector<Texture> textures_;
    std::array<Seat, kRemoteSeats> seats_{};
    std::array<std::atomic<const AppearanceInstance*>, kRemoteSeats> published_{};
    std::deque<std::unique_ptr<AppearanceInstance>> snapshots_; // never freed: a hook may still hold one
    std::vector<Served> served_;
    std::deque<Job> jobs_;
    bool running_ = false, stopping_ = false;
    size_t builds_ = 0;
    uint16_t serial_ = 0;
    std::thread worker_;
};

} // namespace dcp
