#include "perception/video_source.hpp"

VideoSource::VideoSource(const std::string& path, const cv::Rect& roi) : cap_(path)
{
    if (!cap_.isOpened()) return;
    width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    // Clipped once here, so no caller has to think about a rect that runs off the
    // frame — and an empty result disables cropping rather than yielding nothing.
    roi_ = roi & cv::Rect(0, 0, width_, height_);
    if (roi_.width < 1 || roi_.height < 1) roi_ = cv::Rect();
    fps_    = cap_.get(cv::CAP_PROP_FPS);
    if (fps_ <= 0.0) fps_ = 30.0;   // defensive: some containers omit fps
}

bool VideoSource::next(Frame& out)
{
    if (!cap_.read(out.bgr) || out.bgr.empty()) return false;
    if (roi_.width) out.bgr = out.bgr(roi_).clone();
    out.index = next_index_++;
    out.t     = static_cast<double>(out.index) / fps_;
    return true;
}

bool VideoSource::skip_to(long index)
{
    cv::Mat discard;
    while (next_index_ < index) {
        if (!cap_.read(discard) || discard.empty()) return false;
        ++next_index_;
    }
    return true;
}
