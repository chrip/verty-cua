#include "perception/fixed_accumulator.hpp"
#include <cmath>

FixedAccumulator::FixedAccumulator(int frame_w, int frame_h)
{
    counter_ = cv::Mat::zeros(frame_h, frame_w, CV_32SC1);
}

void FixedAccumulator::accumulate(const cv::Mat& prev_gray, const cv::Mat& curr_gray)
{
    cv::Mat diff, smooth;
    cv::absdiff(prev_gray, curr_gray, diff);
    cv::blur(diff, smooth, cv::Size(3, 3));

    cv::Mat stable = smooth < SAD_THRESHOLD;      // CV_8U, 255 where unchanged
    counter_.setTo(0, ~stable);                   // reset the pixels that moved
    cv::add(counter_, 1, counter_, stable);       // ++ the stable ones
}

void FixedAccumulator::reset()
{
    counter_.setTo(0);
}

cv::Mat FixedAccumulator::mask() const
{
    cv::Mat m;
    cv::compare(counter_, FIXED_CONFIRM_FRAMES, m, cv::CMP_GE);
    return m;
}


// ── DynamicAccumulator ────────────────────────────────────────────────────────
DynamicAccumulator::DynamicAccumulator(int frame_w, int frame_h)
{
    counter_ = cv::Mat::zeros(frame_h, frame_w, CV_32SC1);
}

void DynamicAccumulator::accumulate(const cv::Mat& prev_gray, const cv::Mat& curr_gray)
{
    cv::Mat diff, smooth;
    cv::absdiff(prev_gray, curr_gray, diff);
    cv::blur(diff, smooth, cv::Size(3, 3));

    cv::Mat changed = smooth > SAD_THRESHOLD;   // moved WITHOUT a scroll
    cv::add(counter_, 1, counter_, changed);
    ++observations_;                            // the denominator of the rate
}

void DynamicAccumulator::seed(const cv::Mat& mask)
{
    if (mask.empty()) return;
    counter_.setTo(DYNAMIC_CONFIRM_FRAMES, mask);
    mask.copyTo(seeded_);
}

cv::Mat DynamicAccumulator::mask() const
{
    cv::Mat m;
    cv::compare(counter_, DYNAMIC_CONFIRM_FRAMES, m, cv::CMP_GE);

    // Two gates, both required: an absolute floor (a handful of changes is never
    // evidence of anything, however short the session) and a RATE — the pixel
    // must have changed in DYNAMIC_MIN_CHANGE_RATE of the non-scroll transitions
    // it was actually observed in. Without the rate, "changes often" and "the
    // session is long" are the same measurement, and a long tap-through session
    // masks its own content. See DYNAMIC_MIN_CHANGE_RATE for the measurements.
    if (observations_ > 0) {
        const int min_by_rate =
            static_cast<int>(std::ceil(DYNAMIC_MIN_CHANGE_RATE * static_cast<double>(observations_)));
        if (min_by_rate > DYNAMIC_CONFIRM_FRAMES) {
            cv::Mat by_rate;
            cv::compare(counter_, min_by_rate, by_rate, cv::CMP_GE);
            // Seeded pixels are exempt: a completed first pass already confirmed
            // them, and the second pass's partial observation count must not
            // un-confirm them for its own early frames.
            if (!seeded_.empty()) cv::bitwise_or(by_rate, seeded_, by_rate);
            cv::bitwise_and(m, by_rate, m);
        }
    }

    if (cv::countNonZero(m) > DYNAMIC_MASK_MAX_FRACTION * m.total())
        return {};
    return m;
}
