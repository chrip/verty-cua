#pragma once
#include "session_processor.hpp"
#include <string>
#include <vector>

// Writes the machine-readable session report (JSON).
//
// Milestone 3 cut of the output contract: video info, session transition
// tally, one entry per detected page (stitch file, geometry, visit frame
// range, fixed-element rectangles), navigation events, and the scroll-event
// trace with per-event page id. The schema grows toward the full contract in
// Roadmap M6 (REVISIT navigation type and confidence fields arrive with M4).
class SessionReport {
public:
    // stitch_paths[i] = output image written for pages()[i].
    // Returns false if the file could not be written.
    // subframe_state_paths[i][j] = image written for subframes()[i].states[j],
    // empty where nothing was written (a subframe classified as dynamic noise
    // keeps no state).
    static bool write(const std::string& path,
                      const std::string& video_path,
                      const SessionProcessor& session,
                      const std::vector<std::string>& stitch_paths,
                      const std::vector<std::vector<std::string>>& subframe_state_paths = {},
                      const std::vector<std::vector<std::string>>& subframe_doc_paths = {});
};
