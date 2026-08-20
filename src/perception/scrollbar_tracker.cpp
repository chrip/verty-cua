#include "perception/scrollbar_tracker.hpp"
#include <algorithm>

namespace {

// Median gray per row over a column band. A median, not a mean: the thumb has
// rounded corners and a 1px outline, which a mean would smear across the level
// boundary and blur the run's start/end by a pixel or two.
void band_profile(const cv::Mat& gray, const cv::Rect& band,
                  std::vector<uint8_t>& out)
{
    out.resize(static_cast<size_t>(band.height));
    std::vector<uint8_t> row(static_cast<size_t>(band.width));
    for (int y = 0; y < band.height; ++y) {
        const uint8_t* p = gray.ptr<uint8_t>(band.y + y) + band.x;
        std::copy(p, p + band.width, row.begin());
        std::nth_element(row.begin(), row.begin() + row.size() / 2, row.end());
        out[static_cast<size_t>(y)] = row[row.size() / 2];
    }
}

struct TwoLevel {
    bool ok    = false;
    int  dark  = 0;
    int  light = 0;
};

// A scrollbar column is bimodal: one track level and one thumb level. Bins
// within SB_LEVEL_TOL of a peak belong to the SAME level (anti-aliasing, JPEG
// ringing), otherwise a single level splits into neighbouring bins and the
// second "peak" is just its own skirt.
TwoLevel two_levels(const std::vector<uint8_t>& prof)
{
    TwoLevel r;
    if (prof.empty()) return r;

    int hist[256] = {0};
    for (uint8_t v : prof) ++hist[v];

    auto peak = [&](int exclude) {
        int best = -1, best_n = 0;
        for (int v = 0; v < 256; ++v) {
            if (exclude >= 0 && std::abs(v - exclude) <= SB_LEVEL_TOL) continue;
            if (hist[v] > best_n) { best_n = hist[v]; best = v; }
        }
        return best;
    };
    auto level_count = [&](int c) {
        int n = 0;
        for (int v = std::max(0, c - SB_LEVEL_TOL); v <= std::min(255, c + SB_LEVEL_TOL); ++v)
            n += hist[v];
        return n;
    };

    const int p1 = peak(-1);
    if (p1 < 0) return r;
    const int p2 = peak(p1);
    if (p2 < 0) return r;
    if (std::abs(p1 - p2) < SB_MIN_CONTRAST) return r;
    if (static_cast<double>(level_count(p1) + level_count(p2))
            / static_cast<double>(prof.size()) < SB_LEVEL_COVER) return r;

    r.ok    = true;
    r.dark  = std::min(p1, p2);
    r.light = std::max(p1, p2);
    return r;
}

// The thumb is the ONE contiguous run of the darker level. Returns the longest
// run plus the share of all dark pixels it contains: a real bar concentrates
// them in that single run (the arrow glyphs are the only others), while a
// textured content column scatters them over the whole height.
//
// The thumb is assumed DARKER than its track — true for every platform
// scrollbar in the corpus (measured 192/166/118 thumb vs 239 track). A theme
// that inverts this simply fails to locate a bar, and every consumer falls
// back to content evidence; that degradation is deliberate, not an oversight.
ScrollbarReading dark_run(const std::vector<uint8_t>& prof, int dark, double& purity)
{
    ScrollbarReading best;
    const int n = static_cast<int>(prof.size());
    int total = 0, start = -1;
    for (int y = 0; y <= n; ++y) {
        const bool is_dark =
            y < n && std::abs(static_cast<int>(prof[static_cast<size_t>(y)]) - dark) <= SB_LEVEL_TOL;
        if (is_dark) {
            ++total;
            if (start < 0) start = y;
        } else if (start >= 0) {
            if (y - start > best.thumb_h) { best.thumb_top = start; best.thumb_h = y - start; }
            start = -1;
        }
    }
    purity     = total > 0 ? static_cast<double>(best.thumb_h) / total : 0.0;
    best.valid = best.thumb_h >= SB_MIN_THUMB_H && best.thumb_h <= n - SB_MIN_THUMB_H;
    return best;
}

}   // namespace

