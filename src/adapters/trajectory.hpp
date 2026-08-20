#pragma once
#include <string>
#include <vector>

// One normalized CUA step: the state before an action, the action, the state
// after it. Every trajectory source (trycua bridge, Qwen-CUA demo, MiniWoB++)
// is reduced to this by a Python adapter in adapters/, which writes a TSV
// manifest. Keeping the source-specific mess in Python buys two things: the
// vision tool stays free of a JSON dependency, and a new source is a new
// script rather than a new parser here.
//
// The manifest is TSV with a header line naming the columns, so column order
// is not load-bearing:
//
//   index  action  x  y  dy  text  before  after
//
// Missing numeric fields are written as an empty cell, not 0 -- a click at
// (0,0) and a click with no coordinate are different things.
struct TrajectoryStep {
    int         index = 0;
    std::string action;          // click, double_click, right_click, type, key,
                                 // scroll, drag, wait, ... -- source vocabulary
                                 // normalized by the adapter.
    bool        has_coord = false;
    int         x = 0, y = 0;    // action coordinate, frame pixels
    bool        has_dy = false;
    int         dy = 0;          // requested scroll amount, if the source says
    std::string text;            // typed text, for `type`
    std::string before;          // path to the frame before the action
    std::string after;           // path to the frame after it
};

struct Trajectory {
    std::string                  id;
    std::string                  source;
    std::vector<TrajectoryStep>  steps;
};

// Throws std::runtime_error on a malformed manifest.
Trajectory load_manifest(const std::string& path);
