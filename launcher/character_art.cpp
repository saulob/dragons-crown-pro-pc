#include "character_art.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>

#include "cpk_patch.h"
#include "fms_sheet.h"
#include "game_mbs.h"
#include "game_texture.h"

namespace dcp {
namespace {

constexpr float kPortraitScale = 2.0f;
constexpr float kPreviewScale = 2.0f;
constexpr float kItemPictureScale = 1.0f;
constexpr float kUseItemIconScale = 2.0f;
constexpr uint8_t kUseItemIconTexture = 1; // texture 0 of UseItem00 holds the glow

std::optional<SpriteImage> CropToContent(const SpriteImage& image) {
    int x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + 3] < 8) continue;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    SpriteImage cropped;
    cropped.width = x1 - x0;
    cropped.height = y1 - y0;
    cropped.scale = image.scale;
    cropped.rgba.resize(size_t(cropped.width) * size_t(cropped.height) * 4);
    for (int y = 0; y < cropped.height; ++y) {
        std::copy_n(image.rgba.begin() + (size_t(y + y0) * size_t(image.width) + size_t(x0)) * 4, size_t(cropped.width) * 4,
                    cropped.rgba.begin() + size_t(y) * size_t(cropped.width) * 4);
    }
    return cropped;
}

std::string_view UpperName(CharacterClass klass) {
    switch (klass) {
    case CharacterClass::kFighter: return "FIGHTER";
    case CharacterClass::kAmazon: return "AMAZON";
    case CharacterClass::kElf: return "ELF";
    case CharacterClass::kDwarf: return "DWARF";
    case CharacterClass::kSorceress: return "SORCERESS";
    case CharacterClass::kWizard: return "WIZARD";
    }
    return "";
}

std::string_view PortraitAnimation(CharacterClass klass) {
    switch (klass) {
    case CharacterClass::kFighter: return "ICON_FIGHTER";
    case CharacterClass::kAmazon: return "ICON_AMAZON";
    case CharacterClass::kElf: return "ICON_EIF";
    case CharacterClass::kDwarf: return "ICON_DWARF";
    case CharacterClass::kSorceress: return "ICON_SORCERESS";
    case CharacterClass::kWizard: return "ICON_WIZARD";
    }
    return "";
}

std::optional<size_t> FindAnimation(const Mbs& mbs, std::string_view name) {
    for (size_t i = 0; i < mbs.animations.size(); ++i) {
        if (mbs.animations[i].name == name) return i;
    }
    return std::nullopt;
}

} // namespace

int NativeColorMessage(CharacterClass klass, int color) {
    if (color < 0 || color >= kNativeColorCount) return -1;
    switch (klass) {
    case CharacterClass::kFighter: return 871 + color;
    case CharacterClass::kAmazon: return 876 + color;
    case CharacterClass::kDwarf: return 881 + color;
    case CharacterClass::kElf: return 886 + color;
    case CharacterClass::kSorceress: return 891 + color;
    case CharacterClass::kWizard: return 896 + color;
    }
    return -1;
}

std::string NativeColorAtlas(CharacterClass klass, int color) {
    if (color < 0 || color >= kNativeColorCount) return {};
    return "PLAYER/" + std::string(UpperName(klass)) + "0" + std::to_string(color) + ".FTX";
}

CharacterArtSources ArtSourcesOf(const std::filesystem::path& app0, const char* regional_archive) {
    CharacterArtSources sources;
    std::vector<std::string> names = {"DRACRO_PATCH_JP.CPK"};
    if (regional_archive) names.emplace_back(regional_archive);
    names.emplace_back("DRACRO.CPK");
    for (size_t i = 0; i < names.size(); ++i) {
        if (std::find(names.begin(), names.begin() + std::ptrdiff_t(i), names[i]) != names.begin() + std::ptrdiff_t(i)) continue;
        std::error_code ec;
        if (!app0.empty() && std::filesystem::is_regular_file(app0 / names[i], ec)) sources.archives.push_back(app0 / names[i]);
    }
    return sources;
}

std::filesystem::path EditorCacheDir(const std::filesystem::path& user_dir) {
    return user_dir / "cache" / "editor";
}

