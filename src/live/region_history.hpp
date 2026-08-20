#pragma once
#include <vector>

#include <opencv2/opencv.hpp>

// ── Per-region visual history ────────────────────────────────────────────────
// Verty already answers "have I been on this page before?" at page level
// (PageIdentifier), and SubframeTracker already models a region as having
// several stable states. This applies the same idea per BLOCK_SIZE block across
// a CUA trajectory, so a change can be told apart by its history rather than
// only by its magnitude:
//
//   NOVEL    this block has never looked like this before
//   REVISIT  this block has looked exactly like this before -- an earlier state
//            was re-established (a menu closed, a dialog dismissed, an edit
//            undone, a hover ended)
//
// A binary "changed / unchanged" mask cannot distinguish "a dropdown opened"
// from "the dropdown closed again": both are simply change. The distinction is
// available from pixels alone, with no model, because the evidence is already
// on disk -- it is just never looked up.
//
// Memory stays small because only DISTINCT appearances are kept per block, and
// most blocks (chrome, whitespace, static content) only ever have one.
enum class BlockClass : unsigned char {
    Unchanged = 0,
    Novel     = 1,   // new appearance -- render green
    Revisit   = 2,   // a previously-seen appearance returned -- render red
};

struct RegionChangeSummary {
    int novel_blocks   = 0;
    int revisit_blocks = 0;
    int changed_blocks = 0;   // novel + revisit
};

class RegionHistory {
public:
    // `mad_max`   : how close a block must be to a stored appearance to count as
    //               the same state. IDENT_MATCH_MAD (9.0) is the value Verty
    //               already uses to decide a block matches remembered content.
    // `max_states`: cap on distinct appearances kept per block. A block that
    //               churns endlessly (a clock, a spinner) would otherwise grow
    //               without bound; past the cap it stops learning, and its
    //               changes read as Novel, which is the honest answer for a
    //               region whose history is not summarizable.
    explicit RegionHistory(double mad_max = 9.0, int max_states = 8)
        : mad_max_(mad_max), max_states_(max_states) {}

    // Record a frame as observed history without classifying anything. Used to
    // seed the first frame of a trajectory.
    void observe(const cv::Mat& gray);

    // Classify every changed block of `after` against everything seen so far,
    // then fold `after` into the history. `changed` is the System-0 mask.
    // Returns a per-block label image (CV_8U, one pixel per block).
    cv::Mat classify_and_observe(const cv::Mat& after_gray, const cv::Mat& changed,
                                 RegionChangeSummary& out);

    // Upscale a per-block label image back to pixel resolution, for rendering.
    static cv::Mat expand(const cv::Mat& block_labels, cv::Size frame_size);

private:
    void ensure_grid(const cv::Mat& gray);
    bool seen_before(int bx, int by, const cv::Mat& block) const;
    void remember(int bx, int by, const cv::Mat& block);

    double mad_max_;
    int    max_states_;
    int    cols_ = 0, rows_ = 0;
    // states_[by * cols_ + bx] : the distinct appearances of that block.
    std::vector<std::vector<cv::Mat>> states_;
};
