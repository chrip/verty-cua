#include "page_registry.hpp"
#include <iostream>

PageRegistry::PageRegistry(std::string video_path, int frame_w, int frame_h,
                           double fps, bool verbose, Sensitivity sensitivity)
    : video_path_(std::move(video_path)),
      frame_w_(frame_w), frame_h_(frame_h), fps_(fps), verbose_(verbose),
      sensitivity_(sensitivity)
{
}

cv::Rect PageRegistry::interior_of(const PageRecord& rec) const
{
    int header = 0, footer = 0, left = 0, right = 0;
    for (const FixedRect& fr : rec.stitcher->fixed_rects()) {
        switch (fr.kind) {
        case FixedKind::Header: header = fr.rect.height; break;
        case FixedKind::Footer: footer = fr.rect.height; break;
        case FixedKind::Left:   left   = fr.rect.width;  break;
        case FixedKind::Right:  right  = fr.rect.width;  break;
        }
    }
    return {left, header, frame_w_ - left - right, frame_h_ - header - footer};
}

PageRegistry::Assignment
PageRegistry::assign(const PageVisit& visit, const cv::Mat& first_frame_gray)
{
    Assignment a;

    // Identify against every known page; the most confident match wins.
    // Under HIGH sensitivity every visit is its own document version — no
    // identification, no merging (the production semantics of the parameter).
    int  best_page = -1;
    Identification best;
    for (size_t i = 0; sensitivity_ == Sensitivity::Normal && i < records_.size(); ++i) {
        Identification id = identifier_.match(first_frame_gray,
                                              records_[i].canvas_gray,
                                              interior_of(records_[i]),
                                              records_[i].stitcher->noise_mask());
        if (verbose_)
            std::cout << "[registry] visit [" << visit.firstFrame << ".."
                      << visit.lastFrame << "] vs page " << i
                      << ": confidence " << id.confidence
                      << " conc " << id.concentration
                      << " blocks " << id.matched_blocks
                      << (id.matched ? " (match, vt0=" + std::to_string(id.vt0) + ")" : "")
                      << "\n";
        if (id.matched && id.confidence > best.confidence) {
            best      = id;
            best_page = static_cast<int>(i);
        }
    }

    if (best_page >= 0) {
        a.pageId     = best_page;
        a.vtStart    = best.vt0;
        a.confidence = best.confidence;
        a.navType    = "REVISIT";
    } else {
        a.pageId  = static_cast<int>(records_.size());
        a.navType = records_.empty() ? "" : "NAVIGATE";   // visit 0 is no navigation
        records_.push_back({});
        records_.back().stitcher = std::make_unique<PageStitcher>(
            video_path_, frame_w_, frame_h_, fps_, verbose_, a.pageId);
    }

    PageRecord& rec = records_[a.pageId];
    rec.stitcher->add_visit(visit, a.vtStart);
    rec.canvas = rec.stitcher->reconstruct();
    cv::cvtColor(rec.canvas, rec.canvas_gray, cv::COLOR_BGR2GRAY);
    return a;
}
