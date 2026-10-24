// verty-serve -- M2: the visual state check as a live local service.
//
// One HTTP endpoint answers the question a CUA agent needs after every action:
// did the screen do what this action should have done, and can that be settled
// without a model? No LLM, no GPU, ~9 ms per call.
//
//   POST /transition?session=ID&before=PATH&after=PATH&action=click&x=..&y=..
//     -> { verdict, extent, reason, transition, dy, changed_frac, ... }
//
// State lives per session, keyed by the agent's run id, because several signals
// only exist across steps: which block appearances have been seen before (novel
// vs revisit), where the last click put the focus (which is what a keystroke
// acts on), and how many consecutive steps have introduced nothing new (an agent
// going in circles).
//
// Frames are passed as PATHS, not bodies. This is a loopback service running
// beside the agent on the same machine, the screenshots are already on disk, and
// accepting paths keeps the wire format simple enough to need no JSON parser.
// It also means the service must never be exposed off-host: it reads any file it
// is asked for.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/opencv.hpp>

#include "live/drag_gesture.hpp"
#include "live/session.hpp"
#include "live/visual_delta.hpp"
#include "perception/transition_classifier.hpp"
#include "perception/types.hpp"
#include "verification/action_verifier.hpp"

namespace {

const char* type_name(TransitionType t)
{
    switch (t) {
    case TransitionType::Identical:  return "identical";
    case TransitionType::Scroll:     return "scroll";
    case TransitionType::PageChange: return "page_change";
    case TransitionType::Unstable:   return "unstable";
    }
    return "unknown";
}

std::string json_escape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;
        }
    }
    return o;
}

std::string url_decode(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { o += ' '; }
        else if (s[i] == '%' && i + 2 < s.size()) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::map<std::string, std::string> parse_query(const std::string& q)
{
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < q.size()) {
        const size_t amp = q.find('&', i);
        const std::string pair = q.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos)
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return out;
}

int qint(const std::map<std::string, std::string>& q, const char* k, int def, bool* found = nullptr)
{
    const auto it = q.find(k);
    if (it == q.end() || it->second.empty()) { if (found) *found = false; return def; }
    try { const int v = std::stoi(it->second); if (found) *found = true; return v; }
    catch (...) { if (found) *found = false; return def; }
}

std::string qstr(const std::map<std::string, std::string>& q, const char* k)
{
    const auto it = q.find(k);
    return it == q.end() ? std::string() : it->second;
}

// ── Server state ─────────────────────────────────────────────────────────────
struct Server {
    std::mutex mu;
    std::unordered_map<std::string, SessionState> sessions;
    TransitionClassifier classifier{Sensitivity::Normal};
    VerifierConfig cfg;
    long served = 0;
};

std::string respond(int code, const std::string& body)
{
    std::ostringstream o;
    o << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error") << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n" << body;
    return o.str();
}

std::string error(int code, const std::string& msg)
{
    return respond(code, "{\"error\":\"" + json_escape(msg) + "\"}");
}

