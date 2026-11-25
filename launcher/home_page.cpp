#include "home_page.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <utility>

#include "header_art.h"
#include "localization/ui_text.h"
#include "save_data.h"
#include "trg/launcher.h"

namespace dcp {
namespace {

constexpr char kDash[] = "\xE2\x80\x94";
constexpr float kHeadingSize = 24.0f;
constexpr float kCardNumberSize = 28.0f;
constexpr float kActionTextSize = 20.0f;
constexpr const char* kNowLabels[4] = {"Players online now", "Players playing online", "Open online games", "Peak players today"};
constexpr const char* kTodayLabels[3] = {"Online sessions today", "Online games created today", "Players joined today"};
constexpr const char* kMonthLabels[1] = {"Multiplayer sessions"};
constexpr float kDensities[] = {1.0f, 0.9f, 0.8f, 0.72f};

const ImVec4 kNavy(0.027f, 0.047f, 0.102f, 1.0f);

double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float FontPixels(ImFont* font, float size) {
    ImGui::PushFont(font, size);
    const float pixels = ImGui::GetFontSize();
    ImGui::PopFont();
    return pixels;
}

float TextWidth(ImFont* font, float size, const char* text) {
    ImGui::PushFont(font, size);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    return width;
}

void ShadowedText(ImDrawList* draw, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text, float shadow,
                  float wrap = 0.0f) {
    draw->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 170), text, nullptr, wrap);
    draw->AddText(font, size, at, color, text, nullptr, wrap);
}

void Skeleton(ImDrawList* draw, ImVec2 min, ImVec2 max, const trg::Theme& theme, float rounding) {
    const float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 3.0f);
    draw->AddRectFilled(min, max, trg::Col(theme.text, 0.05f + 0.04f * pulse), rounding);
}

void Panel(ImDrawList* draw, ImVec2 min, ImVec2 max, const trg::Theme& theme, float scale, float fill_alpha,
           float border_alpha) {
    draw->AddRectFilled(min, max, trg::Col(kNavy, fill_alpha), 12 * scale);
    draw->AddRect(min, max, trg::Col(theme.accent, border_alpha), 12 * scale, 0, std::max(1.0f, std::floor(scale)));
}

void SectionLabel(const trg::Ui& ui, ImVec2 at, const char* text) {
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextHelp);
    ShadowedText(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), at, trg::Col(ui.theme().accent, 0.95f), Tr(text),
                 ui.scale());
    ImGui::PopFont();
}

constexpr const char* kHeroLabels[3] = {"Downloads", "Online sessions since launch", "Players since launch"};
const FileIconAsset* const kHeroIcons[3] = {&kDownloadIcon, &kWifiIcon, &kAvatarIcon};
constexpr float kHeroNumberSizes[3] = {40.0f, 32.0f, 32.0f};

struct HeroLayout {
    bool stacked = false;
    int columns = 3, rows = 1;
    float height = 0, pad = 0, gap = 0, column_w = 0, item_h = 0, stats_h = 0, icon_px = 0;
    float number_h[3] = {}, number_w[3] = {};
    float label_h = 0, action_w = 0, action_h = 0, small_h = 0, actions_h = 0;
};