CharacterArt::CharacterArt(CharacterArtSources sources) : sources_(std::move(sources)) {}
CharacterArt::~CharacterArt() = default;

void CharacterArt::OpenArchives() {
    if (!archives_.empty()) return;
    for (const std::filesystem::path& path : sources_.archives) {
        auto archive = std::make_unique<CpkArchive>();
        if (archive->Open(path)) archives_.push_back(std::move(archive));
    }
}

std::vector<uint8_t> CharacterArt::Read(std::string_view entry) {
    OpenArchives();
    for (const auto& archive : archives_) {
        if (const CpkEntry* found = archive->Find(entry)) {
            std::vector<uint8_t> data = archive->Read(*found);
            if (!data.empty()) return data;
        }
    }
    error_ = "the game file " + std::string(entry) + " is missing or unreadable";
    return {};
}

bool CharacterArt::LoadPortraits() {
    portraits_tried_ = true;
    std::string why;
    const std::optional<Mbs> mbs = ParseMbs(Read("stage/Interface.mbs"), &why);
    if (!mbs) {
        if (!why.empty()) error_ = "stage/Interface.mbs: " + why;
        return false;
    }
    std::vector<uint8_t> pack = Read("stage/Interface.ftx");
    if (pack.empty()) return false;
    std::map<uint8_t, FtxSurface> surfaces;
    for (const CharacterClass klass : kCharacterClasses) {
        const std::optional<size_t> animation = FindAnimation(*mbs, PortraitAnimation(klass));
        if (!animation) {
            error_ = "stage/Interface.mbs has no " + std::string(PortraitAnimation(klass));
            return false;
        }
        SpriteRequest request;
        request.mbs = &*mbs;
        request.animation = *animation;
        request.scale = kPortraitScale;
        for (const uint32_t layer : FrameLayers(*mbs, *animation, 0)) {
            const uint8_t texture = mbs->layers[layer].texture;
            if (texture >= request.textures.size()) continue;
            if (!surfaces.contains(texture)) {
                std::optional<FtxSurface> surface = ReadFtxSurface(pack, texture, &why);
                if (!surface) {
                    error_ = "stage/Interface.ftx: " + why;
                    return false;
                }
                surfaces.emplace(texture, std::move(*surface));
            }
            request.textures[texture] = &surfaces.at(texture);
        }
        portraits_[klass] = RenderSprite(request);
    }
    return true;
}

std::map<std::string, SpriteImage> CharacterArt::StageIcons(const std::string& stage, const std::vector<std::string>& animations) {
    std::map<std::string, SpriteImage> icons;
    std::string why;
    const std::optional<Mbs> mbs = ParseMbs(Read("stage/" + stage + ".mbs"), &why);
    if (!mbs) return icons;
    std::vector<uint8_t> pack = Read("stage/" + stage + ".ftx");
    if (pack.empty()) return icons;
    std::map<uint8_t, FtxSurface> surfaces;
    for (const std::string& name : animations) {
        const std::optional<size_t> animation = FindAnimation(*mbs, name);
        if (!animation) continue;
        SpriteRequest request;
        request.mbs = &*mbs;
        request.animation = *animation;
        request.scale = 1.0f;
        bool complete = true;
        for (const uint32_t layer : FrameLayers(*mbs, *animation, 0)) {
            const uint8_t texture = mbs->layers[layer].texture;
            if (texture >= request.textures.size()) continue;
            if (!surfaces.contains(texture)) {
                std::optional<FtxSurface> surface = ReadFtxSurface(pack, texture, &why);
                if (!surface) {
                    complete = false;
                    continue;
                }
                surfaces.emplace(texture, std::move(*surface));
            }
            request.textures[texture] = &surfaces.at(texture);
        }
        if (!complete) continue;
        if (std::optional<SpriteImage> cropped = CropToContent(RenderSprite(request))) icons.emplace(name, std::move(*cropped));
    }
    return icons;
}

