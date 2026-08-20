#pragma once
#include "perception/types.hpp"
#include "perception/transition_classifier.hpp"
#include <vector>

// Folds the per-frame transition stream into page visits (Roadmap M3).
//
// A PAGE_CHANGE transition ends the current visit and opens a *transition
// window* (navigations are multi-frame: white flash, progressive render).
// The window closes once PAGE_STABLE_CONFIRM consecutive stable transitions
// (Identical or Scroll) are seen; the new visit starts at the first frame of
// that stable run. Frames inside the window belong to no visit and are
// excluded from stitching. UNSTABLE transitions outside a window stay part of
// the current visit (in-page animation, popup).
class SessionSegmenter {
public:
    struct Result {
        std::vector<PageVisit> visits;
        std::vector<long>      navFrames;   // first frame of visit k (k ≥ 1)
    };

    // transitions[i] = classification of the transition INTO frame i
    // (transitions[0] is a placeholder and ignored). transitions.size() must
    // equal the session's frame count.
    [[nodiscard]] Result segment(const std::vector<Transition>& transitions) const;
};
