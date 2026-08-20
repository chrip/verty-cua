#include "perception/page_identifier.hpp"
#include <map>
#include <random>

Identification PageIdentifier::match(const cv::Mat& frame_gray,
                                     const cv::Mat& canvas_gray,
                                     const cv::Rect& interior,
                                     const cv::Mat& noise_mask) const
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
        const cv::Rect r(px, py, BLOCK_SIZE, BLOCK_SIZE);
        if (!noise_mask.empty()
            && cv::mean(noise_mask(r))[0] / 255.0 > 0.25) continue;   // ad: no identity
        const cv::Mat block = frame_gray(r);

        // Near-uniform blocks match anywhere — no page-identity signal.
        cv::Scalar mean, stddev;
        cv::meanStdDev(block, mean, stddev);
        if (stddev[0] < IDENT_MIN_STDDEV) continue;
        ++usable;

        // Best vertical match of this block in its own canvas column.
        long  best_y   = -1;
        float best_mad = std::numeric_limits<float>::max();
        for (int cy = 0; cy + BLOCK_SIZE <= canvas_gray.rows; ++cy) {
            // No Mat header per candidate row — this loop runs once per canvas
            // row per sampled block and is the identifier's whole cost.
            const float mad = patch_mad_at(canvas_gray, px, cy, block);
            if (mad < best_mad) { best_mad = mad; best_y = cy; }
        }
        // Vote only on NEAR-IDENTICAL matches (not merely similar): a weak
        // match lands at whatever offset minimises MAD, scattering votes and
        // masking the true peak's concentration. The tight bar keeps the vote
        // histogram clean so concentration is meaningful.
        if (best_y >= 0 && best_mad < IDENT_MATCH_MAD)
            votes[best_y - py]++;
    }

    if (usable == 0 || votes.empty()) return id;

    // Peak with slack: a candidate offset's score counts all votes within
    // ±IDENT_VT_SLACK px (stitch quantization / compression jitter).
    long best_vt = 0;
    int  best_score = 0, total_votes = 0;
    for (const auto& [vt, n] : votes) {
        total_votes += n;
        int score = 0;
        for (long s = vt - IDENT_VT_SLACK; s <= vt + IDENT_VT_SLACK; ++s) {
            auto it = votes.find(s);
            if (it != votes.end()) score += it->second;
        }
        if (score > best_score) { best_score = score; best_vt = vt; }
    }
    if (best_score < 5) return id;   // no coherent offset at all

    // Concentration = share of ALL matching votes that agree on this single
    // offset. A true revisit — even one whose content partly changed (a news
    // page that reloaded some articles) — has its surviving blocks clustered
    // at ONE offset, so concentration is high. Two DIFFERENT pages sharing a
    // chrome template collide at MANY offsets (the template repeats), so their
    // votes scatter and concentration stays low even when a best-offset overlap
    // fraction looks deceptively high. Measured 2026-07-05: rw1 reload 0.6-0.7
    // vs the revisit_noscroll sibling-page collision 0.34.
    id.concentration = static_cast<double>(best_score) / total_votes;

    // Region verification at vt0: walk a grid over the frame↔canvas
    // INTERSECTION and count textured, non-noise blocks that match (min MAD
    // over ±IDENT_VT_SLACK shifts). Robust against partial content change:
    // popped-in images fail individually without sinking the whole match.
    const long vt0 = std::max(0L, best_vt);
    int checked = 0, matched = 0;
    for (int py = interior.y; py + BLOCK_SIZE <= interior.y + interior.height; py += 48) {
        const long cy = vt0 + py;
        if (cy < 0 || cy + BLOCK_SIZE > canvas_gray.rows) continue;
        for (int px = interior.x; px + BLOCK_SIZE <= interior.x + interior.width; px += 48) {
            const cv::Rect r(px, py, BLOCK_SIZE, BLOCK_SIZE);
            if (!noise_mask.empty()
                && cv::mean(noise_mask(r))[0] / 255.0 > 0.25) continue;
            const cv::Mat block = frame_gray(r);
            cv::Scalar mean, stddev;
            cv::meanStdDev(block, mean, stddev);
            if (stddev[0] < IDENT_MIN_STDDEV) continue;
            ++checked;
            float best_mad = std::numeric_limits<float>::max();
            for (long s = -IDENT_VT_SLACK; s <= IDENT_VT_SLACK; ++s) {
                const long y = cy + s;
                if (y < 0 || y + BLOCK_SIZE > canvas_gray.rows) continue;
                best_mad = std::min(best_mad,
                                    patch_mad_at(canvas_gray, px, static_cast<int>(y), block));
            }
            if (best_mad < IDENT_MATCH_MAD) ++matched;   // near-identical, not just similar
        }
    }
    if (checked < 12) return id;   // intersection too small to judge

    id.vt0           = vt0;
    id.confidence    = static_cast<double>(matched) / checked;
    id.matched_blocks = matched;
    // A REVISIT needs BOTH a strong overlap where the pages intersect AND a
    // concentrated offset (or, for a heavily-reloaded page, a large absolute
    // cluster of near-identical blocks at one offset even if the surviving
    // fraction dips below the overlap bar).
    id.matched = (id.confidence >= REVISIT_MIN_CONFIDENCE
                  || id.matched_blocks >= REVISIT_MIN_MATCH_BLOCKS)
                 && id.concentration >= REVISIT_MIN_CONCENTRATION;
    return id;
}