std::vector<uint32_t> ItemPictureLayers(const Mbs& mbs) {
    std::vector<uint32_t> shown;
    if (mbs.animations.empty()) return shown;
    const MbsAnimation& animation = mbs.animations.front();
    // Each track of the throw animation is a complete copy of the weapon; the last is drawn on top, nearest to the viewer.
    const MbsTrack* front = nullptr;
    for (uint8_t t = 0; t < animation.track_count; ++t) {
        if (mbs.tracks[animation.first_track + t].frame_count) front = &mbs.tracks[animation.first_track + t];
    }
    if (!front) return shown;
    const MbsKey& key = mbs.keys[mbs.frames[front->first_frame].key];
    // Within a track the motion trail repeats the weapon's parts, fading in: only the last copy is the weapon itself.
    for (uint32_t i = 0; i < key.layer_count; ++i) {
        const uint32_t index = key.first_layer + i;
        const MbsLayer& layer = mbs.layers[index];
        bool repeated = false;
        for (uint32_t later = i + 1; later < key.layer_count; ++later) repeated |= mbs.layers[key.first_layer + later].id == layer.id;
        if (repeated || layer.color >= mbs.colors.size()) continue;
        const MbsColorQuad& color = mbs.colors[layer.color];
        const bool opaque = color.center.a == 255 ||
                            std::any_of(color.corners.begin(), color.corners.end(), [](const MbsColor& c) { return c.a == 255; });
        if (opaque) shown.push_back(index);
    }
    return shown;
}

std::vector<uint32_t> UseItemIconLayers(const Mbs& mbs, size_t animation) {
    std::vector<uint32_t> icon;
    for (const uint32_t layer : FrameLayers(mbs, animation, 0)) {
        if (mbs.layers[layer].texture == kUseItemIconTexture) icon.push_back(layer);
    }
    return icon;
}

std::map<std::string, SpriteImage> CharacterArt::UseItemIcons(const std::vector<std::string>& animations) {
    std::map<std::string, SpriteImage> icons;
    const std::optional<Mbs> mbs = ParseMbs(Read("player/UseItem00.mbs"));
    if (!mbs) return icons;
    const std::optional<FtxSurface> surface = ReadFtxSurface(Read("player/UseItem00.ftx"), kUseItemIconTexture);
    if (!surface) return icons;
    for (const std::string& name : animations) {
        const std::optional<size_t> animation = FindAnimation(*mbs, name);
        if (!animation) continue;
        const std::vector<uint32_t> layers = UseItemIconLayers(*mbs, *animation);
        if (layers.empty()) continue;
        const std::set<uint32_t> shown(layers.begin(), layers.end());
        SpriteRequest request;
        request.mbs = &*mbs;
        request.animation = *animation;
        request.scale = kUseItemIconScale;
        request.textures[kUseItemIconTexture] = &*surface;
        request.visible = [&](uint32_t layer) { return shown.contains(layer); };
        if (std::optional<SpriteImage> cropped = CropToContent(RenderSprite(request))) icons.emplace(name, std::move(*cropped));
    }
    return icons;
}

std::optional<SpriteImage> CharacterArt::ItemPicture(const std::string& model, const std::string& atlas) {
    const std::optional<Mbs> mbs = ParseMbs(Read(model));
    if (!mbs || mbs->animations.empty()) return std::nullopt;
    std::optional<FtxSurface> surface = ReadFtxSurface(Read(atlas), 0);
    if (!surface) return std::nullopt;
    const std::vector<uint32_t> layers = ItemPictureLayers(*mbs);
    const std::set<uint32_t> shown(layers.begin(), layers.end());
    SpriteRequest request;
    request.mbs = &*mbs;
    request.scale = kItemPictureScale;
    request.textures.fill(&*surface);
    request.visible = [&](uint32_t layer) { return shown.contains(layer); };
    return CropToContent(RenderSprite(request));
}

const SpriteImage* CharacterArt::Portrait(CharacterClass klass) {
    if (!portraits_tried_) LoadPortraits();
    const auto found = portraits_.find(klass);
    return found == portraits_.end() ? nullptr : &found->second;
}

const Mbs* CharacterArt::Model(CharacterClass klass) {
    if (const auto found = models_.find(klass); found != models_.end()) return found->second ? &*found->second : nullptr;
    const std::string name = "PLAYER/" + std::string(UpperName(klass)) + "00_SELECT.MBS";
    std::string why;
    std::optional<Mbs> mbs = ParseMbs(Read(name), &why);
    if (mbs && PreviewAnimation(klass) >= mbs->animations.size()) mbs.reset();
    if (!mbs && !why.empty()) error_ = name + ": " + why;
    std::optional<Mbs>& slot = models_[klass];
    slot = std::move(mbs);
    return slot ? &*slot : nullptr;
}

