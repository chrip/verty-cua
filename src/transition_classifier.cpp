#include "transition_classifier.hpp"
#include <algorithm>
#include <vector>

// ── scroll_explains ───────────────────────────────────────────────────────────
// Vote histograms can be fooled by periodic content (stripes, text lines) into
// a "valid" dy on a page change. A genuine scroll must EXPLAIN the transition:
// of the pixels that CHANGED between the frames (unshifted), nearly all must be
// explained by the dy shift. Restricting to unshifted-changed pixels keeps
// chrome and whitespace out of the denominator (they never changed), so the
// check needs no fixed mask; the per-band MEDIAN makes it robust against a
// bounded animated region, which only dirties its own bands.
// Measured (maskless): true scrolls ≤ 0.002; false scrolls 0.114 and 0.999.
bool TransitionClassifier::scroll_explains(const cv::Mat& prev_gray,
                                           const cv::Mat& curr_gray,
                                           int dy, const cv::Mat& ignore_mask)
{
    const int H = curr_gray.rows;
    const int W = curr_gray.cols;
    if (std::abs(dy) >= H) return false;

    // curr(y) corresponds to prev(y + dy); overlapping row ranges.
    const int overlap = H - std::abs(dy);
    const cv::Rect prev_roi(0, std::max(dy, 0),  W, overlap);
    const cv::Rect curr_roi(0, std::max(-dy, 0), W, overlap);

    auto changed_mask = [](const cv::Mat& a, const cv::Mat& b) {
        cv::Mat diff, smooth, changed;
        cv::absdiff(a, b, diff);
        cv::blur(diff, smooth, cv::Size(3, 3));
        cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
        return changed;
    };

    // Both in curr coordinates: what changed without the shift, and what still
    // differs under the dy hypothesis.
    cv::Mat changed0 = changed_mask(prev_gray(curr_roi), curr_gray(curr_roi));
    cv::Mat changedS = changed_mask(prev_gray(prev_roi), curr_gray(curr_roi));
    if (!ignore_mask.empty())
        changed0.setTo(0, ignore_mask(curr_roi));

    cv::Mat unexplained;
    cv::bitwise_and(changed0, changedS, unexplained);

    std::vector<double> fracs;
    std::vector<int>    quiet;   // bands with no measurable residual
    for (int y = 0; y + RESIDUAL_BAND_H <= overlap; y += RESIDUAL_BAND_H) {
        const cv::Rect band(0, y, W, RESIDUAL_BAND_H);
        const int n = cv::countNonZero(changed0(band));
        if (n < 200) { quiet.push_back(y); continue; }   // no evidence in it
        fracs.push_back(static_cast<double>(cv::countNonZero(unexplained(band))) / n);
    }
    // A real scroll changes many rows; if nothing changed anywhere, a nonzero
    // dy claim has no supporting evidence.
    if (fracs.empty()) return false;

    // A quiet band carries no residual, but for a nonzero dy it is not neutral
    // either: content moves through EVERY band, so a band with texture that
    // stayed put is evidence against the hypothesis. Enough of the informative
    // bands must therefore have changed at all — otherwise the median can be
    // carried by a couple of flat-on-flat coincidences (measured: an in-place
    // panel swap 320px below a same-shade header produced a perfect median at
    // dy = −320 from 3 changed bands out of 18, on all ten repetitions).
    //
    // Only the quiet bands need their texture measured, and only while the
    // ratio can still fall below the bar — a scroll that changed most of its
    // bands passes before any stddev is computed.
    // changed / informative ≥ FRAC  ⟺  informative ≤ changed / FRAC
    const double max_informative =
        static_cast<double>(fracs.size()) / SCROLL_MIN_CHANGED_BAND_FRAC;
    if (static_cast<double>(fracs.size() + quiet.size()) > max_informative) {
        double informative = static_cast<double>(fracs.size());
        for (int y : quiet) {
            cv::Scalar mean, stddev;
            cv::meanStdDev(curr_gray(curr_roi)(cv::Rect(0, y, W, RESIDUAL_BAND_H)), mean, stddev);
            if (stddev[0] < IDENT_MIN_STDDEV) continue;   // flat: nothing to show
            if (++informative > max_informative) return false;
        }
    }

    std::nth_element(fracs.begin(), fracs.begin() + fracs.size() / 2, fracs.end());
    return fracs[fracs.size() / 2] <= SCROLL_RESIDUAL_MEDIAN_MAX;
}

