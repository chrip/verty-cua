#include "page_identifier.hpp"
#include <map>
#include <random>

Identification PageIdentifier::match(const cv::Mat& frame_gray,
                                     const cv::Mat& canvas_gray,
                                     const cv::Rect& interior) const
{
    Identification id;
    if (interior.width < BLOCK_SIZE || interior.height < BLOCK_SIZE) return id;
    if (canvas_gray.rows < BLOCK_SIZE) return id;

    // Deterministic sampling inside the interior (block-aligned top-left).
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist_x(interior.x, interior.x + interior.width  - BLOCK_SIZE);
    std::uniform_int_distribution<int> dist_y(interior.y, interior.y + interior.height - BLOCK_SIZE);

    std::map<long, int> votes;   // vt0 → count
    int usable = 0;
    const int max_attempts = IDENT_SAMPLES * 8;

    for (int a = 0; a < max_attempts && usable < IDENT_SAMPLES; ++a) {
        const int px = dist_x(rng);
        const int py = dist_y(rng);
        const cv::Mat block = frame_gray(cv::Rect(px, py, BLOCK_SIZE, BLOCK_SIZE));

        // Near-uniform blocks match anywhere — no page-identity signal.
        cv::Scalar mean, stddev;
        cv::meanStdDev(block, mean, stddev);
        if (stddev[0] < IDENT_MIN_STDDEV) continue;
        ++usable;

        // Best vertical match of this block in its own canvas column.
        long  best_y   = -1;
        float best_mad = std::numeric_limits<float>::max();
        for (int cy = 0; cy + BLOCK_SIZE <= canvas_gray.rows; ++cy) {
            const float mad = patch_mad(canvas_gray(cv::Rect(px, cy, BLOCK_SIZE, BLOCK_SIZE)), block);
            if (mad < best_mad) { best_mad = mad; best_y = cy; }
        }
        if (best_y >= 0 && best_mad < SAD_THRESHOLD)
            votes[best_y - py]++;
    }

    if (usable == 0 || votes.empty()) return id;

    // Peak with slack: a candidate offset's score counts all votes within
    // ±IDENT_VT_SLACK px (stitch quantization / compression jitter).
    long best_vt = 0;
    int  best_score = 0;
    for (const auto& [vt, _] : votes) {
        int score = 0;
        for (long s = vt - IDENT_VT_SLACK; s <= vt + IDENT_VT_SLACK; ++s) {
            auto it = votes.find(s);
            if (it != votes.end()) score += it->second;
        }
        if (score > best_score) { best_score = score; best_vt = vt; }
    }

    id.vt0        = std::max(0L, best_vt);
    id.confidence = static_cast<double>(best_score) / usable;
    id.matched    = id.confidence >= REVISIT_MIN_CONFIDENCE;
    return id;
}