bool CharacterArt::LoadFigure(CharacterClass klass, int color) {
    if (figure_ && figure_->klass == klass && figure_->color == color) return true;
    figure_.reset();
    const Mbs* mbs = Model(klass);
    if (!mbs) return false;
    const std::string upper(UpperName(klass));
    const std::string atlas_name = NativeColorAtlas(klass, color);
    std::vector<uint8_t> atlas = Read(atlas_name);
    // The weapon and shield are the game's defaults; the equipped item is not read.
    std::vector<uint8_t> arms = Read("PLAYER/" + upper + "00_ARMS01.FTX");
    std::vector<uint8_t> shield;
    if (klass == CharacterClass::kFighter) shield = Read("PLAYER/FIGHTER00_SHIELD01.FTX");
    std::string why;
    Figure figure{klass, color, mbs, ReadFtxSurface(atlas, 0, &why), {}, ReadFtxSurface(std::move(arms), 0, &why), {}};
    if (klass == CharacterClass::kDwarf) figure.extra = ReadFtxSurface(atlas, 1, &why);
    if (!shield.empty()) figure.guard = ReadFtxSurface(std::move(shield), 0, &why);
    if (!figure.body || !figure.weapon || (klass == CharacterClass::kDwarf && !figure.extra) ||
        (klass == CharacterClass::kFighter && !figure.guard)) {
        error_ = atlas_name + ": " + (why.empty() ? std::string("unreadable") : why);
        return false;
    }
    figure_ = std::move(figure);
    return true;
}

SpriteImage CharacterArt::RenderFigure(const CustomLook* look) {
    const Figure& figure = *figure_;
    // Texture ids as the game binds them: the Dwarf's weapon is 2 and its atlas continues as 1.
    SpriteRequest request;
    request.mbs = figure.mbs;
    request.animation = PreviewAnimation(figure.klass);
    request.scale = kPreviewScale;
    request.textures[0] = &*figure.body;
    if (figure.klass == CharacterClass::kDwarf) {
        request.textures[1] = &*figure.extra;
        request.textures[2] = &*figure.weapon;
    } else {
        request.textures[1] = &*figure.weapon;
        if (figure.guard) request.textures[2] = &*figure.guard;
    }
    if (!look || !look->active()) return RenderSprite(request);

    const std::vector<RegionMap>* maps = Regions(figure.klass);
    if (!maps) return RenderSprite(request);
    const std::span<const BodySurface> bodies = BodySurfacesOf(figure.klass);
    std::vector<LookShader> shaders;
    for (size_t b = 0; b < bodies.size() && b < maps->size(); ++b) {
        const FtxSurface& base = bodies[b].texture == 0 ? *figure.body : *figure.extra;
        shaders.emplace_back(figure.klass, *look, (*maps)[b], [&base](int x, int y) { return base.Texel(x, y); });
    }
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(figure.klass);
    std::vector<WeaponShader> weapon_shaders;
    for (size_t w = 0; w < weapons.size(); ++w) {
        const FtxSurface* base = request.textures[weapons[w].texture];
        weapon_shaders.emplace_back(figure.klass, *look, w, [base](int x, int y) { return base ? base->Texel(x, y) : std::array<uint8_t, 4>{}; },
                                    base ? base->width() : 0, base ? base->height() : 0);
    }
    request.shade = [&](uint32_t, uint8_t texture, int x, int y, std::array<uint8_t, 4>& texel) {
        for (size_t b = 0; b < shaders.size(); ++b) {
            if (bodies[b].texture == texture) shaders[b].ApplyAt(x, y, texel);
        }
        for (size_t w = 0; w < weapon_shaders.size(); ++w) {
            if (weapons[w].texture == texture) weapon_shaders[w].Apply(texel);
        }
    };
    return RenderSprite(request);
}

