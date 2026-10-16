// The Controller prompts patch (prompts/xbox-prompts.dcpx) compiled into the executable: launcher/embed.cmake turns
// the file into prompts_patch_data.cpp at build time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace dcp {

extern const uint8_t kXboxPromptsPatch[];
extern const size_t kXboxPromptsPatchSize;

inline std::span<const uint8_t> XboxPromptsPatch() {
    return {kXboxPromptsPatch, kXboxPromptsPatchSize};
}

} // namespace dcp
