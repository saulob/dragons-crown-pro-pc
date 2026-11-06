// OpenGL fast paths without OpenGL: the write watch against real page faults, the state tracker and the stream ring
// against recording entry points. No context, no window, no game files.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Inlined: this test links neither the video core nor libxxhash.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "video_core/renderer_opengl/gl_guest_watch.cpp"
#include "video_core/renderer_opengl/gl_stream_ring.cpp"

namespace gl {
#define DCP_GL_DEFINE_CORE(prototype, name) decltype(&::prototype) name;
#define DCP_GL_DEFINE(type, name) type name;
DCP_GL_CORE_FUNCTIONS(DCP_GL_DEFINE_CORE)
DCP_GL_EXTENSION_FUNCTIONS(DCP_GL_DEFINE)
DCP_GL_OPTIONAL_FUNCTIONS(DCP_GL_DEFINE)
#undef DCP_GL_DEFINE_CORE
#undef DCP_GL_DEFINE
} // namespace gl

namespace {

using OpenGL::GlState;
using OpenGL::GuestWriteWatch;
using OpenGL::RingStorage;
using OpenGL::StreamRing;
namespace Perf = OpenGL::Perf;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

constexpr u64 Page = 4096;
constexpr u64 ArenaBytes = 64 * 1024 * 1024;

GuestWriteWatch* g_watch;
VAddr g_arena_begin;
std::atomic<u64> g_protect_calls{0};

void ProtectHost(VAddr address, u64 size, bool writable) {
    g_protect_calls.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, writable ? PAGE_READWRITE : PAGE_READONLY, &old);
#else
    mprotect(reinterpret_cast<void*>(address), size, writable ? PROT_READ | PROT_WRITE : PROT_READ);
#endif
}

// Ends the run itself: nothing here may reach the system's crash reporting and its dialogs.
[[noreturn]] void DieOnUnhandledFault() {
    std::fputs("FAIL an unhandled page fault ended the run\n", stdout);
    std::fflush(stdout);
    std::_Exit(3);
}

// The same decision as the rasterizer's handler, with the arena standing in for mapped guest memory.
bool HandleFault(VAddr address) {
    using FaultResult = GuestWriteWatch::FaultResult;
    GuestWriteWatch* watch = g_watch;
    if (!watch || address - g_arena_begin >= ArenaBytes) {
        return false;
    }
    if (watch->OnWriteFault(address) == FaultResult::Released) {
        OpenGL::ResetFaultRetries();
        return true;
    }
    return OpenGL::RetryFault(address);
}

#ifdef _WIN32
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->ExceptionInformation[0] == 1 &&
        HandleFault(VAddr(record->ExceptionInformation[1]))) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DieOnUnhandledFault();
}

void InstallFaultHandler() {
    AddVectoredExceptionHandler(1, OnException);
}
#else
void OnSignal(int, siginfo_t* info, void*) {
    if (!HandleFault(reinterpret_cast<VAddr>(info->si_addr))) {
        DieOnUnhandledFault();
    }
}

void InstallFaultHandler() {
    struct sigaction action{};
    action.sa_sigaction = OnSignal;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
}
#endif

