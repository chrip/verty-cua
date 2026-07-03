#pragma once
#include "stitcher.hpp"
#include <string>

// Writes the machine-readable session report (JSON).
//
// Milestone 2 first cut of the output contract: video info, transition tally,
// fixed-element rectangles, and the scroll-event trace with timestamps.
// The schema grows toward the full contract in Roadmap M6 (pages, visits,
// navigation events, confidence fields).
class SessionReport {
public:
    // Returns false if the file could not be written.
    static bool write(const std::string& path,
                      const std::string& video_path,
                      const Stitcher& stitcher);
};
