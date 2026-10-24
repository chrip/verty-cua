#pragma once
#include <opencv2/core.hpp>

#include "live/block_motion.hpp"
#include "live/session.hpp"
#include "verification/action.hpp"

// ── What a drag carried, and where to ────────────────────────────────────────
// A drag is the one action whose outcome is a statement about identity: the
// thing that was under the pointer at the start is now under it at the end.
// Geometry alone cannot check that -- "something changed near the drop point"
// is also what a rejected drop, a highlight or a hover produces. The block
// motion field can: it names the pixels that moved, where they were and where
// they went.
//
// Two shapes of drag reach the verifier:
//
//   drag (left_click_drag)   one action; it starts wherever the pointer already
//                            was -- the session's last coordinate -- and ends at
//                            the action's own coordinate.
//   mouse_down ... mouse_up  a hand-rolled gesture; it starts at the press and
//                            ends at the release.
//
// The second needs a second reference frame. While a card is held, the app
// paints its source slot faded, so the frame just before the release holds no
// exact copy of the card that lands. The frame from before the press does --
// which is the multiple-reference-frame idea of every modern encoder, applied
// to the one place it is needed: the motion is measured across the whole
// gesture, press frame to release frame.
struct DragGesture {
    bool        valid = false;
    cv::Point   start, end;
    BlockMotion motion;   // across the whole gesture
};

// Call before the session advances past this step. `before`/`after` are this
// step's frames, single-channel and of equal size.
DragGesture measure_drag(const SessionState& s, const Action& a,
                         const cv::Mat& before, const cv::Mat& after);
