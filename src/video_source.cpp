#include "video_source.hpp"

VideoSource::VideoSource(const std::string& path) : cap_(path)
{
    if (!cap_.isOpened()) return;
    width_  = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
    height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
    fps_    = cap_.get(cv::CAP_PROP_FPS);
    if (fps_ <= 0.0) fps_ = 30.0;   // defensive: some containers omit fps
}

bool VideoSource::next(Frame& out)
{
    if (!cap_.read(out.bgr) || out.bgr.empty()) return false;
    out.index = next_index_++;
    out.t     = static_cast<double>(out.index) / fps_;
    return true;
}
