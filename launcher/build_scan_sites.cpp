// Masked signatures of every game-executable site the port's build-specific features depend on, recovered from the
// supported builds. "??" is a displacement that changes with the build's layout.
#include "build_scan.h"

namespace dcp::scan {

const ReferenceBuild kReferenceBuilds[kReferenceBuildCount] = {
    {"CUSA10454 1.00 USA", 0x11f8060, 0x54c690},
    {"CUSA07001 1.00 Japan", 0x11e8060, 0x54a800},
    {"CUSA07001 1.02 Japan", 0x11e8060, 0x54a660},
    {"CUSA07001 1.03 Japan", 0x11e8060, 0x54a720},
    {"CUSA10209 1.00 Asia/Hong Kong", 0x1238060, 0x54a790},
};

namespace {

constexpr SiteSpec kSiteTable[] = {
    {"True ultrawide", "submit", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec "
        "18 41 89 fc 48 8d 3d ?? ?? ?? ?? 41 89 f6",
        0, 1, {0x5811c0, 0x57f260, 0x57f190, 0x57f250, 0x57f2c0}},
    {"True ultrawide", "fill call 1", SiteKind::kCall,
        "b9 02 00 00 00 48 89 c2 ff 75 d0 6a 00 e8 ?? ?? "
        "?? ?? 48 83 c4 18 eb ?? 31 c0 48 83 c4 08 5b 41 "
        "5c",
        13, 1, {0x581ac3, 0x57fb63, 0x57fa93, 0x57fb53, 0x57fbc3}},
    {"True ultrawide", "fill call 2", SiteKind::kCall,
        "b9 02 00 00 00 8b 7d d4 41 53 41 52 e8 ?? ?? ?? "
        "?? 48 83 c4 18 eb ?? 31 c0 48 83 c4 08 5b 41 5c",
        12, 1, {0x581bb6, 0x57fc56, 0x57fb86, 0x57fc46, 0x57fcb6}},
    {"True ultrawide", "fill call 3", SiteKind::kCall,
        "88 48 6f b9 04 00 00 00 48 89 c2 8b 7d d4 41 52 "
        "6a 00 e8 ?? ?? ?? ?? 48 83 c4 18 eb ?? 31 c0 48 "
        "83 c4 08 5b 41 5c 41 5d 41 5e 41 5f",
        18, 1, {0x581869, 0x57f909, 0x57f839, 0x57f8f9, 0x57f969}},
    {"True ultrawide", "frame commit", SiteKind::kWindow,
        "48 8d bc 03 b0 02 00 00 e8 ?? ?? ?? ?? 8b 45 d4 "
        "42 88 84 2b 20 0b 00 00 4c 01 fb 4c 01 eb 48 89 "
        "df e8 ?? ?? ?? ??",
        13, 1, {0x5a209d, 0x5a012d, 0x5a007d, 0x5a012d, 0x5a018d}},
    {"True ultrawide", "queue", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec "
        "18 4c 8b 25 ?? ?? ?? ?? 49 89 f6 89 d3 49 89 ff",
        0, 1, {0x597830, 0x5958d0, 0x595800, 0x5958c0, 0x595930}},
    {"True ultrawide", "part queue call", SiteKind::kCall,
        "48 69 c9 d0 02 00 00 48 8d bc 08 b0 02 00 00 e8 "
        "?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 48 8b 00 48 3b "
        "45 d0",
        15, 1, {0x57530c, 0x5733ac, 0x5732dc, 0x57339c, 0x57340c}},
    {"True ultrawide", "model draw", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 54 53 49 89 fe 41 8b "
        "86 b8 00 00 00 f6 c4 08 0f 84 ?? ?? ?? ??",
        0, 1, {0x579ac0, 0x577b60, 0x577a90, 0x577b50, 0x577bc0}},
    {"True ultrawide", "widget draw call", SiteKind::kCall,
        "c5 fa 11 8b 74 01 00 00 48 8b 43 38 66 89 83 54 "
        "02 00 00 48 c1 e8 20 89 83 58 02 00 00 e8 ?? ?? "
        "?? ?? c5 fa 10 43 30 c5 fa 10 4b 34 48 b8 00 00 "
        "00 00 00 00 80 3f c5 fa 58 45 d8",
        29, 3, {0x4ec2ec, 0x4ea14c, 0x4ea6cc, 0x4ea78c, 0x4ea15c}},
    {"True ultrawide", "map draw call", SiteKind::kCall,
        "80 bb 9b 01 00 00 00 74 ?? 48 8d bb 00 01 00 00 "
        "e8 ?? ?? ?? ?? 31 f6 48 89 df e8 ?? ?? ?? ?? be "
        "01 00 00 00",
        16, 1, {0x541b89, 0x53fbe9, 0x53fb69, 0x53fc29, 0x53fb79}},
    {"True ultrawide", "facility draw call", SiteKind::kCall,
        "44 89 e6 e8 ?? ?? ?? ?? 4c 89 ff e8 ?? ?? ?? ?? "
        "41 0f b6 f6 4c 89 ef e8 ?? ?? ?? ?? 49 8d bd a0 "
        "3b 00 00",
        11, 1, {0x3a5055, 0x3a4445, 0x3a4885, 0x3a48d5, 0x3a45b5}},
    {"True ultrawide", "worldmap draw", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 30 4c "
        "8b 25 ?? ?? ?? ?? 48 89 fb 40 84 f6 49 8b 04 24",
        0, 1, {0x549a80, 0x547c20, 0x547a60, 0x547b20, 0x547bb0}},
    {"True ultrawide", "fade jumps", SiteKind::kJump,
        "48 81 c3 b0 00 00 00 48 89 df 48 83 c4 08 5b 5d "
        "e9 ?? ?? ?? ?? 48 83 c4 08 5b 5d c3 90 90 90",
        16, 2, {0x44293b, 0x441b6b, 0x441f4b, 0x44200b, 0x441cdb}},
    {"True ultrawide", "camera view call", SiteKind::kCall,
        "48 89 df e8 ?? ?? ?? ?? 48 89 df e8 ?? ?? ?? ?? "
        "c5 fa 10 43 70 c5 fa 10 4b 74 c5 fa 5c 53 78",
        11, 1, {0x118e9, 0x118e9, 0x118e9, 0x118e9, 0x118e9}},
    {"True ultrawide", "view build", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec "
        "88 00 00 00 48 8b 0d ?? ?? ?? ?? 48 8b 01",
        0, 1, {0x110b0, 0x110b0, 0x110b0, 0x110b0, 0x110b0}},
    {"True ultrawide", "pass ortho load 1", SiteKind::kLoad,
        "0f 1f 80 00 00 00 00 41 83 fd 20 7c ?? c5 fa 10 "
        "05 ?? ?? ?? ?? 44 89 e8 41 83 fd 2a c5 f8 28 c8 "
        "c5 fa 10 05 ?? ?? ?? ??",
        13, 1, {0x10686, 0x10686, 0x10686, 0x10686, 0x10686}},
    {"True ultrawide", "pass ortho load 2", SiteKind::kLoad,
        "c5 fa 10 05 ?? ?? ?? ?? 77 ?? c5 fa 10 05 ?? ?? "
        "?? ?? 48 b9 00 00 00 00 18 04 00 00 48 0f a3 c1",
        10, 1, {0x106bb, 0x106bb, 0x106bb, 0x106bb, 0x106bb}},
    {"True ultrawide", "pass ortho load 3", SiteKind::kLoad,
        "44 89 e8 41 83 fd 2a c5 f8 28 c8 c5 fa 10 05 ?? "
        "?? ?? ?? c5 f8 28 d0 c5 fa 10 05 ?? ?? ?? ?? c5 "
        "f8 28 d8",
        11, 1, {0x10699, 0x10699, 0x10699, 0x10699, 0x10699}},
    {"True ultrawide", "pass ortho load 4", SiteKind::kLoad,
        "48 b9 00 00 00 00 18 04 00 00 48 0f a3 c1 c5 f8 "
        "28 c8 c5 fa 10 05 ?? ?? ?? ?? c5 f8 28 d0 c5 fa "
        "10 05 ?? ?? ?? ?? c5 f8 28 d8",
        18, 1, {0x106d5, 0x106d5, 0x106d5, 0x106d5, 0x106d5}},
    {"True ultrawide", "pass ortho load 5", SiteKind::kLoad,
        "c5 fa 10 05 ?? ?? ?? ?? 73 ?? c5 fa 10 05 ?? ?? "
        "?? ?? c5 f8 28 c8 c5 fa 10 05 ?? ?? ?? ?? c5 f8 "
        "28 d0",
        10, 1, {0x106f7, 0x106f7, 0x106f7, 0x106f7, 0x106f7}},
    {"True ultrawide", "pass ortho load 6", SiteKind::kLoad,
        "c5 fa 10 05 ?? ?? ?? ?? c5 f8 28 c8 c5 fa 10 05 "
        "?? ?? ?? ?? c5 f8 28 d0 c5 fa 10 05 ?? ?? ?? ?? "
        "c5 f8 28 d8",
        12, 1, {0x10703, 0x10703, 0x10703, 0x10703, 0x10703}},
    {"System type", "card site 1", SiteKind::kCall,
        "be 24 01 00 00 eb ?? be 22 01 00 00 e8 ?? ?? ?? "
        "?? 4c 89 ff 4c 89 f6 48 8d 8d 58 ff ff ff eb ??",
        12, 1, {0x4d31e3, 0x4d11f3, 0x4d16b3, 0x4d1773, 0x4d1363}},
    {"System type", "card site 2", SiteKind::kCall,
        "48 8d 8d 58 ff ff ff eb ?? be 22 01 00 00 e8 ?? "
        "?? ?? ?? 4c 89 ff 4c 89 f6 48 8d 8d 50 ff ff ff "
        "e8 ?? ?? ?? ??",
        14, 1, {0x4d31fc, 0x4d120c, 0x4d16cc, 0x4d178c, 0x4d137c}},
    {"System type", "set text", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec "
        "38 48 8b 1d ?? ?? ?? ?? 49 89 f5 49 89 fe 4d 85 "
        "ed 48 8b 03 48 89 45 d0",
        0, 1, {0x58f0e0, 0x58d180, 0x58d0b0, 0x58d170, 0x58d1e0}},
    {"Skip offers", "story window", SiteKind::kWindow,
        "41 80 bd ?? ?? 00 00 00 48 8b 82 b0 00 00 00 74 "
        "?? 44 22 a5 3b fd ff ff 22 8d 28 fd ff ff 45 20 "
        "fe 45 20 e6 44 20 f1 75 ?? ba 09 00 00 00 eb ??",
        0, 1, {0x3aed82, 0x3ae162, 0x3ae522, 0x3ae572, 0x3ae2d2}},
    {"Skip offers", "tutorial window", SiteKind::kWindow,
        "48 8b bb f0 00 00 00 e8 ?? ?? ?? ?? 84 c0 0f 84 "
        "?? ?? ?? ?? 4c 8b 6d ?? 4c 89 ef e8 ?? ?? ?? ??",
        12, 1, {0x3a8ad9, 0x3a7f1c, 0x3a8309, 0x3a8359, 0x3a808c}},
    {"Startup save", "step tail", SiteKind::kHook,
        "e8 ?? ?? ?? ?? 49 8b 06 be 01 00 00 00 ba 04 00 "
        "00 00 48 8b 80 a0 00 00 00 4c 89 f7 5b 41 5c 41 "
        "5e 41 5f 5d ff e0 90 90 90 90 90 90 90 90 90 90",
        18, 1, {0x4ddba0, 0x4dbb00, 0x4dc060, 0x4dc120, 0x4dbc70}},
    {"Startup save", "block owner", SiteKind::kFunction,
        "48 8d 87 07 eb 20 00 c3 90 90 90 90 90 90 90 90 "
        "55 48 89 e5 41 56 53 48 89 fb 49 89 f6 48 8d bb "
        "24 f9 0a 00",
        0, 1, {0x81a20, 0x812c0, 0x813a0, 0x813a0, 0x81430}},
    {"Startup save", "accept", SiteKind::kFunction,
        "55 48 89 e5 41 56 53 49 89 f6 48 89 fb e8 ?? ?? "
        "?? ?? 48 8b b8 b0 00 00 00 48 85 ff 74 ?? 0f b6 "
        "43 05",
        0, 1, {0x8bb50, 0x8b230, 0x8b4d0, 0x8b4d0, 0x8b3a0}},
    {"Japanese NPC voices", "NPC cue site", SiteKind::kHook,
        "90 83 fe 01 0f 85 ?? ?? ?? ?? b8 1b 97 01 00 81 "
        "ff 6b 10 00 00 0f 84 ?? ?? ?? ?? 8d 87 48 f4 ff "
        "ff",
        10, 1, {0x68bc9, 0x685a9, 0x68609, 0x68609, 0x68669}},
    {"Japanese NPC voices", "resource acquire", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec "
        "58 48 8b 05 ?? ?? ?? ?? 49 89 f4 31 db 49 89 fe",
        0, 1, {0x57cbb0, 0x57ac50, 0x57ab80, 0x57ac40, 0x57acb0}},
    {"Cheats", "hook: damage", SiteKind::kHook,
        "41 0f 44 cd 45 85 ed 41 0f 4e cd 89 8d dc fd ff "
        "ff e8 ?? ?? ?? ?? 89 c3 c5 fa 2c 85 a8 fe ff ff",
        11, 1, {0xd8703, 0xd7d23, 0xd7fd3, 0xd8023, 0xd7eb3}},
    {"Cheats", "hook: use mana", SiteKind::kHook,
        "e9 ?? ?? ?? ?? 90 90 90 90 90 90 90 55 48 89 e5 "
        "41 57 41 56 41 55 41 54 53 48 81 ec 78 01 00 00",
        12, 1, {0xd6d70, 0xd6390, 0xd6640, 0xd6690, 0xd6520}},
    {"Cheats", "hook: mana drain", SiteKind::kHook,
        "90 90 90 90 90 90 90 90 90 90 55 48 89 e5 41 56 "
        "53 48 89 fb c5 fa 2c 8b 08 01 00 00 0f bf 83 b8 "
        "00 00 00",
        10, 1, {0x345380, 0x344870, 0x344c60, 0x344cb0, 0x344a00}},
    {"Cheats", "hook: arrows", SiteKind::kHook,
        "0f 49 c8 89 8f d4 3a 00 00 c3 90 55 48 89 e5 41 "
        "57 41 56 41 55 41 54 53 48 83 ec 28 4c 8b 25 ?? "
        "?? ?? ??",
        11, 1, {0x1b06f0, 0x1afc80, 0x1affc0, 0x1b0010, 0x1afe10}},
    {"Cheats", "hook: skill points", SiteKind::kHook,
        "be 01 00 00 00 48 89 df e8 ?? ?? ?? ?? 66 44 89 "
        "ab c8 00 00 00 66 45 89 6e 24 48 89 df 48 8b 03 "
        "ff 90 08 08 00 00",
        13, 1, {0x537e8a, 0x535e9a, 0x535e0a, 0x535eca, 0x535eaa}},
    {"Cheats", "hook: player tick", SiteKind::kHook,
        "90 90 90 90 90 90 90 90 90 90 55 48 89 e5 41 57 "
        "41 56 53 50 be be 01 00 00 31 d2 31 c9 45 31 c0",
        10, 1, {0x2ea420, 0x2e9930, 0x2e9d00, 0x2e9d50, 0x2e9ac0}},
    {"Cheats", "hook: durability", SiteKind::kHook,
        "89 85 f4 fc ff ff 45 8b 66 48 83 bd 04 fd ff ff "
        "00 0f 89 ?? ?? ?? ?? 48 8b 85 f8 fc ff ff 48 8b "
        "80 e0 01 00 00",
        10, 1, {0xfe183, 0xfd773, 0xfda53, 0xfdaa3, 0xfd903}},
    {"Cheats", "hook: lives", SiteKind::kHook,
        "48 c7 85 40 fe ff ff 00 00 00 00 ff c8 83 f8 ff "
        "0f 4d c8 48 8d 05 ?? ?? ?? ?? 45 31 e4 45 84 f6",
        11, 1, {0x2ec7cb, 0x2ebcdb, 0x2ec0ab, 0x2ec0fb, 0x2ebe6b}},
    {"Cheats", "hook: walk speed", SiteKind::kHook,
        "c5 fa 10 05 ?? ?? ?? ?? c5 fa 11 45 b8 48 8b 5d "
        "80 48 8b 03 48 89 df ff 90 f0 0c 00 00 c5 fa 11 "
        "45 a8",
        13, 1, {0x2ecd0a, 0x2ec21a, 0x2ec5ea, 0x2ec63a, 0x2ec3aa}},
    {"Cheats", "hook: gold shop", SiteKind::kHook,
        "0f 4e c1 31 c9 85 c0 0f 49 c8 41 89 8c 24 38 f2 "
        "0a 00 e8 ?? ?? ?? ?? 41 89 c6 41 83 fe ff 0f 84 "
        "?? ?? ?? ??",
        10, 1, {0x52e0b3, 0x52c1eb, 0x52c0ab, 0x52c16b, 0x52c1fb}},
    {"Cheats", "hook: gold continue", SiteKind::kHook,
        "0f 49 ca 48 81 c3 28 01 00 00 89 88 38 f2 0a 00 "
        "48 89 df 5b 41 5e 5d e9 ?? ?? ?? ?? 90 90",
        10, 1, {0x43d562, 0x43c792, 0x43cb72, 0x43cc32, 0x43c902}},
    {"Cheats", "hook: gold stable", SiteKind::kHook,
        "0f 49 c1 48 8b 0d ?? ?? ?? ?? 41 89 84 24 38 f2 "
        "0a 00 49 8b 45 00 48 8b 80 a0 00 00 00 48 8b 09",
        10, 1, {0x3bc693, 0x3bb9f3, 0x3bbdc3, 0x3bbe13, 0x3bbb63}},
    {"Cheats", "hook: gold beggar", SiteKind::kHook,
        "0f 4e c8 45 31 e4 85 c9 41 0f 48 cc 89 8a 38 f2 "
        "0a 00 e8 ?? ?? ?? ?? 48 8b b8 b8 00 00 00 48 8d "
        "35 ?? ?? ?? ??",
        12, 1, {0x3bff22, 0x3bf222, 0x3bf632, 0x3bf682, 0x3bf392}},
    {"Cheats", "hook: gold base", SiteKind::kHook,
        "b9 ff e0 f5 05 3d ff e0 f5 05 0f 4e c8 41 89 8d "
        "38 f2 0a 00 e8 ?? ?? ?? ?? 44 89 e7 e8 ?? ?? ?? "
        "?? 0f bf 78 10",
        13, 1, {0x3c2420, 0x3c1720, 0x3c1b30, 0x3c1b80, 0x3c1890}},
    {"Cheats", "hook: gold inn", SiteKind::kHook,
        "0f 4e c8 31 c0 85 c9 0f 49 c1 41 89 85 38 f2 0a "
        "00 e8 ?? ?? ?? ?? 48 89 df e8 ?? ?? ?? ?? 0f be "
        "c0",
        10, 1, {0x3c3b82, 0x3c3029, 0x3c3439, 0x3c3489, 0x3c3199}},
    {"Cheats", "hook: gold inn 2", SiteKind::kHook,
        "b8 00 00 00 00 85 c9 0f 48 c8 41 89 8e 38 f2 0a "
        "00 48 8b bd 70 dc ff ff e8 ?? ?? ?? ?? 0f be c0",
        10, 1, {0x3c40da, 0x3c35ac, 0x3c39bc, 0x3c3a0c, 0x3c371c}},
    {"Cheats", "hook: gold list", SiteKind::kHook,
        "81 f9 ff e0 f5 05 0f 4e d1 85 d2 0f 48 d6 41 89 "
        "96 38 f2 0a 00 80 4d b3 20 80 48 43 20 49 8b 8c "
        "24 48 0e 00 00",
        14, 1, {0x4953cd, 0x49454d, 0x4949dd, 0x494a9d, 0x4946bd}},
    {"Cheats", "hook: gold list 2", SiteKind::kHook,
        "81 ff ff e0 f5 05 0f 4e f7 85 f6 0f 48 f2 41 89 "
        "b6 38 f2 0a 00 80 4d b3 20 80 48 43 20 49 8b 8f "
        "48 0e 00 00",
        14, 1, {0x49562d, 0x4947ad, 0x494c3d, 0x494cfd, 0x49491d}},
    {"Cheats", "hook: gold equip", SiteKind::kHook,
        "0f 4e c8 31 c0 85 c9 0f 49 c1 89 82 38 f2 0a 00 "
        "e8 ?? ?? ?? ??",
        10, 1, {0x4cc47a, 0x4ca55a, 0x4ca9ea, 0x4caaaa, 0x4ca6ca}},
    {"Cheats", "hook: experience", SiteKind::kHook,
        "c5 fa 2a cb c5 fa 59 c1 c4 e1 fa 2c c0 48 83 c4 "
        "08 5b 41 5c 41 5d 41 5e 41 5f 5d c3 55 48 89 e5 "
        "41 57",
        13, 1, {0x27511, 0x26e61, 0x26ee1, 0x26ee1, 0x26f21}},
    {"Cheats", "hook: skill use", SiteKind::kHook,
        "74 ?? be 01 00 00 00 48 89 df e8 ?? ?? ?? ?? 80 "
        "7b 4c 00 75 ?? 4c 89 e7 e8 ?? ?? ?? ??",
        10, 1, {0x1af0dc, 0x1ae66c, 0x1ae9ac, 0x1ae9fc, 0x1ae7fc}},
    {"Cheats", "hook: skill use mirror", SiteKind::kHook,
        "48 89 c7 5b 41 5c 41 5e 41 5f 5d e9 ?? ?? ?? ?? "
        "5b 41 5c 41 5e 41 5f 5d c3 90 90 90 90 90",
        11, 1, {0x1af12a, 0x1ae6ba, 0x1ae9fa, 0x1aea4a, 0x1ae84a}},
    {"Cheats", "hook: skill item wear", SiteKind::kHook,
        "72 ?? be 01 00 00 00 4c 89 ef e8 ?? ?? ?? ?? 48 "
        "8b bb 10 05 00 00 44 89 fe e8 ?? ?? ?? ??",
        10, 1, {0x225f90, 0x225520, 0x225860, 0x2258b0, 0x2256b0}},
    {"Cheats", "hook: skill item wear mirror", SiteKind::kHook,
        "74 ?? be 01 00 00 00 48 89 c7 e8 ?? ?? ?? ?? 49 "
        "8b 06 48 3b 45 d0 75 ?? 48 81 c4 e8 00 00 00",
        10, 1, {0x225fb1, 0x225541, 0x225881, 0x2258d1, 0x2256d1}},
    {"Cheats", "hook: skill budget", SiteKind::kHook,
        "75 ?? 48 ff c3 48 83 fb 56 75 ?? 8b 5d a4 45 01 "
        "f4 41 01 dd 45 29 ec 0f 8d ?? ?? ?? ?? b8 0f 00 "
        "00 00 49 8d bf 38 01 00 00",
        11, 1, {0x8ab24, 0x8a204, 0x8a4a4, 0x8a4a4, 0x8a374}},
    {"Cheats", "function: IsDead", SiteKind::kFunction,
        "48 8b 8f b8 09 00 00 48 85 c9 74 ?? 83 b9 b0 00 "
        "00 00 00 b0 01 7e ?? 8a 8f 03 01 00 00 b0 83 80 "
        "c1 37 80 f9 08",
        0, 1, {0x281ba0, 0x281110, 0x281470, 0x2814c0, 0x2812a0}},
    {"Cheats", "function: Refill", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 54 53 41 89 d6 48 89 "
        "fb 40 84 f6 74 ?? c5 7a 2c bb 04 01 00 00 84 c9 "
        "74 ?? 44 39 bb b0 00 00 00",
        0, 1, {0xe3be0, 0xe31d0, 0xe34b0, 0xe3500, 0xe3360}},
    {"Cheats", "function: GetGold", SiteKind::kFunction,
        "55 48 89 e5 48 8b 07 ff 90 d8 00 00 00 48 8b b8 "
        "e8 00 00 00 e8 ?? ?? ?? ?? 8b 80 38 f2 0a 00 5d "
        "c3 90 90 90 90",
        0, 1, {0x43d460, 0x43c690, 0x43ca70, 0x43cb30, 0x43c800}},
    {"Cheats", "function: AddGold", SiteKind::kFunction,
        "55 48 89 e5 41 56 53 48 89 fb 41 89 f6 48 8b 03 "
        "ff 90 d0 00 00 00 48 8b b8 e8 00 00 00 e8 ?? ?? "
        "?? ??",
        0, 1, {0x43d490, 0x43c6c0, 0x43caa0, 0x43cb60, 0x43c830}},
    {"Cheats", "function: RecordMode", SiteKind::kFunction,
        "80 bf 01 01 00 00 00 b8 63 00 00 00 75 ?? 8a 47 "
        "6f 31 d2 89 c1 80 e1 fe 3c 31 b8 02 00 00 00 0f "
        "97 c2 c1 e2 02",
        0, 1, {0x83ae0, 0x831c0, 0x83460, 0x83460, 0x83330}},
    {"Cheats", "function: RecordStage", SiteKind::kFunction,
        "80 7f 6f 2d 0f 92 c0 c3 90 90 90 90 90 90 90 90 "
        "80 7f 6f 2c 0f 97 c0 c3 90 90 90 90 90 90 90",
        0, 1, {0x86ee0, 0x865c0, 0x86860, 0x86860, 0x86730}},
    {"Cheats", "function: MaxLevel", SiteKind::kFunction,
        "84 d2 b9 fe 00 00 00 b8 22 00 00 00 0f 45 c1 40 "
        "84 f6 75 ?? 84 d2 75 ?? b8 62 00 00 00 83 ff 04 "
        "77 ?? 48 63 c7",
        0, 1, {0x4f950, 0x4f2a0, 0x4f320, 0x4f320, 0x4f360}},
    {"Cheats", "function: LevelEntry", SiteKind::kFunction,
        "40 80 fe fe b9 fe 00 00 00 89 fa 0f 42 ce 31 c0 "
        "83 ff 05 77 ?? 48 8d 05 ?? ?? ?? ?? 48 63 14 90 "
        "48 01 c2 ff e2 89 c8",
        0, 1, {0x2f9da0, 0x2f92b0, 0x2f9680, 0x2f96d0, 0x2f9440}},
    {"Cheats", "function: LevelPattern", SiteKind::kFunction,
        "48 0f be 47 27 48 83 f8 05 77 ?? 48 8d 0d ?? ?? "
        "?? ?? 8b 04 81 c3 b8 fc ff ff ff c3 90 90 90 90 "
        "55 48 89 e5 41 57",
        0, 1, {0x81fe0, 0x81880, 0x81960, 0x81960, 0x819f0}},
    {"Cheats", "function: SetLevel", SiteKind::kFunction,
        "40 80 fe ff 75 ?? 80 7f 27 ff 75 ?? 40 38 77 26 "
        "74 ?? 40 88 77 26 c3 90 90 90 90 90 90 90 90 90 "
        "80 7f 27 ff",
        0, 1, {0x81fb0, 0x81850, 0x81930, 0x81930, 0x819c0}},
    {"Cheats", "function: WriteStats", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 53 50 89 cb 49 89 ff 80 "
        "fb 63 72 ?? 0f b6 cb 4c 89 ff 48 83 c4 08 5b 41 "
        "5e 41 5f 5d e9 ?? ?? ?? ??",
        0, 1, {0x82000, 0x818a0, 0x81980, 0x81980, 0x81a10}},
    {"Cheats", "function: EquipmentCount", SiteKind::kFunction,
        "48 8b 87 10 05 00 00 48 85 c0 74 ?? 8a 80 52 03 "
        "00 00 b1 0a 3c 0a 7f ?? 84 c0 79 ?? 31 c0 c3",
        0, 1, {0x224b40, 0x2240d0, 0x224410, 0x224460, 0x224260}},
    {"Cheats", "function: EquipmentItem", SiteKind::kFunction,
        "85 f6 78 ?? 48 8b 87 10 05 00 00 31 c9 48 85 c0 "
        "74 ?? 8a 80 52 03 00 00 b9 0a 00 00 00 3c 0a 7f "
        "?? 84 c0 79 ?? 31 c0 0f be c8 48 63 c6 48 6b d0 "
        "7c",
        0, 1, {0x2251b0, 0x224740, 0x224a80, 0x224ad0, 0x2248d0}},
    {"Cheats", "function: IsGear", SiteKind::kFunction,
        "55 48 89 e5 41 56 53 48 89 fb 8b 7b 40 81 e7 ff "
        "ff ff 01 e8 ?? ?? ?? ?? 41 b6 01 84 c0 75 ?? 8b "
        "7b 40 81 e7 ff ff ff 01 e8 ?? ?? ?? ?? 84 c0 75 "
        "?? 8b 7b 40 81 e7 ff ff ff 01 e8 ?? ?? ?? ?? 41 "
        "89 c6",
        0, 1, {0x21d20, 0x21a60, 0x21a80, 0x21a80, 0x21940}},
    {"Cheats", "function: SetDurability", SiteKind::kFunction,
        "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec "
        "18 41 89 f6 41 89 d7 48 89 fb 45 85 f6 0f 88 ?? "
        "?? ?? ?? 48 8b 83 08 05 00 00",
        0, 1, {0x225fe0, 0x225570, 0x2258b0, 0x225900, 0x225700}},
    {"Exit Game", "sheet fill", SiteKind::kHook,
        "48 8b 87 f8 00 00 00 8a 48 18 80 f9 03 74 09 80 "
        "f9 01 74 3f 84 c9 75 7a 8b 48 14 48 85 c9 74 72",
        0, 1, {0x58e330, 0x58c3d0, 0x58c300, 0x58c3c0, 0x58c430}},
    {"Exit Game", "row widget", SiteKind::kCall,
        "bf c0 05 00 00 e8 ?? ?? ?? ?? 49 89 c6 4c 89 ee "
        "4c 89 f7 e8 ?? ?? ?? ?? 4c 89 e7 4c 89 f6 41 c7 "
        "86 50 03 00 00 00 05 00 00 41 c7 86 54 03 00 00 "
        "32 00 00 00 4d 89 be 20 03 00 00 e8 ?? ?? ?? ?? "
        "be 7e 01 00 00 31 d2 4c 89 e7 e8 ?? ?? ?? ?? be "
        "7f 01 00 00 31 d2 4c 89 e7 e8 ?? ?? ?? ?? be 80 "
        "01 00 00 31 d2 4c 89 e7 e8 ?? ?? ?? ?? be 81 01 "
        "00 00 31 d2 4c 89 e7 e8 ?? ?? ?? ?? be 82 01 00 "
        "00 31 d2 4c 89 e7 e8 ?? ?? ?? ?? be 84 01 00 00 "
        "31 d2 4c 89 e7 e8 ?? ?? ?? ?? 48 8b 7d b0 be 05 "
        "00 00 00 e8",
        163, 1, {0x53c22a, 0x53a24a, 0x53a1aa, 0x53a26a, 0x53a25a}},
    {"Exit Game", "part table", SiteKind::kLoad,
        "83 fa 25 77 0b 48 8d 15 ?? ?? ?? ?? 8b 74 8a 0c "
        "ff d0",
        5, 2, {0x53cfd7, 0x53afc7, 0x53af37, 0x53aff7, 0x53afd7}},
    {"Exit Game", "menu tick", SiteKind::kHook,
        "ff 90 78 01 00 00 83 c0 02 41 b4 01 83 f8 06 0f "
        "87 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ??",
        6, 1, {0x53d064, 0x53b054, 0x53afc4, 0x53b084, 0x53b064}},
    {"Exit Game", "help limit", SiteKind::kWindow,
        "8d 88 85 01 00 00 83 f8 05 0f 42 f1",
        8, 2, {0x53d348, 0x53b338, 0x53b2a8, 0x53b368, 0x53b348}},
    {"Exit Game", "part TITLE_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 0b 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53cf20, 0x53af10, 0x53ae80, 0x53af40, 0x53af20}},
    {"Exit Game", "part START_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 12 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53d4bf, 0x53b4af, 0x53b41f, 0x53b4df, 0x53b4bf}},
    {"Exit Game", "part START_2", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 15 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53d7c2, 0x53b7b2, 0x53b722, 0x53b7e2, 0x53b7c2}},
    {"Exit Game", "part UPLOAD_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 2e 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53dd30, 0x53bd0d, 0x53bc7d, 0x53bd3d, 0x53bd30}},
    {"Exit Game", "part UPLOAD_2", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 31 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53e01a, 0x53bffc, 0x53bf6c, 0x53c02c, 0x53c01a}},
    {"Exit Game", "part DOWNLOAD_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 4a 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53ea30, 0x53ca92, 0x53ca10, 0x53cad0, 0x53ca22}},
    {"Exit Game", "part DOWNLOAD_2", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 4d 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53ef70, 0x53cfd2, 0x53cf50, 0x53d010, 0x53cf62}},
    {"Exit Game", "part OPTION_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 66 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53fb4d, 0x53dbad, 0x53db2d, 0x53dbed, 0x53db3d}},
    {"Exit Game", "part OPTION_2", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 69 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53fcbf, 0x53dd1f, 0x53dc9f, 0x53dd5f, 0x53dcaf}},
    {"Exit Game", "part GALLERY_1", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 8b 00 00 00 c5 f8 57 c0 ff 50 40",
        8, 1, {0x53fe9c, 0x53defc, 0x53de7c, 0x53df3c, 0x53de8c}},
    {"Exit Game", "part GALLERY_2", SiteKind::kWindow,
        "?? 8d ?? d0 03 00 00 be 8e 00 00 00 c5 f8 57 c0 e8 ?? ?? ?? ?? 4c 89 ff 44 89 f6 e8",
        8, 1, {0x53ff1a, 0x53df7a, 0x53defa, 0x53dfba, 0x53df0a}},
};

constexpr RelationSpec kRelationTable[] = {
    {RelationKind::kCallsSite, "True ultrawide", "fill call 1", "submit"},
    {RelationKind::kCallsSite, "True ultrawide", "fill call 2", "submit"},
    {RelationKind::kCallsSite, "True ultrawide", "fill call 3", "submit"},
    {RelationKind::kCallsSite, "True ultrawide", "part queue call", "queue"},
    {RelationKind::kCallsSite, "True ultrawide", "widget draw call", "model draw"},
    {RelationKind::kCallsSite, "True ultrawide", "map draw call", "model draw"},
    {RelationKind::kCallsSite, "True ultrawide", "facility draw call", "model draw"},
    {RelationKind::kCallsSite, "True ultrawide", "fade jumps", "model draw"},
    {RelationKind::kCallsSite, "True ultrawide", "camera view call", "view build"},
    {RelationKind::kSameTarget, "System type", "card site 1", "card site 2"},
};

} // namespace

std::span<const SiteSpec> Sites() { return kSiteTable; }
std::span<const RelationSpec> Relations() { return kRelationTable; }

} // namespace dcp::scan
