// The Kraken tentacle null mast fix (DCP-8LX692): the faulting sequence in every supported build, the patch and what it
// does to the null and non-null paths, and every refusal. The trace's own lines are tested with the cheat hooks.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "cheat_hooks.h"
#include "guest_hook.h"
#include "kraken_fix.h"

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(condition)) {                                             \
            ++g_failures;                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);    \
        }                                                               \
    } while (0)

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kMastRead[] = {0x49, 0x8b, 0x87, 0xd8, 0x38, 0x00, 0x00};
constexpr uint8_t kFlagCompare[] = {0x80, 0xb8, 0xc1, 0x38, 0x00, 0x00, 0x00};
constexpr uint8_t kSequence[] = {0x49, 0x8b, 0x87, 0xd8, 0x38, 0x00, 0x00, 0x80, 0xb8, 0xc1, 0x38, 0x00, 0x00, 0x00};
constexpr uint8_t kBranchToPosition[] = {0x74, 0x1b};
constexpr uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00};
constexpr uint8_t kPosition[] = {0x49, 0x8b, 0x87, 0xc0, 0x38, 0x00, 0x00};
constexpr uint8_t kStateChange[] = {0x49, 0x8b, 0xbf, 0xc0, 0x38, 0x00, 0x00};
constexpr uint64_t kWindowBytes = 70;
constexpr uint64_t kPositionInWindow = 43;
constexpr uint64_t kStateChangeInWindow = 16;
constexpr uint64_t kSiteInWindow = 7;
constexpr uint64_t kReferenceFunctionToWindow = 0x407;
constexpr uint64_t kFunctionBytes = 0x468;
constexpr uint64_t kVirtualSlotBytes = 0xc20;

struct Build {
    const char* key;
    const char* folder;
    bool research;
    uint64_t window;
};

constexpr Build kBuilds[] = {
    {"CUSA10454-1.00", "v1.03", false, 0x242ec7},
    {"CUSA10454-1.01", "CUSA10454-1.01", true, 0x242ec7},
    {"CUSA07001-1.00", "CUSA07001-1.00", false, 0x242457},
    {"CUSA07001-1.02", "CUSA07001-1.02", false, 0x242797},
    {"CUSA07001-1.03", "CUSA07001-1.03", false, 0x2427e7},
    {"CUSA10209-1.00", "CUSA10209-1.00", false, 0x2425e7},
    {"CUSA10209-1.02", nullptr, false, 0},
    {"CUSA10487-1.03", nullptr, false, 0},
};

