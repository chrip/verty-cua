#pragma once
#include "types.hpp"
#include "transition_classifier.hpp"
#include "video_source.hpp"
#include <string>
#include <vector>

// How a fixed region is anchored in the viewport.
enum class FixedKind { Header, Footer, Left, Right };

struct FixedRect {
    cv::Rect  rect;   // bounding box in viewport coordinates
    FixedKind kind;
};

// One detected scroll step (from the mask-aware pass).
struct ScrollEvent {
    long   frame = 0;   // frame index at which the viewport reached vt
    double t     = 0.0; // seconds since video start (frame / fps)
    int    dy    = 0;   // scroll delta vs previous frame (positive = down)
    long   vt    = 0;   // viewport top in document coordinates after the step
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
// Construct with a video path, check ok(), then call run(). After run() the
// session outputs (scroll events, transition counts, fixed rects) are readable
// via the accessors below.
class Stitcher {
public:
    explicit Stitcher(std::string video_path, bool verbose = false);

    // True once the video opened and its frame dimensions are known.
    [[nodiscard]] bool ok() const { return ok_; }

    // Runs all three passes; returns the reconstructed image (empty on failure).
    [[nodiscard]] cv::Mat run();

    // ── Session outputs (valid after run()) ─────────────────────────────────
    [[nodiscard]] const std::vector<FixedRect>&   fixed_rects() const { return rects_; }
    [[nodiscard]] const std::vector<ScrollEvent>& scroll_events() const { return scroll_events_; }
    [[nodiscard]] const TransitionCounts& transition_counts() const { return counts_; }

    [[nodiscard]] long   doc_height() const { return max_vt_ + frame_h_; }
    [[nodiscard]] long   frame_count() const { return static_cast<long>(vt_by_frame_.size()); }
    [[nodiscard]] int    frame_w() const { return frame_w_; }
    [[nodiscard]] int    frame_h() const { return frame_h_; }
    [[nodiscard]] double fps() const { return fps_; }

private:
    std::string video_path_;
    bool        verbose_;
    bool        ok_      = false;
    int         frame_w_ = 0;
    int         frame_h_ = 0;
    double      fps_     = 0.0;

    TransitionClassifier classifier_;
    TransitionCounts     counts_;         // tallied in the mask-aware pass
    std::vector<long>    vt_by_frame_;    // viewport top (doc coords) per frame index
    long                 max_vt_ = 0;
    std::vector<ScrollEvent> scroll_events_;

    cv::Mat stable_counter_;   // frame_h × frame_w, CV_32SC1
    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat first_frame_;      // frame at the scroll top    — source for header/sidebars
    cv::Mat last_frame_;       // frame at the scroll bottom — source for the footer

    std::vector<FixedRect> rects_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    void    analyze();          // pass 1
    void    refine_scrolls();   // pass 2
    cv::Mat reconstruct();      // pass 3

    void accumulate_fixed(const cv::Mat& prev_gray, const cv::Mat& curr_gray);
    void extract_rects();
    void build_scroll_events();
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;
};
