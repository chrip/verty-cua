#pragma once
#include "types.hpp"
#include "scroll_detector.hpp"
#include <string>
#include <vector>

// How a fixed region is anchored in the viewport.
enum class FixedKind { Header, Footer, Left, Right };

struct FixedRect {
    cv::Rect  rect;   // bounding box in viewport coordinates
    FixedKind kind;
};

// Reconstructs a full-page screenshot from a screen recording of a scrolling
// session, in three passes over the video:
//
//   1. analyze()        — accumulate the position-fixed pixel mask and a rough
//                         per-frame scroll offset, then reduce the mask to
//                         header / footer / left / right fixed-band rectangles.
//   2. refine_scrolls() — re-detect the scroll offsets, now excluding the fixed
//                         areas from the search so only scrolling content votes.
//   3. reconstruct()    — copy each frame's scrolling interior into the tall
//                         document canvas at its scroll offset, then stamp the
//                         fixed bands on top: header at the top, footer at the
//                         bottom, sidebars spanning the full document height.
//
// Construct with a video path, check ok(), then call run().
class Stitcher {
public:
    explicit Stitcher(std::string video_path, bool verbose = false);

    // True once the video opened and its frame dimensions are known.
    [[nodiscard]] bool ok() const { return ok_; }

    // Runs all three passes; returns the reconstructed image (empty on failure).
    [[nodiscard]] cv::Mat run();

private:
    std::string video_path_;
    bool        verbose_;
    bool        ok_      = false;
    int         frame_w_ = 0;
    int         frame_h_ = 0;

    ScrollDetector    scroll_;
    std::vector<long> vt_by_frame_;   // viewport top (doc coords) per frame index
    long              max_vt_ = 0;

    cv::Mat stable_counter_;   // frame_h × frame_w, CV_32SC1
    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat first_frame_;      // frame at the scroll top    — source for header/sidebars
    cv::Mat last_frame_;       // frame at the scroll bottom — source for the footer

    std::vector<FixedRect> rects_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    [[nodiscard]] cv::VideoCapture open() const;
    void    analyze();          // pass 1
    void    refine_scrolls();   // pass 2
    cv::Mat reconstruct();      // pass 3

    void accumulate_fixed(const cv::Mat& prev_gray, const cv::Mat& curr_gray);
    void extract_rects();
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;

    [[nodiscard]] long doc_height() const { return max_vt_ + frame_h_; }
};
