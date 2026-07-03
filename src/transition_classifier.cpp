#include "transition_classifier.hpp"

Transition TransitionClassifier::classify(const cv::Mat& prev_gray,
                                          const cv::Mat& curr_gray,
                                          const cv::Mat& fixed_mask)
{
    cv::Mat diff;
    cv::absdiff(prev_gray, curr_gray, diff);
    if (cv::mean(diff)[0] < IDENTICAL_FRAME_MAD)
        return {TransitionType::Identical, 0};

    ScrollResult s = scroll_.detect(prev_gray, curr_gray, fixed_mask);
    if (s.valid && s.dy != 0)
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