// Inside the 40 bits the watch covers, which a default allocation on 64-bit Windows is not.
u8* AllocateArena() {
    for (u64 hint = 0x20'0000'0000ull; hint < 0x80'0000'0000ull; hint += 0x1'0000'0000ull) {
#ifdef _WIN32
        void* memory = VirtualAlloc(reinterpret_cast<void*>(hint), ArenaBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void* memory = mmap(reinterpret_cast<void*>(hint), ArenaBytes, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            memory = nullptr;
        }
#endif
        if (memory && VAddr(memory) + ArenaBytes < GuestWriteWatch::AddressLimit) {
            return static_cast<u8*>(memory);
        }
    }
    return nullptr;
}

void Write(VAddr address, u8 value) {
    *reinterpret_cast<volatile u8*>(address) = value;
}

// Past the block one fault releases, and past the cooldown any test leaves behind.
constexpr u64 Far = 64 * Page;
constexpr u32 LongestCooldownFrames = 1024;

struct ScopedWatch {
    explicit ScopedWatch(u8* arena, u32 cooldown = GuestWriteWatch::DefaultCooldownFrames)
        : base{VAddr(arena)}, watch{ProtectHost, cooldown} {
        ProtectHost(base, ArenaBytes, true);
        g_watch = &watch;
    }
    ~ScopedWatch() {
        g_watch = nullptr;
    }
    VAddr base;
    GuestWriteWatch watch;
};

std::optional<u64> ArmWaiting(GuestWriteWatch& watch, VAddr address, u64 size) {
    for (u32 frame = 0; frame <= 2 * LongestCooldownFrames; ++frame) {
        if (const auto stamp = watch.TryArm(address, size)) {
            return stamp;
        }
        watch.AdvanceFrame();
    }
    return std::nullopt;
}

void TestWatchBasics(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr image_a = scoped.base;
    const VAddr image_b = scoped.base + Far;

    u64 stamp_a = *watch.TryArm(image_a, 8 * Page);
    const u64 stamp_b = *watch.TryArm(image_b, 8 * Page);
    CHECK(!watch.WrittenSince(image_a, 8 * Page, stamp_a));
    CHECK(watch.Faults() == 0);

    // Reads never fault: an unchanged image costs nothing.
    CHECK(arena[0] == 0);
    CHECK(watch.Faults() == 0);

    Write(image_a + 3 * Page, 7);
    CHECK(reinterpret_cast<u8*>(image_a)[3 * Page] == 7);
    CHECK(watch.Faults() == 1);
    CHECK(watch.WrittenSince(image_a, 8 * Page, stamp_a));
    // The other image's epoch check fails, but its pages say nothing changed.
    CHECK(!watch.WrittenSince(image_b, 8 * Page, stamp_b));

    // One fault releases the whole block: a sequential writer does not fault per page.
    for (u64 page = 0; page < 8; ++page) {
        Write(image_a + page * Page, 1);
    }
    CHECK(watch.Faults() == 1);

    // Addresses nobody armed, and a range bigger than a leaf.
    using FaultResult = GuestWriteWatch::FaultResult;
    CHECK(watch.OnWriteFault(scoped.base + 40 * 1024 * 1024) == FaultResult::NotOurs);
    CHECK(watch.OnWriteFault(GuestWriteWatch::AddressLimit + Page) == FaultResult::NotOurs);
    CHECK(!watch.TryArm(GuestWriteWatch::AddressLimit, Page));
    CHECK(!watch.TryArm(scoped.base, 0));
    watch.Forget(scoped.base, 512ull * 1024 * 1024 * 1024);
    watch.BeforeHostWrite(GuestWriteWatch::AddressLimit + Page, Page);
}

void TestCooldown(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr image = scoped.base;

    const u64 stamp = *watch.TryArm(image, 8 * Page);
    Write(image, 1);
    CHECK(watch.WrittenSince(image, 8 * Page, stamp));

    // A fresh write keeps the range out of the watch; a read into it can still be in the kernel.
    for (u32 frame = 0; frame + 1 < GuestWriteWatch::DefaultCooldownFrames; ++frame) {
        CHECK(!watch.TryArm(image, 8 * Page));
        watch.AdvanceFrame();
    }
    const u64 protects = g_protect_calls.load();
    CHECK(!watch.TryArm(image, 8 * Page));
    CHECK(g_protect_calls.load() == protects);
    watch.AdvanceFrame();
    const auto armed = watch.TryArm(image, 8 * Page);
    CHECK(armed.has_value());
    CHECK(!watch.WrittenSince(image, 8 * Page, *armed));

    // Quiet from here on: a static image costs neither faults nor protection calls.
    const u64 faults = watch.Faults();
    const u64 protects_after = g_protect_calls.load();
    for (u32 frame = 0; frame < 5000; ++frame) {
        watch.AdvanceFrame();
        CHECK(!watch.WrittenSince(image, 8 * Page, *armed));
    }
    CHECK(watch.Faults() == faults);
    CHECK(g_protect_calls.load() == protects_after);
}

void TestOverlappingRanges(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr first = scoped.base;
    const VAddr second = scoped.base + 16 * Page;
    constexpr u64 Size = 32 * Page;

    u64 stamp_first = *watch.TryArm(first, Size);
    u64 stamp_second = *watch.TryArm(second, Size);

    // Only the second image's tail.
    Write(second + 28 * Page, 1);
    CHECK(!watch.WrittenSince(first, Size, stamp_first));
    CHECK(watch.WrittenSince(second, Size, stamp_second));

    // Both images own this page.
    Write(second + 2 * Page, 2);
    CHECK(watch.WrittenSince(first, Size, stamp_first));

    // Two images inside one page, whichever armed it first.
    const VAddr shared = scoped.base + Far;
    stamp_first = *watch.TryArm(shared, Page / 2);
    stamp_second = *watch.TryArm(shared + Page / 2, Page / 2);
    Write(shared, 3);
    CHECK(watch.WrittenSince(shared, Page / 2, stamp_first));
    CHECK(watch.WrittenSince(shared + Page / 2, Page / 2, stamp_second));

    // Re-arming one image must not hide the other image's pending change.
    const VAddr wide = scoped.base + 2 * Far;
    const u64 wide_stamp = *watch.TryArm(wide, Size);
    Write(wide + 20 * Page, 4);
    const auto inner = ArmWaiting(watch, wide + 16 * Page, 8 * Page);
    CHECK(inner.has_value());
    CHECK(watch.WrittenSince(wide, Size, wide_stamp));
}

// A page written every frame stops being protected almost at once, and hashing covers it.
void TestHotPage(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr hot = scoped.base;
    constexpr u32 Frames = 6000;

    u64 stamp = 0;
    bool armed = false;
    u32 hashed_frames = 0;
    for (u32 frame = 0; frame < Frames; ++frame) {
        watch.AdvanceFrame();
        if (!armed || watch.WrittenSince(hot, Page, stamp)) {
            ++hashed_frames;
            const auto arm = watch.TryArm(hot, Page);
            armed = arm.has_value();
            stamp = arm.value_or(0);
        }
        Write(hot, u8(frame));
    }
    // 16, 32 ... 1024 frames, then one fault every 1024.
    CHECK(watch.Faults() <= 12);
    CHECK(hashed_frames > Frames - 12);

    // The writer stops: the page goes back to the watch, and stays quiet.
    const auto calmed = ArmWaiting(watch, hot, Page);
    CHECK(calmed.has_value());
    const u64 faults = watch.Faults();
    for (u32 frame = 0; frame < 3000; ++frame) {
        watch.AdvanceFrame();
        CHECK(!watch.WrittenSince(hot, Page, *calmed));
    }
    CHECK(watch.Faults() == faults);

    // Long quiet forgives the past: the next write starts from the base cooldown.
    Write(hot, 1);
    for (u32 frame = 0; frame < GuestWriteWatch::DefaultCooldownFrames; ++frame) {
        watch.AdvanceFrame();
    }
    CHECK(watch.TryArm(hot, Page).has_value());
}

// A static texture next to a hot page leaves the watch only for the base cooldown.
void TestColdNeighborOfHotPage(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr hot = scoped.base;
    const VAddr neighbor = scoped.base + 8 * Page;

    u64 hot_stamp = *watch.TryArm(hot, Page);
    const u64 neighbor_stamp = *watch.TryArm(neighbor, 4 * Page);
    for (u32 round = 0; round < 3; ++round) {
        Write(hot, 1);
        hot_stamp = *ArmWaiting(watch, hot, Page);
    }
    Write(hot, 2);
    CHECK(watch.WrittenSince(neighbor, 4 * Page, neighbor_stamp));
    CHECK(!watch.TryArm(hot, Page));
    const auto neighbor_armed = ArmWaiting(watch, neighbor, 4 * Page);
    CHECK(neighbor_armed.has_value());
    CHECK(!watch.TryArm(hot, Page));
    CHECK(!watch.WrittenSince(neighbor, 4 * Page, *neighbor_armed));
    (void)hot_stamp;
}

// The fill behind the storm: 15 MiB of padded 2560x1440, rewritten by the GPU thread every frame.
void TestHostWritesNeverFault(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    constexpr u64 Size = 240 * 64 * 1024;
    const VAddr surface = scoped.base;

    u64 stamp = *watch.TryArm(surface, Size);
    bool armed = true;
    const u64 protects = g_protect_calls.load();
    for (u32 frame = 0; frame < 120; ++frame) {
        watch.AdvanceFrame();
        if (armed) {
            CHECK(!watch.WrittenSince(surface, Size, stamp));
        }
        watch.BeforeHostWrite(surface, Size);
        std::memset(reinterpret_cast<void*>(surface), int(frame), Size);
        if (armed) {
            CHECK(watch.WrittenSince(surface, Size, stamp));
        }
        const auto arm = watch.TryArm(surface, Size);
        armed = arm.has_value();
        stamp = arm.value_or(0);
    }
    CHECK(watch.Faults() == 0);
    CHECK(g_protect_calls.load() - protects <= 4);

    // A write the watch was not told of, overlapping one it was, is still seen.
    const VAddr other = scoped.base + 32 * 1024 * 1024;
    const u64 other_stamp = *watch.TryArm(other, Page);
    watch.BeforeHostWrite(other - Page, 3 * Page);
    CHECK(watch.WrittenSince(other, Page, other_stamp));
    CHECK(watch.Faults() == 0);
}

void TestProtectionLostWithoutFault(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    const VAddr reprotected = scoped.base;
    u64 stamp = *watch.TryArm(reprotected, Page);
    ProtectHost(reprotected, Page, true);
    watch.NoteWritten(reprotected, Page);
    CHECK(watch.WrittenSince(reprotected, Page, stamp));
    stamp = *ArmWaiting(watch, reprotected, Page);
    const u64 before = watch.Faults();
    Write(reprotected, 4);
    CHECK(watch.Faults() == before + 1);
    CHECK(watch.WrittenSince(reprotected, Page, stamp));

    // A page the states call armed but a remap left writable: arming protects it all the same.
    const VAddr remapped = scoped.base + Far;
    const u64 first = *watch.TryArm(remapped, Page);
    ProtectHost(remapped, Page, true);
    const u64 second = *watch.TryArm(remapped, Page);
    Write(remapped, 5);
    CHECK(watch.Faults() == before + 2);
    CHECK(watch.WrittenSince(remapped, Page, first));
    CHECK(watch.WrittenSince(remapped, Page, second));
}

void TestUnmapWhileWatched(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    using FaultResult = GuestWriteWatch::FaultResult;
    const VAddr mapped = scoped.base;

    const u64 stamp = *watch.TryArm(mapped, 4 * Page);
    watch.Forget(mapped, 4 * Page);
    CHECK(watch.WrittenSince(mapped, 4 * Page, stamp));
    CHECK(watch.OnWriteFault(mapped) == FaultResult::NotOurs);

    // The hook can run before the unmap: the page must be left writable, not read-only and unknown.
    const u64 faults = watch.Faults();
    Write(mapped, 1);
    CHECK(watch.Faults() == faults);
    CHECK(reinterpret_cast<u8*>(mapped)[0] == 1);

    // Nor may an image still in the cache arm memory that is on its way out.
    CHECK(!watch.TryArm(mapped, 4 * Page));

    // Writers keep hitting the range while it is armed and forgotten over and over.
    std::atomic<bool> stop{false};
    std::thread writer{[&] {
        u32 value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            Write(scoped.base + Far + (value++ % 8) * Page, 1);
        }
    }};
    for (u32 round = 0; round < 2000; ++round) {
        watch.AdvanceFrame();
        (void)watch.TryArm(scoped.base + Far, 8 * Page);
        if (round % 3 == 0) {
            watch.Forget(scoped.base + Far, 8 * Page);
            for (u32 frame = 0; frame < 70; ++frame) {
                watch.AdvanceFrame();
            }
        }
    }
    stop = true;
    writer.join();
}

