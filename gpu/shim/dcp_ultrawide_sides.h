// SPDX-License-Identifier: GPL-2.0-or-later
// The side areas of a target wider than 16:9, which the final presenter blacks out during a True Ultrawide fade.

#pragma once

#include <cmath>
#include <cstdint>

namespace DcpUltrawide {

struct Size {
    uint32_t width = 0, height = 0;
    bool operator==(const Size&) const = default;
};

/// [left, right) x [top, bottom); empty when nothing is inside.
struct Rect {
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    bool Empty() const {
        return right <= left || bottom <= top;
    }
};

/// The parts of a target wider than 16:9 outside its centred 16:9 area; both empty for other shapes.
struct SideAreas {
    Rect left, right;
};

inline SideAreas SidesOf(Size target) {
    const uint32_t centre = uint32_t(std::lround(target.height * 16.0 / 9.0));
    if (target.width <= centre + 1) {
        return {};
    }
    const uint32_t side = (target.width - centre) / 2;
    return {{0, 0, side, target.height}, {target.width - side, 0, target.width, target.height}};
}

} // namespace DcpUltrawide
