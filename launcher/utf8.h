// TRG Launcher and its settings file take UTF-8 paths.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace dcp {

inline std::string Utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

inline std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}


inline bool IsValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > text.size() || (n == 2 && c < 0xc2)) return false;
        uint32_t cp = n == 1 ? c : c & (0x7f >> n);
        for (size_t k = 1; k < n; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xc0) != 0x80) return false;
            cp = cp << 6 | (b & 0x3f);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp < 0xe000)) {
            return false;
        }
        i += n;
    }
    return true;
}

} // namespace dcp
