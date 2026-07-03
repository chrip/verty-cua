#pragma once
#include "types.hpp"

// Per-pixel stability accumulator — the evidence base for fixed-element
// detection. A pixel's counter increments while it stays unchanged across a
// scroll transition and resets to 0 the moment it changes. Truly
// position-fixed pixels (header/footer/sidebars) survive every scroll;
// scrolling content does not.
//
// Used by PageStitcher (per-visit fixed mask) and by SessionProcessor (online
// chrome estimate during segmentation, reset at every page change).
class FixedAccumulator {
public:
    FixedAccumulator(int frame_w, int frame_h);

    // Feed one scroll transition. A light 3×3 blur suppresses single-pixel
    // compression noise.
    void accumulate(const cv::Mat& prev_gray, const cv::Mat& curr_gray);

    // Forget everything (e.g. after a page change).
    void reset();

    // Confirmed-fixed mask: 255 where the counter ≥ FIXED_CONFIRM_FRAMES
    // (CV_8UC1, frame-sized).
    [[nodiscard]] cv::Mat mask() const;

private:
    cv::Mat counter_;   // frame_h × frame_w, CV_32SC1
};