void TestDestroyWhileRangesExist(u8* arena) {
    const VAddr base = VAddr(arena);
    {
        ScopedWatch scoped{arena};
        (void)scoped.watch.TryArm(base, 32 * Page);
        (void)scoped.watch.TryArm(base + Far, 32 * Page);
    }
    // No watch and no handler now: a page left protected would end this process.
    CHECK(g_watch == nullptr);
    Write(base + 5 * Page, 1);
    Write(base + Far + 20 * Page, 1);
}

// Shutdown with guest threads still running: pages go back, and a late fault stays harmless.
void TestShutdownOrder(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    using FaultResult = GuestWriteWatch::FaultResult;
    const VAddr range = scoped.base;
    std::atomic<bool> stop{false};
    (void)watch.TryArm(range, 64 * Page);
    std::thread writer{[&] {
        u32 value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            Write(range + (value++ % 64) * Page, 1);
        }
    }};
    std::this_thread::yield();
    watch.ReleaseAll();
    stop = true;
    writer.join();
    // A fault that waited for ReleaseAll may still have retried; none can happen from here on.
    const u64 faults = watch.Faults();
    for (u64 page = 0; page < 64; ++page) {
        Write(range + page * Page, 2);
    }
    CHECK(watch.Faults() == faults);
    CHECK(watch.OnWriteFault(range) == FaultResult::Retry);
    CHECK(watch.TakeStats().armed_pages_now == 0);
}

void TestFaultsCannotSpinForever() {
    using OpenGL::ResetFaultRetries;
    using OpenGL::RetryFault;
    const VAddr address = 0x1000000;
    ResetFaultRetries();
    int allowed = 0;
    while (allowed < 100000 && RetryFault(address)) {
        ++allowed;
    }
    CHECK(allowed > 0);
    CHECK(allowed < 100000);
    CHECK(!RetryFault(address));
    // Progress anywhere else, or a release, starts the count again.
    CHECK(RetryFault(address + 2 * Page));
    CHECK(RetryFault(address));
    ResetFaultRetries();
    CHECK(RetryFault(address));
    ResetFaultRetries();
}

