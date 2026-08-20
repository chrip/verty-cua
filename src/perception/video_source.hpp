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

    // `roi` restricts every frame to a rectangle of it. That is what makes the
    // pipeline recursive: a subframe is not a special case downstream, it is the
    // same pipeline reading a smaller frame. An empty rect means the whole frame.
    explicit VideoSource(const std::string& path, const cv::Rect& roi = cv::Rect());

    [[nodiscard]] bool   ok() const     { return cap_.isOpened(); }
    [[nodiscard]] int    width() const  { return roi_.width  ? roi_.width  : width_; }
    [[nodiscard]] int    height() const { return roi_.height ? roi_.height : height_; }
    [[nodiscard]] double fps() const    { return fps_; }

    // Reads the next frame; returns false at end of stream.
    bool next(Frame& out);

    // Decode-and-discard until the next frame to be read has `index`.
    // Sequential (no codec seeking) — deterministic on every container.
    // Returns false if the stream ends first.
    bool skip_to(long index);

private:
    cv::VideoCapture cap_;
    int    width_      = 0;
    int    height_     = 0;
    double fps_        = 0.0;
    long   next_index_ = 0;
    cv::Rect roi_;
};
