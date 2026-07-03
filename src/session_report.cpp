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
    out << "  \"video\": {\"path\": \"" << escape(video_path) << "\", "
        << "\"fps\": " << fps << ", "
        << "\"frames\": " << session.frame_count() << ", "
        << "\"viewport\": {\"w\": " << session.frame_w() << ", \"h\": " << session.frame_h() << "}},\n";

    const TransitionCounts& tc = session.transition_counts();
    out << "  \"transitions\": {\"identical\": " << tc.identical
        << ", \"scroll\": " << tc.scroll
        << ", \"pageChange\": " << tc.pageChange
        << ", \"unstable\": " << tc.unstable << "},\n";

    // ── Pages ────────────────────────────────────────────────────────────────
    out << "  \"pages\": [\n";
    const auto& pages = session.pages();
    for (size_t i = 0; i < pages.size(); ++i) {
        const auto& p = pages[i];
        out << "    {\"id\": " << i
            << ", \"stitch\": \"" << escape(i < stitch_paths.size() ? stitch_paths[i] : "") << "\""
            << ", \"width\": " << p.image.cols
            << ", \"docHeight\": " << p.docHeight
            << ", \"visit\": {\"firstFrame\": " << p.visit.firstFrame
            << ", \"lastFrame\": " << p.visit.lastFrame
            << ", \"tEnter\": " << (p.visit.firstFrame / fps)
            << ", \"tExit\": " << (p.visit.lastFrame / fps) << "}"
            << ", \"fixedRects\": [";
        for (size_t k = 0; k < p.fixedRects.size(); ++k) {
            const cv::Rect& r = p.fixedRects[k].rect;
            out << (k ? ", " : "")
                << "{\"kind\": \"" << kind_name(p.fixedRects[k].kind) << "\", "
                << "\"x\": " << r.x << ", \"y\": " << r.y << ", "
                << "\"w\": " << r.width << ", \"h\": " << r.height << "}";
        }
        out << "]}" << (i + 1 < pages.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    // ── Navigations (REVISIT classification arrives with Roadmap M4) ────────
    out << "  \"navigations\": [";
    const auto& navs = session.nav_frames();
    for (size_t i = 0; i < navs.size(); ++i) {
        out << (i ? ", " : "")
            << "{\"frame\": " << navs[i]
            << ", \"t\": " << (navs[i] / fps)
            << ", \"from\": " << i
            << ", \"to\": " << (i + 1)
            << ", \"type\": \"NAVIGATE\"}";
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
