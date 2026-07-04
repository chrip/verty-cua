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
                          const std::vector<std::string>& stitch_paths)
{
    std::ofstream out(path);
    if (!out) return false;
    out << std::fixed << std::setprecision(3);

    const double fps = session.fps();

    out << "{\n";
    out << "  \"schema\": 1,\n";
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
            << ", \"docHeight\": " << p.docHeight
            << ", \"visits\": [";
        for (size_t v = 0; v < p.visits.size(); ++v) {
            const auto& t = p.visits[v];
            out << (v ? ", " : "")
                << "{\"firstFrame\": " << t.visit.firstFrame
                << ", \"lastFrame\": " << t.visit.lastFrame
                << ", \"tEnter\": " << (t.visit.firstFrame / fps)
                << ", \"tExit\": " << (t.visit.lastFrame / fps)
                << ", \"vtStart\": " << t.vt_start << "}";
        }
        out << "], \"fixedRects\": [";
        for (size_t k = 0; k < p.fixedRects.size(); ++k) {
            const cv::Rect& r = p.fixedRects[k].rect;
            out << (k ? ", " : "")
                << "{\"kind\": \"" << kind_name(p.fixedRects[k].kind) << "\", "
                << "\"x\": " << r.x << ", \"y\": " << r.y << ", "
                << "\"w\": " << r.width << ", \"h\": " << r.height
                << ", \"uncertaintyPx\": " << FIXED_BAND_UNCERTAINTY_PX << "}";
        }
        out << "], \"overlays\": [";
        for (size_t k = 0; k < p.overlays.size(); ++k) {
            const cv::Rect& r = p.overlays[k];
            out << (k ? ", " : "")
                << "{\"x\": " << r.x << ", \"y\": " << r.y << ", "
                << "\"w\": " << r.width << ", \"h\": " << r.height << "}";
        }
        out << "]}" << (i + 1 < pages.size() ? "," : "") << "\n";
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
    out << "  ]\n";
    out << "}\n";

    return out.good();
}
