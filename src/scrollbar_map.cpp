#include "scrollbar_map.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

namespace {

double median_of(std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return v[mid];
}

}   // namespace

ScrollbarMap::ScrollbarMap(int frame_h) : frame_h_(frame_h) {}

bool ScrollbarMap::add(const ScrollbarReading& r, long vt)
{
    if (!r.valid || vt < 0) return false;
    // Only a map whose slope was actually FITTED may refuse a contradicting
    // anchor. The refusal exists to protect a well-determined map from an offset
    // chain that broke — but a map still carrying the geometric fallback slope,
    // because its anchors so far sat at one thumb position, has nothing to
    // protect and everything to learn. Measured: on rw1 the first visit never
    // scrolls, so eight anchors land on one thumb position and fix a geometric
    // slope of 13.28; the deep third visit then travels 222 thumb px with a true
    // slope of 14.01, every one of its anchors contradicts, and the map rejected
    // them all until it invalidated itself — losing the document-height estimate
    // on precisely the session that most needs it (4736px captured of ~13000).
    if (valid_ && slope_fitted_ && matches(r)
        && std::abs(static_cast<double>(vt) - (a_ * r.thumb_top + b_)) > SB_MAX_RESIDUAL_PX) {
        ++rejected_;
        return false;
    }
    anchors_.push_back({r.thumb_top, r.thumb_h, vt});
    refit();
    return true;
}

// ── refit ─────────────────────────────────────────────────────────────────────
// Recompute the map from every anchor that shares the dominant thumb height.
void ScrollbarMap::refit()
{
    valid_        = false;
    slope_fitted_ = false;
    used_         = 0;
    residual_     = 0.0;
    if (anchors_.empty() || frame_h_ <= 0) return;

    // Fingerprint = the dominant thumb height. A document that GREW mid-session
    // (lazy-loaded images, infinite feed) shrinks its thumb, so its anchors
    // carry several heights; the dominant one wins and the rest are dropped
    // rather than averaged into a map that describes neither document state.
    int best_h = 0, best_n = 0;
    for (const Anchor& a : anchors_) {
        int n = 0;
        for (const Anchor& b : anchors_)
            if (std::abs(b.thumb_h - a.thumb_h) <= SB_THUMB_H_TOL) ++n;
        if (n > best_n) { best_n = n; best_h = a.thumb_h; }
    }
    fingerprint_ = best_h;
    if (fingerprint_ <= 0) return;

    std::vector<Anchor> fit;
    for (const Anchor& a : anchors_)
        if (std::abs(a.thumb_h - fingerprint_) <= SB_THUMB_H_TOL) fit.push_back(a);
    used_ = static_cast<int>(fit.size());
    if (used_ < SB_MIN_CAL_POINTS) return;

    int lo = fit.front().thumb_top, hi = lo;
    for (const Anchor& a : fit) { lo = std::min(lo, a.thumb_top); hi = std::max(hi, a.thumb_top); }
    const double a_geo = static_cast<double>(frame_h_) / fingerprint_;

    if (hi - lo >= SB_MIN_CAL_SPREAD) {
        // Least squares over the anchors. Preferred where they span enough
        // track: it absorbs the thumb's sub-pixel rendering, which the integer
        // thumb height cannot express (measured: max residual 7.0px fitted vs
        // 14.9px geometric on rw4's deep page).
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        const double n = static_cast<double>(used_);
        for (const Anchor& a : fit) {
            const double x = a.thumb_top, y = static_cast<double>(a.vt);
            sx += x; sy += y; sxx += x * x; sxy += x * y;
        }
        const double denom = n * sxx - sx * sx;
        if (denom <= 0.0) return;
        const double a_fit = (n * sxy - sx * sy) / denom;
        // Sanity gate: the slope is fixed by scrollbar geometry, so a fit that
        // disagrees with it means either the column is not a scrollbar or the
        // anchor offsets themselves are wrong. Either way the map is refused —
        // a wrong absolute placement is worse than no placement at all.
        if (std::abs(a_fit - a_geo) > SB_SLOPE_TOL_FRAC * a_geo) return;
        a_            = a_fit;
        slope_fitted_ = true;
    } else {
        // Anchors all sit at nearly one thumb position: there is no baseline to
        // fit a slope against, so geometry supplies it and the anchors only fix
        // the intercept.
        a_ = a_geo;
    }

    std::vector<double> offs;
    offs.reserve(fit.size());
    for (const Anchor& a : fit) offs.push_back(static_cast<double>(a.vt) - a_ * a.thumb_top);
    b_ = median_of(offs);   // median, not mean: one bad anchor must not tilt the map

    for (const Anchor& a : fit)
        residual_ = std::max(residual_, std::abs(static_cast<double>(a.vt) - (a_ * a.thumb_top + b_)));
    if (residual_ > SB_MAX_RESIDUAL_PX) return;

    // A map that had to refuse a large share of its own anchors is not
    // describing this document's scrollbar at all.
    const double offered = static_cast<double>(used_ + rejected_);
    if (offered > 0 && rejected_ / offered > SB_MAX_REJECT_FRAC) return;

    valid_ = true;
}

