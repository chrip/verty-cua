#pragma once
#include "types.hpp"

// Detects the dominant Y-scroll between two consecutive frames using a
// histogram vote over a random sample of blocks.
class ScrollDetector {
public:
    // Returns the dominant scroll (dy > 0 = page scrolled down).
    ScrollResult detect(const cv::Mat& prev_gray, const cv::Mat& curr_gray);

private:
    // For one sampled block in curr, find the dy in [-MAX_SCROLL,+MAX_SCROLL]
    // that minimises MAD vs prev.  Returns the best dy and its MAD.
    std::pair<int, float> best_dy(const cv::Mat& prev_gray,
                                  const cv::Mat& curr_gray,
                                  int px, int py) const;
};
