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
                 bool verbose, Sensitivity sensitivity = Sensitivity::Normal,
                 const ScrollbarTracker* scrollbar = nullptr);

    struct Assignment {
        int         pageId     = 0;
        long        vtStart    = 0;
        double      confidence = 0.0;    // 0 for the first visit / new pages
        std::string navType;             // "NAVIGATE" | "REVISIT" ("" for visit 0)
        // True when the visit is a CONTINUATION of the page it followed, entered
        // by a scroll jump too large for frame-to-frame matching. It is not a
        // navigation at all, so it produces no navigation event — the jump is
        // reported as one large scroll event instead.
        bool        continuation = false;
        long        vtUncertainty = 0;   // declared error of vtStart (px)
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
    // Non-const, for the one caller that has to reach a stitcher after
    // segmentation: stamping a page's in-place regions needs their settled states,
    // which are only known once every visit has been seen.
    [[nodiscard]] std::vector<PageRecord>& records() { return records_; }

    // Handed to every page stitcher this registry creates (see
    // PageStitcher::set_subframes): those stitchers run their own classifier
    // passes and need the same knowledge of in-place regions as the session pass.
    void set_subframes(std::vector<cv::Rect> rects) { subframes_ = std::move(rects); }

private:
    std::string video_path_;
    int         frame_w_;
    int         frame_h_;
    double      fps_;
    bool        verbose_;
    Sensitivity sensitivity_;

    const ScrollbarTracker* scrollbar_ = nullptr;
    std::vector<cv::Rect>   subframes_;   // session-wide, not owned
    PageIdentifier          identifier_;
    std::vector<PageRecord> records_;
    int                     last_page_ = -1;        // page of the previous visit

    // The scroll area of a page within the viewport (inside its fixed bands).
    [[nodiscard]] cv::Rect interior_of(const PageRecord& rec) const;

    // Intra-page re-localization: is this frame the page we were already on,
    // seen at a document offset its canvas does not cover yet? Fills vt/
    // uncertainty and returns true if so. See the implementation for the guards.
    [[nodiscard]] bool continues_last_page(const cv::Mat& first_frame_gray,
                                           long& vt, long& uncertainty) const;
};