// A write either reaches what the next hash reads, or is reported. Arms race the faults on purpose: a fault that
// released a page after an arm had protected it would lose every write that follows.
void TestWatchLosesNoWrites(u8* arena) {
    ScopedWatch scoped{arena, 0};
    GuestWriteWatch& watch = scoped.watch;
    constexpr u64 Pages = 256;
    constexpr u32 Writers = 3;
    constexpr int Rounds = 400;
    const VAddr base = VAddr(arena) + 32 * 1024 * 1024;
    const auto sum_of = [](VAddr address, u64 size) {
        u64 total = 0;
        for (u64 offset = 0; offset < size; offset += Page) {
            total += *reinterpret_cast<const volatile u32*>(address + offset);
        }
        return total;
    };
    std::atomic<int> round{-1};
    std::atomic<u32> finished{0};
    std::vector<std::thread> writers;
    for (u32 id = 0; id < Writers; ++id) {
        writers.emplace_back([&, id] {
            std::mt19937 random{id + 1};
            for (int expected = 0; expected < Rounds; ++expected) {
                while (round.load(std::memory_order_acquire) < expected) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 16; ++i) {
                    // Sparse enough that some rounds leave pages armed.
                    if (random() % 4 == 0) {
                        std::atomic_ref<u32>(*reinterpret_cast<u32*>(base + (random() % Pages) * Page))
                            .fetch_add(1, std::memory_order_relaxed);
                    }
                }
                finished.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    struct RacingArm {
        VAddr address;
        u64 size;
        u64 stamp;
        u64 sum;
    };
    std::vector<RacingArm> arms;
    std::mt19937 random{99};
    int unseen = 0;
    const auto verify = [&](VAddr address, u64 size, u64 stamp, u64 expected_sum) {
        if (sum_of(address, size) != expected_sum && !watch.WrittenSince(address, size, stamp)) {
            ++unseen;
        }
    };
    for (int current = 0; current < Rounds; ++current) {
        finished.store(0, std::memory_order_release);
        arms.clear();
        const u64 stamp = *watch.TryArm(base, Pages * Page);
        const u64 expected_sum = sum_of(base, Pages * Page);
        round.store(current, std::memory_order_release);
        while (finished.load(std::memory_order_acquire) != Writers) {
            const u64 first = random() % Pages;
            const u64 size = std::min<u64>(8, Pages - first) * Page;
            const VAddr address = base + first * Page;
            const u64 arm_stamp = *watch.TryArm(address, size);
            arms.push_back({address, size, arm_stamp, sum_of(address, size)});
        }
        verify(base, Pages * Page, stamp, expected_sum);
        for (const RacingArm& arm : arms) {
            verify(arm.address, arm.size, arm.stamp, arm.sum);
        }
    }
    for (auto& writer : writers) {
        writer.join();
    }
    CHECK(unseen == 0);
}

// The texture cache's decision end to end, against writers and with the real cooldowns: when the writers stop, the
// next check of every image must see the bytes in memory.
void TestNoStaleImages(u8* arena) {
    ScopedWatch scoped{arena};
    GuestWriteWatch& watch = scoped.watch;
    struct Model {
        VAddr address;
        u64 size;
        bool watched;
        u64 stamp;
        u64 hash;
    };
    std::vector<Model> images;
    for (u32 i = 0; i < 24; ++i) {
        // Packed like a texture pool: neighbors share pages and blocks, some overlap.
        images.push_back({scoped.base + i * 3 * Page + (i % 3) * 512, 5 * Page, false, 0, 0});
    }
    const VAddr pool = scoped.base;
    constexpr u64 PoolBytes = 80 * Page;
    const auto refresh = [&](Model& image) {
        if (image.watched && !watch.WrittenSince(image.address, image.size, image.stamp)) {
            return false;
        }
        const auto stamp = watch.TryArm(image.address, image.size);
        image.watched = stamp.has_value();
        image.stamp = stamp.value_or(0);
        image.hash = XXH3_64bits(reinterpret_cast<const void*>(image.address), image.size);
        return true;
    };
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (u32 id = 0; id < 3; ++id) {
        writers.emplace_back([&, id] {
            std::mt19937 random{id + 7};
            while (!stop.load(std::memory_order_relaxed)) {
                std::atomic_ref<u8>(*reinterpret_cast<u8*>(pool + random() % PoolBytes))
                    .fetch_add(1, std::memory_order_relaxed);
                if (random() % 64 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (u32 frame = 0; frame < 1500; ++frame) {
        watch.AdvanceFrame();
        for (Model& image : images) {
            refresh(image);
        }
    }
    stop = true;
    for (auto& writer : writers) {
        writer.join();
    }
    int stale = 0;
    for (Model& image : images) {
        refresh(image);
        stale += image.hash != XXH3_64bits(reinterpret_cast<const void*>(image.address), image.size) ? 1 : 0;
    }
    CHECK(stale == 0);
    // The writers never stopped, so the images must have left the watch rather than fault without end.
    const u64 per_frame_limit = 24 * 3;
    CHECK(watch.Faults() < 1500 * per_frame_limit);
}

std::map<std::string, int> g_calls;

template <typename... Args>
void Called(const char* name, Args...) {
    ++g_calls[name];
}

int Calls(const char* name) {
    return g_calls[name];
}

void InstallStateFakes() {
    gl::Enable = [](GLenum) { Called("Enable"); };
    gl::Disable = [](GLenum) { Called("Disable"); };
    gl::UseProgram = [](GLuint) { Called("UseProgram"); };
    gl::BindFramebuffer = [](GLenum, GLuint) { Called("BindFramebuffer"); };
    gl::BindVertexArray = [](GLuint) { Called("BindVertexArray"); };
    gl::BindBuffer = [](GLenum, GLuint) { Called("BindBuffer"); };
    gl::ActiveTexture = [](GLenum) { Called("ActiveTexture"); };
    gl::BindTexture = [](GLenum, GLuint) { Called("BindTexture"); };
    gl::BindSampler = [](GLuint, GLuint) { Called("BindSampler"); };
    gl::Viewport = [](GLint, GLint, GLsizei, GLsizei) { Called("Viewport"); };
    gl::Scissor = [](GLint, GLint, GLsizei, GLsizei) { Called("Scissor"); };
    gl::DepthRange = [](GLdouble, GLdouble) { Called("DepthRange"); };
    gl::Enablei = [](GLenum, GLuint) { Called("Enablei"); };
    gl::Disablei = [](GLenum, GLuint) { Called("Disablei"); };
    gl::ColorMaski = [](GLuint, GLboolean, GLboolean, GLboolean, GLboolean) { Called("ColorMaski"); };
    gl::BlendFuncSeparate = [](GLenum, GLenum, GLenum, GLenum) { Called("BlendFuncSeparate"); };
    gl::BlendFuncSeparatei = [](GLuint, GLenum, GLenum, GLenum, GLenum) { Called("BlendFuncSeparatei"); };
    gl::VertexAttribPointer = [](GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {
        Called("VertexAttribPointer");
    };
    gl::VertexAttribIPointer = [](GLuint, GLint, GLenum, GLsizei, const void*) { Called("VertexAttribIPointer"); };
    gl::VertexAttribDivisor = [](GLuint, GLuint) { Called("VertexAttribDivisor"); };
    gl::StencilMaskSeparate = [](GLenum, GLuint) { Called("StencilMaskSeparate"); };
}

void TestState() {
    g_calls.clear();
    InstallStateFakes();
    GlState state;
    using Cap = GlState::Cap;

    // The first call always reaches the driver: the context's real state is unknown.
    state.Enable(Cap::DepthTest, false);
    state.Enable(Cap::DepthTest, false);
    CHECK(Calls("Disable") == 1);
    state.Enable(Cap::DepthTest, true);
    CHECK(Calls("Enable") == 1);
    state.Enable(Cap::ScissorTest, true);
    CHECK(Calls("Enable") == 2);

    state.UseProgram(5);
    state.UseProgram(5);
    state.UseProgram(6);
    CHECK(Calls("UseProgram") == 2);

    state.BindFramebuffer(3);
    state.BindFramebuffer(3);
    CHECK(Calls("BindFramebuffer") == 1);
    state.BindDrawFramebuffer(3);
    CHECK(Calls("BindFramebuffer") == 1);
    state.BindReadFramebuffer(4);
    CHECK(Calls("BindFramebuffer") == 2);
    state.BindFramebuffer(3);
    CHECK(Calls("BindFramebuffer") == 3);

    // A name deleted can come back as another object.
    state.ForgetFramebuffers();
    state.BindFramebuffer(3);
    CHECK(Calls("BindFramebuffer") == 4);

    // Bindings are per unit and per target.
    state.BindTexture(1, GL_TEXTURE_2D, 10);
    state.BindTexture(1, GL_TEXTURE_2D, 10);
    CHECK(Calls("BindTexture") == 1);
    state.BindTexture(2, GL_TEXTURE_2D, 10);
    CHECK(Calls("BindTexture") == 2);
    state.BindTexture(1, GL_TEXTURE_2D_ARRAY, 10);
    CHECK(Calls("BindTexture") == 3);
    state.BindTexture(1, GL_TEXTURE_2D, 10);
    CHECK(Calls("BindTexture") == 3);
    state.ForgetTextures();
    state.BindTexture(1, GL_TEXTURE_2D, 10);
    CHECK(Calls("BindTexture") == 4);
    // A filtered bind must not leave the edit behind it on the wrong unit.
    state.BindForEdit(GL_TEXTURE_2D, 20);
    const int activations = Calls("ActiveTexture");
    state.ActiveTexture(1);
    state.BindForEdit(GL_TEXTURE_2D, 20);
    CHECK(Calls("ActiveTexture") == activations + 2);

    // The element buffer binding is the vertex array's.
    state.BindVertexArray(1);
    state.BindElementBuffer(9);
    state.BindElementBuffer(9);
    CHECK(Calls("BindBuffer") == 1);
    state.BindVertexArray(2);
    state.BindVertexArray(1);
    state.BindElementBuffer(9);
    CHECK(Calls("BindBuffer") == 2);

    state.Viewport(0, 0, 100, 100);
    state.Viewport(0, 0, 100, 100);
    state.Viewport(0, 0, 100, 101);
    CHECK(Calls("Viewport") == 2);
    state.DepthRange(0.0, 1.0);
    state.DepthRange(0.0, 1.0);
    CHECK(Calls("DepthRange") == 1);

    state.BlendEnable(1, true);
    state.BlendEnable(1, true);
    state.BlendEnable(2, true);
    CHECK(Calls("Enablei") == 2);
    state.ColorMask(0, 0xF);
    state.ColorMask(0, 0xF);
    state.ColorMask(0, 0x7);
    CHECK(Calls("ColorMaski") == 2);

    // Without per-target blending there is one state, whichever target asks.
    state.BlendFunc(0, false, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    state.BlendFunc(1, false, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    CHECK(Calls("BlendFuncSeparate") == 1);
    state.BlendFunc(0, true, GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    state.BlendFunc(1, true, GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    CHECK(Calls("BlendFuncSeparatei") == 2);

    state.StencilMask(GL_FRONT, 0xFF);
    state.StencilMask(GL_BACK, 0xFF);
    state.StencilMask(GL_BACK, 0xFF);
    CHECK(Calls("StencilMaskSeparate") == 2);

    state.VertexAttrib(0, false, 3, GL_FLOAT, false, 16, 64);
    state.VertexAttrib(0, false, 3, GL_FLOAT, false, 16, 64);
    CHECK(Calls("VertexAttribPointer") == 1);
    state.VertexAttrib(0, false, 3, GL_FLOAT, false, 16, 128);
    CHECK(Calls("VertexAttribPointer") == 2);
    state.VertexAttrib(0, true, 3, GL_INT, false, 16, 128);
    CHECK(Calls("VertexAttribIPointer") == 1);
    state.VertexDivisor(0, 1);
    state.VertexDivisor(0, 1);
    CHECK(Calls("VertexAttribDivisor") == 1);

    // Someone else used the context: everything is sent again, except what lives in our vertex array.
    state.Invalidate();
    state.Enable(Cap::DepthTest, true);
    CHECK(Calls("Enable") == 3);
    state.UseProgram(6);
    CHECK(Calls("UseProgram") == 3);
    state.Viewport(0, 0, 100, 101);
    CHECK(Calls("Viewport") == 3);
    state.BindSampler(0, 0);
    state.BindSampler(0, 0);
    CHECK(Calls("BindSampler") == 1);
    state.VertexAttrib(0, true, 3, GL_INT, false, 16, 128);
    CHECK(Calls("VertexAttribIPointer") == 1);
}

constexpr u32 FakeRingBytes = 16 * 1024 * 1024;
constexpr u32 FakeSegmentBytes = FakeRingBytes / StreamRing::SegmentCount;

std::vector<u8> g_buffer_memory;
std::vector<std::uintptr_t> g_waits;
std::uintptr_t g_next_fence = 0;
bool g_persistent_map_fails = false;

void InstallRingFakes() {
    InstallStateFakes();
    g_buffer_memory.assign(FakeRingBytes, 0);
    g_waits.clear();
    g_next_fence = 0;
    gl::GetIntegerv = [](GLenum, GLint* value) { *value = FakeRingBytes / 4; };
    gl::GenBuffers = [](GLsizei, GLuint* name) { *name = 1; };
    gl::GenTextures = [](GLsizei, GLuint* name) { *name = 2; };
    gl::DeleteBuffers = [](GLsizei, const GLuint*) { Called("DeleteBuffers"); };
    gl::DeleteTextures = [](GLsizei, const GLuint*) {};
    gl::TexBuffer = [](GLenum, GLenum, GLuint) {};
    gl::BufferStorage = [](GLenum, GLsizeiptr, const void*, GLbitfield) { Called("BufferStorage"); };
    gl::BufferData = [](GLenum, GLsizeiptr, const void*, GLenum) { Called("BufferData"); };
    gl::MapBufferRange = [](GLenum, GLintptr offset, GLsizeiptr, GLbitfield flags) -> void* {
        Called("MapBufferRange");
        if ((flags & GL_MAP_PERSISTENT_BIT) != 0 && g_persistent_map_fails) {
            return nullptr;
        }
        return g_buffer_memory.data() + offset;
    };
    gl::UnmapBuffer = [](GLenum) -> GLboolean {
        Called("UnmapBuffer");
        return GL_TRUE;
    };
    gl::FenceSync = [](GLenum, GLbitfield) { return reinterpret_cast<GLsync>(++g_next_fence); };
    gl::DeleteSync = [](GLsync) { Called("DeleteSync"); };
    gl::ClientWaitSync = [](GLsync fence, GLbitfield, GLuint64) -> GLenum {
        g_waits.push_back(reinterpret_cast<std::uintptr_t>(fence));
        return GL_ALREADY_SIGNALED;
    };
}

struct RecordedUpload {
    GLint x, y;
    GLsizei width, height;
    u32 first_value;
};
std::vector<RecordedUpload> g_uploads;

void InstallGlesFakes() {
    InstallRingFakes();
    g_uploads.clear();
    gl::DepthRangef = [](GLfloat, GLfloat) { Called("DepthRangef"); };
    gl::TexStorage2D = [](GLenum, GLsizei, GLenum, GLsizei, GLsizei) { Called("TexStorage2D"); };
    gl::TexParameteri = [](GLenum, GLenum, GLint) {};
    gl::TexSubImage2D = [](GLenum, GLint, GLint x, GLint y, GLsizei width, GLsizei height, GLenum, GLenum,
                           const void* pixels) {
        u32 first = 0;
        std::memcpy(&first, pixels, sizeof(first));
        g_uploads.push_back({x, y, width, height, first});
    };
    gl::PolygonMode = nullptr;
    gl::ProvokingVertex = nullptr;
}

void TestGlesState() {
    g_calls.clear();
    InstallGlesFakes();
    GlState state{true, false};
    using Cap = GlState::Cap;
    for (const Cap cap : {Cap::ColorLogicOp, Cap::FramebufferSrgb, Cap::ProgramPointSize, Cap::DepthClamp}) {
        state.Enable(cap, true);
        state.Enable(cap, false);
    }
    CHECK(Calls("Enable") == 0 && Calls("Disable") == 0);
    state.Enable(Cap::PrimitiveRestart, true);
    CHECK(Calls("Enable") == 1);
    GlState with_clamp{true, true};
    with_clamp.Enable(Cap::DepthClamp, true);
    CHECK(Calls("Enable") == 2);

    state.DepthRange(0.0, 1.0);
    CHECK(Calls("DepthRangef") == 1 && Calls("DepthRange") == 0);
    state.PolygonMode(GL_FILL);
    state.ProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    state.PrimitiveRestartIndex(0xFFFF);
    state.LogicOp(GL_COPY);

    // The upload unit must exist: OpenGL ES guarantees 32, not 48.
    GLenum last_unit = 0;
    static GLenum* unit_seen;
    unit_seen = &last_unit;
    gl::ActiveTexture = [](GLenum unit) { *unit_seen = unit; };
    state.BindForEdit(GL_TEXTURE_2D, 9);
    CHECK(last_unit == GL_TEXTURE0 + 31);
    GlState desktop;
    desktop.BindForEdit(GL_TEXTURE_2D, 9);
    CHECK(last_unit == GL_TEXTURE0 + 47);
}

void TestRingReserve() {
    g_calls.clear();
    InstallGlesFakes();
    GlState state{true, true};
    StreamRing texture_ring{state, RingStorage::ShaderTexture};
    u32 offset = 0;
    u8* out = texture_ring.Reserve(16, 4, offset);
    CHECK(out != nullptr && offset == 0);
    const std::array<u32, 4> words{21, 22, 23, 24};
    std::memcpy(out, words.data(), sizeof(words));
    texture_ring.Commit();
    CHECK(g_uploads.size() == 1 && g_uploads[0].width == 4 && g_uploads[0].first_value == 21);

    out = texture_ring.Reserve(texture_ring.Capacity(), 4, offset);
    CHECK(out == nullptr && offset == StreamRing::NoOffset);
    out = texture_ring.Reserve(4, 4, offset);
    CHECK(out != nullptr && offset == 16);

    GlState desktop;
    StreamRing buffer_ring{desktop, true};
    CHECK(buffer_ring.Reserve(4, 4, offset) == nullptr);
}

void TestClientRing() {
    g_calls.clear();
    InstallGlesFakes();
    GlState state{true, true};
    StreamRing ring{state, RingStorage::ClientMemory};
    CHECK(ring.Buffer() == 0 && ring.Texture() == 0);
    CHECK(ring.PointerBase() != 0);
    ring.BeginDraw();
    const u32 data = 0xAABBCCDD;
    const u32 offset = ring.Push(&data, sizeof(data), 16);
    CHECK(offset != StreamRing::NoOffset);
    ring.Commit();
    CHECK(std::memcmp(reinterpret_cast<const void*>(uintptr_t(ring.PointerBase() + offset)), &data, sizeof(data)) == 0);
    CHECK(Calls("BufferStorage") == 0 && Calls("BufferData") == 0 && Calls("MapBufferRange") == 0);
}

void TestShaderTextureRing() {
    g_calls.clear();
    InstallGlesFakes();
    GlState state{true, true};
    constexpr GLsizei row = 1 << Shader::Backend::GLSL::RingRowBits;
    StreamRing ring{state, RingStorage::ShaderTexture};
    CHECK(Calls("TexStorage2D") == 1);
    CHECK(ring.TextureTarget() == GL_TEXTURE_2D && ring.Texture() != 0);

    ring.BeginDraw();
    const std::array<u32, 3> small{7, 8, 9};
    const u32 offset = ring.Push(small.data(), sizeof(small), 4);
    CHECK(offset == 0);
    CHECK(g_uploads.empty());
    ring.Commit();
    CHECK(g_uploads.size() == 1 && g_uploads[0].x == 0 && g_uploads[0].y == 0 && g_uploads[0].width == 3 &&
          g_uploads[0].height == 1 && g_uploads[0].first_value == 7);
    ring.Commit();
    CHECK(g_uploads.size() == 1);

    const std::array<u32, 1> next{11};
    ring.Push(next.data(), sizeof(next), 4);
    ring.Commit();
    CHECK(g_uploads.size() == 2 && g_uploads[1].x == 3 && g_uploads[1].width == 1 && g_uploads[1].first_value == 11);

    // Across a row boundary whole rows go up, and the first keeps what was already there.
    std::vector<u32> wide(size_t(row) + 8, 5);
    ring.Push(wide.data(), u32(wide.size() * sizeof(u32)), 4);
    ring.Commit();
    CHECK(g_uploads.size() == 3 && g_uploads[2].x == 0 && g_uploads[2].y == 0 && g_uploads[2].width == row &&
          g_uploads[2].height == 2 && g_uploads[2].first_value == 7);

    // A shared buffer read as shader data never reaches the driver as a buffer.
    CHECK(Calls("BufferStorage") == 0 && Calls("BufferData") == 0);
}

void TestPersistentRing() {
    g_calls.clear();
    InstallRingFakes();
    GlState state;
    StreamRing ring{state, true};
    CHECK(ring.Persistent());
    CHECK(Calls("BufferStorage") == 1);
    CHECK(Calls("BufferData") == 0);
    CHECK(Calls("MapBufferRange") == 1);

    std::vector<u8> data(FakeSegmentBytes, 0xAB);

    // The data is in the buffer at once: no map per draw.
    ring.BeginDraw();
    const u32 first = ring.Push(data.data(), 64, 4);
    CHECK(first == 0);
    CHECK(g_buffer_memory[0] == 0xAB);
    ring.Commit();
    CHECK(Calls("MapBufferRange") == 1);

    // Finishing a segment fences it, once the draws using it have been issued.
    ring.BeginDraw();
    CHECK(g_next_fence == 0);
    ring.Push(data.data(), FakeSegmentBytes, 4);
    CHECK(g_next_fence == 0);
    ring.BeginDraw();
    CHECK(g_next_fence == 1);

    // Up to the wrap nothing waits for the GPU; the wrap fences what is left.
    u32 offset = StreamRing::NoOffset;
    while (offset != 0) {
        ring.BeginDraw();
        CHECK(g_waits.empty());
        offset = ring.Push(data.data(), FakeSegmentBytes, 4);
    }
    // The wrap also fences the segment it left part filled.
    CHECK(g_next_fence == 12);

    // Writing over a segment waits for the fence of its previous use, before any byte lands.
    CHECK(g_waits.size() == 1);
    CHECK(g_waits[0] == 1);
    CHECK(Calls("DeleteSync") == 1);

    // Each segment is waited for once per pass, in order.
    ring.Push(data.data(), 64, 4);
    CHECK(g_waits.size() == 2);
    CHECK(g_waits[1] == 2);
    ring.Push(data.data(), 64, 4);
    CHECK(g_waits.size() == 2);
}

void TestGuestCopies() {
    g_calls.clear();
    InstallRingFakes();
    GlState state;
    StreamRing ring{state, true};
    std::vector<u8> guest(4096, 1);
    const VAddr address = VAddr(guest.data());

    ring.BeginDraw();
    const u32 copy = ring.PushGuest(address, 4096, 4);
    CHECK(ring.PushGuest(address, 4096, 4) == copy);
    CHECK(Perf::counters[Perf::CopiesReused] >= 1);

    // Same address, other bytes: by content.
    guest[100] = 2;
    const u32 changed = ring.PushGuest(address, 4096, 4);
    CHECK(changed != copy);
    guest[100] = 1;
    CHECK(ring.PushGuest(address, 4096, 4) == copy);

    // A stricter alignment than the copy has cannot reuse it.
    ring.Push(guest.data(), 3, 1);
    const u32 unaligned = ring.PushGuest(address + 1, 16, 1);
    CHECK(ring.PushGuest(address + 1, 16, 256) % 256 == 0);
    CHECK(unaligned % 256 != 0);

    ring.ForgetGuestCopies();
    CHECK(ring.PushGuest(address, 4096, 4) != copy);

    // A later draw must not read a segment whose fence came before it.
    const u32 before_fence = ring.PushGuest(address, 4096, 4);
    std::vector<u8> filler(FakeSegmentBytes, 3);
    ring.Push(filler.data(), FakeSegmentBytes, 4);
    ring.BeginDraw();
    CHECK(g_next_fence == 1);
    CHECK(before_fence < FakeSegmentBytes);
    CHECK(ring.PushGuest(address, 4096, 4) >= FakeSegmentBytes);
}

void TestMappedRing() {
    g_calls.clear();
    InstallRingFakes();
    GlState state;
    StreamRing ring{state, false};
    CHECK(!ring.Persistent());
    CHECK(Calls("BufferData") == 1);
    CHECK(Calls("BufferStorage") == 0);

    std::vector<u8> data(FakeSegmentBytes, 0xCD);
    ring.BeginDraw();
    ring.Push(data.data(), 64, 4);
    CHECK(g_buffer_memory[0] == 0);
    ring.Commit();
    CHECK(g_buffer_memory[0] == 0xCD);
    CHECK(Calls("MapBufferRange") == 1);
    ring.Commit();
    CHECK(Calls("MapBufferRange") == 1);

    // OpenGL 3.3 path: a wrap orphans the storage, there are no fences.
    for (u32 i = 0; i < StreamRing::SegmentCount; ++i) {
        ring.BeginDraw();
        ring.Push(data.data(), FakeSegmentBytes / 2, 4);
        ring.Commit();
    }
    CHECK(Calls("BufferData") == 1);
    for (u32 i = 0; i < StreamRing::SegmentCount; ++i) {
        ring.BeginDraw();
        ring.Push(data.data(), FakeSegmentBytes, 4);
        ring.Commit();
    }
    CHECK(Calls("BufferData") >= 2);
    CHECK(g_next_fence == 0);
    CHECK(g_waits.empty());

    // A failed persistent map leaves a working mapped ring.
    g_calls.clear();
    g_persistent_map_fails = true;
    StreamRing fallback{state, true};
    g_persistent_map_fails = false;
    CHECK(!fallback.Persistent());
    CHECK(Calls("BufferData") == 1);
    CHECK(Calls("DeleteBuffers") == 1);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// A direct call would leave the tracker believing old state, and the next filtered call would silently do nothing.
void TestNothingBypassesTheTracker() {
    const std::filesystem::path folder =
        std::filesystem::path(DCP_SOURCE_DIR) / "gpu/shadps4/video_core/renderer_opengl";
    const std::array tracked{
        "gl::Enable(",           "gl::Disable(",          "gl::Enablei(",           "gl::Disablei(",
        "gl::UseProgram(",       "gl::BindFramebuffer(",  "gl::BindVertexArray(",   "gl::BindBuffer(",
        "gl::ActiveTexture(",    "gl::BindTexture(",      "gl::BindSampler(",       "gl::Viewport(",
        "gl::Scissor(",          "gl::DepthRange(",       "gl::DepthFunc(",         "gl::DepthMask(",
        "gl::StencilFuncSeparate(", "gl::StencilMaskSeparate(", "gl::StencilOpSeparate(", "gl::CullFace(",
        "gl::FrontFace(",        "gl::PolygonMode(",      "gl::PolygonOffset(",     "gl::LineWidth(",
        "gl::ProvokingVertex(",  "gl::PrimitiveRestartIndex(", "gl::BlendColor(",   "gl::LogicOp(",
        "gl::ColorMaski(",       "gl::BlendFuncSeparate(", "gl::BlendFuncSeparatei(", "gl::BlendEquationSeparate(",
        "gl::BlendEquationSeparatei(", "gl::VertexAttribPointer(", "gl::VertexAttribIPointer(",
        "gl::VertexAttribDivisor(",
    };
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().filename() == "gl_state.h") {
            continue;
        }
        ++files;
        const std::string text = ReadText(entry.path());
        for (const char* call : tracked) {
            if (text.find(call) != std::string::npos) {
                std::printf("FAIL %s calls %s directly\n", entry.path().filename().string().c_str(), call);
                CHECK(false);
            }
        }
    }
    CHECK(files >= 10);
}

} // namespace

int main() {
    InstallFaultHandler();
    u8* arena = AllocateArena();
    CHECK(arena != nullptr);
    if (arena) {
        g_arena_begin = VAddr(arena);
        TestWatchBasics(arena);
        TestCooldown(arena);
        TestOverlappingRanges(arena);
        TestHotPage(arena);
        TestColdNeighborOfHotPage(arena);
        TestHostWritesNeverFault(arena);
        TestProtectionLostWithoutFault(arena);
        TestUnmapWhileWatched(arena);
        TestDestroyWhileRangesExist(arena);
        TestShutdownOrder(arena);
        TestWatchLosesNoWrites(arena);
        TestNoStaleImages(arena);
    }
    TestFaultsCannotSpinForever();
    TestState();
    TestPersistentRing();
    TestGuestCopies();
    TestMappedRing();
    TestGlesState();
    TestRingReserve();
    TestClientRing();
    TestShaderTextureRing();
    TestNothingBypassesTheTracker();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
