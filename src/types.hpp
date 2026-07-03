#pragma once
#include <opencv2/opencv.hpp>

// ── Constants ─────────────────────────────────────────────────────────────────
constexpr int BLOCK_SIZE = 16;          // pixels per block side
constexpr int MAX_SCROLL = 400;         // max pixels scrolled per frame
constexpr int SCROLL_STEP = 1;          // pixel step for scroll search
constexpr int SCROLL_SAMPLES = 120;     // blocks sampled for scroll histogram
constexpr int SAD_THRESHOLD = 18;       // max mean absolute diff to call a match
constexpr int FIXED_CONFIRM_FRAMES = 6; // stable scroll events before a pixel is FIXED
constexpr double IDENTICAL_FRAME_MAD = 2.0;      // below: duplicate frame / pause
constexpr double PAGE_CHANGE_MIN_FRACTION = 0.4; // changed-area fraction ⇒ new page
// A detected scroll dy must also EXPLAIN the transition: after shifting the
// previous frame by dy, at most this fraction of the (non-fixed, overlapping)
// area may still differ. Guards against false scrolls on page changes, where
// periodic content (stripes, text lines) can win the vote histogram.
// Measured: true scrolls ≤ 0.04, false scroll on a page change 0.27.
constexpr double SCROLL_RESIDUAL_MAX = 0.15;

// The stitched interior is cropped this many rows inside the detected
// header/footer bands. If a band is under-detected by a pixel or two, every
// frame would otherwise write that chrome residue at a different document row
// — a thin line at every scroll step. The header/footer stamps are extended
// by the same margin (sourced from the shallowest/deepest frame, where those
// rows show the correct adjacent content), so no gap remains.
constexpr int INTERIOR_ROW_MARGIN = 4;
constexpr int PAGE_STABLE_CONFIRM = 3;  // stable transitions that close a page-change window

// ── Page identity (REVISIT recognition) ──────────────────────────────────────
constexpr int    IDENT_SAMPLES = 120;           // blocks sampled per candidate match
constexpr double IDENT_MIN_STDDEV = 4.0;        // skip near-uniform blocks (match anywhere)
constexpr double REVISIT_MIN_CONFIDENCE = 0.5;  // vote fraction to accept a REVISIT
constexpr int    IDENT_VT_SLACK = 2;            // vote bin slack (px) around the peak

// One page visit: a contiguous, stable frame range showing a single page.
// Frames between visits (navigation transitions) belong to no visit.
struct PageVisit {
    long firstFrame = 0;   // inclusive
    long lastFrame  = 0;   // inclusive
};

// ── Scroll detection result ───────────────────────────────────────────────────
struct ScrollResult {
    int  dy    = 0;      // dominant scroll (pixels, positive = scrolled down)
    int  votes = 0;      // histogram bin count at dy
    bool valid = false;  // enough evidence found
};

// ── Inline helpers ────────────────────────────────────────────────────────────

// Mean absolute difference between two same-size single-channel patches.
inline float patch_mad(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return static_cast<float>(cv::mean(diff)[0]);
}

// Extract a BLOCK_SIZE×BLOCK_SIZE gray patch from a frame (bounds-checked).
// Returns an empty Mat if out of bounds.
inline cv::Mat extract_block(const cv::Mat& gray, int px, int py) {
    if (px < 0 || py < 0 ||
        px + BLOCK_SIZE > gray.cols ||
        py + BLOCK_SIZE > gray.rows)
        return {};
    return gray(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE));
}
