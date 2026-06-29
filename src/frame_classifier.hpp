#pragma once
#include "types.hpp"

// Two-pass block classifier for a single frame transition.
//
// Pass 1 – seed classification using the dominant scroll offset detected by
//           ScrollDetector.  Each block is tested against dy=0 (STATIC) and
//           dy=scroll_dy (SCROLLED).  Unresolved blocks stay UNKNOWN.
//
// Pass 2 – BFS region-grow from every seeded SCROLLED block.  If an UNKNOWN
//           neighbour also matches at the same dy it joins that class.
//           Remaining UNKNOWNs become NEW (or STATIC where dy=0 matches).
//
// Fixed-element tracking lives in GlobalCanvas; this class only classifies
// one frame transition and does not mutate the canvas.
class FrameClassifier {
public:
    // prev_gray / curr_gray : full-frame single-channel images.
    // scroll_dy             : dominant scroll (from ScrollDetector.detect).
    // fixed_mask            : CV_8UC1, 255 = position-fixed block position.
    // doc_y_top             : document Y of viewport top before this frame.
    [[nodiscard]]
    Grid classify(const cv::Mat& prev_gray,
                  const cv::Mat& curr_gray,
                  int scroll_dy,
                  const cv::Mat& fixed_mask,
                  long doc_y_top) const;

private:
    void pass1(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
               int scroll_dy, const cv::Mat& fixed_mask, Grid& grid) const;

    void pass2(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
               int scroll_dy, Grid& grid) const;

    // Assign document Y to every block
    void assign_doc_y(Grid& grid, long doc_y_top) const;
};
