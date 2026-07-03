#pragma once
#include "types.hpp"
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
    // fixed_mask (optional, CV_8UC1 frame-sized, 255 = fixed): passed through to
    // the ScrollDetector and used to restrict the changed-area measurement to
    // scrollable pixels. Empty mask ⇒ whole frame (first pass / one-pass use).
    Transition classify(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                        const cv::Mat& fixed_mask = cv::Mat());

private:
    ScrollDetector scroll_;

    // True iff shifting prev by dy leaves at most SCROLL_RESIDUAL_MAX of the
    // (non-fixed, overlapping) area changed — i.e. the dy explains the frame.
    [[nodiscard]] static bool scroll_explains(const cv::Mat& prev_gray,
                                              const cv::Mat& curr_gray,
                                              int dy, const cv::Mat& fixed_mask);
};
