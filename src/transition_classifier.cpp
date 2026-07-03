#include "transition_classifier.hpp"

// ── scroll_explains ───────────────────────────────────────────────────────────
// Vote histograms can be fooled by periodic content (stripes, text lines) into
// a "valid" dy on a page change. A genuine scroll must EXPLAIN the transition:
// after shifting prev by dy, the overlapping non-fixed area must be nearly
// unchanged (measured: ≤0.04 for true scrolls, 0.27 for a false one).
bool TransitionClassifier::scroll_explains(const cv::Mat& prev_gray,
                                           const cv::Mat& curr_gray,
                                           int dy, const cv::Mat& fixed_mask)
{
    const int H = curr_gray.rows;
    if (std::abs(dy) >= H) return false;

    // curr(y) corresponds to prev(y + dy); take the overlapping row ranges.
    const int overlap = H - std::abs(dy);
    const cv::Rect prev_roi(0, std::max(dy, 0),  curr_gray.cols, overlap);
    const cv::Rect curr_roi(0, std::max(-dy, 0), curr_gray.cols, overlap);

    cv::Mat diff, smooth, changed;
    cv::absdiff(prev_gray(prev_roi), curr_gray(curr_roi), diff);
    cv::blur(diff, smooth, cv::Size(3, 3));
    cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);

    double area = static_cast<double>(diff.total());
    if (!fixed_mask.empty()) {
        const cv::Mat mask_roi = fixed_mask(curr_roi);
        changed.setTo(0, mask_roi);
        area -= cv::countNonZero(mask_roi);
    }
    if (area <= 0.0) return false;

    return cv::countNonZero(changed) / area <= SCROLL_RESIDUAL_MAX;
}

Transition TransitionClassifier::classify(const cv::Mat& prev_gray,
                                          const cv::Mat& curr_gray,
                                          const cv::Mat& fixed_mask)
{
    cv::Mat diff;
    cv::absdiff(prev_gray, curr_gray, diff);
    if (cv::mean(diff)[0] < IDENTICAL_FRAME_MAD)
        return {TransitionType::Identical, 0};

    ScrollResult s = scroll_.detect(prev_gray, curr_gray, fixed_mask);
    if (s.valid && s.dy != 0 && scroll_explains(prev_gray, curr_gray, s.dy, fixed_mask))
        return {TransitionType::Scroll, s.dy};

    // No scroll explains the change — measure how much of the scrollable area
    // actually changed. 3×3 blur suppresses single-pixel compression noise.
    cv::Mat smooth, changed;
    cv::blur(diff, smooth, cv::Size(3, 3));
    cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);

    double area = static_cast<double>(diff.total());
    if (!fixed_mask.empty()) {
        changed.setTo(0, fixed_mask);               // fixed chrome doesn't count
        area -= cv::countNonZero(fixed_mask);
    }
    if (area <= 0.0) return {TransitionType::Unstable, 0};

    double fraction = cv::countNonZero(changed) / area;
    return {fraction > PAGE_CHANGE_MIN_FRACTION ? TransitionType::PageChange
                                                : TransitionType::Unstable, 0};
}
