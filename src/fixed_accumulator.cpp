#include "fixed_accumulator.hpp"

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
