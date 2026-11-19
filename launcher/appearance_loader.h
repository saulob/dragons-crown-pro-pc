// Prepares a character's Appearance page off the UI thread. The job works on its own CharacterArt over the same game
// files, so it shares no state with the UI; the page adopts the result on the UI thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "appearance_custom.h"
#include "character_art.h"

namespace dcp {

struct AppearanceRequest {
    CharacterArtSources sources;
    CharacterClass klass = CharacterClass::kFighter;
    int color = 0;
    CustomLook look;
};

struct AppearancePrepared {
    std::unique_ptr<CharacterArt> art;
    bool ok = false;
    std::string error;
};

AppearancePrepared PrepareAppearance(const AppearanceRequest& request);

class AppearanceLoader {
public:
    using Job = std::function<AppearancePrepared()>;

    AppearanceLoader() = default;
    AppearanceLoader(const AppearanceLoader&) = delete;
    AppearanceLoader& operator=(const AppearanceLoader&) = delete;
    // Waits for a running job: it never outlives the loader.
    ~AppearanceLoader();

    // Starts `job` on a worker thread; refused (0) while an earlier job is running or its result is uncollected.
    uint64_t Start(Job job);
    bool busy() const { return worker_.joinable(); }
    // The finished job's serial and result, once.
    std::optional<std::pair<uint64_t, AppearancePrepared>> Collect();

private:
    std::thread worker_;
    std::atomic<bool> done_{false};
    AppearancePrepared result_;
    uint64_t serial_ = 0;
};

} // namespace dcp
