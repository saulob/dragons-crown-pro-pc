#pragma once

#include <functional>

namespace dcp {

enum class PlayRequest { kStarting, kBusy, kFilesNotReady, kSaveFailed };

// PLAY's gate. An accepted request saves the settings once; a failed save keeps the launcher open and nothing starts.
class PlayGate {
public:
    PlayRequest Request(bool busy, bool files_ready, const std::function<bool()>& save) {
        if (starting_ || busy) {
            return PlayRequest::kBusy;
        }
        if (!files_ready) {
            return PlayRequest::kFilesNotReady;
        }
        if (!save()) {
            return PlayRequest::kSaveFailed;
        }
        starting_ = true;
        return PlayRequest::kStarting;
    }

    bool starting() const { return starting_; }
    // The game could not be started: the launcher stays open and PLAY can be pressed again.
    void Cancel() { starting_ = false; }

private:
    bool starting_ = false;
};

} // namespace dcp
