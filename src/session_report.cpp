#include "session_report.hpp"
#include <fstream>
#include <iomanip>

namespace {

const char* kind_name(FixedKind k)
{
    switch (k) {
    case FixedKind::Header: return "header";
    case FixedKind::Footer: return "footer";
    case FixedKind::Left:   return "left";
    case FixedKind::Right:  return "right";
    case FixedKind::Scrollbar: return "scrollbar";
    }
    return "unknown";
}

// Minimal JSON string escaping for the values we emit (paths).
std::string escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}   // namespace

bool SessionReport::write(const std::string& path,
                          const std::string& video_path,
                          const SessionProcessor& session,
                          const std::vector<std::string>& stitch_paths,
                          const Paths3& state_paths,
                          const Paths3& doc_paths)
{
    std::ofstream out(path);
    if (!out) return false;
    out << std::fixed << std::setprecision(3);

    const double fps = session.fps();

    out << "{\n";
    // schema 3 renames pages[].docHeight to capturedExtent and adds
    // documentHeight / reachedBottom / driftPx, plus a per-frame answer table.
    // The old name was the honesty problem: it reported the deepest offset the
    // session HAPPENED to reach, which a consumer reads as the height of the
    // document. On a session that stopped early those differ by thousands of
    // pixels, and gaps[] was empty, so the report claimed complete coverage of a
    // document it had seen a third of. capturedExtent cannot be misread, and
    // where the scrollbar was legible the real height is stated next to it with
    // its own uncertainty.
    // (schema 2 added pages[].gaps and visits[].vtUncertaintyPx.)
    // schema 4 added subframes[]: regions that changed while the document did
    // not, each classified by whether it ever held still (see
    // subframe_tracker.hpp). A "screenflow" carries its settled states as
    // separate images with their own position in the frame, plus innerDocuments[]
    // where it scrolled its own content; "noise" carries none, because a playing
    // video has no state worth keeping.
    // schema 5 moves them INSIDE the page they belong to. A subframe is on a page,
    // its written files carry that page in their name, and one page can hold
    // several — an ajax gallery and a scrollable comment section below it.
    // schema 6 adds subframes[].stampedState: which of the states the page's own
    // stitch shows inside that rectangle. The page image cannot show all of them,
    // so it shows one, and a consumer that lays out the states itself needs to know
    // which one is already there — otherwise it double-renders that state and has
    // no way to tell which pixels of the page it may replace.
    // schema 7 adds subframes[].documentY: the document row the region's viewport
    // rect.y sits at on this page. Every other rectangle in a page entry is either
    // already in document coordinates (the stitch) or is chrome that has no single
    // document row; a subframe was the one thing reported in viewport coordinates
    // that DOES have one, and the report made a consumer join frames[] on a state's
    // frame number to find it. Measured in the corpus: one region at viewport
    // y=622 is document row 742, 1582 and 622 on three different pages of
    // rw6_shopcatalog, because the user had scrolled somewhere else each time — so
    // the viewport rect alone is not enough to place anything, and a region
    // classified as noise has no state, hence no frame, hence was unplaceable.
    out << "  \"schema\": 7,\n";
    out << "  \"video\": {\"path\": \"" << escape(video_path) << "\", "
        << "\"fps\": " << fps << ", "
        << "\"frames\": " << session.frame_count() << ", "
        << "\"viewport\": {\"w\": " << session.frame_w() << ", \"h\": " << session.frame_h() << "}},\n";

    const TransitionCounts& tc = session.transition_counts();
    out << "  \"transitions\": {\"identical\": " << tc.identical
        << ", \"scroll\": " << tc.scroll
        << ", \"pageChange\": " << tc.pageChange
        << ", \"unstable\": " << tc.unstable << "},\n";

    // ── Pages (one entry per DISTINCT page; a page can have several visits) ──
    out << "  \"pages\": [\n";
    const auto& pages = session.pages();
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& p = pages[i];
        out << "    {\"id\": " << i
            << ", \"stitch\": \"" << escape(i < stitch_paths.size() ? stitch_paths[i] : "") << "\""
            << ", \"width\": " << p.image.cols
            << ", \"capturedExtent\": " << p.capturedExtent
            << ", \"visits\": [";
        for (size_t v = 0; v < p.visits.size(); ++v) {
            const auto& t = p.visits[v];
            out << (v ? ", " : "")
                << "{\"firstFrame\": " << t.visit.firstFrame
                << ", \"lastFrame\": " << t.visit.lastFrame
                << ", \"tEnter\": " << (t.visit.firstFrame / fps)
                << ", \"tExit\": " << (t.visit.lastFrame / fps)
                << ", \"vtStart\": " << t.vt_start
                << ", \"vtUncertaintyPx\": " << t.vt_uncertainty
                << ", \"relocalized\": " << (t.bridged ? "true" : "false") << "}";
        }
        out << "], \"documentHeight\": ";
        if (p.documentHeight > 0)
            out << "{\"value\": " << p.documentHeight
                << ", \"uncertaintyPx\": " << p.documentHeightUncertainty
                << ", \"source\": \"scrollbar\"}";
        else
            out << "null";     // no legible scrollbar: the height is unknowable
        out << ", \"reachedBottom\": "
            << (p.reachedBottom < 0 ? "null" : (p.reachedBottom ? "true" : "false"));
        out << ", \"driftPx\": ";
        if (p.driftPx >= 0) out << p.driftPx; else out << "null";
        out << ", \"fixedRects\": [";
        for (size_t k = 0; k < p.fixedRects.size(); ++k) {
            const cv::Rect& r = p.fixedRects[k].rect;
            out << (k ? ", " : "")
                << "{\"kind\": \"" << kind_name(p.fixedRects[k].kind) << "\", "
                << "\"x\": " << r.x << ", \"y\": " << r.y << ", "
                << "\"w\": " << r.width << ", \"h\": " << r.height
                // The scrollbar band is DETECTED, not inferred from pixel
                // stability, so its extent is exact — unlike the four content
                // bands, which can only over-detect.
                << ", \"uncertaintyPx\": "
                << (p.fixedRects[k].kind == FixedKind::Scrollbar
                        ? 0 : FIXED_BAND_UNCERTAINTY_PX) << "}";
        }
        out << "], \"overlays\": [";
        for (size_t k = 0; k < p.overlays.size(); ++k) {
            const cv::Rect& r = p.overlays[k];
            out << (k ? ", " : "")
                << "{\"x\": " << r.x << ", \"y\": " << r.y << ", "
                << "\"w\": " << r.width << ", \"h\": " << r.height << "}";
        }
        // Document rows for which no SCROLLING CONTENT was ever observed:
        // skipped by a jump (blank in the stitch), or hidden at every reachable
        // offset by a viewport-fixed element (the stitch shows that element).
        // Either way the page content there was never captured and cannot be
        // reconstructed; saying so is part of the contract.
        out << "], \"gaps\": [";
        for (size_t k = 0; k < p.gaps.size(); ++k)
            out << (k ? ", " : "")
                << "{\"from\": " << p.gaps[k].from << ", \"to\": " << p.gaps[k].to << "}";

        // ── Subframes of this page ───────────────────────────────────────────
        // A region that changed while the document did not. Its position is in
        // FRAME coordinates, which is what a consumer needs to lay a state back
        // over the page it came from, and a state's image is exactly that
        // rectangle. A page can hold several: an ajax gallery and a scrollable
        // comment section below it are two.
        out << "], \"subframes\": [";
        for (size_t k = 0; k < p.subframes.size(); ++k) {
            const auto& sf = p.subframes[k];
            const auto* sp = (i < state_paths.size() && k < state_paths[i].size())
                                 ? &state_paths[i][k] : nullptr;
            const auto* dp = (i < doc_paths.size() && k < doc_paths[i].size())
                                 ? &doc_paths[i][k] : nullptr;
            out << (k ? ",\n      " : "\n      ")
                << "{\"rect\": {\"x\": " << sf.rect.x << ", \"y\": " << sf.rect.y
                << ", \"w\": " << sf.rect.width << ", \"h\": " << sf.rect.height << "}, "
                // Where rect.y is in the PAGE's document coordinates — the row to
                // lay this region's images over the stitch at. x, w and h are the
                // same in both spaces (no horizontal scrolling is modelled), so
                // only y is restated. Not to be confused with the coordinates of
                // innerDocuments[] below, which are the SUBFRAME's own document.
                //
                // null when the region does not sit at one document row: it was
                // fixed in the viewport, so it sat at every row and none, or its
                // rect drifted; and null when this page localized none of the
                // frames the region is evidenced by (a page-change window or a
                // blank visit carries no offset). In every null case the region's
                // position on the page image is not merely unstated but unknown,
                // and the states must not be laid over the stitch at all.
                << "\"documentY\": "
                << (sf.document_y >= 0 ? std::to_string(sf.document_y) : "null") << ", "
                << "\"kind\": \"" << (sf.screenflow ? "screenflow" : "noise") << "\", "
                << "\"inPlaceChanges\": " << sf.in_place_changes << ", "
                << "\"settledStates\": " << sf.states.size() << ", "
                // Which state the PAGE image shows in this rectangle — normally the
                // one the region was left in. null when the page shows no state
                // there and its rectangle is reconstructed content: a noise region
                // (no state is worth preferring) or a screen flow none of whose
                // states could be placed in this page's document coordinates.
                << "\"stampedState\": "
                << (sf.stamped_state >= 0 ? std::to_string(sf.stamped_state) : "null")
                << ", ";
            // Runs the subframe SCROLLED through, stitched by the same pipeline one
            // level down. Their images are taller than the subframe — that is the
            // point — so capturedExtent is in the SUBFRAME's document coordinates.
            out << "\"innerDocuments\": [";
            for (size_t m = 0; m < sf.inner.size(); ++m) {
                const auto& d = sf.inner[m];
                const bool have = dp && m < dp->size() && !(*dp)[m].empty();
                out << (m ? ", " : "")
                    << "{\"firstFrame\": " << d.first_frame
                    << ", \"lastFrame\": " << d.last_frame
                    << ", \"capturedExtent\": " << d.extent
                    << ", \"innerScrolls\": " << d.scrolls
                    << ", \"image\": " << (have ? "\"" + escape((*dp)[m]) + "\"" : "null")
                    << "}";
            }
            // A state's IMAGE covers the subframe's `rect` (above) — not the
            // rectangle below it. `changedRect` is what changed to produce this
            // state, which is smaller and moves about: one tap repaints a whole
            // panel and the next only its body. It was named `rect` up to schema 6,
            // beside `image`, and that was a trap — measured across the corpus, all
            // 58 states of the sessions that have any reported a `rect` that is not
            // the rectangle their own PNG covers, rw7's worst by 365px and a factor
            // of 11 in area (330x46 claimed, 361x507 shipped). To place a state:
            // x and w from the subframe's rect, h from it too, y from this state's
            // documentY.
            out << "], \"states\": [";
            for (size_t m = 0; m < sf.states.size(); ++m) {
                const auto& st = sf.states[m];
                const bool have = sp && m < sp->size() && !(*sp)[m].empty();
                out << (m ? ", " : "")
                    << "{\"frame\": " << st.frame
                    << ", \"t\": " << std::format("{:.3f}", st.t)
                    << ", \"heldFrames\": " << st.dwell
                    << ", \"documentY\": "
                    << (st.document_y >= 0 ? std::to_string(st.document_y) : "null")
                    << ", \"changedRect\": {\"x\": " << st.box.x << ", \"y\": " << st.box.y
                    << ", \"w\": " << st.box.width << ", \"h\": " << st.box.height << "}"
                    << ", \"image\": " << (have ? "\"" + escape((*sp)[m]) + "\"" : "null")
                    << "}";
            }
            out << "]}";
        }
        out << (p.subframes.empty() ? "" : "\n    ") << "]}"
            << (i + 1 < pages.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    // ── Navigations ──────────────────────────────────────────────────────────
    out << "  \"navigations\": [";
    const auto& navs = session.navigations();
    for (size_t i = 0; i < navs.size(); ++i) {
        const auto& n = navs[i];
        out << (i ? ", " : "")
            << "{\"frame\": " << n.frame
            << ", \"t\": " << (n.frame / fps)
            << ", \"from\": " << n.from
            << ", \"to\": " << n.to
            << ", \"type\": \"" << n.type << "\""
            << ", \"confidence\": " << n.confidence
            << ", \"vtStart\": " << n.vtStart << "}";
    }
    out << "],\n";

    // ── Scroll events (flat, with page id) ───────────────────────────────────
    out << "  \"scrollEvents\": [\n";
    size_t total = 0;
    for (const auto& p : pages) total += p.scrollEvents.size();
    size_t written = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        for (const ScrollEvent& e : pages[i].scrollEvents) {
            out << "    {\"t\": " << e.t << ", \"frame\": " << e.frame
                << ", \"pageId\": " << i
                << ", \"dy\": " << e.dy << ", \"vt\": " << e.vt << "}"
                << (++written < total ? "," : "") << "\n";
        }
    }
    out << "  ],\n";

    // ── Per-frame answers ────────────────────────────────────────────────────
    // One row per frame of the video: which page it showed and how far down, or
    // null with the reason there is no answer ("transition" = the frame fell in
    // a page-change window, "blank" = it belonged to a visit dropped as a blank
    // loading screen). The scroll events above list only the frames where the
    // offset CHANGED; this lists them all, so a consumer can never mistake an
    // unlisted frame for an unmoved one.
    out << "  \"frames\": [\n";
    const auto& fr = session.frames();
    for (size_t i = 0; i < fr.size(); ++i) {
        out << "    {\"frame\": " << fr[i].frame
            << ", \"t\": " << fr[i].t
            << ", \"pageId\": " << (fr[i].pageId < 0 ? std::string("null")
                                                      : std::to_string(fr[i].pageId))
            << ", \"vt\": " << (fr[i].vt < 0 ? std::string("null")
                                              : std::to_string(fr[i].vt))
            << ", \"reason\": " << (fr[i].reason.empty()
                                        ? std::string("null")
                                        : "\"" + fr[i].reason + "\"")
            << "}" << (i + 1 < fr.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    return out.good();
}
