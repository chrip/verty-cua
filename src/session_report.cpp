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
                          const Stitcher& st)
{
    std::ofstream out(path);
    if (!out) return false;
    out << std::fixed << std::setprecision(3);

    out << "{\n";
    out << "  \"video\": {\"path\": \"" << escape(video_path) << "\", "
        << "\"fps\": " << st.fps() << ", "
        << "\"frames\": " << st.frame_count() << ", "
        << "\"viewport\": {\"w\": " << st.frame_w() << ", \"h\": " << st.frame_h() << "}},\n";

    const TransitionCounts& tc = st.transition_counts();
    out << "  \"transitions\": {\"identical\": " << tc.identical
        << ", \"scroll\": " << tc.scroll
        << ", \"pageChange\": " << tc.pageChange
        << ", \"unstable\": " << tc.unstable << "},\n";

    out << "  \"docHeight\": " << st.doc_height() << ",\n";

    out << "  \"fixedRects\": [";
    const auto& rects = st.fixed_rects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const cv::Rect& r = rects[i].rect;
        out << (i ? ", " : "")
            << "{\"kind\": \"" << kind_name(rects[i].kind) << "\", "
            << "\"x\": " << r.x << ", \"y\": " << r.y << ", "
            << "\"w\": " << r.width << ", \"h\": " << r.height << "}";
    }
    out << "],\n";

    out << "  \"scrollEvents\": [\n";
    const auto& events = st.scroll_events();
    for (size_t i = 0; i < events.size(); ++i) {
        const ScrollEvent& e = events[i];
        out << "    {\"t\": " << e.t << ", \"frame\": " << e.frame
            << ", \"dy\": " << e.dy << ", \"vt\": " << e.vt << "}"
            << (i + 1 < events.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";

    return out.good();
}
