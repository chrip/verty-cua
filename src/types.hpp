#pragma once
#include <cstdint>
#include <vector>
#include <opencv2/opencv.hpp>

// ── Constants ─────────────────────────────────────────────────────────────────
constexpr int BLOCK_SIZE = 16;          // pixels per block side
constexpr int MAX_SCROLL = 400;         // max pixels scrolled per frame
constexpr int SCROLL_STEP = 1;          // pixel step for scroll search
constexpr int SCROLL_SAMPLES = 120;     // blocks sampled for scroll histogram
constexpr int SAD_THRESHOLD = 18;       // max mean absolute diff to call a match
constexpr int FIXED_CONFIRM_FRAMES = 6; // scroll events before a block is FIXED

// ── Block classification ──────────────────────────────────────────────────────
enum class BlockClass : uint8_t {
    UNKNOWN  = 0,
    STATIC,    // same viewport position, same content
    SCROLLED,  // content found at +dy in prev frame (dy = scroll amount)
    NEW,       // not found anywhere in prev frame
    FIXED,     // position-fixed element (header / sidebar / footer)
};

// ── Per-block record for one frame ───────────────────────────────────────────
struct Block {
    int gx = 0, gy = 0;        // grid column / row (block units)
    BlockClass cls = BlockClass::UNKNOWN;
    int scroll_dy = 0;          // pixel offset into prev frame (SCROLLED only)
    long doc_y = 0;             // top-left Y in document coordinates
};

// ── Full grid of blocks for one frame ────────────────────────────────────────
using Grid = std::vector<std::vector<Block>>;  // [row][col]

// ── Scroll detection result ───────────────────────────────────────────────────
struct ScrollResult {
    int dy = 0;          // dominant scroll (pixels, positive = scrolled down)
    int votes = 0;       // histogram bin count at dy
    bool valid = false;  // enough evidence found
};

// ── Inline helpers ────────────────────────────────────────────────────────────

// Mean absolute difference between two same-size single-channel patches
inline float patch_mad(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return static_cast<float>(cv::mean(diff)[0]);
}

// Extract a BLOCK_SIZE×BLOCK_SIZE gray patch from frame (bounds-checked)
// Returns empty Mat if out of bounds.
inline cv::Mat extract_block(const cv::Mat& gray, int px, int py) {
    if (px < 0 || py < 0 ||
        px + BLOCK_SIZE > gray.cols ||
        py + BLOCK_SIZE > gray.rows)
        return {};
    return gray(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE));
}

// Does curr block at (px,py) match prev block at (px, py+dy) ?
inline bool blocks_match(const cv::Mat& prev_gray, const cv::Mat& curr_gray,
                         int px, int py, int dy) {
    auto a = extract_block(prev_gray, px, py + dy);
    auto b = extract_block(curr_gray, px, py);
    if (a.empty() || b.empty()) return false;
    return patch_mad(a, b) < SAD_THRESHOLD;
}
