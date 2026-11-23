#include "modifier_values.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace dcp {
namespace {

struct Record {
    uint16_t id;
    int16_t minimum;
    int16_t maximum;
    int16_t bonus_maximum;
    uint8_t slot;
};

constexpr Record kRecords[] = {
#include "modifier_table.inc"
};

} // namespace

std::optional<ModifierSpec> ModifierSpecOf(uint16_t id) {
    const Record* found = std::lower_bound(std::begin(kRecords), std::end(kRecords), id, [](const Record& r, uint16_t v) { return r.id < v; });
    if (found == std::end(kRecords) || found->id != id) return std::nullopt;
    return ModifierSpec{found->minimum, found->maximum, found->bonus_maximum, found->slot};
}

std::optional<size_t> ModifierTextIndex(uint16_t id) {
    if (!ModifierSpecOf(id)) return std::nullopt;
    if (id < 185) return id;
    if (id < 373) return size_t(id) + 0x39d;
    if (id < 751) return size_t(id) + 0x7e8;
    return size_t(id) + 0xc2f;
}

int ModifierShownValue(const ModifierSpec& spec, float stored) {
    float level = stored < 0.0f ? 0.0f : std::min(200.0f, stored);
    int bonus = 0;
    if (level > 100.0f) {
        const float over = level - 100.0f;
        const float extra = float(spec.bonus_maximum) - float(spec.maximum);
        const float scaled = over * extra;
        bonus = int(scaled * 0.01f);
        level = 100.0f;
    }
    const float ratio = level / 100.0f;
    const float span = float(spec.maximum) - float(spec.minimum);
    const float lifted = span * ratio;
    return int(lifted + float(spec.minimum)) + bonus;
}

ModifierShown ModifierShownOf(const ModifierSpec& spec, float stored) {
    ModifierShown shown;
    shown.value = ModifierShownValue(spec, stored);
    shown.minimum = shown.maximum = shown.value;
    for (int value = kModifierStoredMin; value <= kModifierStoredMax; ++value) {
        const int number = ModifierShownValue(spec, float(value));
        shown.minimum = std::min(shown.minimum, number);
        shown.maximum = std::max(shown.maximum, number);
    }
    shown.editable = shown.minimum < shown.maximum && std::isfinite(stored) && stored >= float(kModifierStoredMin) &&
                     stored <= float(kModifierStoredMax) && stored == std::floor(stored);
    return shown;
}

int ModifierStoredForShown(const ModifierSpec& spec, int shown, int current) {
    const int current_shown = ModifierShownValue(spec, float(current));
    int target = shown;
    if (target != current_shown) {
        int below = INT32_MIN, above = INT32_MAX;
        for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) {
            const int value = ModifierShownValue(spec, float(stored));
            if (value == target) {
                below = above = value;
                break;
            }
            if (value < target) below = std::max(below, value);
            if (value > target) above = std::min(above, value);
        }
        if (below == INT32_MIN) target = above;
        else if (above == INT32_MAX) target = below;
        else {
            target = target - below <= above - target ? below : above;
            if (target == current_shown) target = target == below ? above : below;
        }
    }
    int best = current;
    int best_distance = INT32_MAX;
    for (int stored = kModifierStoredMin; stored <= kModifierStoredMax; ++stored) {
        if (ModifierShownValue(spec, float(stored)) != target) continue;
        const int distance = std::abs(stored - current);
        if (distance < best_distance) {
            best = stored;
            best_distance = distance;
        }
    }
    return best;
}

} // namespace dcp
