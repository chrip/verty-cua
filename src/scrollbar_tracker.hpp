#pragma once
#include "types.hpp"
#include <vector>

// One reading of the vertical scrollbar in one frame.
struct ScrollbarReading {
    bool valid     = false;
    int  thumb_top = 0;   // viewport row of the thumb's topmost pixel
    int  thumb_h   = 0;   // thumb height — a fingerprint of the DOCUMENT height
};

// Reads the browser's vertical scrollbar out of the pixels.
//
// Why this exists: incremental scroll tracking is a chain of frame-to-frame
// deltas, and the chain BREAKS when a jump is larger than the shared-pixel
// window (`viewport − header − footer`). Content matching against the page's
// canvas rescues a jump that lands in already-seen territory, but a fling into
// UNSEEN territory leaves no content evidence anywhere. The scrollbar is the
// one remaining pixel signal that states the absolute position outright.
//
// Detection is deliberately structural rather than colour-based (browsers,
// themes and platforms all differ, and the thumb changes colour while hovered
// or dragged — measured 192 idle, 166 hover, 118 dragged on one session): a
// scrollbar column carries exactly TWO gray levels and the darker one forms a
// SINGLE contiguous run. Both properties are checked per frame, and a column
// must look like that in most observed frames before the band is accepted.
//
// Usage: observe() every frame of one pass, locate() once, then read() any
// frame. A session without a scrollbar (our CDP-recorded lab fixtures) simply
// never locates one, and every consumer falls back to content evidence.
class ScrollbarTracker {
public:
    ScrollbarTracker(int frame_w, int frame_h);

    // Evidence for locating the band: one call per frame (grayscale).
    void observe(const cv::Mat& gray);

    // Decide the band from the evidence gathered so far; returns located().
    bool locate();

    [[nodiscard]] bool     located() const { return located_; }
    [[nodiscard]] cv::Rect band() const { return band_; }   // viewport coords

    // Read the thumb in one frame. Invalid if the band is not located, or if
    // this frame does not show the two-level structure (a page that cannot
    // scroll has no thumb; a modal overlay can cover the bar).
    [[nodiscard]] ScrollbarReading read(const cv::Mat& gray) const;

private:
    // Evidence gathered per candidate column (indexed from the right edge).
    struct ColumnEvidence {
        int  frames  = 0;   // frames in which the column looked two-level
        int  min_top = 0;
        int  max_top = 0;
        bool seen    = false;
    };

    int      frame_w_;
    int      frame_h_;
    bool     located_ = false;
    cv::Rect band_;
    int      observed_ = 0;
    std::vector<ColumnEvidence> cols_;

    // Two-level test for one column; on success fills the dark run.
    [[nodiscard]] bool column_run(const cv::Mat& gray, int x,
                                  ScrollbarReading& run) const;
};
