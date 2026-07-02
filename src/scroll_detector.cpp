#include "scroll_detector.hpp"
#include <random>
#include <map>
#include <algorithm>
#include <limits>

// ── best_dy ──────────────────────────────────────────────────────────────────
// Slide curr block (px,py) over prev column px across the full Y search range.
// We skip dy=0 here so that static blocks (headers/footers) don't dominate.
std::pair<int, float> ScrollDetector::best_dy(const cv::Mat& prev_gray,
                                               const cv::Mat& curr_gray,
                                               int px, int py) const
{
    auto b = extract_block(curr_gray, px, py);
    if (b.empty()) return {0, std::numeric_limits<float>::max()};

    int   best_dy_val = 0;
    float best_mad    = std::numeric_limits<float>::max();

    for (int dy = -MAX_SCROLL; dy <= MAX_SCROLL; dy += SCROLL_STEP) {
        if (dy == 0) continue;  // skip STATIC; we want the scroll component
        auto a = extract_block(prev_gray, px, py + dy);
        if (a.empty()) continue;
        float mad = patch_mad(a, b);
        if (mad < best_mad) {
            best_mad    = mad;
            best_dy_val = dy;
        }
    }
    return {best_dy_val, best_mad};
}

// ── detect ────────────────────────────────────────────────────────────────────
ScrollResult ScrollDetector::detect(const cv::Mat& prev_gray,
                                    const cv::Mat& curr_gray,
                                    const cv::Mat& fixed_mask)
{
    const int cols = curr_gray.cols;
    const int rows = curr_gray.rows;

    const int blocks_x = cols / BLOCK_SIZE;
    const int blocks_y = rows / BLOCK_SIZE;
    if (blocks_x == 0 || blocks_y == 0) return {};

    // Avoid top/bottom strips: fixed headers/footers sit there and would
    // produce spurious dy=0 votes that hide the true scroll.
    const int margin_rows = 2;
    const int gy_min = margin_rows;
    const int gy_max = blocks_y - margin_rows - 1;
    if (gy_min >= gy_max) return {};

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist_x(0, blocks_x - 1);
    std::uniform_int_distribution<int> dist_y(gy_min, gy_max);

    // A sample block is usable only if it is (mostly) NOT fixed. With no mask
    // (one-pass / first pass) every block is usable — full-viewport fallback.
    auto block_is_scrollable = [&](int px, int py) {
        if (fixed_mask.empty()) return true;
        if (px + BLOCK_SIZE > fixed_mask.cols || py + BLOCK_SIZE > fixed_mask.rows)
            return true;
        double fixed_frac =
            cv::mean(fixed_mask(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE)))[0] / 255.0;
        return fixed_frac <= 0.25;   // reject blocks that are mostly fixed chrome
    };

    // Histogram: scroll_dy → vote count. Keep drawing until we have SCROLL_SAMPLES
    // usable (scrolling) blocks, or we run out of attempts.
    std::map<int, int> hist;
    int collected = 0;
    const int max_attempts = SCROLL_SAMPLES * 8;
    for (int a = 0; a < max_attempts && collected < SCROLL_SAMPLES; ++a) {
        int gx = dist_x(rng);
        int gy = dist_y(rng);
        int px = gx * BLOCK_SIZE;
        int py = gy * BLOCK_SIZE;
        if (!block_is_scrollable(px, py)) continue;
        ++collected;

        auto [dy, mad] = best_dy(prev_gray, curr_gray, px, py);
        if (mad < SAD_THRESHOLD) {
            // Round to nearest pixel for the histogram key
            hist[dy]++;
        }
    }

    if (hist.empty()) return {0, 0, false};

    // Find the peak and runner-up vote counts
    int best_votes   = 0, second_votes = 0;
    int best_dy_val  = 0;
    for (const auto& [dy, votes] : hist) {
        if (votes > best_votes) {
            second_votes = best_votes;
            best_votes   = votes;
            best_dy_val  = dy;
        } else if (votes > second_votes) {
            second_votes = votes;
        }
    }

    ScrollResult res;
    res.dy    = best_dy_val;
    res.votes = best_votes;

    // Two-part validity gate:
    //   1. Absolute floor: at least 10% of samples agree on this dy.
    //   2. Dominance: best must beat runner-up by ≥ 50%.
    //      Static/identical frames produce a nearly flat histogram because gray
    //      blocks match at ANY dy offset; the ratio catches that ambiguity.
    const int min_votes   = std::max(5, collected / 10);
    const int second_floor = std::max(1, second_votes);
    res.valid = (res.votes >= min_votes)
             && (res.votes >= second_floor * 3 / 2);   // ≥ 1.5× runner-up

    // If dominant bin is 0 (or ±1 rounding noise), treat as no scroll
    if (std::abs(res.dy) <= 1 && res.valid) {
        res.dy    = 0;
        res.valid = false;
    }

    // If the best dy lands exactly on the search boundary it means the actual
    // scroll exceeded MAX_SCROLL and was silently clamped — the value is not
    // reliable.  Reject it rather than letting a capped estimate corrupt the
    // document coordinate accumulation.
    if (std::abs(res.dy) == MAX_SCROLL) {
        res.dy    = 0;
        res.valid = false;
    }

    return res;
}