HeroLayout MeasureHero(const trg::Ui& ui, float width, float density, bool play, const std::string (&values)[3]) {
    const float s = ui.scale();
    HeroLayout hero;
    hero.pad = 18 * s * density;
    hero.gap = 8 * s;
    float widest = 0.0f;
    for (int i = 0; i < 3; ++i) {
        hero.number_h[i] = FontPixels(ui.fonts().bold, kHeroNumberSizes[i] * density);
        hero.number_w[i] = TextWidth(ui.fonts().bold, kHeroNumberSizes[i] * density, values[i].c_str());
        widest = std::max(widest, hero.number_w[i]);
    }
    hero.icon_px = std::floor(hero.number_h[1] * 0.7f);
    hero.action_h = 46 * s * (density > 0.85f ? 1.0f : 0.9f);
    hero.small_h = ImGui::GetFrameHeight();
    hero.actions_h = hero.action_h + hero.gap + hero.small_h;
    const float small_w = 2 * (std::max(ImGui::CalcTextSize(Tr("ONLINE LOBBY")).x, ImGui::CalcTextSize(Tr("MANAGE GAME")).x) +
                               ImGui::GetStyle().FramePadding.x * 2) + hero.gap;
    hero.action_w = std::max({230 * s, small_w, TextWidth(ui.fonts().bold, kActionTextSize, Tr(play ? "PLAY" : "INSTALL GAME")) + 48 * s});

    const float minimum = std::max(120 * s, hero.icon_px + 6 * s + widest + 14 * s);
    const float beside = width - hero.action_w - hero.pad * 2 - 24 * s;
    hero.stacked = int(beside / minimum) < 3;
    const float room = hero.stacked ? width - hero.pad * 2 : beside;
    hero.columns = std::clamp(int(room / minimum), 1, 3);
    hero.column_w = room / float(hero.columns);
    hero.rows = (3 + hero.columns - 1) / hero.columns;

    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    hero.label_h = 0.0f;
    for (const char* label : kHeroLabels) {
        hero.label_h = std::max(hero.label_h, ImGui::CalcTextSize(Tr(label), nullptr, false, std::max(1.0f, hero.column_w - 14 * s)).y);
    }
    ImGui::PopFont();
    hero.item_h = *std::max_element(hero.number_h, hero.number_h + 3) + 2 * s + hero.label_h;
    hero.stats_h = float(hero.rows) * hero.item_h + float(hero.rows - 1) * hero.gap;
    const float inner = hero.stacked ? hero.stats_h + hero.gap * 2 + hero.actions_h : std::max(hero.stats_h, hero.actions_h);
    hero.height = inner + hero.pad * 2;
    return hero;
}

int ColumnsFor(int count, float width, float gap, float minimum) {
    const int fit = std::clamp(int((width + gap) / (minimum + gap)), 1, count);
    for (int columns = fit; columns > 1; --columns) {
        if (count % columns == 0) {
            return columns;
        }
    }
    return 1;
}

CardLayout MeasureCards(const trg::Ui& ui, float width, float density, const char* const* labels, int count, float minimum_height = 0) {
    const float s = ui.scale();
    CardLayout cards;
    cards.gap = 10 * s;
    cards.pad = 13 * s * density;
    cards.columns = ColumnsFor(count, width, cards.gap, 140 * s);
    cards.width = (width - cards.gap * float(cards.columns - 1)) / float(cards.columns);
    cards.number_size = kCardNumberSize * density;
    cards.number = FontPixels(ui.fonts().bold, cards.number_size);
    ImGui::PushFont(ui.fonts().regular, trg::layout::kTextHelp);
    cards.label = ImGui::GetFontSize();
    float label_h = 0.0f;
    for (int i = 0; i < count; ++i) {
        label_h = std::max(label_h, ImGui::CalcTextSize(Tr(labels[i]), nullptr, false, std::max(1.0f, cards.width - cards.pad * 2)).y);
    }
    ImGui::PopFont();
    cards.height = std::max(cards.pad * 2 + cards.number + 4 * s + label_h, count == cards.columns ? minimum_height : 0.0f);
    return cards;
}

float CardsHeight(const CardLayout& cards, int count) {
    const int rows = (count + cards.columns - 1) / cards.columns;
    return float(rows) * cards.height + float(rows - 1) * cards.gap;
}

struct ClassLayout {
    int columns = 1, rows = 1;
    float pad = 0, portrait = 0, item_w = 0, item_h = 0, height = 0;
};

float ClassItemWidth(const trg::Ui& ui, const std::vector<home::RankedClass>& ranked, float portrait) {
    float text = 0.0f;
    for (const home::RankedClass& row : ranked) {
        text = std::max({text, TextWidth(ui.fonts().semibold, trg::layout::kTextHelp, Tr(row.name.c_str())),
                         TextWidth(ui.fonts().semibold, trg::layout::kTextHelp, row.share.c_str())});
    }
    return (portrait > 0 ? portrait + 8 * ui.scale() : 0.0f) + text + 16 * ui.scale();
}