std::string handle_transition(Server& srv, const std::map<std::string, std::string>& q)
{
    const std::string before_path = qstr(q, "before");
    const std::string after_path  = qstr(q, "after");
    if (before_path.empty() || after_path.empty())
        return error(400, "before and after are required");
    if (before_path == after_path)
        return error(400, "before and after are the same file -- that measures nothing");

    cv::Mat before = cv::imread(before_path, cv::IMREAD_GRAYSCALE);
    cv::Mat after  = cv::imread(after_path,  cv::IMREAD_GRAYSCALE);
    if (before.empty()) return error(400, "cannot read before frame: " + before_path);
    if (after.empty())  return error(400, "cannot read after frame: " + after_path);
    if (before.size() != after.size())
        return error(400, "frames differ in size; a resize is not a transition");

    Action action;
    action.kind = action_kind_from_string(qstr(q, "action"));
    bool hx = false, hy = false;
    const int x = qint(q, "x", 0, &hx), y = qint(q, "y", 0, &hy);
    action.has_coord = hx && hy;
    action.coord = {x, y};
    bool hdy = false;
    action.dy = qint(q, "dy", 0, &hdy);
    action.has_dy = hdy;
    action.text = qstr(q, "text");

    const std::string sid = qstr(q, "session").empty() ? "default" : qstr(q, "session");

    const auto t0 = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(srv.mu);
    SessionState& S = srv.sessions[sid];
    if (S.steps == 0) S.history.observe(before);

    const VisualDelta d = measure_delta(before, after, srv.classifier);

    cv::Mat diff, smooth, changed;
    cv::absdiff(before, after, diff);
    cv::blur(diff, smooth, cv::Size(3, 3));
    cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
    RegionChangeSummary rc;
    S.history.classify_and_observe(after, changed, rc);

    ChangeLocality loc;
    if (action.has_coord)
        loc = measure_locality(before, after, action.coord, srv.cfg.click_radius, d);

    VerifierContext ctx;
    ctx.has_focus = S.has_focus;
    ctx.focus = S.focus;
    if (ctx.has_focus)
        ctx.focus_locality = measure_locality(before, after, S.focus,
                                              srv.cfg.key_focus_radius, d);

    // Only drags pay for the motion field; every other step returns at once.
    const DragGesture drag = measure_drag(S, action, before, after);
    ctx.drag = &drag;

    const VerificationResult vr = verify_action(action, d, loc, srv.cfg, ctx);
    S.note_press(action, before);
    S.advance(action.has_coord, action.coord, rc.novel_blocks);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    ++srv.served;

    // Change that cannot be attributed to the action. Not a verdict -- a
    // separate channel, because "the action did nothing AND something else
    // happened" is two facts and the verdict can only carry one. For a GUI test
    // agent the second is often the interesting one: an overlay, a notification,
    // or another session editing the same document.
    // Ask whether change exists AWAY from the action, not whether the NEAREST
    // change is far from it.
    //
    // The first version gated on dist_to_change > click_far_px, which is blind
    // to the only case that matters. When an agent clicks a control and
    // something unrelated updates elsewhere in the same window, the nearest
    // change is the control the agent just touched: dist_to_change is 0 on
    // interrupted and uninterrupted steps alike. Measured against a lab that
    // logs its own interruptions, that rule fired on 0 of 2 interrupted steps.
    //
    // The share of change lying outside the action's neighbourhood does
    // separate them cleanly -- 63-70% beyond 400px on the interrupted steps,
    // 0% on the clean ones -- and it was already being computed.
    const double away = loc.valid ? 1.0 - loc.frac_within_r : 1.0;
    const bool external =
        d.changed_frac > 1e-4
        && ((action.has_coord && loc.valid && away >= srv.cfg.external_away_frac)
            || (!action.has_coord && !ctx.has_focus
                && action.kind != ActionKind::Type && action.kind != ActionKind::Key
                && d.changed_frac > 1e-3));

    std::ostringstream j;
    j << std::fixed;
    j << "{\"verdict\":\"" << verdict_name(vr.verdict) << "\""
      << ",\"extent\":\"" << extent_name(vr.extent) << "\""
      << ",\"reason\":\"" << json_escape(vr.reason) << "\""
      << ",\"transition\":\"" << type_name(d.transition) << "\""
      << ",\"dy\":" << d.dy
      << std::setprecision(6)
      << ",\"changed_frac\":" << d.changed_frac
      << ",\"changed_frac_strong\":" << d.changed_frac_strong
      << ",\"changed_bbox\":[" << d.bbox.x << "," << d.bbox.y << ","
                               << d.bbox.width << "," << d.bbox.height << "]"
      << ",\"components\":" << d.components
      << ",\"blocks_novel\":" << rc.novel_blocks
      << ",\"blocks_revisit\":" << rc.revisit_blocks
      << ",\"no_novel_run\":" << S.no_novel_run
      << ",\"external_change\":" << (external ? "true" : "false")
      << ",\"session\":\"" << json_escape(sid) << "\""
      << ",\"step\":" << S.steps;
    if (loc.valid)
        j << std::setprecision(1) << ",\"dist_to_change\":" << loc.dist_to_change;
    j << std::setprecision(3) << ",\"ms\":" << ms << "}";
    return respond(200, j.str());
}

std::string handle(Server& srv, const std::string& method, const std::string& path,
                   const std::map<std::string, std::string>& q)
{
    if (path == "/health") {
        std::lock_guard<std::mutex> lock(srv.mu);
        std::ostringstream j;
        j << "{\"ok\":true,\"sessions\":" << srv.sessions.size()
          << ",\"served\":" << srv.served << "}";
        return respond(200, j.str());
    }
    if (path == "/transition") {
        if (method != "POST" && method != "GET") return error(405, "use POST");
        return handle_transition(srv, q);
    }
    if (path == "/session/reset") {
        std::lock_guard<std::mutex> lock(srv.mu);
        const std::string sid = qstr(q, "session").empty() ? "default" : qstr(q, "session");
        srv.sessions.erase(sid);
        return respond(200, "{\"ok\":true,\"reset\":\"" + json_escape(sid) + "\"}");
    }
    return error(404, "no such endpoint: " + path);
}

}  // namespace

int main(int argc, char** argv)
{
    int port = 4505;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-p" || a == "--port") && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::cout <<
            "Usage: " << argv[0] << " [-p PORT]\n\n"
            "  Loopback-only visual state check for CUA agents (default port 4505).\n\n"
            "  POST /transition?session=ID&before=PATH&after=PATH&action=KIND[&x=&y=&dy=&text=]\n"
            "  POST /session/reset?session=ID\n"
            "  GET  /health\n\n"
            "  Frames are read from PATH on the local filesystem. Never expose this\n"
            "  off-host: it will read any file it is given.\n";
            return 0;
        }
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::cerr << "socket failed\n"; return 1; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only, deliberately. See the file header.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed on port " << port << "\n"; return 1;
    }
    if (::listen(fd, 16) < 0) { std::cerr << "listen failed\n"; return 1; }

    Server srv;
    std::cerr << "verty-serve listening on 127.0.0.1:" << port << "\n";

    for (;;) {
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) continue;

        std::string req;
        char buf[8192];
        // Request line + headers are all that is needed; the body is unused.
        for (;;) {
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos) break;
            if (req.size() > (1u << 20)) break;
        }

        std::string out;
        std::istringstream rs(req);
        std::string method, target;
        rs >> method >> target;
        if (method.empty() || target.empty()) {
            out = error(400, "malformed request");
        } else {
            const size_t qm = target.find('?');
            const std::string path = qm == std::string::npos ? target : target.substr(0, qm);
            const auto q = qm == std::string::npos
                               ? std::map<std::string, std::string>{}
                               : parse_query(target.substr(qm + 1));
            try {
                out = handle(srv, method, path, q);
            } catch (const std::exception& e) {
                out = error(500, e.what());
            }
        }
        ::send(c, out.data(), out.size(), MSG_NOSIGNAL);
        ::close(c);
    }
}
