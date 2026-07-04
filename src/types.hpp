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
// A detected scroll dy must also EXPLAIN the transition. After shifting the
// previous frame by dy, the overlap is cut into horizontal bands and each
// band's changed fraction is measured; the MEDIAN band must be nearly clean.
// The median is robust against a bounded animated region (which only pollutes
// its own bands) while a false dy on a page change leaves most bands dirty.
// Measured: true scrolls (animation raging in view) median 0.000; false
// scrolls 0.081 and 0.909.
constexpr double SCROLL_RESIDUAL_MEDIAN_MAX = 0.05;
constexpr int    RESIDUAL_BAND_H = 32;   // band height (px) for the median

// The stitched interior is cropped this many rows inside the detected
// header/footer bands, for two reasons: (a) a band under-detected by a pixel
// or two would smear a chrome line at every scroll step; (b) chrome drop
// shadows (e.g. a 6px header box-shadow) darken the adjacent content rows —
// pixels that are neither stable (they blend with scrolling content) nor
// clean content, and would poison every write's edge rows. The margin must
// clear the shadow. The header/footer stamps are extended by the same margin
// (sourced from the shallowest/deepest frame, where those rows show the
// correct adjacent content), so no gap remains.
constexpr int INTERIOR_ROW_MARGIN = 12;

// When an incoming frame write disagrees with already-stitched rows by more
// than this (mean abs diff per row), the existing rows are kept — dynamic
// content (animation, floating buttons) is frozen at its first observation
// instead of being smeared across the page.
constexpr double ROW_CONFLICT_MAD = 10.0;

// ── Interior fixed overlays (floating buttons, cookie banners) ───────────────
// A viewport-fixed element that is NOT edge-anchored appears in the pixel
// fixed mask as an interior blob. Genuine overlays are compact and textured;
// stable whitespace (the historical false-positive that ruled out floating
// rects) is sprawling and uniform, so it fails these filters.
constexpr int    OVERLAY_MIN_AREA   = 576;   // ≥ 24×24 px of confirmed-fixed pixels
constexpr double OVERLAY_MIN_FILL   = 0.5;   // fixed-pixel fill ratio of the bbox
constexpr double OVERLAY_MIN_STDDEV = 8.0;   // texture: uniform blobs are not UI
constexpr int    OVERLAY_PAD        = 4;     // bbox padding (anti-aliasing, shadow)

// Reported uncertainty of the fixed-band extents (session report). The bands
// can only OVER-detect — flat content adjacent to the chrome reads as stable
// and is absorbed into the band (a documented non-decision: the margin is
// provably invisible in the output). Largest over-detection measured on the
// fixtures: +39px (sidebars). The true band edge lies within
// [detected − uncertainty, detected].
constexpr int FIXED_BAND_UNCERTAINTY_PX = 48;
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