ClassLayout MeasureClasses(const trg::Ui& ui, float width, float density, const std::vector<home::RankedClass>& ranked, bool faces) {
    const float s = ui.scale();
    ClassLayout layout;
    layout.pad = 13 * s * density;
    layout.portrait = faces ? 36 * s * density : 0.0f;
    const float text_h = 2 * FontPixels(ui.fonts().semibold, trg::layout::kTextHelp) + 2 * s;
    layout.item_h = std::max(layout.portrait, text_h) + 6 * s;
    layout.item_w = ClassItemWidth(ui, ranked, layout.portrait);
    const int count = std::max<int>(1, int(ranked.size()));
    layout.columns = std::clamp(int(std::max(1.0f, width - layout.pad * 2) / std::max(layout.item_w, 1.0f)), 1, count);
    layout.rows = (count + layout.columns - 1) / layout.columns;
    layout.height = layout.pad * 2 + float(layout.rows) * layout.item_h - 6 * s;
    return layout;
}

struct MonthLayout {
    bool side = true;
    float gap = 0, third = 0, heading = 0, row = 0, total = 0;
    CardLayout card;
    ClassLayout classes;
    float card_width = 0, classes_width = 0;
};

MonthLayout MeasureMonth(const trg::Ui& ui, float width, float density, const std::vector<home::RankedClass>& ranked, bool faces) {
    const float s = ui.scale();
    MonthLayout month;
    month.gap = 10 * s;
    month.heading = FontPixels(ui.fonts().semibold, trg::layout::kTextHelp) + 4 * s;
    month.side = width >= 600 * s;
    month.third = (width - 2 * month.gap) / 3;
    month.card_width = month.side ? month.third : width;
    month.classes_width = month.side ? width - month.third - month.gap : width;
    month.classes = MeasureClasses(ui, month.classes_width, density, ranked, faces);
    month.card = MeasureCards(ui, month.card_width, density, kMonthLabels, 1, month.side ? month.classes.height : 0.0f);
    month.row = month.side ? std::max(month.card.height, month.classes.height)
                           : month.card.height + month.gap + month.heading + month.classes.height;
    month.total = month.heading + month.row;
    return month;
}

} // namespace

HomePage::HomePage(home::HomeSources sources, home::Fetch fetch, HomeHooks hooks)
    : stats_(std::move(sources), std::move(fetch)), hooks_(std::move(hooks)) {}

HomePage::~HomePage() { ForgetFaces(); }

void HomePage::ForgetFaces() {
    for (const auto& [key, face] : faces_) {
        if (face.texture != ImTextureID{} && hooks_.destroy_texture) {
            hooks_.destroy_texture(face.texture);
        }
    }
    faces_.clear();
}

void HomePage::Tick(bool visible) {
    stats_.Tick(SteadySeconds(), static_cast<int64_t>(std::time(nullptr)), visible);
    if (!visible) {
        return;
    }
    const bool ready = hooks_.game_ready && hooks_.game_ready();
    if (!sources_known_ || ready != sources_ready_) {
        sources_known_ = true;
        sources_ready_ = ready;
        portraits_.Use(ready && hooks_.portrait_sources ? hooks_.portrait_sources() : CharacterArtSources{});
    }
    portraits_.Poll();
    if (portraits_.generation() != faces_generation_) {
        faces_generation_ = portraits_.generation();
        ForgetFaces();
    }
}

const HomePage::FaceTexture* HomePage::Face(CharacterClass klass, int pixels) {
    const SpriteImage* image = portraits_.Image(klass);
    if (!image || image->width <= 0 || image->height <= 0 || !hooks_.create_texture || pixels < 4) {
        return nullptr;
    }
    const auto key = std::make_pair(static_cast<int>(klass), pixels);
    if (const auto found = faces_.find(key); found != faces_.end()) {
        return found->second.texture == ImTextureID{} ? nullptr : &found->second;
    }
    FaceTexture face;
    const float fit = float(pixels) / float(std::max(image->width, image->height));
    face.width = std::max(1, int(std::lround(float(image->width) * fit)));
    face.height = std::max(1, int(std::lround(float(image->height) * fit)));
    const std::vector<uint8_t> rgba = ScaleArtRgba(image->rgba.data(), image->width, image->height, face.width, face.height);
    if (!rgba.empty()) {
        face.texture = hooks_.create_texture(rgba.data(), face.width, face.height);
    }
    const FaceTexture& stored = faces_.emplace(key, face).first->second;
    return stored.texture == ImTextureID{} ? nullptr : &stored;
}

