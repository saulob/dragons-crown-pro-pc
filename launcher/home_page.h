// The launcher's Home page: downloads, current and recent multiplayer activity, the community, and the PLAY / INSTALL GAME
// action.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "home_community.h"
#include "home_portraits.h"
#include "home_stats.h"
#include "page_icons.h"

namespace trg {
class Ui;
}

namespace dcp {

struct HomeHooks {
    std::function<bool()> game_ready;
    std::function<void()> play;              // the footer PLAY's own request
    std::function<void()> open_play_page;    // install and game management stay on Play
    std::function<void()> open_online_page;  // the Lobby lives on Online
    std::function<CharacterArtSources()> portrait_sources;  // no archives when the game is not installed
    std::function<ImTextureID(const uint8_t* rgba, int width, int height)> create_texture;
    std::function<void(ImTextureID)> destroy_texture;
    std::function<ImTextureID(const FileIconAsset& icon, int pixels)> icon_texture;
};

struct CardLayout {
    int columns = 4;
    float gap = 0, pad = 0, width = 0, height = 0, number = 0, number_size = 0, label = 0;
};

class HomePage {
public:
    HomePage(home::HomeSources sources, home::Fetch fetch, HomeHooks hooks);
    ~HomePage();

    // Every frame, whichever page is open; requests start only while Home is visible.
    void Tick(bool visible);
    void Draw(trg::Ui& ui);

private:
    struct FaceTexture {
        ImTextureID texture{};
        int width = 0, height = 0;
    };

    void DrawHero(trg::Ui& ui, float width, float density, bool ready, const std::string (&values)[3]);
    void DrawCardGrid(trg::Ui& ui, ImVec2 origin, const CardLayout& cards, const char* const* labels, const std::string* values,
                      int count);
    void DrawCards(trg::Ui& ui, const char* heading, const char* const* labels, const std::string* values, int count,
                   float density, float width);
    void DrawClasses(trg::Ui& ui, ImVec2 origin, float width, float height, float density,
                     const std::vector<home::RankedClass>& ranked, bool faces);
    void DrawMonth(trg::Ui& ui, float width, float density, const std::vector<home::RankedClass>& ranked, bool faces);
    const FaceTexture* Face(CharacterClass klass, int pixels);
    void ForgetFaces();

    home::HomeStats stats_;
    home::ClassPortraits portraits_;
    HomeHooks hooks_;
    std::map<std::pair<int, int>, FaceTexture> faces_;
    unsigned faces_generation_ = 0;
    bool sources_known_ = false;
    bool sources_ready_ = false;
};

} // namespace dcp
