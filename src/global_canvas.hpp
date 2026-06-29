#pragma once
#include "types.hpp"
#include <map>
#include <unordered_map>

// Accumulates the full document content across all frames.
//
// Coordinate system
// ─────────────────
// doc_y = 0 is the top of the page (first frame's top).
// Positive doc_y goes downward.  The viewport top moves from 0 to +∞ as the
// user scrolls down.
//
// Storage
// ───────
// scrollable content : keyed by (gx, doc_block_y) where doc_block_y =
//                      doc_y / BLOCK_SIZE.  Each entry is a 16×16 BGR patch.
// fixed content      : keyed by (gx, gy_viewport) — the block's fixed
//                      viewport position in grid units.
//
// Fixed-element tracking
// ──────────────────────
// A block position is promoted to FIXED after FIXED_CONFIRM_FRAMES consecutive
// scroll events where the block is STATIC while scroll_dy ≠ 0.
class GlobalCanvas {
public:
    explicit GlobalCanvas(int frame_w, int frame_h);

    // Ingest the very first frame (no prev frame available).
    void ingest_first(const cv::Mat& bgr);

    // Update canvas with the classified blocks from frame N.
    //   bgr        : current frame (colour)
    //   prev_gray  : previous frame grayscale (for fixed detection)
    //   curr_gray  : current frame grayscale
    //   grid       : classified blocks from FrameClassifier
    //   scroll_dy  : dominant scroll (pixels)
    void update(const cv::Mat& bgr,
                const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                const Grid& grid, int scroll_dy);

    // Current fixed-element mask (CV_8UC1, 255 = fixed, size = grid dims).
    [[nodiscard]] const cv::Mat& fixed_mask() const { return fixed_mask_; }

    // Current viewport top in document coordinates.
    [[nodiscard]] long viewport_top() const { return viewport_top_; }

    // Extent of the document
    [[nodiscard]] long doc_height() const;  // pixels
    [[nodiscard]] int  doc_width()  const { return frame_w_; }

    // Build a full-resolution BGR image of all scrollable content
    // (does NOT include fixed elements — use fixed_content() for that).
    [[nodiscard]] cv::Mat stitch_scroll() const;

    // Access fixed-element patches keyed by (gx, gy) viewport grid position.
    [[nodiscard]]
    const std::map<std::pair<int,int>, cv::Mat>& fixed_content() const {
        return fixed_content_;
    }

    // Grid dimensions
    [[nodiscard]] int grid_cols() const { return grid_cols_; }
    [[nodiscard]] int grid_rows() const { return grid_rows_; }

private:
    int frame_w_, frame_h_;
    int grid_cols_, grid_rows_;

    long viewport_top_ = 0;  // doc Y of viewport top, updated per scroll

    // Scrollable content: (gx, doc_block_y) → 16×16 BGR patch
    std::map<std::pair<int, long>, cv::Mat> scroll_content_;

    // Fixed-element content: (gx, gy) → 16×16 BGR patch
    std::map<std::pair<int,int>, cv::Mat> fixed_content_;

    // Per-block static-despite-scroll counters for fixed detection
    cv::Mat static_counter_;  // CV_32SC1, grid dims

    // Confirmed fixed mask (CV_8UC1)
    cv::Mat fixed_mask_;

    // Fixed detection: checks block stability at dy=0 directly from raw frames,
    // independent of the main SCROLLED/STATIC classification.
    void promote_fixed_candidates(const cv::Mat& prev_gray,
                                   const cv::Mat& curr_gray,
                                   int scroll_dy);
    void store_block(const cv::Mat& bgr, const Block& b);
};