void HomePage::Draw(trg::Ui& ui) {
    const float s = ui.scale();
    const float width = ImGui::GetContentRegionAvail().x, available = ImGui::GetContentRegionAvail().y;
    const bool ready = hooks_.game_ready && hooks_.game_ready();
    const bool portuguese = localization::PortugueseBrazilSelected();
    const home::HomeView& view = stats_.view();
    const home::ServiceStats* lifetime_source = view.service ? &*view.service : nullptr;
    const auto lifetime = [&](std::optional<int64_t> home::LifetimeStats::*field) {
        const std::optional<int64_t> value = lifetime_source && lifetime_source->lifetime ? (*lifetime_source->lifetime).*field : std::nullopt;
        return value ? home::FormatCount(*value, portuguese) : std::string(kDash);
    };
    const std::string hero_values[3] = {view.downloads ? home::FormatCount(static_cast<int64_t>(*view.downloads), portuguese) : kDash,
                                        lifetime(&home::LifetimeStats::sessions), lifetime(&home::LifetimeStats::players)};

    const home::ServiceStats* service = view.service ? &*view.service : nullptr;
    const std::vector<home::RankedClass> ranked =
        service && service->month && service->month->classes ? home::RankClasses(*service->month->classes)
                                                              : std::vector<home::RankedClass>{};
    const bool faces = portraits_.loading() || portraits_.Image(CharacterClass::kFighter) != nullptr;

    float density = kDensities[0];
    for (const float candidate : kDensities) {
        density = candidate;
        const float heading = FontPixels(ui.fonts().bold, kHeadingSize * candidate) + 6 * s;
        const float section = FontPixels(ui.fonts().semibold, trg::layout::kTextHelp) + 6 * s;
        const float hero = MeasureHero(ui, width, candidate, ready, hero_values).height;
        const float rows = CardsHeight(MeasureCards(ui, width, candidate, kNowLabels, 4), 4) +
                           CardsHeight(MeasureCards(ui, width, candidate, kTodayLabels, 3), 3);
        const float month = MeasureMonth(ui, width, candidate, ranked, faces).total;
        const float notes = 2 * (FontPixels(nullptr, trg::layout::kTextHelp) + 6 * s);
        if (heading + hero + 2 * section + rows + month + notes + 56 * s <= available) {
            break;
        }
    }

    ImGui::PushFont(ui.fonts().bold, kHeadingSize * density);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ShadowedText(draw, ImGui::GetFont(), ImGui::GetFontSize(), ImGui::GetCursorScreenPos(), trg::Col(ui.theme().accent),
                 Tr("THE KINGDOM SO FAR"), s);
    ImGui::Dummy(ImVec2(width, ImGui::GetFontSize()));
    ImGui::PopFont();
    ui.Spacer(6);
    DrawHero(ui, width, density, ready, hero_values);
    ui.Spacer(12 * density);

    const auto count = [&](const std::optional<int64_t>& value) {
        return value ? home::FormatCount(*value, portuguese) : std::string(kDash);
    };
    const auto live = [&](std::optional<int64_t> home::LiveStats::*field) {
        return count(service && service->live ? (*service->live).*field : std::nullopt);
    };
    const auto today = [&](std::optional<int64_t> home::TodayStats::*field) {
        return count(service && service->today ? (*service->today).*field : std::nullopt);
    };
    const std::string now[4] = {live(&home::LiveStats::online_now), live(&home::LiveStats::playing_online),
                                live(&home::LiveStats::open_games), live(&home::LiveStats::peak_today)};
    DrawCards(ui, "RIGHT NOW", kNowLabels, now, 4, density, width);
    ui.Spacer(10 * density);
    const std::string day[3] = {today(&home::TodayStats::online_sessions), today(&home::TodayStats::games_created),
                                today(&home::TodayStats::players_joined)};
    DrawCards(ui, "TODAY", kTodayLabels, day, 3, density, width);
    ui.Spacer(10 * density);
    DrawMonth(ui, width, density, ranked, faces);
    ui.Spacer(8 * density);
    if (view.service_failed) {
        ui.Help("Live statistics unavailable");
    } else if (view.service_updated_at && SteadySeconds() - *view.service_updated_at < 2 * home::kServiceTtlSeconds) {
        ui.Help("Updated just now");
    }
    ui.Help("Downloads come from GitHub releases. Multiplayer numbers are anonymous aggregate statistics from the online "
            "service.");
}

