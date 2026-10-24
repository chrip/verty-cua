#include "live/drag_gesture.hpp"

#include "live/visual_delta.hpp"

DragGesture measure_drag(const SessionState& s, const Action& a,
                         const cv::Mat& before, const cv::Mat& after)
{
    DragGesture g;
    if (!a.has_coord) return g;

    if (a.kind == ActionKind::Drag && s.has_focus) {
        g.valid  = true;
        g.start  = s.focus;
        g.end    = a.coord;
        g.motion = measure_motion(before, after, change_mask(before, after));
    } else if (a.kind == ActionKind::MouseUp && s.has_press
               && s.press_frame.size() == after.size()) {
        g.valid  = true;
        g.start  = s.press;
        g.end    = a.coord;
        g.motion = measure_motion(s.press_frame, after, change_mask(s.press_frame, after));
    }
    return g;
}
