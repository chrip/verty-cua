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

// Reconstructs the full-page screenshot of ONE page, assembled from one or
// more visits (a visit is a contiguous stable frame range; a REVISIT of the
// same page merges into the same stitcher — Roadmap M4).
//
// Per visit, two analysis passes over the visit's frames:
//   1. analyze_visit  — accumulate the position-fixed pixel mask (shared
//                       across all visits of the page) and a rough per-frame
//                       scroll offset, then refresh the fixed-band rectangles.
//   2. refine_visit   — re-detect the scroll offsets with the fixed areas
//                       excluded from the search; authoritative offsets.
//
// A visit enters the document at vt_start: 0 for the first visit, or the
// entry offset recovered by the PageIdentifier for a revisit (browsers
// restore the scroll position, so a revisit may start mid-document).
//
// reconstruct() merges all visits into one canvas: every visit's scrolling
// interior is copied at its absolute offsets, then the fixed bands are
// stamped on top (header at the top, footer at the bottom, sidebars spanning
// the full document height).
class PageStitcher {
public:
    PageStitcher(std::string video_path, int frame_w, int frame_h, double fps,
                 bool verbose = false, int page_id = 0);

    // Analyze one visit of this page (runs both analysis passes).
    void add_visit(const PageVisit& visit, long vt_start);

    // Merge all visits into the page canvas (callable after every add_visit).
    [[nodiscard]] cv::Mat reconstruct();

    // ── Page state (valid after add_visit / reconstruct) ────────────────────
    struct VisitTrace {
        PageVisit         visit;
        long              vt_start = 0;
        std::vector<long> vt_by_frame;   // absolute doc coords per LOCAL index
    };

    [[nodiscard]] const std::vector<VisitTrace>&  visits() const { return visits_; }
    [[nodiscard]] const std::vector<FixedRect>&   fixed_rects() const { return rects_; }
    [[nodiscard]] const std::vector<ScrollEvent>& scroll_events() const { return scroll_events_; }
    // Interior viewport-fixed elements (floating buttons etc.), viewport coords.
    [[nodiscard]] const std::vector<cv::Rect>& overlays() const { return overlays_; }
    [[nodiscard]] long doc_height() const { return max_vt_ + frame_h_; }

private:
    std::string video_path_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;
    int         page_id_;

    TransitionClassifier classifier_;
    FixedAccumulator     accumulator_;   // chrome evidence, shared by all visits
    std::vector<VisitTrace> visits_;
    long max_vt_ = 0;                    // deepest viewport top over all visits
    long min_vt_ = -1;                   // shallowest (-1 = none yet)
    std::vector<ScrollEvent> scroll_events_;

    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat first_frame_;      // frame at the shallowest vt — source for header/sidebars
    cv::Mat last_frame_;       // frame at the deepest vt    — source for the footer

    std::vector<FixedRect> rects_;
    std::vector<cv::Rect>  overlays_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    // Opens the video positioned at the given visit's first frame.
    [[nodiscard]] VideoSource open_at(const PageVisit& v) const;

    void analyze_visit(const PageVisit& v, long vt_start);   // pass 1
    void refine_visit(const PageVisit& v, long vt_start);    // pass 2

    void extract_rects();
    void extract_overlays();
    void append_scroll_events(const VisitTrace& trace);
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;
};
