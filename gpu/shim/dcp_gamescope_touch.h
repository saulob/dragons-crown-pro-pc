// SPDX-License-Identifier: GPL-2.0-or-later
// Asks Gamescope to hand a touchscreen to the game as touch input instead of pointer input. Window thread only.
#pragma once

#include <cstdint>
#include <SDL3/SDL.h>
#include "dcp_touch_gamescope.h"

namespace DcpGamescopeTouch {

enum class Result { Unavailable, Applied, Unchanged };

#ifdef __linux__
// The game's own X connection to Gamescope's Xwayland server; libX11 is loaded on demand (SDL may be on Wayland).
class Claim {
public:
    Claim() = default;
    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    Result Acquire() {
        if (!Open()) {
            return Result::Unavailable;
        }
        return Write(true) ? Result::Applied : Result::Unchanged;
    }

    void Maintain(Uint64 now_ms) {
        if (!state_.applied || now_ms - checked_ms_ < DCP_GAMESCOPE_TOUCH_RECHECK_MS) {
            return;
        }
        checked_ms_ = now_ms;
        Write(true);
    }

    void Release() {
        if (!display_) {
            return;
        }
        Write(false);
        close_(display_);
        display_ = nullptr;
    }

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

    bool Open() {
        if (display_) {
            return true;
        }
        if (!library_) {
            library_ = SDL_LoadObject("libX11.so.6");
        }
        if (!library_) {
            return false;
        }
        const auto open = Load<OpenFn>("XOpenDisplay");
        const auto intern = Load<InternFn>("XInternAtom");
        const auto root = Load<RootFn>("XDefaultRootWindow");
        close_ = Load<CloseFn>("XCloseDisplay");
        get_ = Load<GetFn>("XGetWindowProperty");
        change_ = Load<ChangeFn>("XChangeProperty");
        free_ = Load<FreeFn>("XFree");
        flush_ = Load<FlushFn>("XFlush");
        if (!open || !intern || !root || !close_ || !get_ || !change_ || !free_ || !flush_) {
            return false;
        }
        display_ = open(nullptr);
        if (!display_) {
            return false;
        }
        atom_ = intern(display_, DCP_GAMESCOPE_TOUCH_ATOM, 0);
        root_ = root(display_);
        return true;
    }

    template <typename Fn>
    Fn Load(const char* name) {
        return reinterpret_cast<Fn>(SDL_LoadFunction(library_, name));
    }

    bool Read(bool& have, std::uint32_t& value) {
        unsigned long type = 0, count = 0, after = 0;
        int format = 0;
        unsigned char* data = nullptr;
        const bool ok = get_(display_, root_, atom_, 0, 1, 0, kCardinal, &type, &format, &count, &after, &data) == 0;
        have = ok && type == kCardinal && format == 32 && count >= 1 && data;
        value = have ? static_cast<std::uint32_t>(*reinterpret_cast<long*>(data) & 0xffffffffL) : 0;
        if (data) {
            free_(data);
        }
        return ok;
    }

    // Claims (`claim`) or gives back the mode; returns whether a value was written.
    bool Write(bool claim) {
        bool have = false;
        std::uint32_t current = 0, value = 0;
        if (!Read(have, current)) {
            return false;
        }
        const DcpTouchModeAction action = claim ? dcp_touch_mode_claim(&state_, have, current, &value)
                                                : dcp_touch_mode_release(&state_, have, current, &value);
        if (action != DCP_TOUCH_MODE_WRITE) {
            return false;
        }
        const long data = static_cast<long>(value);
        change_(display_, root_, atom_, kCardinal, 32, 0, reinterpret_cast<const unsigned char*>(&data), 1);
        flush_(display_);
        return true;
    }

    SDL_SharedObject* library_ = nullptr;
    Display* display_ = nullptr;
    CloseFn close_ = nullptr;
    GetFn get_ = nullptr;
    ChangeFn change_ = nullptr;
    FreeFn free_ = nullptr;
    FlushFn flush_ = nullptr;
    unsigned long atom_ = 0, root_ = 0;
    DcpTouchModeState state_{};
    Uint64 checked_ms_ = 0;
};
#else
class Claim {
public:
    Result Acquire() { return Result::Unavailable; }
    void Maintain(Uint64) {}
    void Release() {}
};
#endif

} // namespace DcpGamescopeTouch