const SpriteImage* CharacterArt::Preview(CharacterClass klass, int color) {
    if (color < 0 || color >= kNativeColorCount) return nullptr;
    const auto key = std::make_pair(klass, color);
    if (const auto cached = previews_.find(key); cached != previews_.end()) {
        return cached->second ? &*cached->second : nullptr;
    }
    std::optional<SpriteImage>& slot = previews_[key];
    if (!LoadFigure(klass, color)) return nullptr;
    slot = RenderFigure(nullptr);
    return &*slot;
}

const SpriteImage* CharacterArt::CustomPreview(CharacterClass klass, int color, const CustomLook& look) {
    if (color < 0 || color >= kNativeColorCount) return nullptr;
    if (!look.active()) return Preview(klass, color);
    if (custom_ && custom_->klass == klass && custom_->color == color && custom_->look == look) return &custom_->image;
    if (!LoadFigure(klass, color)) return nullptr;
    custom_ = CustomRender{klass, color, look, RenderFigure(&look)};
    return &custom_->image;
}

std::optional<Rgb> CharacterArt::PartAverage(CharacterClass klass, int color, const CustomPart& part) {
    const std::vector<RegionMap>* maps = Regions(klass);
    if (!maps || maps->empty() || color < 0 || color >= kNativeColorCount || !LoadFigure(klass, color)) return std::nullopt;
    if (part.weapon >= 0) {
        const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
        if (size_t(part.weapon) >= weapons.size()) return std::nullopt;
        const FtxSurface* surface = WeaponSurface(weapons[size_t(part.weapon)].texture);
        if (!surface) return std::nullopt;
        return WeaponShader(klass, CustomLook{}, size_t(part.weapon), [surface](int x, int y) { return surface->Texel(x, y); },
                            surface->width(), surface->height())
            .NativeColor(part.accent);
    }
    const FtxSurface& body = *figure_->body;
    const LookShader shader(klass, CustomLook{}, maps->front(), [&body](int x, int y) { return body.Texel(x, y); });
    return shader.NativeColor(klass, part);
}

size_t PreviewAnimation(CharacterClass klass) {
    return klass == CharacterClass::kElf || klass == CharacterClass::kWizard ? 1 : 0;
}

const FtxSurface* CharacterArt::WeaponSurface(uint8_t texture) const {
    if (!figure_) return nullptr;
    if (figure_->klass == CharacterClass::kDwarf) return texture == 2 && figure_->weapon ? &*figure_->weapon : nullptr;
    if (texture == 1 && figure_->weapon) return &*figure_->weapon;
    if (texture == 2 && figure_->guard) return &*figure_->guard;
    return nullptr;
}

const CustomPart* CharacterArt::PartAt(CharacterClass klass, const SpriteImage& image, int x, int y) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return nullptr;
    const size_t p = size_t(y) * size_t(image.width) + size_t(x);
    if (image.layer[p] == kNoLayer) return nullptr;
    if (const CustomPart* weapon = WeaponPartOfTexture(klass, image.texture[p])) {
        const CustomPart* accent = weapon->weapon >= 0 ? WeaponAccentOf(klass, size_t(weapon->weapon)) : nullptr;
        const FtxSurface* surface = accent && figure_ && figure_->klass == klass ? WeaponSurface(image.texture[p]) : nullptr;
        if (!surface) return weapon;
        const uint32_t texel = image.texel[p], width = uint32_t(surface->width());
        return AccentWeight(surface->Texel(int(texel % width), int(texel / width))) >= 0.5f ? accent : weapon;
    }
    const std::vector<RegionMap>* maps = Regions(klass);
    if (!maps) return nullptr;
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    for (size_t b = 0; b < bodies.size() && b < maps->size(); ++b) {
        if (bodies[b].texture != image.texture[p]) continue;
        const RegionMap& map = (*maps)[b];
        const int x = int(image.texel[p] % uint32_t(map.width)), y = int(image.texel[p] / uint32_t(map.width));
        if (const uint16_t pieces = map.Pieces(x, y)) {
            for (const CustomPart& part : CustomPartsOf(klass)) {
                if (const uint16_t bit = PieceBitOf(klass, part); bit && (pieces & bit)) return &part;
            }
        }
        return PartOfRegion(klass, map.At(x, y));
    }
    return nullptr;
}

