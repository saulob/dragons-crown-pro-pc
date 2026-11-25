// The class faces the Home page shows, rendered from the installed game by the Editor's own CharacterArt on a worker
// thread. Without a readable game there are none, and the page draws text only.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "character_art.h"

namespace dcp::home {

using PortraitImages = std::array<std::optional<SpriteImage>, 6>;
using PortraitLoader = std::function<PortraitImages(const CharacterArtSources&)>;

// Every class's face, or none at all when the archives cannot be read.
PortraitImages LoadClassPortraits(const CharacterArtSources& sources);

class ClassPortraits {
public:
    explicit ClassPortraits(PortraitLoader loader = LoadClassPortraits);
    ~ClassPortraits();
    ClassPortraits(const ClassPortraits&) = delete;
    ClassPortraits& operator=(const ClassPortraits&) = delete;

    // Loads for these archives unless they are the ones in hand; no archives drop the faces.
    void Use(const CharacterArtSources& sources);
    void Poll();
    const SpriteImage* Image(CharacterClass klass) const;
    bool loading() const { return request_ != nullptr; }
    // Changes whenever the set of faces does.
    unsigned generation() const { return generation_; }

private:
    struct Request;
    PortraitLoader loader_;
    std::string key_;
    PortraitImages images_;
    std::shared_ptr<Request> request_;
    unsigned generation_ = 0;
};

} // namespace dcp::home
