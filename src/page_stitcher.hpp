#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
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
    long   frame = 0;   // GLOBAL frame index at which the viewport reached vt
    double t     = 0.0; // seconds since video start (frame / fps)
    int    dy    = 0;   // scroll delta vs previous frame (positive = down)
    long   vt    = 0;   // viewport top in document coordinates after the step
};

// Reconstructs the full-page screenshot of ONE page visit — a frame range
// [visit.firstFrame, visit.lastFrame] of the session video — in three passes:
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
// The SessionProcessor owns segmentation and constructs one PageStitcher per
// visit. Scroll events are reported with GLOBAL frame indices and timestamps.
class PageStitcher {
public:
    PageStitcher(std::string video_path, PageVisit visit,
                 int frame_w, int frame_h, double fps,
                 bool verbose = false, int page_id = 0);

    // Runs all three passes; returns the reconstructed image (empty on failure).
    [[nodiscard]] cv::Mat run();

    // ── Page outputs (valid after run()) ────────────────────────────────────
    [[nodiscard]] const std::vector<FixedRect>&   fixed_rects() const { return rects_; }
    [[nodiscard]] const std::vector<ScrollEvent>& scroll_events() const { return scroll_events_; }
    [[nodiscard]] long doc_height() const { return max_vt_ + frame_h_; }
    [[nodiscard]] const PageVisit& visit() const { return visit_; }

private:
    std::string video_path_;
    PageVisit   visit_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;
    int         page_id_;

    TransitionClassifier classifier_;
    FixedAccumulator     accumulator_;
    std::vector<long>    vt_by_frame_;   // viewport top per LOCAL frame index
    long                 max_vt_ = 0;
    std::vector<ScrollEvent> scroll_events_;

    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat first_frame_;      // frame at the scroll top    — source for header/sidebars
    cv::Mat last_frame_;       // frame at the scroll bottom — source for the footer

    std::vector<FixedRect> rects_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    // Opens the video positioned at the visit's first frame (nullopt-like:
    // returned source is !ok() on failure).
    [[nodiscard]] VideoSource open_at_visit() const;
    // True while the frame belongs to this visit.
    [[nodiscard]] bool in_visit(long index) const { return index <= visit_.lastFrame; }

    void    analyze();          // pass 1
    void    refine_scrolls();   // pass 2
    cv::Mat reconstruct();      // pass 3

    void extract_rects();
    void build_scroll_events();
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;
};
