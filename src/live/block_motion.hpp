#pragma once
#include <vector>

#include <opencv2/core.hpp>

// ── Where did the changed pixels come from? ──────────────────────────────────
// The change mask says WHERE the screen changed. It cannot say whether the new
// pixels are new: a dragged card, a list that closed up behind it, a pane that
// scrolled its own content, a dialog that moved -- each lights up the mask
// exactly like freshly drawn content, and a verifier reading geometry alone has
// no notion of "the same thing, somewhere else".
//
// Screen encoders answer this question for compression, and the answer carries
// over unchanged. AV1 (libaom's hash_motion.c) and AV2 hash every block at
// every pixel position of the reference frame and look the current block up,
// rather than searching for it: screen content moves by whole pixels and is
// copied exactly, so an exact match is not merely likely but the normal case.
// The CUA frames are lossless PNG screenshots, which is the setting those tools
// were built for.
//
// The lookup runs in the cheap direction. Only the changed, textured grid
// blocks of `after` are queried, typically a few hundred; they go into a small
// table, and one rolling-hash pass over `before` checks every window position
// against it. Cost is one pass over the frame regardless of how much changed.
//
// A changed block found in `before` at another position MOVED; one found
// nowhere is NEW. Blocks that moved by the same vector are grouped into a
// cluster -- one object, or one list closing up -- with the rectangle it
// occupied before and the one it occupies now.
struct MotionCluster {
    cv::Point mv;        // displacement, after - before
    int       blocks = 0;
    cv::Rect  src;       // where those blocks were in `before`
    cv::Rect  dst;       // where they are in `after`
};

struct BlockMotion {
    int changed_blocks = 0;   // grid blocks touched by the change mask
    int textured_blocks = 0;  // of those, textured enough to identify
    int moved_blocks = 0;     // found in `before` at a different position
    int new_blocks = 0;       // found nowhere in `before`
    int ambiguous_blocks = 0; // repeated too often in `before` to place
    // Spatially connected groups of moved blocks sharing one vector, largest
    // first. Groups of fewer than kMinClusterBlocks blocks are dropped: two
    // glyph blocks that happen to recur elsewhere are coincidence, not an object.
    std::vector<MotionCluster> clusters;

    static constexpr int kMinClusterBlocks = 3;
};

// Both frames single-channel, same size. `changed` is the System-0 mask
// (absdiff -> 3x3 blur -> SAD_THRESHOLD), frame-sized CV_8U.
BlockMotion measure_motion(const cv::Mat& before_gray, const cv::Mat& after_gray,
                           const cv::Mat& changed);
