#include "options.h"

#include <SDL3/SDL_keyboard.h>

#include "localization/ui_text.h"

namespace dcp {
namespace {

constexpr char kTimes[] = " \xC3\x97 ";

std::string Dimensions(int width, int height) {
    return std::to_string(width) + kTimes + std::to_string(height);
}

} // namespace

WindowSize WindowSizeOf(int height, const DcpAspectRatio& aspect) {
    const long width = (static_cast<long>(height) * aspect.width * 2 + aspect.height) / (2 * aspect.height);
    return {static_cast<int>(width), height};
}

std::vector<WindowSize> WindowSizesFor(const DcpAspectRatio& aspect) {
    std::vector<WindowSize> sizes;
    for (const int height : kWindowHeights) {
        sizes.push_back(WindowSizeOf(height, aspect));
    }
    return sizes;
}

const char* PageName(PageId page) {
    switch (page) {
    case PageId::kPlay: return "Play";
    case PageId::kDisplay: return "Display";
    case PageId::kGraphics: return "Graphics";
    case PageId::kGameplay: return "Gameplay";
    case PageId::kCheats: return "Cheats";
    case PageId::kControls: return "Controls";
    case PageId::kOnline: return "Online";
    case PageId::kTrophies: return "Trophies";
    case PageId::kEditor: return "Editor";
    case PageId::kBugReport: return "Bug Report";
    case PageId::kAbout: return "About";
    }
    return "";
}

std::string ResolutionLabel(const DcpResolution& resolution) {
    return std::string(Tr(resolution.name)) + " \xE2\x80\x94 " + Dimensions(resolution.width, resolution.height);
}

std::string WindowSizeLabel(WindowSize size) {
    std::string label = Dimensions(size.width, size.height);
    if (size == kGamePictureSize) {
        label += "   ";
        label += Tr("(the game's own size)");
    }
    return label;
}

std::string MonitorLabel(int monitor, const std::vector<Display>& displays) {
    const std::string number = std::to_string(monitor);
    if (monitor < 1 || monitor > static_cast<int>(displays.size())) {
        return Tr("Display ") + number + Tr(" (not connected)");
    }
    const Display& display = displays[static_cast<size_t>(monitor - 1)];
    std::string label = Tr("Display ") + number + (display.primary ? Tr(" (primary)") : "") + "   " +
                        Dimensions(display.bounds.w, display.bounds.h);
    if (!display.name.empty() && display.name != "Generic PnP Monitor") {
        label += "   " + display.name;
    }
    return label;
}

std::string KeyDisplayName(int scancode) {
    if (scancode == SDL_SCANCODE_RETURN) {
        return "Enter";
    }
    return dcp_key_bindable(scancode) ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : std::string();
}

namespace {

std::string DefaultKeyName(DcpKeyAction action) {
    char name[32];
    return dcp_key_name(dcp_key_actions[action].default_scancode, name, sizeof(name));
}

} // namespace

int SavedKey(const trg::SettingsStore& settings, DcpKeyAction action) {
    const int saved = dcp_key_from_name(settings.Get(dcp_key_actions[action].ini_key).c_str());
    return saved ? saved : dcp_key_actions[action].default_scancode;
}

void SetKey(trg::SettingsStore& settings, DcpKeyAction action, int scancode) {
    char name[32];
    if (dcp_key_name(scancode, name, sizeof(name))[0]) {
        settings.Set(dcp_key_actions[action].ini_key, name);
    }
}

void ResetKeys(trg::SettingsStore& settings) {
    for (int action = 0; action < DCP_KEY_COUNT; ++action) {
        settings.Set(dcp_key_actions[action].ini_key, DefaultKeyName(static_cast<DcpKeyAction>(action)));
    }
}

DcpMouseMap SavedMouse(const trg::SettingsStore& settings) {
    DcpMouseMap map;
    dcp_mouse_defaults(&map);
    for (const DcpMouseActionInfo& action : dcp_mouse_actions) {
        dcp_mouse_apply(&map, action.ini_key, settings.Get(action.ini_key).c_str());
    }
    dcp_mouse_apply(&map, dcp_mouse_controls_key, settings.Get(dcp_mouse_controls_key).c_str());
    dcp_mouse_apply(&map, dcp_wheel_up_key, settings.Get(dcp_wheel_up_key).c_str());
    dcp_mouse_apply(&map, dcp_wheel_down_key, settings.Get(dcp_wheel_down_key).c_str());
    dcp_mouse_apply(&map, dcp_pointer_sensitivity_key, settings.Get(dcp_pointer_sensitivity_key).c_str());
    dcp_mouse_apply(&map, dcp_pointer_mode_key, settings.Get(dcp_pointer_mode_key).c_str());
    dcp_mouse_apply(&map, dcp_trackpad_key, settings.Get(dcp_trackpad_key).c_str());
    dcp_mouse_normalize(&map);
    return map;
}

bool SetMouseButton(trg::SettingsStore& settings, DcpMouseAction action, int button) {
    const DcpMouseMap saved = SavedMouse(settings);
    if (!dcp_mouse_binding_allowed(&saved, action, button)) {
        return false;
    }
    settings.Set(dcp_mouse_actions[action].ini_key, dcp_mouse_button_name(button));
    return true;
}

MouseTargetResult SetMouseTarget(trg::SettingsStore& settings, int button, int target) {
    const DcpMouseMap saved = SavedMouse(settings);
    DcpMouseMap changed = saved;
    MouseTargetResult result;
    result.applied = dcp_mouse_assign_target(&changed, button, target, &result.moved_from) != 0;
    if (!result.applied) {
        return result;
    }
    for (const DcpMouseActionInfo& action : dcp_mouse_actions) {
        const int index = static_cast<int>(&action - dcp_mouse_actions);
        if (action.target >= 0 && changed.button[index] != saved.button[index]) {
            settings.Set(action.ini_key, dcp_mouse_button_name(changed.button[index]));
        }
    }
    return result;
}

void SetPointerMode(trg::SettingsStore& settings, int mode) {
    settings.Set(dcp_pointer_mode_key, dcp_pointer_mode_name(mode));
}

void SetWheelTarget(trg::SettingsStore& settings, bool up, int target) {
    settings.Set(up ? dcp_wheel_up_key : dcp_wheel_down_key, dcp_wheel_target_name(target));
}

void SetPointerSensitivity(trg::SettingsStore& settings, int percent) {
    settings.Set(dcp_pointer_sensitivity_key, std::to_string(dcp_pointer_sensitivity_normalize(percent)));
}

int SensitivityStepOf(int percent) {
    return (dcp_pointer_sensitivity_normalize(percent) - DCP_POINTER_SENSITIVITY_MIN) / DCP_POINTER_SENSITIVITY_STEP;
}

int SensitivityOfStep(int step) {
    return dcp_pointer_sensitivity_normalize(DCP_POINTER_SENSITIVITY_MIN + step * DCP_POINTER_SENSITIVITY_STEP);
}

void ResetMouse(trg::SettingsStore& settings) {
    for (const DcpMouseActionInfo& action : dcp_mouse_actions) {
        settings.Set(action.ini_key, dcp_mouse_button_name(action.default_button));
    }
    settings.Set(dcp_wheel_up_key, dcp_wheel_target_name(DCP_WHEEL_NONE));
    settings.Set(dcp_wheel_down_key, dcp_wheel_target_name(DCP_WHEEL_NONE));
    settings.Set(dcp_pointer_sensitivity_key, std::to_string(DCP_POINTER_SENSITIVITY_DEFAULT));
    settings.Set(dcp_pointer_mode_key, dcp_pointer_mode_name(DCP_POINTER_MODE_HOLD));
    settings.Set(dcp_trackpad_key, dcp_trackpad_setting_name(DCP_TRACKPAD_AUTO));
}

} // namespace dcp
