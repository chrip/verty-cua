#pragma once
#include "types.hpp"
#include "fixed_accumulator.hpp"
#include "page_stitcher.hpp"
#include "session_segmenter.hpp"
#include <string>
#include <vector>

// Orchestrates a whole session (Roadmap M3):
//
//   1. Classify every frame transition (session pass, no fixed mask yet).
//   2. Segment the transition stream into page visits (SessionSegmenter).
//   3. Run one PageStitcher per visit — each reconstructs its page's
//      full-page screenshot and reports its scroll events / fixed rects.
//
// One page visit per detected page; REVISIT recognition (merging a returning
// page into its earlier stitch) is Roadmap M4.
class SessionProcessor {
public:
    explicit SessionProcessor(std::string video_path, bool verbose = false);

    [[nodiscard]] bool ok() const { return ok_; }

    // Runs the whole pipeline; returns false if nothing could be processed.
    [[nodiscard]] bool run();

    // One reconstructed page (valid after run()).
    struct PageResult {
        PageVisit visit;
        cv::Mat   image;
        long      docHeight = 0;
        std::vector<FixedRect>   fixedRects;
        std::vector<ScrollEvent> scrollEvents;   // global frames / timestamps
    };

    // ── Session outputs (valid after run()) ─────────────────────────────────
    [[nodiscard]] const std::vector<PageResult>& pages() const { return pages_; }
    [[nodiscard]] const std::vector<long>& nav_frames() const { return nav_frames_; }
    [[nodiscard]] const TransitionCounts& transition_counts() const { return counts_; }

    [[nodiscard]] long   frame_count() const { return frame_count_; }
    [[nodiscard]] int    frame_w() const { return frame_w_; }
    [[nodiscard]] int    frame_h() const { return frame_h_; }
    [[nodiscard]] double fps() const { return fps_; }

private:
    std::string video_path_;
    bool        verbose_;
    bool        ok_          = false;
    int         frame_w_     = 0;
    int         frame_h_     = 0;
    double      fps_         = 0.0;
    long        frame_count_ = 0;

    TransitionClassifier    classifier_;
    TransitionCounts        counts_;       // session-level (segmentation pass)
    std::vector<PageResult> pages_;
    std::vector<long>       nav_frames_;

    // Session pass: classification of the transition into every frame
    // (index 0 is a placeholder).
    [[nodiscard]] std::vector<Transition> classify_transitions();
};
