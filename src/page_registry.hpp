#pragma once
#include "types.hpp"
#include "page_identifier.hpp"
#include "page_stitcher.hpp"
#include <memory>
#include <string>
#include <vector>

// Owns the session's distinct pages and assigns every visit to one of them
// (Roadmap M4).
//
// For each visit after the first, the visit's first stable frame is matched
// against every known page's canvas (PageIdentifier). A match makes the visit
// a REVISIT: it resumes that page's PageStitcher at the recovered entry
// offset, extending/refreshing the existing stitch. No match (or a
// low-confidence one) starts a new page — a duplicate stitch is a milder
// failure than merging two different pages into one canvas.
class PageRegistry {
public:
    PageRegistry(std::string video_path, int frame_w, int frame_h, double fps,
                 bool verbose);

    struct Assignment {
        int         pageId     = 0;
        long        vtStart    = 0;
        double      confidence = 0.0;    // 0 for the first visit / new pages
        std::string navType;             // "NAVIGATE" | "REVISIT" ("" for visit 0)
    };

    // Identify the visit (by its first stable frame), fold it into the right
    // PageStitcher, and refresh that page's canvas.
    Assignment assign(const PageVisit& visit, const cv::Mat& first_frame_gray);

    // One record per DISTINCT page.
    struct PageRecord {
        std::unique_ptr<PageStitcher> stitcher;
        cv::Mat canvas;        // current reconstruction (refreshed per visit)
        cv::Mat canvas_gray;
    };

    [[nodiscard]] const std::vector<PageRecord>& records() const { return records_; }

private:
    std::string video_path_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;

    PageIdentifier          identifier_;
    std::vector<PageRecord> records_;

    // The scroll area of a page within the viewport (inside its fixed bands).
    [[nodiscard]] cv::Rect interior_of(const PageRecord& rec) const;
};
