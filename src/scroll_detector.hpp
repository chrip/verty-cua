#pragma once
#include "types.hpp"

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
    ScrollResult detect(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                        const cv::Mat& fixed_mask = cv::Mat());

private:
    // For one sampled block in curr, find the dy in [-MAX_SCROLL,+MAX_SCROLL]
    // that minimises MAD vs prev.  Returns the best dy and its MAD.
    std::pair<int, float> best_dy(const cv::Mat& prev_gray,
                                  const cv::Mat& curr_gray,
                                  int px, int py) const;
};