Transition TransitionClassifier::classify(const cv::Mat& prev_gray,
                                          const cv::Mat& curr_gray,
                                          const cv::Mat& fixed_mask,
                                          const cv::Mat& dynamic_mask)
{
    cv::Mat diff;
    cv::absdiff(prev_gray, curr_gray, diff);
    if (cv::mean(diff)[0] < IDENTICAL_FRAME_MAD)
        return {TransitionType::Identical, 0};

    // If everything that changed lies inside confirmed persistent noise (a
    // playing fixed video ad), the page itself did not move: Identical. This
    // also lets the segmenter's debounce window close during ad churn.
    if (!dynamic_mask.empty()) {
        cv::Mat masked = diff.clone();
        masked.setTo(0, dynamic_mask);
        const double area = static_cast<double>(diff.total())
                            - cv::countNonZero(dynamic_mask);
        if (area > 0 && cv::sum(masked)[0] / area < IDENTICAL_FRAME_MAD)
            return {TransitionType::Identical, 0};
    }

    // The vote histogram proposes a dy; the explained-fraction veto decides.
    // The proposal is used even when the histogram's own validity gates fail:
    // animated content steals votes and can break the dominance gate on a
    // perfectly genuine scroll — the veto is the reliable arbiter for both
    // false positives AND weak-vote true positives.
    // The veto is deliberately MASKLESS: chrome and whitespace drop out of the
    // unshifted-changed denominator on their own. Passing a pixel mask would
    // exclude stable whitespace and CONCENTRATE the evidence on periodic
    // regions (stripes, tiled images), letting an alias dy that explains only
    // those regions pass — measured on the revisit back-navigation, where a
    // masked veto accepted an alias and hid the page change entirely.
    ScrollResult s = scroll_.detect(prev_gray, curr_gray, fixed_mask);
    for (int dy : s.proposals)
        if (scroll_explains(prev_gray, curr_gray, dy, dynamic_mask))
            return {TransitionType::Scroll, dy};

    // No scroll explains the change. A PAGE CHANGE is a change SPREAD over the
    // viewport: count the horizontal bands that changed significantly. Spread
    // needs no chrome mask — shared chrome merely leaves its bands unchanged —
    // so it also works on sessions that never scrolled (where the online
    // chrome estimate cannot learn). A concentrated change (cookie banner,
    // animation, popup) touches few bands and stays Unstable under Normal.
    cv::Mat smooth, changed;
    cv::blur(diff, smooth, cv::Size(3, 3));
    cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
    cv::Mat ignore;   // chrome + persistent noise carry no page-change signal
    if      (fixed_mask.empty())   ignore = dynamic_mask;
    else if (dynamic_mask.empty()) ignore = fixed_mask;
    else                           cv::bitwise_or(fixed_mask, dynamic_mask, ignore);
    if (!ignore.empty())
        changed.setTo(0, ignore);

    int changed_bands = 0, bands = 0;
    for (int y = 0; y + RESIDUAL_BAND_H <= changed.rows; y += RESIDUAL_BAND_H) {
        const cv::Rect band(0, y, changed.cols, RESIDUAL_BAND_H);
        double area = static_cast<double>(band.area());
        if (!ignore.empty()) area -= cv::countNonZero(ignore(band));
        if (area < 1000) continue;                 // band is (almost) all chrome
        ++bands;
        if (cv::countNonZero(changed(band)) / area > BAND_CHANGED_MIN_FRAC)
            ++changed_bands;
    }
    if (bands == 0) return {TransitionType::Unstable, 0};

    const double threshold = sensitivity_ == Sensitivity::High
                                 ? PAGE_CHANGE_HIGH_SPREAD
                                 : PAGE_CHANGE_MIN_SPREAD;
    const double spread = static_cast<double>(changed_bands) / bands;
    return {spread > threshold ? TransitionType::PageChange
                               : TransitionType::Unstable, 0};
}
