#pragma once
#include <opencv2/opencv.hpp>
#include <string>

// Sequential frame reader for a session video — the single place that knows
// about decoding. Yields frames together with their index and timestamp
// (t = index / fps), so downstream code never touches cv::VideoCapture.
class VideoSource {
public:
    struct Frame {
        cv::Mat bgr;         // decoded BGR frame
        long    index = 0;   // 0-based frame index
        double  t     = 0.0; // seconds since video start (index / fps)
    };

    explicit VideoSource(const std::string& path);

    [[nodiscard]] bool   ok() const     { return cap_.isOpened(); }
    [[nodiscard]] int    width() const  { return width_; }
    [[nodiscard]] int    height() const { return height_; }
    [[nodiscard]] double fps() const    { return fps_; }

    // Reads the next frame; returns false at end of stream.
    bool next(Frame& out);

private:
    cv::VideoCapture cap_;
    int    width_      = 0;
    int    height_     = 0;
    double fps_        = 0.0;
    long   next_index_ = 0;
};
