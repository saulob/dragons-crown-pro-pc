#include "home_portraits.h"

#include <utility>

#include <trg/platform.h>

#include "utf8.h"

namespace dcp::home {
namespace {

std::string KeyOf(const CharacterArtSources& sources) {
    std::string key;
    for (const std::filesystem::path& archive : sources.archives) {
        key += Utf8(archive) + "\n";
    }
    return key;
}

} // namespace

PortraitImages LoadClassPortraits(const CharacterArtSources& sources) {
    PortraitImages images;
    if (sources.archives.empty()) {
        return images;
    }
    CharacterArt art(sources);
    for (const CharacterClass klass : kCharacterClasses) {
        const SpriteImage* face = art.Portrait(klass);
        if (!face || face->rgba.empty()) {
            return PortraitImages{};
        }
        images[static_cast<size_t>(klass)] = *face;
    }
    return images;
}

struct ClassPortraits::Request {
    std::unique_ptr<trg::Task> task = std::make_unique<trg::Task>();
    std::shared_ptr<PortraitImages> images = std::make_shared<PortraitImages>();

    ~Request() {
        task->Cancel();
        if (task->running()) {
            // The worker reads the game's archives and owns copies of everything else; leaving its thread unjoined keeps
            // a slow disk from holding up the launcher's exit.
            (void)task.release();
        }
    }
};

ClassPortraits::ClassPortraits(PortraitLoader loader) : loader_(std::move(loader)) {}

ClassPortraits::~ClassPortraits() = default;

void ClassPortraits::Use(const CharacterArtSources& sources) {
    const std::string key = KeyOf(sources);
    if (key == key_) {
        return;
    }
    request_.reset();
    key_ = key;
    images_ = PortraitImages{};
    ++generation_;
    if (key.empty() || !loader_) {
        return;
    }
    auto request = std::make_shared<Request>();
    const bool started = request->task->Start("", [loader = loader_, sources, out = request->images](trg::Task&) -> std::string {
        *out = loader(sources);
        return "";
    });
    if (started) {
        request_ = std::move(request);
    }
}

void ClassPortraits::Poll() {
    if (!request_ || request_->task->Finish() == trg::Task::State::kIdle) {
        return;
    }
    images_ = std::move(*request_->images);
    request_.reset();
    ++generation_;
}

const SpriteImage* ClassPortraits::Image(CharacterClass klass) const {
    const std::optional<SpriteImage>& face = images_[static_cast<size_t>(klass)];
    return face ? &*face : nullptr;
}

} // namespace dcp::home
