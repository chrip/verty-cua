#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
#include "scrollbar_map.hpp"
#include "scrollbar_tracker.hpp"
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
    // scrollbar: located vertical scrollbar of the session, or nullptr. Used to
    // calibrate this page's thumb→offset map, which lets a later visit be
    // re-localized after a jump that content matching cannot follow.
    PageStitcher(std::string video_path, int frame_w, int frame_h, double fps,
                 bool verbose = false, int page_id = 0,
                 const ScrollbarTracker* scrollbar = nullptr);

    // Analyze one visit of this page (runs both analysis passes).
    //
    // bridged marks a visit that CONTINUES the previous one after a jump too
    // large for frame-to-frame matching: it emits the jump as one large scroll
    // event, because that is what it was. vt_uncertainty is the declared error
    // of vt_start (0 when the offset came from content, non-zero when it was
    // re-localized from the scrollbar).
    void add_visit(const PageVisit& visit, long vt_start,
                   bool bridged = false, long vt_uncertainty = 0);

    // Merge all visits into the page canvas (callable after every add_visit).
    [[nodiscard]] cv::Mat reconstruct();

    // ── Page state (valid after add_visit / reconstruct) ────────────────────
    struct VisitTrace {
        PageVisit         visit;
        long              vt_start = 0;
        long              vt_uncertainty = 0;   // declared error of vt_start (px)
        bool              bridged  = false;     // entered by a re-localized jump
        std::vector<long> vt_by_frame;   // absolute doc coords per LOCAL index
    };

    // A document row range no frame ever showed. A jump skips content, and the
    // skipped rows CANNOT be reconstructed from pixels that were never
    // captured — so they are left blank and declared here rather than being
    // silently passed off as page content.
    struct Gap {
        long from = 0;   // inclusive
        long to   = 0;   // exclusive
    };

    [[nodiscard]] const std::vector<VisitTrace>&  visits() const { return visits_; }
    [[nodiscard]] const std::vector<FixedRect>&   fixed_rects() const { return rects_; }
    [[nodiscard]] const std::vector<ScrollEvent>& scroll_events() const { return scroll_events_; }
    // Interior viewport-fixed elements (floating buttons etc.), viewport coords.
    [[nodiscard]] const std::vector<cv::Rect>& overlays() const { return overlays_; }
    // Confirmed persistent-noise mask (fixed video ads), viewport coords.
    [[nodiscard]] const cv::Mat& noise_mask() const { return noise_mask_; }
    [[nodiscard]] long doc_height() const { return max_vt_ + frame_h_; }
    // Document rows never observed (valid after reconstruct()).
    [[nodiscard]] const std::vector<Gap>& gaps() const { return gaps_; }
    // This page's calibrated scrollbar map (invalid if never calibrated).
    [[nodiscard]] const ScrollbarMap& scrollbar_map() const { return sb_map_; }

private:
    std::string video_path_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;
    int         page_id_;

    TransitionClassifier classifier_;
    FixedAccumulator     accumulator_;           // chrome evidence, all visits
    DynamicAccumulator   noise_accumulator_;     // persistent-noise evidence
    const ScrollbarTracker* scrollbar_ = nullptr;   // session-wide, not owned
    ScrollbarMap         sb_map_;                // this page's thumb→offset map
    std::vector<Gap>     gaps_;                  // never-observed document rows
    std::vector<VisitTrace> visits_;
    long max_vt_ = 0;                    // deepest viewport top over all visits
    long min_vt_ = -1;                   // shallowest (-1 = none yet)
    std::vector<ScrollEvent> scroll_events_;

    cv::Mat fixed_mask_;       // frame_h × frame_w, CV_8UC1 (0/255)
    cv::Mat noise_mask_;       // confirmed persistent noise (video ads)
    cv::Mat first_frame_;      // frame at the shallowest vt — source for header/sidebars
    cv::Mat last_frame_;       // frame at the deepest vt    — source for the footer

    std::vector<FixedRect> rects_;
    std::vector<cv::Rect>  overlays_;
    int header_h_ = 0, footer_h_ = 0, left_w_ = 0, right_w_ = 0;

    // Opens the video positioned at the given visit's first frame.
    [[nodiscard]] VideoSource open_at(const PageVisit& v) const;

    void analyze_visit(const PageVisit& v, long vt_start);   // pass 1
    void refine_visit(const PageVisit& v, long vt_start,     // pass 2
                      bool bridged, long vt_uncertainty);

    void extract_rects();
    void extract_overlays();
    void add_overlay(const cv::Rect& o);   // merge into the page-wide overlay list
    void append_scroll_events(const VisitTrace& trace);
    void stamp(cv::Mat& canvas, const FixedRect& fr, long doc_h) const;
};
