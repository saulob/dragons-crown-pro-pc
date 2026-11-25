// The part of the synthetic executable whose shapes are public: scan signatures, render sizes, LZCNT, fonts.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fixture.h"
#include "game_profile.h"

namespace parity {

inline constexpr uint16_t kFontProbeCell = 0x700;

// `layout` gives the render size table, `profile` the font, `label` the LZCNT table, `reference` the scan signatures' places.
void PlantScanned(Fx* fx, const dcp::GameBuild& layout, const dcp::TranslationProfile& profile, std::string_view label,
                  size_t reference);
void PlantStartupSave(Fx* fx, std::string_view label);
size_t ReferenceOf(std::string_view label);
// Where the text change mark is: Boss Battle and the Early Story hint reach the same function.
uint64_t TextChangedSite(Fx* fx);

} // namespace parity