void HomePage::DrawHero(trg::Ui& ui, float width, float density, bool ready, const std::string (&values)[3]) {
    const float s = ui.scale();
    const trg::Theme& theme = ui.theme();
    const trg::Fonts& fonts = ui.fonts();
    const home::HomeView& view = stats_.view();
    const bool play = home::PrimaryActionFor(ready) == home::PrimaryAction::kPlay;
    const char* action = play ? "PLAY" : "INSTALL GAME";
    const HeroLayout hero = MeasureHero(ui, width, density, play, values);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Panel(draw, origin, ImVec2(origin.x + width, origin.y + hero.height), theme, s, 0.55f, 0.50f);

    const bool loading = view.downloads_placeholder;
    const float stats_left = origin.x + hero.pad * 1.2f;
    const float stats_top = origin.y + hero.pad + (hero.stacked ? 0.0f : (hero.height - hero.pad * 2 - hero.stats_h) * 0.5f);
    const int pixels = int(hero.icon_px);
    for (int i = 0; i < 3; ++i) {
        const int column = i % hero.columns, row = i / hero.columns;
        const float x = stats_left + float(column) * hero.column_w + (column > 0 ? 12 * s : 0.0f);
        const float y = stats_top + float(row) * (hero.item_h + hero.gap);
        if (column > 0) {
            const float line = stats_left + float(column) * hero.column_w;
            draw->AddLine(ImVec2(line, y), ImVec2(line, y + hero.item_h), trg::Col(theme.accent, 0.22f), std::max(1.0f, std::floor(s)));
        }
        const float number_top = y + (*std::max_element(hero.number_h, hero.number_h + 3) - hero.number_h[i]) * 0.5f;
        float text_x = x;
        if (hooks_.icon_texture) {
            if (const ImTextureID icon = hooks_.icon_texture(*kHeroIcons[i], pixels); icon != ImTextureID{}) {
                draw->AddImage(ImTextureRef(icon), ImVec2(x, number_top + (hero.number_h[i] - float(pixels)) * 0.5f),
                               ImVec2(x + float(pixels), number_top + (hero.number_h[i] - float(pixels)) * 0.5f + float(pixels)),
                               ImVec2(0, 0), ImVec2(1, 1), trg::Col(theme.accent));
                text_x = x + float(pixels) + 6 * s;
            }
        }
        const bool known = values[i] != kDash;
        if (loading && i == 0) {
            Skeleton(draw, ImVec2(text_x, number_top + hero.number_h[i] * 0.15f),
                     ImVec2(text_x + std::min(160 * s, hero.column_w - 20 * s), number_top + hero.number_h[i] * 0.85f), theme, 6 * s);
        } else {
            ImGui::PushFont(fonts.bold, kHeroNumberSizes[i] * density);
            ShadowedText(draw, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_x, number_top),
                         trg::Col(!known ? theme.dim : i == 0 ? theme.accent : theme.text), values[i].c_str(), 2 * s);
            ImGui::PopFont();
        }
        ImGui::PushFont(fonts.semibold, trg::layout::kTextLabel);
        ShadowedText(draw, ImGui::GetFont(), ImGui::GetFontSize(),
                     ImVec2(x, y + *std::max_element(hero.number_h, hero.number_h + 3) + 2 * s), trg::Col(theme.text, 0.82f),
                     Tr(kHeroLabels[i]), s, std::max(1.0f, hero.column_w - 14 * s));
        ImGui::PopFont();
    }

    const float button_w = hero.stacked ? width - hero.pad * 2 : hero.action_w;
    const float actions_x = hero.stacked ? origin.x + hero.pad : origin.x + width - hero.pad * 1.2f - hero.action_w;
    const float actions_top = hero.stacked ? stats_top + hero.stats_h + hero.gap * 2
                                           : origin.y + hero.pad + (hero.height - hero.pad * 2 - hero.actions_h) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(actions_x, actions_top));
    ImGui::PushFont(fonts.bold, kActionTextSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, hero.action_h * 0.5f);
    const bool pressed = ui.AccentButton(action, ImVec2(button_w, hero.action_h));
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (pressed) {
        if (play && hooks_.play) {
            hooks_.play();
        } else if (!play && hooks_.open_play_page) {
            hooks_.open_play_page();
        }
    }
    const float half = (button_w - hero.gap) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(actions_x, actions_top + hero.action_h + hero.gap));
    // Secondary actions get a thin border in the label color so they stay visible over the hero artwork.
    // The border color is constant across hover/active, so those states keep the same outline.
    const auto secondary_button = [&](const char* label, float w) {
        ImVec4 border = theme.text;
        border.w *= 0.55f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, std::floor(s)));
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        const bool clicked = ImGui::Button(label, ImVec2(w, hero.small_h));
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return clicked;
    };
    if (play) {
        if (secondary_button(Tr("ONLINE LOBBY"), half) && hooks_.open_online_page) {
            hooks_.open_online_page();
        }
        ImGui::SetCursorScreenPos(ImVec2(actions_x + half + hero.gap, actions_top + hero.action_h + hero.gap));
        if (secondary_button(Tr("MANAGE GAME"), half) && hooks_.open_play_page) {
            hooks_.open_play_page();
        }
    } else if (secondary_button(Tr("ONLINE LOBBY"), button_w) && hooks_.open_online_page) {
        hooks_.open_online_page();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + hero.height));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

