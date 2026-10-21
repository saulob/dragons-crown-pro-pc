// The block patches (prompts/*.dcpx) compiled into the executable: launcher/embed.cmake turns each file into a source
// with its bytes at build time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dcp {

extern const uint8_t kXboxPromptsPatch[];
extern const size_t kXboxPromptsPatchSize;
extern const uint8_t kSpiderSafeModePatch[];
extern const size_t kSpiderSafeModePatchSize;

inline std::span<const uint8_t> XboxPromptsPatch() {
    return {kXboxPromptsPatch, kXboxPromptsPatchSize};
}

inline std::span<const uint8_t> SpiderSafeModePatch() {
    return {kSpiderSafeModePatch, kSpiderSafeModePatchSize};
}

} // namespace dcp
