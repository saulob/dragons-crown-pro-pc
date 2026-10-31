#include "npc_voice_resources.h"

#include <algorithm>

#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr std::string_view kNames[] = {
    "snd/dc_sound.acf",           "snd/se_gimmick10000_v.acb", "snd/se_gimmick10200_v.acb", "snd/se_gimmick10400_v.acb",
    "snd/se_gimmick10500_v.acb",  "snd/se_gimmick10600_v.acb", "snd/se_gimmick10800_v.acb", "snd/se_enemy7100_v.acb",
    "snd/se_enemy7200_v.acb",     "snd/se_enemy8800_v.acb",
};

} // namespace

std::span<const std::string_view> NpcVoiceResourceNames() {
    return kNames;
}

bool NpcVoiceResourcesPresent(const std::filesystem::path& app0) {
    CpkArchive archive;
    if (!archive.Open(app0 / "DRACRO.CPK")) {
        return false;
    }
    return std::all_of(std::begin(kNames), std::end(kNames),
                       [&](std::string_view name) { return archive.Find(name) != nullptr; });
}

} // namespace dcp

extern "C" int dcp_npc_voice_resources(const char* app0) {
    return app0 && dcp::NpcVoiceResourcesPresent(dcp::PathFromUtf8(app0)) ? 1 : 0;
}
