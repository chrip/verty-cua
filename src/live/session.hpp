#pragma once
#include <opencv2/core.hpp>

#include "live/region_history.hpp"

// ── What carries across steps ────────────────────────────────────────────────
// M0 and M1 deliberately treated every frame pair as independent, so their
// numbers could not be explained away by hidden state. That bought a clean
// baseline and cost real signal: several action kinds are only interpretable
// relative to what came before.
//
// The clearest case is the keyboard. A key press has no coordinate, so a
// stateless verifier has nowhere to look and hands every one of them to the
// VLM -- 17 steps in the labelled corpus, of which 15 were in fact ordinary
// dropdown navigation producing a small change exactly where the previous click
// had put the focus. The information needed was in the trajectory, one step back.
struct SessionState {
    // Distinct block appearances seen so far -- novel vs revisit.
    RegionHistory history;

    // Where the user's attention was last placed. Keyboard actions inherit it:
    // typing and arrow keys act on whatever the last click focused, so this is
    // the point their change should be near.
    bool      has_focus = false;
    cv::Point focus;

    // Consecutive steps that introduced no new content at all. A run of these is
    // an agent making no progress -- see the stuck-detector note below. Survives focus
    // rings and caret blinks, which "nothing changed" does not.
    int no_novel_run = 0;

    int steps = 0;

    // Called after each step is measured.
    void advance(bool had_coord, cv::Point coord, int novel_blocks)
    {
        if (had_coord) { focus = coord; has_focus = true; }
        no_novel_run = (novel_blocks == 0) ? no_novel_run + 1 : 0;
        ++steps;
    }
};
