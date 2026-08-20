#include "verification/action.hpp"

#include <unordered_map>

ActionKind action_kind_from_string(const std::string& s)
{
    static const std::unordered_map<std::string, ActionKind> kMap = {
        {"click", ActionKind::Click},
        {"left_click", ActionKind::Click},
        {"double_click", ActionKind::DoubleClick},
        {"triple_click", ActionKind::DoubleClick},
        {"right_click", ActionKind::RightClick},
        {"middle_click", ActionKind::Click},
        {"type", ActionKind::Type},
        {"key", ActionKind::Key},
        {"key_down", ActionKind::Key},
        {"key_up", ActionKind::Key},
        {"keypress", ActionKind::Key},
        {"scroll", ActionKind::Scroll},
        {"hscroll", ActionKind::Scroll},
        {"drag", ActionKind::Drag},
        {"left_click_drag", ActionKind::Drag},
        // Observed in a real demo run: the model decomposes a drag by hand when
        // left_click_drag does not achieve what it wanted.
        {"left_mouse_down", ActionKind::MouseDown},
        {"left_mouse_up", ActionKind::MouseUp},
        {"mouse_move", ActionKind::MouseMove},
        {"move", ActionKind::MouseMove},
        {"wait", ActionKind::Wait},
        {"screenshot", ActionKind::Screenshot},
    };
    const auto it = kMap.find(s);
    return it == kMap.end() ? ActionKind::Unknown : it->second;
}

const char* action_kind_name(ActionKind k)
{
    switch (k) {
    case ActionKind::Click:       return "click";
    case ActionKind::DoubleClick: return "double_click";
    case ActionKind::RightClick:  return "right_click";
    case ActionKind::Type:        return "type";
    case ActionKind::Key:         return "key";
    case ActionKind::Scroll:      return "scroll";
    case ActionKind::Drag:        return "drag";
    case ActionKind::MouseDown:   return "mouse_down";
    case ActionKind::MouseUp:     return "mouse_up";
    case ActionKind::MouseMove:   return "mouse_move";
    case ActionKind::Wait:        return "wait";
    case ActionKind::Screenshot:  return "screenshot";
    case ActionKind::Unknown:     return "unknown";
    }
    return "unknown";
}