std::vector<uint8_t> CharacterArt::PartOverlay(CharacterClass klass, const SpriteImage& image, const CustomPart& part,
                                               std::array<uint8_t, 4> tint) {
    std::vector<uint8_t> overlay(image.layer.size() * 4, 0);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (PartAt(klass, image, x, y) == &part) std::memcpy(overlay.data() + (size_t(y) * size_t(image.width) + size_t(x)) * 4, tint.data(), 4);
        }
    }
    return overlay;
}

const std::vector<RegionMap>* CharacterArt::Regions(CharacterClass klass) {
    if (const auto found = regions_.find(klass); found != regions_.end()) return found->second ? &*found->second : nullptr;
    OpenArchives();
    std::string identity;
    for (const std::filesystem::path& path : sources_.archives) {
        std::error_code ec;
        identity += path.filename().string() + ":" + std::to_string(std::filesystem::file_size(path, ec)) + ";";
    }
    std::optional<std::vector<RegionMap>> maps =
        ClassRegions(klass, [this](const std::string& entry) { return Read(entry); }, sources_.cache_dir, identity);
    std::optional<std::vector<RegionMap>>& slot = regions_[klass];
    slot = std::move(maps);
    return slot ? &*slot : nullptr;
}

bool CharacterArt::Ready(CharacterClass klass, int color) const {
    if (!regions_.contains(klass)) return false;
    return color < 0 || color >= kNativeColorCount || previews_.contains({klass, color});
}

bool CharacterArt::Prepare(CharacterClass klass, int color, const CustomLook& look) {
    Regions(klass);
    if (color < 0 || color >= kNativeColorCount) return true;
    if (!Preview(klass, color)) return false;
    if (look.active()) CustomPreview(klass, color, look);
    return true;
}

void CharacterArt::Adopt(CharacterArt&& prepared, CharacterClass klass) {
    if (auto found = prepared.regions_.find(klass); found != prepared.regions_.end() && !regions_.contains(klass)) {
        regions_.insert(prepared.regions_.extract(found));
    }
    for (auto it = prepared.previews_.begin(); it != prepared.previews_.end();) {
        auto next = std::next(it);
        if (it->first.first == klass && !previews_.contains(it->first)) previews_.insert(prepared.previews_.extract(it));
        it = next;
    }
    // A figure points into its model, and a moved map node keeps that address.
    if (prepared.figure_ && prepared.figure_->klass == klass && prepared.models_.contains(klass)) {
        if (figure_ && figure_->klass == klass) figure_.reset();
        if (custom_ && custom_->klass == klass) custom_.reset();
        models_.erase(klass);
        models_.insert(prepared.models_.extract(klass));
        figure_ = std::move(prepared.figure_);
        if (prepared.custom_ && prepared.custom_->klass == klass) custom_ = std::move(prepared.custom_);
    }
}

const std::vector<std::string>& CharacterArt::Sheet(const std::string& sheet, const std::string& suffix) {
    const std::string key = sheet + "_" + suffix;
    const auto cached = sheets_.find(key);
    if (cached != sheets_.end()) return cached->second;
    OpenArchives();
    std::vector<std::string>& records = sheets_[key];
    std::vector<std::string> entries;
    if (!suffix.empty()) entries.push_back("msgsheet/" + sheet + "_" + suffix + ".fms");
    entries.push_back("msgsheet/" + sheet + ".fms");
    entries.push_back("msgsheet/" + sheet + "_US.fms");
    for (const std::string& entry : entries) {
        for (const auto& archive : archives_) {
            if (const CpkEntry* found = archive->Find(entry)) {
                if (const std::optional<FmsSheet> parsed = ParseFms(archive->Read(*found))) {
                    records = parsed->records;
                    return records;
                }
            }
        }
    }
    return records;
}

std::string CharacterArt::ColorName(CharacterClass klass, int color, const std::string& suffix) {
    const int id = NativeColorMessage(klass, color);
    if (id < 0) return {};
    const std::vector<std::string>& records = Sheet("SystemMsg", suffix);
    if (size_t(id) >= records.size()) return {};
    std::string name = records[size_t(id)];
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name;
}

} // namespace dcp