void HomePage::DrawCardGrid(trg::Ui& ui, ImVec2 origin, const CardLayout& cards, const char* const* labels, const std::string* values,
                            int count) {
    const float s = ui.scale();
    const trg::Theme& theme = ui.theme();
    const trg::Fonts& fonts = ui.fonts();
    const bool loading = stats_.view().service_placeholder;
    const float text_w = std::max(1.0f, cards.width - cards.pad * 2);
    ImGui::PushFont(fonts.bold, cards.number_size);
    ImFont* number_font = ImGui::GetFont();
    ImGui::PopFont();
    ImGui::PushFont(fonts.regular, trg::layout::kTextHelp);
    ImFont* label_font = ImGui::GetFont();
    ImGui::PopFont();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; ++i) {
        const ImVec2 min(origin.x + float(i % cards.columns) * (cards.width + cards.gap),
                         origin.y + float(i / cards.columns) * (cards.height + cards.gap));
        const ImVec2 max(min.x + cards.width, min.y + cards.height);
        Panel(draw, min, max, theme, s, 0.62f, 0.16f);
        const ImVec2 text_at(min.x + cards.pad, min.y + cards.pad);
        draw->PushClipRect(min, max, true);
        if (loading) {
            Skeleton(draw, ImVec2(text_at.x, text_at.y + cards.number * 0.15f),
                     ImVec2(text_at.x + std::min(90 * s, text_w), text_at.y + cards.number * 0.85f), theme, 5 * s);
        } else {
            ShadowedText(draw, number_font, cards.number, text_at, trg::Col(values[i] == kDash ? theme.dim : theme.text),
                         values[i].c_str(), s);
        }
        draw->AddText(label_font, cards.label, ImVec2(text_at.x, text_at.y + cards.number + 4 * s), trg::Col(theme.text, 0.72f),
                      Tr(labels[i]), nullptr, text_w);
        draw->PopClipRect();
    }
}

void HomePage::DrawCards(trg::Ui& ui, const char* heading, const char* const* labels, const std::string* values, int count,
                         float density, float width) {
    SectionLabel(ui, ImGui::GetCursorScreenPos(), heading);
    ImGui::Dummy(ImVec2(width, FontPixels(ui.fonts().semibold, trg::layout::kTextHelp)));
    ui.Spacer(4);
    const CardLayout cards = MeasureCards(ui, width, density, labels, count);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    DrawCardGrid(ui, origin, cards, labels, values, count);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + CardsHeight(cards, count)));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

