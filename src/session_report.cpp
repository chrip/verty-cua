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
                          const std::vector<std::vector<std::string>>& subframe_state_paths,
                          const std::vector<std::vector<std::string>>& subframe_doc_paths)
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
    // schema 4 adds subframes[]: regions that changed while the document did
    // not, each classified by whether it ever held still (see
    // subframe_tracker.hpp). A "screenflow" carries its settled states as
    // separate images with their own position in the frame; "noise" carries none,
    // because a playing video has no state worth keeping.
    out << "  \"schema\": 4,\n";
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
        out << "]}" << (i + 1 < pages.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    // ── Subframes ────────────────────────────────────────────────────────────
    // A bounded region that changed while the document around it did not. Its
    // position is given in FRAME coordinates, which is what a consumer needs to
    // lay a state back over the page it came from; a state's own image is exactly
    // that rectangle, so no scaling is implied.
    out << "  \"subframes\": [";
    const auto& subs = session.subframes();
    for (size_t i = 0; i < subs.size(); ++i) {
        const auto& sf = subs[i];
        out << (i ? ",\n    " : "\n    ")
            << "{\"rect\": {\"x\": " << sf.rect.x << ", \"y\": " << sf.rect.y
            << ", \"w\": " << sf.rect.width << ", \"h\": " << sf.rect.height << "}, "
            << "\"kind\": \"" << (sf.is_screenflow() ? "screenflow" : "noise") << "\", "
            << "\"inPlaceChanges\": " << sf.in_place_changes << ", "
            << "\"settledStates\": " << sf.states.size() << ", ";
        // Inner documents: runs the subframe SCROLLED through, stitched by the
        // same pipeline one level down. Their images are taller than the subframe
        // — that is the whole point — so `capturedExtent` is in the subframe's own
        // document coordinates, not the frame's.
        out << "\"innerDocuments\": [";
        for (size_t k = 0; k < sf.inner.size(); ++k) {
            const auto& d = sf.inner[k];
            const bool have = i < subframe_doc_paths.size()
                              && k < subframe_doc_paths[i].size()
                              && !subframe_doc_paths[i][k].empty();
            out << (k ? ", " : "")
                << "{\"firstFrame\": " << d.first_frame
                << ", \"lastFrame\": " << d.last_frame
                << ", \"capturedExtent\": " << d.extent
                << ", \"innerScrolls\": " << d.scrolls
                << ", \"image\": " << (have ? "\"" + escape(subframe_doc_paths[i][k]) + "\"" : "null")
                << "}";
        }
        out << "], \"states\": [";
        if (sf.is_screenflow()) {
            for (size_t k = 0; k < sf.states.size(); ++k) {
                const auto& st = sf.states[k];
                const bool have = i < subframe_state_paths.size()
                                  && k < subframe_state_paths[i].size()
                                  && !subframe_state_paths[i][k].empty();
                out << (k ? ", " : "")
                    << "{\"frame\": " << st.frame
                    << ", \"t\": " << std::format("{:.3f}", st.t)
                    << ", \"heldFrames\": " << st.dwell
                    << ", \"rect\": {\"x\": " << st.box.x << ", \"y\": " << st.box.y
                    << ", \"w\": " << st.box.width << ", \"h\": " << st.box.height << "}"
                    << ", \"image\": " << (have ? "\"" + escape(subframe_state_paths[i][k]) + "\"" : "null")
                    << "}";
            }
        }
        out << "]}";
    }
    out << (subs.empty() ? "" : "\n  ") << "],\n";

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
