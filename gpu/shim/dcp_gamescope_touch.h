// SPDX-License-Identifier: GPL-2.0-or-later
// Asks Gamescope to hand a touchscreen to the game as touch input instead of pointer input. Window thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "dcp_touch_gamescope.h"

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace DcpGamescopeTouch {

enum class Result { Unavailable, Applied, Unchanged };

#ifdef __linux__

// Gamescope applies the newest STEAM_TOUCH_CLICK_MODE write from any of its Xwayland roots, so Passthrough is written on
// the game's own display and on Gamescope's root (the display Steam runs on, discovered locally) and rewritten while the
// game has focus. Each root keeps its own saved value.
class Claim {
public:
    Claim() = default;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    Result Acquire() {
        if (!LoadLibrary()) {
            failure_ = DCP_TOUCH_CLAIM_NO_X11;
            return Result::Unavailable;
        }
        if (root_count_ == 0 && !OpenRoots()) {
            failure_ = DCP_TOUCH_CLAIM_NO_DISPLAY;
            return Result::Unavailable;
        }
        bool wrote = false;
        for (int i = 0; i < root_count_; ++i) {
            Root& root = roots[i];
            bool have = false;
            std::uint32_t current = 0;
            if (!Read(root, have, current)) continue;
            dcp_touch_mode_claim(&root.mode, have, current);
            wrote |= Write(root, DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
        }
        checked_ms_ = SDL_GetTicks();
        if (!wrote) {
            failure_ = DCP_TOUCH_CLAIM_NO_DISPLAY;
            return Result::Unavailable;
        }
        return Result::Applied;
    }

    bool Maintain(Uint64 now_ms) {
        if (root_count_ == 0 || now_ms - checked_ms_ < DCP_GAMESCOPE_TOUCH_RECHECK_MS) {
            return false;
        }
        checked_ms_ = now_ms;
        bool wrote = false;
        for (int i = 0; i < root_count_; ++i) {
            Root& root = roots[i];
            bool have = false;
            std::uint32_t current = 0;
            if (!Read(root, have, current)) continue;
            dcp_touch_mode_claim(&root.mode, have, current);
            wrote |= Write(root, DCP_GAMESCOPE_TOUCH_PASSTHROUGH);
        }
        return wrote;
    }

    void Release() {
        for (int i = 0; i < root_count_; ++i) {
            Root& root = roots[i];
            bool have = false;
            std::uint32_t current = 0, value = 0;
            const bool read = Read(root, have, current);
            if (dcp_touch_mode_release(&root.mode, read && have, current, &value)) {
                Write(root, value);
            }
            close_(root.display);
            root = Root{};
        }
        root_count_ = 0;
    }

    int Roots() const { return root_count_; }
    bool CompositorRoot() const { return compositor_; }
    DcpTouchClaimFailure Failure() const { return failure_; }

private:
    using Display = void;
    using OpenFn = Display* (*)(const char*);
    using CloseFn = int (*)(Display*);
    using InternFn = unsigned long (*)(Display*, const char*, int);
    using RootFn = unsigned long (*)(Display*);
    using GetFn = int (*)(Display*, unsigned long, unsigned long, long, long, int, unsigned long, unsigned long*, int*,
                          unsigned long*, unsigned long*, unsigned char**);
    using ChangeFn = int (*)(Display*, unsigned long, unsigned long, unsigned long, int, int, const unsigned char*, int);
    using FreeFn = int (*)(void*);
    using FlushFn = int (*)(Display*);
    static constexpr unsigned long kCardinal = 6;
    static constexpr int kMaxCandidates = 8;
    static constexpr size_t kNameSize = 16;

    struct Root {
        Display* display = nullptr;
        unsigned long root = 0;
        unsigned long atom = 0;
        DcpTouchModeRoot mode{};
    };

    struct Candidate {
        char name[kNameSize];
    };

    bool LoadLibrary() {
        if (library_) {
            return true;
        }
        library_ = SDL_LoadObject("libX11.so.6");
        if (!library_) {
            return false;
        }
        open_ = Load<OpenFn>("XOpenDisplay");
        intern_ = Load<InternFn>("XInternAtom");
        default_root_ = Load<RootFn>("XDefaultRootWindow");
        close_ = Load<CloseFn>("XCloseDisplay");
        get_ = Load<GetFn>("XGetWindowProperty");
        change_ = Load<ChangeFn>("XChangeProperty");
        free_ = Load<FreeFn>("XFree");
        flush_ = Load<FlushFn>("XFlush");
        if (open_ && intern_ && default_root_ && close_ && get_ && change_ && free_ && flush_) {
            return true;
        }
        SDL_UnloadObject(library_);
        library_ = nullptr;
        return false;
    }

    template <typename Fn>
    Fn Load(const char* name) {
        return reinterpret_cast<Fn>(SDL_LoadFunction(library_, name));
    }

    // A local socket name (no host); anything with a host is left alone.
    static bool LocalDisplayName(const char* display, char* name, size_t size) {
        if (!display) {
            return false;
        }
        const char* at = display;
        if (!std::strncmp(at, "unix:", 5)) at += 4;
        if (*at != ':') {
            return false;
        }
        const char* digits = at + 1;
        int count = 0;
        while (count < 3 && digits[count] >= '0' && digits[count] <= '9') ++count;
        if (count == 0 || (digits[count] >= '0' && digits[count] <= '9')) {
            return false;
        }
        const char* rest = digits + count;
        if (*rest == '.') {
            int screen = 0;
            while (rest[1 + screen] >= '0' && rest[1 + screen] <= '9') ++screen;
            if (screen == 0) {
                return false;
            }
            rest += 1 + screen;
        }
        if (*rest) {
            return false;
        }
        std::snprintf(name, size, ":%.*s", count, digits);
        return true;
    }

    static void AddCandidate(Candidate* list, int& count, const char* name) {
        for (int i = 0; i < count; ++i) {
            if (!std::strcmp(list[i].name, name)) return;
        }
        if (count == kMaxCandidates) {
            return;
        }
        std::snprintf(list[count++].name, kNameSize, "%s", name);
    }

    // Every local X socket of this user; none is contacted unless it answers.
    static void AddSocketCandidates(Candidate* list, int& count) {
        DIR* dir = opendir("/tmp/.X11-unix");
        if (!dir) {
            return;
        }
        const uid_t user = getuid();
        while (const dirent* entry = readdir(dir)) {
            const char* name = entry->d_name;
            if (name[0] != 'X') continue;
            int digits = 0;
            while (digits < 3 && name[1 + digits] >= '0' && name[1 + digits] <= '9') ++digits;
            if (digits == 0 || name[1 + digits]) continue;
            char path[64];
            std::snprintf(path, sizeof(path), "/tmp/.X11-unix/%s", name);
            struct stat info {};
            if (lstat(path, &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != user) continue;
            char display[kNameSize];
            std::snprintf(display, sizeof(display), ":%.*s", digits, name + 1);
            AddCandidate(list, count, display);
        }
        closedir(dir);
    }

    bool OpenRoots() {
        Candidate list[kMaxCandidates];
        int count = 0;
        char own[kNameSize] = {};
        const bool have_own = LocalDisplayName(std::getenv("DISPLAY"), own, sizeof(own));
        compositor_ = false;
        if (have_own) {
            AddCandidate(list, count, own);
        }
        AddSocketCandidates(list, count);

        for (int i = 0; i < count && root_count_ < DCP_GAMESCOPE_TOUCH_MAX_ROOTS; ++i) {
            const bool mine = have_own && !std::strcmp(list[i].name, own);
            Display* const display = open_(list[i].name);
            if (!display) {
                continue;
            }
            const unsigned long marker = intern_(display, DCP_GAMESCOPE_TOUCH_MARKER_ATOM, 1);
            if (!mine && !marker) { // a desktop X server, not one of Gamescope's roots
                close_(display);
                continue;
            }
            if (root_count_ == DCP_GAMESCOPE_TOUCH_MAX_ROOTS) {
                close_(display);
                break;
            }
            Root& root = roots[root_count_++];
            root.display = display;
            root.root = default_root_(display);
            root.atom = intern_(display, DCP_GAMESCOPE_TOUCH_ATOM, 0);
            compositor_ |= !mine;
        }
        return root_count_ > 0;
    }

    bool Read(Root& root, bool& have, std::uint32_t& value) {
        unsigned long type = 0, count = 0, after = 0;
        int format = 0;
        unsigned char* data = nullptr;
        const bool ok =
            get_(root.display, root.root, root.atom, 0, 1, 0, kCardinal, &type, &format, &count, &after, &data) == 0;
        have = ok && type == kCardinal && format == 32 && count >= 1 && data;
        value = have ? static_cast<std::uint32_t>(*reinterpret_cast<long*>(data) & 0xffffffffL) : 0;
        if (data) {
            free_(data);
        }
        return ok;
    }

    bool Write(Root& root, std::uint32_t value) {
        const long data = static_cast<long>(value);
        if (!change_(root.display, root.root, root.atom, kCardinal, 32, 0,
                     reinterpret_cast<const unsigned char*>(&data), 1)) {
            return false;
        }
        flush_(root.display);
        return true;
    }

    SDL_SharedObject* library_ = nullptr;
    OpenFn open_ = nullptr;
    InternFn intern_ = nullptr;
    RootFn default_root_ = nullptr;
    CloseFn close_ = nullptr;
    GetFn get_ = nullptr;
    ChangeFn change_ = nullptr;
    FreeFn free_ = nullptr;
    FlushFn flush_ = nullptr;
    Root roots[DCP_GAMESCOPE_TOUCH_MAX_ROOTS];
    int root_count_ = 0;
    bool compositor_ = false;
    DcpTouchClaimFailure failure_ = DCP_TOUCH_CLAIM_NO_X11;
    Uint64 checked_ms_ = 0;
};
#else
class Claim {
public:
    Result Acquire() { return Result::Unavailable; }
    bool Maintain(Uint64) { return false; }
    void Release() {}
    int Roots() const { return 0; }
    bool CompositorRoot() const { return false; }
    DcpTouchClaimFailure Failure() const { return DCP_TOUCH_CLAIM_NO_X11; }
};
#endif

} // namespace DcpGamescopeTouch
