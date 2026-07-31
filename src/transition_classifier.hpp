#pragma once
#include "types.hpp"
#include <vector>
#include "scroll_detector.hpp"

// Type of one frame-to-frame transition (Roadmap M2).
enum class TransitionType {
    Identical,    // no visible change (duplicate frame / reading pause)
    Scroll,       // dominant vertical scroll of dy pixels
    PageChange,   // large content change not explained by any scroll offset
    Unstable,     // small unexplained change (animation, partial render, popup)
};

struct Transition {
    TransitionType type = TransitionType::Identical;
    int dy = 0;   // scroll delta; meaningful for Scroll only
};

// Per-session tally of transition types (reported in the session report).
struct TransitionCounts {
    long identical  = 0;
    long scroll     = 0;
    long pageChange = 0;
    long unstable   = 0;

    void add(TransitionType t) {
        switch (t) {
        case TransitionType::Identical:  ++identical;  break;
        case TransitionType::Scroll:     ++scroll;     break;
        case TransitionType::PageChange: ++pageChange; break;
        case TransitionType::Unstable:   ++unstable;   break;
        }
    }
};

// Assigns a type to a frame transition:
//   Identical   mean absolute diff below IDENTICAL_FRAME_MAD.
//   Scroll      ScrollDetector finds a valid dominant dy.
//   PageChange  no dy explains the change AND a large fraction of the
//               scrollable area changed. Fixed chrome only changes on
//               navigation, so callers with a fixed mask get the stronger
//               signal: the changed fraction is measured on non-fixed pixels.
//   Unstable    small unexplained change (in-page animation, partial render).
//
// The multi-frame transition-window debounce around PageChange lives with the
// SessionSegmenter (Roadmap M3), where it becomes measurable — this class
// classifies single transitions only.
class TransitionClassifier {
public:
    explicit TransitionClassifier(Sensitivity sensitivity = Sensitivity::Normal)
        : sensitivity_(sensitivity) {}

    // fixed_mask   (optional, 255 = fixed chrome): excluded from scroll voting
    //              and from the page-change spread.
    // dynamic_mask (optional, 255 = confirmed persistent noise, e.g. a fixed
    //              video ad): excluded from the spread AND from the veto's
    //              changed-pixel evidence — a playing ad would otherwise dirty
    //              every band it spans and veto genuine scrolls. It is NOT
    //              used for detector sampling (the M5/M7 lessons).
    // Bounded scrolling animation needs no mask: the median-banded veto
    // tolerates it by construction.
    // `subframes` are rectangles a previous pass found to change IN PLACE while
    // the document stood still (see subframe_tracker.hpp). A scroll dy whose
    // evidence is contained in one of them belongs to that region, not to the
    // document — measured on an embedded phone flow, where the mockup's own
    // scrolling was charged to the page: four dy claims of 54, 133, 45 and 55px
    // on transitions where the phone screen repainted 45-58% of itself and the
    // rest of the frame changed by 0.001-0.37%. The page had not moved, but its
    // canvas grew 287px and its viewport-fixed furniture was written four times
    // down it.
    Transition classify(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                        const cv::Mat& fixed_mask = cv::Mat(),
                        const cv::Mat& dynamic_mask = cv::Mat(),
                        const std::vector<cv::Rect>& subframes = {});

private:
    Sensitivity    sensitivity_;
    ScrollDetector scroll_;

    // True iff shifting prev by dy leaves at most SCROLL_RESIDUAL_MAX of the
    // (non-ignored, overlapping) area changed — i.e. the dy explains the frame.
    [[nodiscard]] static bool scroll_explains(const cv::Mat& prev_gray,
                                              const cv::Mat& curr_gray,
                                              int dy, const cv::Mat& ignore_mask);
};
