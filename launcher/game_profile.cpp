#include "game_profile.h"

#include <string_view>

namespace dcp {
namespace {

constexpr int kWidth = 0;
constexpr int kHeight = 1;

// The render size instructions of the US eboot, in the order the game picks the base PS4 and PS4 Pro sizes.
constexpr DcpResolutionSite kUsResolutionSites[] = {
    {0x5997a8, 1, {0xb8}, 1920, kWidth},  {0x5997ad, 1, {0xb9}, 3840, kWidth},
    {0x5997c3, 1, {0xb8}, 1080, kHeight},  {0x5997c8, 1, {0xb9}, 2160, kHeight},
    {0x599921, 1, {0xb8}, 1920, kWidth},  {0x599926, 1, {0xb9}, 3840, kWidth},
    {0x59993c, 1, {0xb8}, 1080, kHeight},  {0x599941, 1, {0xb9}, 2160, kHeight},
    {0x599b2f, 1, {0xb8}, 3840, kWidth},  {0x599b34, 1, {0xbb}, 1920, kWidth},
    {0x599b39, 2, {0x41, 0xbe}, 1920, kWidth},  {0x599b4a, 2, {0x41, 0xbd}, 2160, kHeight},
    {0x599b50, 1, {0xba}, 1080, kHeight},  {0x599b5a, 2, {0x41, 0xbf}, 1080, kHeight},
    {0x599b79, 1, {0xb8}, 3840, kWidth},  {0x599ddc, 1, {0xb9}, 3840, kWidth},
    {0x599de1, 1, {0xb8}, 1920, kWidth},  {0x599dfc, 1, {0xb9}, 2160, kHeight},
    {0x599e01, 1, {0xb8}, 1080, kHeight},  {0x59a004, 1, {0xb8}, 3840, kWidth},
    {0x59a009, 2, {0x41, 0xbc}, 1920, kWidth},  {0x59a023, 1, {0xb8}, 2160, kHeight},
    {0x59a028, 2, {0x41, 0xb8}, 1080, kHeight},
};

constexpr DcpResolutionSite kAsiaResolutionSites[] = {
    {0x5978a8, 1, {0xb8}, 1920, kWidth},  {0x5978ad, 1, {0xb9}, 3840, kWidth},
    {0x5978c3, 1, {0xb8}, 1080, kHeight},  {0x5978c8, 1, {0xb9}, 2160, kHeight},
    {0x597a21, 1, {0xb8}, 1920, kWidth},  {0x597a26, 1, {0xb9}, 3840, kWidth},
    {0x597a3c, 1, {0xb8}, 1080, kHeight},  {0x597a41, 1, {0xb9}, 2160, kHeight},
    {0x597c2f, 1, {0xb8}, 3840, kWidth},  {0x597c34, 1, {0xbb}, 1920, kWidth},
    {0x597c39, 2, {0x41, 0xbe}, 1920, kWidth},  {0x597c4a, 2, {0x41, 0xbd}, 2160, kHeight},
    {0x597c50, 1, {0xba}, 1080, kHeight},  {0x597c5a, 2, {0x41, 0xbf}, 1080, kHeight},
    {0x597c79, 1, {0xb8}, 3840, kWidth},  {0x597edc, 1, {0xb9}, 3840, kWidth},
    {0x597ee1, 1, {0xb8}, 1920, kWidth},  {0x597efc, 1, {0xb9}, 2160, kHeight},
    {0x597f01, 1, {0xb8}, 1080, kHeight},  {0x598104, 1, {0xb8}, 3840, kWidth},
    {0x598109, 2, {0x41, 0xbc}, 1920, kWidth},  {0x598123, 1, {0xb8}, 2160, kHeight},
    {0x598128, 2, {0x41, 0xb8}, 1080, kHeight},
};

constexpr DcpResolutionSite kJapan100ResolutionSites[] = {
    {0x597848, 1, {0xb8}, 1920, kWidth},  {0x59784d, 1, {0xb9}, 3840, kWidth},
    {0x597863, 1, {0xb8}, 1080, kHeight},  {0x597868, 1, {0xb9}, 2160, kHeight},
    {0x5979c1, 1, {0xb8}, 1920, kWidth},  {0x5979c6, 1, {0xb9}, 3840, kWidth},
    {0x5979dc, 1, {0xb8}, 1080, kHeight},  {0x5979e1, 1, {0xb9}, 2160, kHeight},
    {0x597bcf, 1, {0xb8}, 3840, kWidth},  {0x597bd4, 1, {0xbb}, 1920, kWidth},
    {0x597bd9, 2, {0x41, 0xbe}, 1920, kWidth},  {0x597bea, 2, {0x41, 0xbd}, 2160, kHeight},
    {0x597bf0, 1, {0xba}, 1080, kHeight},  {0x597bfa, 2, {0x41, 0xbf}, 1080, kHeight},
    {0x597c19, 1, {0xb8}, 3840, kWidth},  {0x597e7c, 1, {0xb9}, 3840, kWidth},
    {0x597e81, 1, {0xb8}, 1920, kWidth},  {0x597e9c, 1, {0xb9}, 2160, kHeight},
    {0x597ea1, 1, {0xb8}, 1080, kHeight},  {0x5980a4, 1, {0xb8}, 3840, kWidth},
    {0x5980a9, 2, {0x41, 0xbc}, 1920, kWidth},  {0x5980c3, 1, {0xb8}, 2160, kHeight},
    {0x5980c8, 2, {0x41, 0xb8}, 1080, kHeight},
};

constexpr DcpResolutionSite kJapan102ResolutionSites[] = {
    {0x597778, 1, {0xb8}, 1920, kWidth},  {0x59777d, 1, {0xb9}, 3840, kWidth},
    {0x597793, 1, {0xb8}, 1080, kHeight},  {0x597798, 1, {0xb9}, 2160, kHeight},
    {0x5978f1, 1, {0xb8}, 1920, kWidth},  {0x5978f6, 1, {0xb9}, 3840, kWidth},
    {0x59790c, 1, {0xb8}, 1080, kHeight},  {0x597911, 1, {0xb9}, 2160, kHeight},
    {0x597aff, 1, {0xb8}, 3840, kWidth},  {0x597b04, 1, {0xbb}, 1920, kWidth},
    {0x597b09, 2, {0x41, 0xbe}, 1920, kWidth},  {0x597b1a, 2, {0x41, 0xbd}, 2160, kHeight},
    {0x597b20, 1, {0xba}, 1080, kHeight},  {0x597b2a, 2, {0x41, 0xbf}, 1080, kHeight},
    {0x597b49, 1, {0xb8}, 3840, kWidth},  {0x597dac, 1, {0xb9}, 3840, kWidth},
    {0x597db1, 1, {0xb8}, 1920, kWidth},  {0x597dcc, 1, {0xb9}, 2160, kHeight},
    {0x597dd1, 1, {0xb8}, 1080, kHeight},  {0x597fd4, 1, {0xb8}, 3840, kWidth},
    {0x597fd9, 2, {0x41, 0xbc}, 1920, kWidth},  {0x597ff3, 1, {0xb8}, 2160, kHeight},
    {0x597ff8, 2, {0x41, 0xb8}, 1080, kHeight},
};

constexpr DcpResolutionSite kJapan103ResolutionSites[] = {
    {0x597838, 1, {0xb8}, 1920, kWidth},  {0x59783d, 1, {0xb9}, 3840, kWidth},
    {0x597853, 1, {0xb8}, 1080, kHeight},  {0x597858, 1, {0xb9}, 2160, kHeight},
    {0x5979b1, 1, {0xb8}, 1920, kWidth},  {0x5979b6, 1, {0xb9}, 3840, kWidth},
    {0x5979cc, 1, {0xb8}, 1080, kHeight},  {0x5979d1, 1, {0xb9}, 2160, kHeight},
    {0x597bbf, 1, {0xb8}, 3840, kWidth},  {0x597bc4, 1, {0xbb}, 1920, kWidth},
    {0x597bc9, 2, {0x41, 0xbe}, 1920, kWidth},  {0x597bda, 2, {0x41, 0xbd}, 2160, kHeight},
    {0x597be0, 1, {0xba}, 1080, kHeight},  {0x597bea, 2, {0x41, 0xbf}, 1080, kHeight},
    {0x597c09, 1, {0xb8}, 3840, kWidth},  {0x597e6c, 1, {0xb9}, 3840, kWidth},
    {0x597e71, 1, {0xb8}, 1920, kWidth},  {0x597e8c, 1, {0xb9}, 2160, kHeight},
    {0x597e91, 1, {0xb8}, 1080, kHeight},  {0x598094, 1, {0xb8}, 3840, kWidth},
    {0x598099, 2, {0x41, 0xbc}, 1920, kWidth},  {0x5980b3, 1, {0xb8}, 2160, kHeight},
    {0x5980b8, 2, {0x41, 0xb8}, 1080, kHeight},
};

constexpr PackageLayer kUsLayers[] = {{"01.00", "01.03", false}};
constexpr PackageLayer kJapanBaseLayers[] = {{"01.00", "01.00", false}};
constexpr PackageLayer kJapan102Layers[] = {{"01.00", "01.00", false}, {"01.02", "01.00", true}};
constexpr PackageLayer kJapan103Layers[] = {{"01.00", "01.00", false}, {"01.03", "01.00", true}};
constexpr PackageLayer kAsia100Layers[] = {{"01.00", "01.02", false}};
constexpr PackageLayer kAsia102Layers[] = {{"01.02", "01.00", false}};

constexpr const char* kJapanUpdateFiles[] = {"sce_discmap_patch.plt"};

constexpr GameBuild kGameBuilds[] = {
    {
        "CUSA10454",
        "UP2611-CUSA10454_00-DRAGONSCROWNPRO1",
        "1.00",
        "v1.03",
        "f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a",
        "DRACRO_US.CPK",
        {},
        "CUSA10454 1.00 USA",
        "USA",
        true,
        true,
        kUsLayers,
        kUsResolutionSites,
    },
    {
        "CUSA07001",
        "JP0005-CUSA07001_00-DRAGONSCROWN0001",
        "1.00",
        "CUSA07001-1.00",
        "e838e5c551f4ea2d13a13246dcd5474a888c56aa8abf5110f1219c0ceb0ae05f",
        nullptr,
        {},
        "CUSA07001 1.00 Japan",
        "Japan",
        false,
        false,
        kJapanBaseLayers,
        kJapan100ResolutionSites,
    },
    {
        "CUSA07001",
        "JP0005-CUSA07001_00-DRAGONSCROWN0001",
        "1.02",
        "CUSA07001-1.02",
        "e512685aef4fa1e50eafec8c9699b611992356cbae84e9b0147febceb49843fe",
        "DRACRO_PATCH_JP.CPK",
        kJapanUpdateFiles,
        "CUSA07001 1.02 Japan",
        "Japan",
        false,
        false,
        kJapan102Layers,
        kJapan102ResolutionSites,
    },
    {
        "CUSA07001",
        "JP0005-CUSA07001_00-DRAGONSCROWN0001",
        "1.03",
        "CUSA07001-1.03",
        "dccfd331b02d462f6586330664b230a887928ed1b3909f8625b49623630d1a64",
        "DRACRO_PATCH_JP.CPK",
        kJapanUpdateFiles,
        "CUSA07001 1.03 Japan",
        "Japan",
        false,
        false,
        kJapan103Layers,
        kJapan103ResolutionSites,
    },
    {
        "CUSA10209",
        "HP0177-CUSA10209_00-DRAGONSCROWN0001",
        "1.00",
        "CUSA10209-1.00",
        "d37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b",
        "DRACRO_CK.CPK",
        {},
        "CUSA10209 1.00 Asia/Hong Kong",
        "Asia/Hong Kong",
        false,
        false,
        kAsia100Layers,
        kAsiaResolutionSites,
    },
    {
        "CUSA10209",
        "HP0177-CUSA10209_00-DRAGONSCROWN0001",
        "1.02",
        "CUSA10209-1.02",
        "d37cbc39d84524dd38b818a18bba26ec556bb13e075c493fc927163fec8d039b",
        "DRACRO_CK.CPK",
        {},
        "CUSA10209 1.02 Asia/Hong Kong",
        "Asia/Hong Kong",
        false,
        false,
        kAsia102Layers,
        kAsiaResolutionSites,
    },
};

bool SameLayer(const PackageLayer& a, const PackageLayer& b) {
    return std::string_view(a.app_ver) == b.app_ver && std::string_view(a.sfo_version) == b.sfo_version &&
           a.update == b.update;
}

} // namespace

std::span<const GameBuild> SupportedBuilds() {
    return kGameBuilds;
}

const PackageLayer& FinishedLayer(const GameBuild& build) {
    return build.layers.back();
}

const GameBuild* BaseBuildOf(const GameBuild& build, std::span<const GameBuild> builds) {
    if (build.layers.size() < 2) {
        return nullptr;
    }
    for (const GameBuild& candidate : builds) {
        if (std::string_view(candidate.title_id) == build.title_id && candidate.layers.size() == 1 &&
            SameLayer(candidate.layers.front(), build.layers.front())) {
            return &candidate;
        }
    }
    return nullptr;
}

std::string SupportedBuildsText() {
    const std::span<const GameBuild> builds = SupportedBuilds();
    std::string text;
    for (size_t i = 0; i < builds.size(); ++i) {
        if (i > 0) {
            text += i + 1 == builds.size() ? " and " : ", ";
        }
        text += builds[i].label;
    }
    return text;
}

} // namespace dcp
