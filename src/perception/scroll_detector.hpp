#pragma once
#include "perception/types.hpp"

// Detects the dominant Y-scroll between two consecutive frames using a
// histogram vote over a random sample of blocks.
class ScrollDetector {
public:
    // Returns the dominant scroll (dy > 0 = page scrolled down).
    //
    // fixed_mask (optional, CV_8UC1, frame-sized, 255 = position-fixed): when
    // provided, sample blocks are drawn only from NON-fixed (scrolling) areas so
    // that headers/footers/sidebars don't cast spurious dy≈0 votes. Pass an empty
    // Mat (the default) to search the entire viewport — the one-pass / first-pass
    // fallback used before any fixed mask is known.
    //
    // [dy_lo, dy_hi] (optional) narrows the search to a window around a
    // predicted scroll -- the motion-vector predictor every video encoder tries
    // before a full search. A dy on the window's edge may be a clamp of a scroll
    // that lies outside it, so it is never proposed; the full range keeps its
    // own rule for MAX_SCROLL below.
    ScrollResult detect(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                        const cv::Mat& fixed_mask = cv::Mat(),
                        int dy_lo = -MAX_SCROLL, int dy_hi = MAX_SCROLL);

private:
    // For one sampled block in curr, find the dy in [dy_lo, dy_hi] that
    // minimises MAD vs prev.  Returns the best dy and its MAD.
    std::pair<int, float> best_dy(const cv::Mat& prev_gray,
                                  const cv::Mat& curr_gray,
                                  int px, int py, int dy_lo, int dy_hi) const;
};
