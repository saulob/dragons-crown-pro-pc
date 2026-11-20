// The game's own character art for the Editor, rendered from the user's installed archives: the face portrait the game
// draws for a class (stage/Interface.mbs) and the full figure its Edit character screen shows
// (PLAYER/<CLASS>00_SELECT.MBS over the colour preset's atlas). Nothing is cached on disk.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "appearance_custom.h"
#include "game_texture.h"
#include "save_data.h"
#include "sprite_render.h"

namespace dcp {

class CpkArchive;

// SystemMsg record of a class's colour preset name (Fighter 871 "Sterling Silver" ... Wizard 900), or -1.
int NativeColorMessage(CharacterClass klass, int color);

// The atlas the game samples for a class's colour preset: PLAYER/<CLASS>00.FTX for 0, <CLASS>01..04 for 1..4; empty
// for a value outside 0..4.
std::string NativeColorAtlas(CharacterClass klass, int color);

struct CharacterArtSources {
    std::vector<std::filesystem::path> archives; // searched in order for each entry
    std::filesystem::path cache_dir;              // where learned custom regions are kept; nothing kept when empty
};

// The archives of an installed game folder: the update archive, the build's regional archive, then the base one.
CharacterArtSources ArtSourcesOf(const std::filesystem::path& app0, const char* regional_archive = nullptr);
// The game's per-class choice of select animation.
size_t PreviewAnimation(CharacterClass klass);
std::filesystem::path EditorCacheDir(const std::filesystem::path& user_dir);

class CharacterArt {
public:
    explicit CharacterArt(CharacterArtSources sources);
    ~CharacterArt();

    // Nothing when the game's files are missing or unreadable; error() then says why.
    const SpriteImage* Portrait(CharacterClass klass);
    const SpriteImage* Preview(CharacterClass klass, int color);
    // The preview with a custom look over the preset, re-rendered when the look changes.
    const SpriteImage* CustomPreview(CharacterClass klass, int color, const CustomLook& look);
    // The native colour of a part: the mean of its texels in the preset's atlas.
    std::optional<Rgb> PartAverage(CharacterClass klass, int color, const CustomPart& part);
    // The custom part drawn at a preview pixel, or nullptr; and a tint over every pixel of a part.
    const CustomPart* PartAt(CharacterClass klass, const SpriteImage& image, int x, int y);
    std::vector<uint8_t> PartOverlay(CharacterClass klass, const SpriteImage& image, const CustomPart& part,
                                     std::array<uint8_t, 4> tint);
    // The class's custom regions, learned once from its five atlases; nothing when the game files are unreadable.
    const std::vector<RegionMap>* Regions(CharacterClass klass);
    // The native preset name from the SystemMsg sheet of language `suffix` (msgsheet/SystemMsg_<suffix>.fms), falling back
    // to the unsuffixed and US sheets; empty when no sheet has it or the colour is outside 0..4.
    std::string ColorName(CharacterClass klass, int color, const std::string& suffix);

    const std::string& error() const { return error_; }
    const CharacterArtSources& sources() const { return sources_; }

    // Whether the Appearance page of a class and colour has everything it needs without reading the game files.
    bool Ready(CharacterClass klass, int color) const;
    // Reads and renders what the Appearance page needs first; false when the figure of a valid colour is unreadable.
    bool Prepare(CharacterClass klass, int color, const CustomLook& look);
    // Takes over what `prepared` (same sources, another instance) prepared for `klass`.
    void Adopt(CharacterArt&& prepared, CharacterClass klass);

private:
    void OpenArchives();
    std::vector<uint8_t> Read(std::string_view entry);
    bool LoadPortraits();
    const Mbs* Model(CharacterClass klass);
    bool LoadFigure(CharacterClass klass, int color);
    SpriteImage RenderFigure(const CustomLook* look);
    const FtxSurface* WeaponSurface(uint8_t texture) const; // of the loaded figure, by model texture id

    struct Figure {
        CharacterClass klass;
        int color;
        const Mbs* mbs;
        std::optional<FtxSurface> body, extra, weapon, guard;
    };
    struct CustomRender {
        CharacterClass klass;
        int color;
        CustomLook look;
        SpriteImage image;
    };
    const std::vector<std::string>& Messages(const std::string& suffix);

    CharacterArtSources sources_;
    std::vector<std::unique_ptr<CpkArchive>> archives_;
    std::map<CharacterClass, SpriteImage> portraits_;
    bool portraits_tried_ = false;
    std::map<std::pair<CharacterClass, int>, std::optional<SpriteImage>> previews_;
    std::map<CharacterClass, std::optional<Mbs>> models_;
    std::optional<Figure> figure_;
    std::map<CharacterClass, std::optional<std::vector<RegionMap>>> regions_;
    std::optional<CustomRender> custom_;
    std::map<std::string, std::vector<std::string>> system_messages_;
    std::string error_;
};

} // namespace dcp