// ── construction ──────────────────────────────────────────────────────────────
ScrollbarTracker::ScrollbarTracker(int frame_w, int frame_h)
    : frame_w_(frame_w), frame_h_(frame_h)
{
    cols_.assign(static_cast<size_t>(std::min(SB_SEARCH_COLS, std::max(0, frame_w_))), {});
}

// ── column_run ────────────────────────────────────────────────────────────────
bool ScrollbarTracker::column_run(const cv::Mat& gray, int x,
                                  ScrollbarReading& run) const
{
    std::vector<uint8_t> prof;
    band_profile(gray, cv::Rect(x, 0, 1, frame_h_), prof);

    const TwoLevel lv = two_levels(prof);
    if (!lv.ok) return false;

    double purity = 0.0;
    run = dark_run(prof, lv.dark, purity);
    return run.valid && purity >= SB_RUN_PURITY;
}

// ── observe ───────────────────────────────────────────────────────────────────
void ScrollbarTracker::observe(const cv::Mat& gray)
{
    if (located_ || gray.empty() || gray.cols != frame_w_ || gray.rows != frame_h_) return;
    ++observed_;
    for (size_t i = 0; i < cols_.size(); ++i) {
        const int x = frame_w_ - 1 - static_cast<int>(i);
        if (x < 0) break;
        ScrollbarReading run;
        if (!column_run(gray, x, run)) continue;
        ColumnEvidence& e = cols_[i];
        ++e.frames;
        if (!e.seen) { e.seen = true; e.min_top = e.max_top = run.thumb_top; }
        e.min_top = std::min(e.min_top, run.thumb_top);
        e.max_top = std::max(e.max_top, run.thumb_top);
    }
}

// ── locate ────────────────────────────────────────────────────────────────────
// The band is the run of scrollbar-like columns anchored at the right edge. A
// few non-bar columns are tolerated at the very edge (window border, the bar's
// own anti-aliased outline); anything further in is content and the run stops.
//
// A column qualifies only if it is two-level in most frames AND its dark run
// MOVED. Without the movement test, any static dark block touching the right
// edge qualifies — the lab fixtures' fixed 72px header did, yielding a
// confident 14px "scrollbar" whose map then had to be thrown out by the
// calibration gates. The width ceiling rejects the other family of false
// positives: page content that spans the viewport width (a cookie banner, a
// full-width header) makes a "band" far wider than any real bar.
bool ScrollbarTracker::locate()
{
    if (located_ || observed_ == 0) return located_;

    const int cols   = static_cast<int>(cols_.size());
    const int needed = static_cast<int>(SB_BAR_FRAME_FRAC * observed_);
    auto qualifies = [&](int i) {
        const ColumnEvidence& e = cols_[static_cast<size_t>(i)];
        return e.frames >= needed && e.max_top - e.min_top >= SB_MIN_THUMB_TRAVEL;
    };

    int i = 0;
    while (i < cols && i < SB_EDGE_SLACK && !qualifies(i)) ++i;
    if (i >= cols || !qualifies(i)) return false;

    const int outer = i;
    while (i < cols && qualifies(i)) ++i;
    const int width = i - outer;
    if (width < SB_MIN_WIDTH || width > SB_MAX_WIDTH) return false;

    band_    = cv::Rect(frame_w_ - i, 0, width, frame_h_);
    located_ = true;
    return true;
}

// ── read ──────────────────────────────────────────────────────────────────────
ScrollbarReading ScrollbarTracker::read(const cv::Mat& gray) const
{
    ScrollbarReading r;
    if (!located_ || gray.empty() || gray.cols != frame_w_ || gray.rows != frame_h_) return r;

    std::vector<uint8_t> prof;
    band_profile(gray, band_, prof);

    const TwoLevel lv = two_levels(prof);
    if (!lv.ok) return r;

    double purity = 0.0;
    r = dark_run(prof, lv.dark, purity);
    if (purity < SB_RUN_PURITY) r.valid = false;
    return r;
}
