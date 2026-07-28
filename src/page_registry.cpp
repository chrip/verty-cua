#include "page_registry.hpp"
#include <iostream>

PageRegistry::PageRegistry(std::string video_path, int frame_w, int frame_h,
                           double fps, bool verbose, Sensitivity sensitivity,
                           const ScrollbarTracker* scrollbar)
    : video_path_(std::move(video_path)),
      frame_w_(frame_w), frame_h_(frame_h), fps_(fps), verbose_(verbose),
      sensitivity_(sensitivity), scrollbar_(scrollbar)
{
}

// ── continues_last_page ───────────────────────────────────────────────────────
// Content matching has just failed against every known page. Before declaring a
// navigation, ask the other question: is this the page we were ALREADY on, seen
// at an offset its canvas does not reach yet? That is the signature of a scroll
// jump larger than the shared-pixel window — a fling, a Page-Down chain, or a
// scrollbar-thumb DRAG (measured on rw4_portal: 4356px in one 3.89fps frame
// step, where the identifier reported 0 matching blocks).
//
// Content CANNOT answer it: past the canvas end there is nothing to match
// against, by construction. The scrollbar can — under three guards:
//
//  * ONLY the immediately preceding page is considered. A jump continues the
//    page you are on; letting any page claim a fragment would reopen exactly
//    the false merge that the identifier's vote-concentration test exists to
//    prevent (two different pages sharing one chrome template).
//  * The page's thumb→offset map must be CALIBRATED from that page's own
//    content-tracked scrolls, and this frame's thumb HEIGHT must match the map's
//    fingerprint. Thumb height is a function of document height alone, so it is
//    the one identity signal that survives a zero-overlap jump (measured on
//    rw4: 72px for the 11624px page vs 576px for the 1449px one).
//  * The predicted offset must land where content matching is blind by
//    construction (SB_CONTINUATION_MAX_OVERLAP).
//
// Deliberate non-decision: the frames the segmenter dropped between the two
// visits (its page-change debounce window) are NOT recovered, even though the
// map could place them. They are the frames captured DURING the jump, and a
// screen capture taken mid-repaint is TORN — measured on rw4 frame 150 by
// localizing it band by band inside the DOM reference stitch: rows 60..149 sit
// at document offset 5571 while rows 510..899 sit at 6983, with no match in
// between. Placing such a frame would stitch a torn band into the canvas at one
// of the two offsets. One dropped viewport of coverage is the cheaper error.
bool PageRegistry::continues_last_page(const cv::Mat& first_frame_gray,
                                       long& vt, long& uncertainty) const
{
    // High sensitivity: every visit is its own document version, never merged.
    if (sensitivity_ != Sensitivity::Normal) return false;
    if (last_page_ < 0 || !scrollbar_ || !scrollbar_->located()) return false;

    const PageStitcher& prev = *records_[static_cast<size_t>(last_page_)].stitcher;
    const ScrollbarMap& map  = prev.scrollbar_map();
    const ScrollbarReading r = scrollbar_->read(first_frame_gray);
    if (!map.matches(r)) return false;

    const long predicted  = map.predict(r.thumb_top);
    const long canvas_end = prev.doc_height();
    if (canvas_end - predicted >= SB_CONTINUATION_MAX_OVERLAP) return false;

    vt          = predicted;
    uncertainty = map.uncertainty(predicted);
    return true;
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
        // The scrollbar is deliberately NOT excluded here, even though its thumb
        // tracks the scroll position and so says nothing about WHICH page this
        // is. Excluding it narrows this rect, which reshuffles the identifier's
        // deterministic block sampling, which moves confidence and concentration
        // by a few percent — and rw5_mobileframe sits on a knife edge:
        // concentration 0.46-0.474 against REVISIT_MIN_CONCENTRATION = 0.5 on
        // three visits. Excluding 15 of 1918 columns tipped one of them and cost
        // a duplicate page (5 -> 6). The exclusion is principled but its benefit
        // is unmeasurable; the regression was measured, so the geometry stays.
        // The real issue is the sample-dependence itself — see ROADMAP.md.
        case FixedKind::Scrollbar: break;
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

    long cont_vt = 0, cont_uncertainty = 0;
    if (best_page >= 0 && best_page == last_page_
        && best.confidence >= CONTINUATION_MIN_CONFIDENCE) {
        // Re-localized WITHIN the page we were already on, by content: the jump
        // landed in territory this page's canvas already covers, so the frame
        // matched it outright. Not a navigation — the viewport moved, the
        // document did not (CONTINUATION_MIN_CONFIDENCE separates this from a
        // page that reloaded itself in place).
        a.pageId       = best_page;
        a.vtStart      = best.vt0;
        a.confidence   = best.confidence;
        a.continuation = true;
        if (verbose_)
            std::cout << "[registry] visit [" << visit.firstFrame << ".."
                      << visit.lastFrame << "] CONTINUES page " << a.pageId
                      << " at vt=" << a.vtStart << " (content re-localization, "
                      << "confidence " << a.confidence << ")\n";
    } else if (best_page >= 0) {
        a.pageId     = best_page;
        a.vtStart    = best.vt0;
        a.confidence = best.confidence;
        a.navType    = "REVISIT";
    } else if (continues_last_page(first_frame_gray, cont_vt, cont_uncertainty)) {
        a.pageId        = last_page_;
        a.vtStart       = cont_vt;
        a.vtUncertainty = cont_uncertainty;
        a.continuation  = true;   // no navType: this was never a navigation
        if (verbose_)
            std::cout << "[registry] visit [" << visit.firstFrame << ".."
                      << visit.lastFrame << "] CONTINUES page " << a.pageId
                      << " at vt=" << a.vtStart << "±" << a.vtUncertainty
                      << " (scrollbar re-localization)\n";
    } else {
        a.pageId  = static_cast<int>(records_.size());
        a.navType = records_.empty() ? "" : "NAVIGATE";   // visit 0 is no navigation
        records_.push_back({});
        records_.back().stitcher = std::make_unique<PageStitcher>(
            video_path_, frame_w_, frame_h_, fps_, verbose_, a.pageId, scrollbar_);
    }

    PageRecord& rec = records_[a.pageId];
    rec.stitcher->add_visit(visit, a.vtStart, a.continuation, a.vtUncertainty);
    rec.canvas = rec.stitcher->reconstruct();
    cv::cvtColor(rec.canvas, rec.canvas_gray, cv::COLOR_BGR2GRAY);
    last_page_ = a.pageId;
    return a;
}