long ScrollbarMap::predict(int thumb_top) const
{
    if (!valid_) return 0;
    return std::max(0L, static_cast<long>(std::lround(a_ * thumb_top + b_)));
}

long ScrollbarMap::uncertainty(long predicted) const
{
    if (!valid_) return 0;
    long lo = LONG_MAX, hi = LONG_MIN;
    for (const Anchor& a : anchors_) {
        if (std::abs(a.thumb_h - fingerprint_) > SB_THUMB_H_TOL) continue;
        lo = std::min(lo, a.vt);
        hi = std::max(hi, a.vt);
    }
    long beyond = 0;
    if (lo != LONG_MAX) beyond = std::max({0L, lo - predicted, predicted - hi});
    return static_cast<long>(std::lround(std::max(residual_, a_)
                                         + SB_SLOPE_ERR_FRAC * static_cast<double>(beyond)));
}

bool ScrollbarMap::matches(const ScrollbarReading& r) const
{
    return valid_ && r.valid && std::abs(r.thumb_h - fingerprint_) <= SB_THUMB_H_TOL;
}

// ── document_height ───────────────────────────────────────────────────────────
// vt = a·thumb_top + b, so vt = 0 puts the thumb at track_top = −b/a. Assuming
// the arrow buttons at the two ends are the same size, the track spans
// frame_h − 2·track_top, and the thumb's deepest position is
// track_top + track_h − thumb_h. Feeding that back through the map gives the
// deepest reachable offset, and the document is one viewport taller than that.
long ScrollbarMap::document_height() const
{
    // The slope must have been FITTED from real thumb travel. With the geometric
    // fallback the intercept is pinned but the slope is an assumption, and
    // extrapolating it the full length of a track magnifies the assumption
    // without bound: measured on rw3's single-viewport page 1, which never
    // scrolled, the fallback slope produced 15232px for a 960px document. A
    // height nobody can check is worse than an admitted unknown.
    if (!valid_ || !slope_fitted_ || a_ <= 0.0 || fingerprint_ <= 0) return 0;
    const double track_top = -b_ / a_;
    const double track_h    = static_cast<double>(frame_h_) - 2.0 * track_top;
    if (track_h <= fingerprint_) return 0;             // nonsense geometry
    const double deepest_thumb = track_top + track_h - fingerprint_;
    const double vt_max        = a_ * deepest_thumb + b_;
    if (vt_max < 0.0) return 0;
    return static_cast<long>(std::lround(vt_max + frame_h_));
}

long ScrollbarMap::document_height_uncertainty() const
{
    const long doc = document_height();
    if (doc <= 0) return 0;
    // The map's own error at that offset, plus a·SB_TRACK_SYMMETRY_PX for the
    // assumption that both arrow buttons are the same size.
    return uncertainty(doc - frame_h_)
           + static_cast<long>(std::lround(a_ * SB_TRACK_SYMMETRY_PX));
}