bool ReadBootImage(const fs::path& path, Bytes* image) {
    std::ifstream in(path, std::ios::binary);
    auto read64 = [&in]() {
        uint64_t value = 0;
        in.read(reinterpret_cast<char*>(&value), 8);
        return value;
    };
    char magic[8] = {};
    in.read(magic, 8);
    if (!in || std::memcmp(magic, "DCPBOOT1", 8) != 0) {
        return false;
    }
    const uint64_t size = read64();
    read64();
    const uint64_t segments = read64(), relocations = read64(), imports = read64();
    read64();
    for (int i = 0; i < 5; ++i) read64();
    const uint64_t modules = read64();
    for (uint64_t i = 0; i < modules * 7; ++i) read64();
    const uint64_t bindings = read64();
    for (uint64_t i = 0; i < bindings * 3 + segments * 3; ++i) read64();
    in.seekg(static_cast<std::streamoff>(imports * 128 + relocations * 32), std::ios::cur);
    image->resize(size);
    in.read(reinterpret_cast<char*>(image->data()), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

template <size_t N>
size_t Count(const Bytes& image, const uint8_t (&pattern)[N]) {
    size_t count = 0;
    for (auto at = std::search(image.begin(), image.end(), std::begin(pattern), std::end(pattern)); at != image.end();
         at = std::search(at + 1, image.end(), std::begin(pattern), std::end(pattern))) {
        ++count;
    }
    return count;
}

template <size_t N>
bool Matches(const Bytes& image, uint64_t at, const uint8_t (&pattern)[N]) {
    return at + N <= image.size() && std::memcmp(image.data() + at, pattern, N) == 0;
}

// The 70 bytes of the update from the mast read to the position the flag branch lands on.
Bytes Window(const Bytes& image, uint64_t at) {
    return Bytes(image.begin() + std::ptrdiff_t(at), image.begin() + std::ptrdiff_t(at + kWindowBytes));
}

Bytes Synthetic(uint64_t window_at, const Bytes& window) {
    Bytes image(0x20000, 0x90);
    std::copy(window.begin(), window.end(), image.begin() + std::ptrdiff_t(window_at));
    return image;
}

int32_t Displacement(const Bytes& image, uint64_t at) {
    int32_t value;
    std::memcpy(&value, image.data() + at, sizeof(value));
    return value;
}

bool IsRipLea(const Bytes& image, uint64_t at) {
    return at + 7 <= image.size() && image[at] == 0x48 && image[at + 1] == 0x8d && (image[at + 2] & 0xc7) == 0x05;
}

uint64_t LeaTarget(const Bytes& image, uint64_t at) {
    return at + 7 + Displacement(image, at + 3);
}

// The function holding the site is the one DcKrakenTentacle's constructor names: the update's address is the single
// virtual slot 388 of a vtable whose constructor stores that class name.
bool BelongsToTentacle(const Bytes& image, uint64_t site) {
    const uint64_t window = site - kSiteInWindow;
    uint64_t function = 0;
    for (uint64_t at = window; at > window - 0x600 && function == 0; --at) {
        if (Matches(image, at, kPrologue)) {
            function = at;
        }
    }
    if (function == 0) {
        return false;
    }
    uint8_t slot[8];
    std::memcpy(slot, &function, sizeof(slot));
    size_t slots = 0;
    uint64_t slot_at = 0;
    for (auto at = std::search(image.begin(), image.end(), std::begin(slot), std::end(slot)); at != image.end();
         at = std::search(at + 1, image.end(), std::begin(slot), std::end(slot))) {
        ++slots;
        slot_at = uint64_t(at - image.begin());
    }
    if (slots != 1 || slot_at < kVirtualSlotBytes + 0x10) {
        return false;
    }
    const uint64_t table = slot_at - kVirtualSlotBytes - 0x10;
    const char name[] = "DcKrakenTentacle";
    const uint64_t end = std::min<uint64_t>(image.size(), 0x800000);
    for (uint64_t at = 0; at + 7 <= end; ++at) {
        if (!IsRipLea(image, at) || LeaTarget(image, at) != table) {
            continue;
        }
        for (uint64_t k = at + 7; k < at + 0x100 && k + 7 <= end; ++k) {
            const uint64_t target = IsRipLea(image, k) ? LeaTarget(image, k) : 0;
            if (target != 0 && target + sizeof(name) <= image.size() && std::memcmp(image.data() + target, name, sizeof(name)) == 0) {
                return true;
            }
        }
    }
    return false;
}

struct Step {
    uint64_t address = 0;
    uint64_t next = 0;
    bool zero = false;
};

// The compare and the branch after it, decoded from the image's own bytes. `read` returns the byte at an address.
Step RunCompare(const Bytes& image, uint64_t site, uint64_t rax, const std::function<uint8_t(uint64_t)>& read) {
    Step step;
    step.address = rax + uint64_t(int64_t(Displacement(image, site + 2)));
    step.zero = read(step.address) == image[site + 6];
    const int8_t branch = static_cast<int8_t>(image[site + 8]);
    step.next = site + 9 + (step.zero ? uint64_t(int64_t(branch)) : 0);
    return step;
}

uint64_t g_observed = 0;
int g_observed_count = 0;

void Observer(uint64_t tentacle) {
    g_observed = tentacle;
    ++g_observed_count;
}

struct Patched {
    Bytes memory;
    uint64_t size = 0;
    uint8_t* stubs = nullptr;
    size_t stub_bytes = 0;
};

Patched Prepare(const Bytes& image) {
    Patched patched;
    patched.size = image.size();
    const size_t base = (image.size() + 0xfff) / 0x1000 * 0x1000;
    patched.stub_bytes = kraken_fix_stub_bytes();
    patched.memory.assign(base + patched.stub_bytes + 0x1000, 0);
    std::copy(image.begin(), image.end(), patched.memory.begin());
    patched.stubs = patched.memory.data() + base;
    return patched;
}

bool Untouched(const Patched& patched, const Bytes& image) {
    return std::equal(image.begin(), image.end(), patched.memory.begin());
}

int Install(Patched& patched, const char* key) {
    return kraken_fix_install(patched.memory.data(), patched.size, 0x54c690, key, patched.stubs, patched.stub_bytes);
}

void CheckSequence(const std::string& label, const Build& build, const Bytes& image) {
    const bool listed = build.window != 0;
    CHECK(Count(image, kSequence) == 1);
    uint64_t found = 0;
    for (auto at = std::search(image.begin(), image.end(), std::begin(kSequence), std::end(kSequence)); at != image.end(); ++at) {
        found = uint64_t(at - image.begin());
        break;
    }
    CHECK(!listed || found == build.window);
    const uint64_t window = found;
    CHECK(Matches(image, window, kMastRead) && Matches(image, window + 7, kFlagCompare) &&
          Matches(image, window + 14, kBranchToPosition));
    CHECK(Matches(image, window + kStateChangeInWindow, kStateChange) && Matches(image, window + kPositionInWindow, kPosition));
    CHECK(BelongsToTentacle(image, window + kSiteInWindow));
    if (!BelongsToTentacle(image, window + kSiteInWindow)) {
        std::printf("kraken fix image %s: the site is not in the tentacle update\n", label.c_str());
    }

    uint64_t site = 0;
    CHECK(kraken_fix_locate(image.data(), image.size(), build.key, &site) == 1 && site == window + kSiteInWindow);
    CHECK(Matches(image, site, kFlagCompare));

    Patched patched = Prepare(image);
    CHECK(Install(patched, build.key) == 1);
    CHECK(patched.memory[site] == 0xe9);
    size_t changed_outside = 0;
    for (size_t at = 0; at < image.size(); ++at) {
        if (patched.memory[at] != image[at] && (at < site || at >= site + KRAKEN_FIX_SITE_BYTES)) {
            ++changed_outside;
        }
    }
    CHECK(changed_outside == 0);
    CHECK(std::search(patched.stubs, patched.stubs + patched.stub_bytes, std::begin(kFlagCompare), std::end(kFlagCompare)) !=
          patched.stubs + patched.stub_bytes);
    CHECK(Count(Bytes(patched.memory.begin(), patched.memory.begin() + std::ptrdiff_t(image.size())), kSequence) == 0);

    CHECK(Install(patched, build.key) == 0);
}

// What the compare does with a null handle, with a mast that is not broken and with one that is.
void CheckNullPath(const Bytes& image, uint64_t window) {
    const uint64_t site = window + kSiteInWindow;
    const uint64_t position = window + kPositionInWindow, state_change = window + kStateChangeInWindow;
    alignas(16) static uint8_t mast[0x4000];
    uint64_t reads = 0;
    const auto guest = [&](uint64_t address) {
        ++reads;
        return *reinterpret_cast<const uint8_t*>(address);
    };

    const Step original_null = RunCompare(image, site, 0, [&](uint64_t) { return uint8_t(0); });
    CHECK(original_null.address == 0x38c1);

    GuestRegs regs{};
    regs.rax = 0;
    regs.r15 = 0x1234;
    kraken_fix_set_observer(Observer);
    g_observed_count = 0;
    kraken_fix_null_mast(&regs);
    CHECK(regs.rax != 0 && g_observed_count == 1 && g_observed == 0x1234 && regs.skip == 0);
    const Step guarded = RunCompare(image, site, regs.rax, guest);
    CHECK(guarded.address != 0x38c1 && reads == 1 && guarded.zero && guarded.next == window + kPositionInWindow);
    CHECK(guarded.next == position && Matches(image, guarded.next, kPosition));
    const Step again = RunCompare(image, site, regs.rax, guest);
    CHECK(again.next == position);

    std::memset(mast, 0, sizeof(mast));
    GuestRegs intact{};
    intact.rax = reinterpret_cast<uint64_t>(mast);
    intact.r15 = 0x1234;
    g_observed_count = 0;
    kraken_fix_null_mast(&intact);
    CHECK(intact.rax == reinterpret_cast<uint64_t>(mast) && g_observed_count == 0);
    CHECK(RunCompare(image, site, intact.rax, guest).next == position);
    mast[KRAKEN_FIX_FLAG_OFFSET] = 1;
    const Step broken = RunCompare(image, site, intact.rax, guest);
    CHECK(!broken.zero && broken.next == state_change && Matches(image, broken.next, kStateChange));
    mast[KRAKEN_FIX_FLAG_OFFSET] = 0xff;
    CHECK(RunCompare(image, site, intact.rax, guest).next == state_change);

    kraken_fix_set_observer(nullptr);
    GuestRegs quiet{};
    kraken_fix_null_mast(&quiet);
    CHECK(quiet.rax != 0);
}

void CheckRefusals(const Bytes& reference_window) {
    const uint64_t at = 0x1234;
    const Bytes image = Synthetic(at, reference_window);
    uint64_t site = 0;
    CHECK(kraken_fix_locate(image.data(), image.size(), "CUSA10209-1.02", &site) == 1 && site == at + kSiteInWindow);
    CHECK(kraken_fix_locate(image.data(), image.size(), "CUSA10487-1.03", &site) == 1 && site == at + kSiteInWindow);
    {
        Patched patched = Prepare(image);
        CHECK(Install(patched, "CUSA10487-1.03") == 1 && patched.memory[at + kSiteInWindow] == 0xe9);
    }

    for (uint64_t byte = 0; byte < kWindowBytes; ++byte) {
        Bytes mutated = reference_window;
        mutated[byte] ^= 0x01;
        const Bytes broken = Synthetic(at, mutated);
        Patched patched = Prepare(broken);
        CHECK(kraken_fix_locate(broken.data(), broken.size(), "CUSA10209-1.02", &site) == 1 && site == 0);
        CHECK(Install(patched, "CUSA10209-1.02") == 0 && Untouched(patched, broken));
    }

    Bytes twice = image;
    std::copy(reference_window.begin(), reference_window.end(), twice.begin() + 0x8000);
    Patched duplicate = Prepare(twice);
    CHECK(Install(duplicate, "CUSA10209-1.02") == 0 && Untouched(duplicate, twice));

    Patched empty = Prepare(Bytes(0x20000, 0x90));
    CHECK(Install(empty, "CUSA10209-1.02") == 0);

    // A listed build must have the update where its table says, not just once somewhere.
    Patched moved = Prepare(image);
    CHECK(Install(moved, "CUSA10454-1.00") == 0 && Untouched(moved, image));
    CHECK(Install(moved, "CUSA07001-1.00") == 0 && Untouched(moved, image));

    // Anything not one of the supported builds is left alone, whatever the image holds.
    for (const char* key : {static_cast<const char*>(nullptr), "", "CUSA00000-1.00", "CUSA10454-1.0", "CUSA10454-1.00 ", "cusa10454-1.00",
                            "CUSA10209-1.01", "CUSA10487-1.00"}) {
        Patched unknown = Prepare(image);
        CHECK(kraken_fix_locate(image.data(), image.size(), key, &site) == 0 && site == 0);
        CHECK(Install(unknown, key) == 0 && Untouched(unknown, image));
    }

    Patched cramped = Prepare(image);
    CHECK(kraken_fix_install(cramped.memory.data(), cramped.size, 0x54c690, "CUSA10209-1.02", cramped.stubs,
                             cramped.stub_bytes - 1) == 0);
    CHECK(Untouched(cramped, image));
}

// The loader gives the fix only a known build's key, so an unknown executable is never patched.
void CheckLoaderWiring() {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / "src" / "loader.c", std::ios::binary);
    const std::string loader((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(loader.find("kraken_fix_install(image,size,entry,known_build ? build_key : NULL,") != std::string::npos);
    CHECK(loader.find("kraken_fix_stub_area = known_build ? kraken_fix_stub_bytes() : 0") != std::string::npos);
    CHECK(loader.find("kraken_fix_set_observer(runtime_kraken_null_mast)") != std::string::npos);
}

// The fix's hook meets no cheat hook in the build the cheat table describes.
void CheckCheatHooks(const Bytes& image, uint64_t window) {
    const uint64_t site = window + kSiteInWindow;
    const uint64_t function = site - kSiteInWindow - kReferenceFunctionToWindow;
    for (const GuestHook& hook : dcp::cheats::Hooks()) {
        const bool beside = hook.offset + hook.length <= site || hook.offset >= site + KRAKEN_FIX_SITE_BYTES;
        CHECK(beside);
        CHECK(hook.offset + hook.length <= function || hook.offset >= function + kFunctionBytes);
    }
    GuestHook hook = {"kraken tentacle mast", uint32_t(site), KRAKEN_FIX_SITE_BYTES, {0}, kraken_fix_null_mast};
    std::memcpy(hook.original, image.data() + site, KRAKEN_FIX_SITE_BYTES);
    std::vector<GuestHook> all(dcp::cheats::Hooks().begin(), dcp::cheats::Hooks().end());
    all.push_back(hook);
    size_t bad = 0;
    CHECK(guest_hooks_verify(image.data(), image.size(), 0x54c690, &dcp::cheats::kSupportedImage, all.data(), all.size(), &bad) ==
          GUEST_HOOK_OK);
}

} // namespace

int main() {
    Bytes reference;
    int images = 0;
    for (const Build& build : kBuilds) {
        Bytes image;
        const fs::path root = build.research ? fs::path(DCP_SOURCE_DIR) / "build" / "research" / "image"
                                             : fs::path(DCP_SOURCE_DIR) / "build" / "image";
        if (build.folder == nullptr || !ReadBootImage(root / build.folder / "boot.bin", &image)) {
            continue;
        }
        ++images;
        CheckSequence(build.folder, build, image);
        CheckNullPath(image, build.window);
        if (std::string(build.key) == "CUSA10454-1.00") {
            reference = image;
            CheckCheatHooks(image, build.window);
        }
    }
    std::printf("kraken fix: %d local boot images checked\n", images);

    Bytes window;
    if (!reference.empty()) {
        window = Window(reference, kBuilds[0].window);
    } else {
        const Bytes known = {0x49, 0x8b, 0x87, 0xd8, 0x38, 0x00, 0x00, 0x80, 0xb8, 0xc1, 0x38, 0x00, 0x00, 0x00, 0x74, 0x1b,
                             0x49, 0x8b, 0xbf, 0xc0, 0x38, 0x00, 0x00, 0x80, 0x7f, 0x5b, 0x03, 0x74, 0x0e, 0x48, 0x8b, 0x07,
                             0xbe, 0x03, 0x00, 0x00, 0x00, 0xff, 0x90, 0xa8, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x87, 0xc0, 0x38,
                             0x00, 0x00, 0xc7, 0x80, 0x70, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x48, 0x43, 0xc7, 0x80, 0x74, 0x0c,
                             0x00, 0x00, 0x00, 0x00, 0xaf, 0xc3};
        window = known;
    }
    CHECK(window.size() == kWindowBytes);
    const Bytes synthetic = Synthetic(0x1234, window);
    CheckNullPath(synthetic, 0x1234);
    CheckRefusals(window);
    CheckLoaderWiring();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
