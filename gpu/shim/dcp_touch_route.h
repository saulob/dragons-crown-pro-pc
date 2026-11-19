// SPDX-License-Identifier: GPL-2.0-or-later
// Which SDL events belong to a touchscreen. Window thread only.
#pragma once

#include <SDL3/SDL.h>

namespace DcpTouchRoute {

// SDL repeats every touch as mouse input marked SDL_TOUCH_MOUSEID; the touchscreen path already handles it.
inline bool TouchGeneratedMouse(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.which == SDL_TOUCH_MOUSEID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.which == SDL_TOUCH_MOUSEID;
    default:
        return false;
    }
}

enum class Counted { None, Detected, MouseSuppressed };

// What the touchscreen telemetry counts for an event; `accepted` is whether the event's touch device passed AcceptedTouch.
inline Counted CountedEvent(const SDL_Event& event, bool accepted) {
    if (event.type == SDL_EVENT_FINGER_DOWN && accepted) {
        return Counted::Detected;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && TouchGeneratedMouse(event)) {
        return Counted::MouseSuppressed;
    }
    return Counted::None;
}

inline bool IsFingerEvent(const SDL_Event& event) {
    return event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_MOTION ||
           event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
}

// Only a screen the finger touches directly; trackpads and SDL's mouse- or pen-made touches stay out.
inline bool AcceptedTouch(SDL_TouchID touch, SDL_TouchDeviceType type) {
    return touch != SDL_MOUSE_TOUCHID && touch != SDL_PEN_TOUCHID && type == SDL_TOUCH_DEVICE_DIRECT;
}

} // namespace DcpTouchRoute
