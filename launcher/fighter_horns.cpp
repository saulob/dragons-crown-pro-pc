#include "fighter_horns.h"

namespace dcp {
namespace {

constexpr HornPoint kCut0[] = {{82, 62}, {86, 75}, {86, 95}, {82, 112}, {70, 126}};
constexpr HornPoint kCut1[] = {{176, 66}, {180, 78}, {181, 100}, {180, 118}, {190, 130}};
constexpr HornPoint kCut2[] = {{108, 50}, {116, 57}, {122, 62}, {127, 72}, {127, 85}, {124, 95}, {118, 101}, {108, 107}};
constexpr HornPoint kCut3[] = {{5, 110}, {30, 113}, {50, 117}, {60, 112}, {60, 90}, {62, 76}, {58, 63}};
constexpr HornPoint kCut4[] = {{152, 78}, {157, 88}, {158, 105}, {156, 125}, {165, 136}};
constexpr HornPoint kCut5[] = {{0, 112}, {30, 111}, {50, 106}, {54, 92}, {54, 62}, {62, 50}};
constexpr HornPoint kCut6[] = {{124, 90}, {127, 110}, {128, 130}, {131, 148}, {136, 160}};
constexpr HornPoint kCut7[] = {{68, 62}, {80, 70}, {90, 72}, {100, 74}, {106, 82}, {110, 95}, {108, 108}, {100, 116}, {95, 124}, {90, 134}};
constexpr HornPoint kCut8[] = {{205, 78}, {196, 84}, {193, 95}, {193, 110}, {196, 118}, {205, 122}};
constexpr HornPoint kCut9[] = {{136, 146}, {145, 143}, {155, 141}, {165, 139}, {172, 140}};
constexpr HornPoint kCut10[] = {{74, 80}, {78, 86}, {86, 92}, {90, 102}, {91, 116}, {86, 128}, {76, 135}, {62, 140}};
constexpr HornPoint kCut11[] = {{168, 76}, {172, 84}, {176, 100}, {182, 115}, {190, 128}, {197, 137}, {206, 140}};
constexpr HornPoint kCut12[] = {{58, 92}, {58, 104}, {52, 112}, {50, 125}, {49, 140}, {46, 150}, {38, 158}};
constexpr HornPoint kCut13[] = {{146, 106}, {150, 114}, {152, 125}, {154, 140}, {160, 150}, {172, 156}};
constexpr HornPoint kCut14[] = {{0, 98}, {3, 104}, {15, 108}, {30, 113}, {45, 113}, {52, 105}, {52, 92}, {50, 70}, {48, 58}, {46, 50}, {46, 38}};
constexpr HornPoint kCut15[] = {{118, 84}, {123, 92}, {125, 105}, {124, 120}, {126, 132}, {132, 140}};
constexpr HornPoint kCut16[] = {{6, 30}, {16, 37}, {28, 44}, {45, 48}, {65, 46}, {80, 42}, {95, 40}, {110, 38}, {125, 38}, {145, 38}};
constexpr HornPoint kCut17[] = {{128, 90}, {125, 96}, {121, 106}, {114, 114}, {104, 119}, {92, 124}, {75, 127}, {58, 128}, {45, 131}, {36, 136}, {30, 150}};
constexpr HornPoint kCut18[] = {{10, 78}, {22, 76}, {35, 70}, {47, 63}, {55, 61}, {62, 62}, {66, 68}, {68, 78}, {68, 92}, {65, 102}, {60, 108}, {54, 112}, {50, 122}};
constexpr HornPoint kCut19[] = {{153, 64}, {147, 70}, {143, 85}, {138, 98}, {132, 105}, {128, 114}};
constexpr HornPoint kCut20[] = {{0, 66}, {14, 59}, {15, 60}, {30, 57}, {42, 57}, {50, 61}, {58, 67}, {67, 77}, {80, 87}, {92, 95}, {102, 104}, {109, 112}, {100, 114}, {85, 111}, {70, 109}, {55, 108}, {40, 106}, {25, 103}, {12, 96}, {0, 88}, {0, 66}};
constexpr HornPoint kCut21[] = {{124, 54}, {126, 62}, {127, 75}, {126, 90}, {124, 103}, {128, 115}};
constexpr HornPoint kCut22[] = {{22, 30}, {28, 42}, {30, 52}, {28, 62}, {26, 75}, {26, 90}, {28, 100}, {35, 105}, {45, 110}, {55, 111}, {62, 108}, {68, 100}, {70, 88}, {68, 75}, {66, 62}, {66, 55}, {70, 46}, {76, 36}};
constexpr HornPoint kCut23[] = {{126, 80}, {130, 90}, {131, 105}, {130, 120}, {132, 132}, {140, 142}};
constexpr HornPoint kCut24[] = {{50, 52}, {58, 54}, {66, 53}, {74, 50}, {80, 46}};
constexpr HornPoint kCut25[] = {{95, 52}, {97, 60}, {99, 66}, {100, 75}, {96, 85}, {98, 97}, {104, 106}, {112, 111}, {122, 112}, {132, 108}, {137, 103}, {141, 112}};

constexpr HornCut kHorns0[] = {{{30, 60}, kCut0}, {{230, 80}, kCut1}};
constexpr HornCut kHorns1[] = {{{40, 72}, kCut2}};
constexpr HornCut kHorns2[] = {{{30, 60}, kCut0}, {{230, 80}, kCut1}};
constexpr HornCut kHorns3[] = {{{20, 60}, kCut3}, {{210, 90}, kCut4}};
constexpr HornCut kHorns4[] = {{{20, 60}, kCut3}, {{210, 90}, kCut4}};
constexpr HornCut kHorns5[] = {{{30, 60}, kCut0}, {{230, 80}, kCut1}};
constexpr HornCut kHorns6[] = {{{30, 30}, kCut5}, {{180, 115}, kCut6}};
constexpr HornCut kHorns7[] = {{{30, 30}, kCut5}, {{180, 115}, kCut6}};
constexpr HornCut kHorns8[] = {{{40, 105}, kCut7}, {{199, 97}, kCut8}, {{150, 148}, kCut9}};
constexpr HornCut kHorns9[] = {{{25, 60}, kCut10}, {{185, 90}, kCut11}};
constexpr HornCut kHorns10[] = {{{25, 60}, kCut12}, {{185, 80}, kCut13}};
constexpr HornCut kHorns11[] = {{{25, 60}, kCut14}, {{160, 90}, kCut15}};
constexpr HornCut kHorns12[] = {{{100, 22}, kCut16}, {{180, 128}, kCut17}};
constexpr HornCut kHorns13[] = {{{25, 120}, kCut18}, {{160, 130}, kCut19}};
constexpr HornCut kHorns14[] = {{{30, 80}, kCut20}, {{180, 85}, kCut21}};
constexpr HornCut kHorns15[] = {{{50, 40}, kCut22}, {{150, 85}, kCut23}};
constexpr HornCut kHorns16[] = {{{68, 38}, kCut24}, {{150, 40}, kCut25}};

constexpr HornSprite kSprites[] = {
    {372, 1856, 636, 2048, kHorns0},
    {756, 1436, 988, 1588, kHorns1},
    {2040, 288, 2304, 480, kHorns2},
    {2268, 1668, 2520, 1868, kHorns3},
    {2436, 1412, 2688, 1612, kHorns4},
    {2464, 1140, 2728, 1332, kHorns5},
    {2720, 0, 2952, 208, kHorns6},
    {2764, 844, 2996, 1052, kHorns7},
    {2924, 1868, 3132, 2048, kHorns8},
    {2952, 256, 3168, 460, kHorns9},
    {3108, 1412, 3312, 1624, kHorns10},
    {3352, 1140, 3548, 1344, kHorns11},
    {3496, 1608, 3716, 1768, kHorns12},
    {3560, 844, 3744, 1056, kHorns13},
    {3566, 256, 3798, 408, kHorns14},
    {3664, 0, 3840, 204, kHorns15},
    {3844, 1608, 4016, 1800, kHorns16},
};

} // namespace

std::span<const HornSprite> FighterHornSprites() {
    return kSprites;
}

} // namespace dcp