void HomePage::DrawClasses(trg::Ui& ui, ImVec2 origin, float width, float height, float density, const std::vector<home::RankedClass>& ranked,
                           bool faces) {
    const float s = ui.scale();
    const trg::Theme& theme = ui.theme();
    const ClassLayout layout = MeasureClasses(ui, width, density, ranked, faces);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    Panel(draw, origin, ImVec2(origin.x + width, origin.y + height), theme, s, 0.62f, 0.16f);
    draw->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);

    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextHelp);
    ImFont* name_font = ImGui::GetFont();
    const float name_size = ImGui::GetFontSize();
    ImGui::PopFont();
    const float left = origin.x + layout.pad;
    const float top = origin.y + std::max(0.0f, (height - layout.height) * 0.5f) + layout.pad;
    if (ranked.empty()) {
        draw->AddText(name_font, name_size, ImVec2(left, origin.y + (height - name_size) * 0.5f), trg::Col(theme.dim), kDash);
    }
    const int pixels = int(std::lround(layout.portrait));
    for (size_t i = 0; i < ranked.size(); ++i) {
        const float x = left + float(int(i) % layout.columns) * layout.item_w;
        const float y = top + float(int(i) / layout.columns) * layout.item_h;
        float text_x = x;
        if (layout.portrait > 0) {
            const ImVec2 box_min(x, y), box_max(x + layout.portrait, y + layout.portrait);
            draw->AddRectFilled(box_min, box_max, trg::Col(kNavy, 0.7f), 6 * s);
            CharacterClass klass = CharacterClass::kFighter;
            for (const CharacterClass candidate : kCharacterClasses) {
                if (ranked[i].name == ClassName(candidate)) klass = candidate;
            }
            if (const FaceTexture* face = Face(klass, pixels)) {
                const ImVec2 at(box_min.x + std::floor((layout.portrait - float(face->width)) * 0.5f),
                                box_min.y + std::floor((layout.portrait - float(face->height)) * 0.5f));
                draw->AddImage(ImTextureRef(face->texture), at, ImVec2(at.x + float(face->width), at.y + float(face->height)));
            }
            draw->AddRect(box_min, box_max, trg::Col(theme.accent, 0.35f), 6 * s, 0, std::max(1.0f, std::floor(s)));
            text_x = box_max.x + 8 * s;
        }
        const float text_h = 2 * name_size + 2 * s;
        const float text_top = y + (std::max(layout.portrait, text_h) - text_h) * 0.5f;
        ShadowedText(draw, name_font, name_size, ImVec2(text_x, text_top), trg::Col(theme.text), Tr(ranked[i].name.c_str()), s);
        draw->AddText(name_font, name_size, ImVec2(text_x, text_top + name_size + 2 * s), trg::Col(theme.accent, 0.95f),
                      ranked[i].share.c_str());
    }
    draw->PopClipRect();
}

void HomePage::DrawMonth(trg::Ui& ui, float width, float density, const std::vector<home::RankedClass>& ranked, bool faces) {
    const bool portuguese = localization::PortugueseBrazilSelected();
    const home::ServiceStats* service = stats_.view().service ? &*stats_.view().service : nullptr;
    const MonthLayout month = MeasureMonth(ui, width, density, ranked, faces);
    const std::string sessions = service && service->month && service->month->multiplayer_sessions
                                     ? home::FormatCount(*service->month->multiplayer_sessions, portuguese)
                                     : std::string(kDash);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float body = origin.y + month.heading;

    SectionLabel(ui, origin, "LAST 30 DAYS");
    DrawCardGrid(ui, ImVec2(origin.x, body), month.card, kMonthLabels, &sessions, 1);
    if (month.side) {
        const float classes_x = origin.x + month.third + month.gap;
        SectionLabel(ui, ImVec2(classes_x, origin.y), "MOST PLAYED CLASSES");
        DrawClasses(ui, ImVec2(classes_x, body), month.classes_width, month.row, density, ranked, faces);
    } else {
        const float heading_y = body + month.card.height + month.gap;
        SectionLabel(ui, ImVec2(origin.x, heading_y), "MOST PLAYED CLASSES");
        DrawClasses(ui, ImVec2(origin.x, heading_y + month.heading), month.classes_width, month.classes.height, density, ranked, faces);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + month.total));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

} // namespace dcp
