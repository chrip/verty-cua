// verty-replay -- offline analysis of recorded CUA trajectories.
//
// M0 (measurement): runs the existing Verty perception primitives over every
// recorded (S_before, action, S_after) triple and reports what the pixels say
// happened -- how much changed, where, in how many pieces, and whether a scroll
// explains it.
//
// M1 (verification): with --verify, also runs the action-conditioned
// expectation model and reports, per step, whether a cheap check could have
// answered the question the VLM was asked.
//
// The two stay separable on purpose. The M0 numbers are what M1's thresholds
// were fitted to, so a run must be able to produce them without M1's opinion
// mixed in.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "adapters/trajectory.hpp"
#include "live/region_history.hpp"
#include "live/session.hpp"
#include "live/visual_delta.hpp"
#include "perception/transition_classifier.hpp"
#include "perception/types.hpp"
#include "verification/action_verifier.hpp"

namespace fs = std::filesystem;

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

std::string escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;
        }
    }
    return out;
}

struct Args {
    std::vector<std::string> manifests;
    std::string out = "replay.jsonl";
    int         radius = 120;
    bool        verify = false;
    std::string change_maps;   // dir for per-step block-class PNGs ("" = off)
    bool        verbose = false;
};

void usage(const char* prog)
{
    std::cout <<
    "Usage: " << prog << " <manifest.tsv> [more.tsv ...] [-o out.jsonl] [options]\n"
    "\n"
    "  Reads normalized trajectory manifests (see adapters/) and writes one\n"
    "  JSON object per action step describing what changed on screen.\n"
    "\n"
    "  -o  output JSONL path (default replay.jsonl)\n"
    "  -R  locality radius in px around the action coordinate (default 120)\n"
    "  --verify   also run the M1 expectation model and emit a verdict\n"
    "  --change-maps DIR   write a per-step PNG classifying each changed block\n"
    "              as novel (a state never seen before) or revisit (an earlier\n"
    "              state re-established). Rendered by review_sheet.py.\n"
    "  -v  per-step logging to stderr\n";
}

Args parse_args(int argc, char** argv)
{
    Args a;
    if (argc < 2) { usage(argv[0]); std::exit(1); }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "-v")                    a.verbose = true;
        else if (arg == "--verify")              a.verify  = true;
        else if (arg == "-o" && i + 1 < argc)    a.out = argv[++i];
        else if (arg == "-R" && i + 1 < argc)    a.radius = std::stoi(argv[++i]);
        else if (arg == "--change-maps" && i + 1 < argc) a.change_maps = argv[++i];
        else if (arg == "-h" || arg == "--help") { usage(argv[0]); std::exit(0); }
        else if (!arg.empty() && arg[0] == '-')  { usage(argv[0]); std::exit(1); }
        else                                     a.manifests.push_back(arg);
    }
    if (a.manifests.empty()) { usage(argv[0]); std::exit(1); }
    return a;
}

}  // namespace

int main(int argc, char** argv)
{
    const Args args = parse_args(argc, argv);
    std::ofstream out(args.out);
    if (!out) { std::cerr << "cannot write " << args.out << "\n"; return 1; }

    TransitionClassifier classifier(Sensitivity::Normal);
    const VerifierConfig vcfg;

    long steps = 0, skipped = 0;
    long tally[4] = {0, 0, 0, 0};
    long verdicts[3] = {0, 0, 0};
    double total_ms = 0.0;

    for (const std::string& mpath : args.manifests) {
        Trajectory traj;
        try {
            traj = load_manifest(mpath);
        } catch (const std::exception& e) {
            std::cerr << "skip " << mpath << ": " << e.what() << "\n";
            continue;
        }

        // History is per trajectory: block appearances from one session say
        // nothing about another.
        SessionState session;
        bool history_seeded = false;

        for (const TrajectoryStep& s : traj.steps) {
            cv::Mat before = cv::imread(s.before, cv::IMREAD_GRAYSCALE);
            cv::Mat after  = cv::imread(s.after,  cv::IMREAD_GRAYSCALE);
            if (before.empty() || after.empty()) {
                std::cerr << "skip " << traj.id << " step " << s.index
                          << ": unreadable frame\n";
                ++skipped;
                continue;
            }
            if (before.size() != after.size()) {
                // A resolution change is a real event (window resize, display
                // mode) but not one a pixel comparison can express.
                std::cerr << "skip " << traj.id << " step " << s.index
                          << ": frame size changed\n";
                ++skipped;
                continue;
            }

            if (!history_seeded) { session.history.observe(before); history_seeded = true; }

            const auto t0 = std::chrono::steady_clock::now();
            // No fixed/dynamic masks: this measures what a STATELESS frame pair
            // yields. Carrying the accumulators across a trajectory is M2's job,
            // and mixing it in here would make the baseline unattributable.
            const VisualDelta d = measure_delta(before, after, classifier);

            Action action;
            action.kind = action_kind_from_string(s.action);
            action.has_coord = s.has_coord;
            action.coord = {s.x, s.y};
            action.has_dy = s.has_dy;
            action.dy = s.dy;
            action.text = s.text;

            ChangeLocality loc;
            if (s.has_coord)
                loc = measure_locality(before, after, action.coord, args.radius, d);

            // Novel vs revisit, from the block history alone -- no model.
            RegionChangeSummary rc;
            {
                cv::Mat diff, smooth, changed;
                cv::absdiff(before, after, diff);
                cv::blur(diff, smooth, cv::Size(3, 3));
                cv::compare(smooth, SAD_THRESHOLD, changed, cv::CMP_GT);
                const cv::Mat labels = session.history.classify_and_observe(after, changed, rc);
                if (!args.change_maps.empty()) {
                    std::error_code ec;
                    fs::create_directories(fs::path(args.change_maps) / traj.id, ec);
                    std::ostringstream nm;
                    nm << args.change_maps << "/" << traj.id << "/"
                       << std::setw(3) << std::setfill('0') << s.index << ".png";
                    cv::imwrite(nm.str(), RegionHistory::expand(labels, after.size()));
                }
            }

            // Keyboard actions carry no coordinate of their own; measure their
            // locality at the point the last click focused instead.
            VerifierContext ctx;
            ctx.has_focus = session.has_focus;
            ctx.focus = session.focus;
            if (ctx.has_focus)
                ctx.focus_locality = measure_locality(before, after, session.focus,
                                                      vcfg.key_focus_radius, d);

            VerificationResult vr;
            if (args.verify) vr = verify_action(action, d, loc, vcfg, ctx);

            session.advance(s.has_coord, {s.x, s.y}, rc.novel_blocks);

            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            total_ms += ms;

            tally[static_cast<int>(d.transition)]++;
            if (args.verify) verdicts[static_cast<int>(vr.verdict)]++;
            ++steps;

            std::ostringstream j;
            j << std::fixed;
            j << "{\"trajectory\":\"" << escape(traj.id) << "\""
              << ",\"source\":\"" << escape(traj.source) << "\""
              << ",\"step\":" << s.index
              << ",\"action\":\"" << escape(s.action) << "\"";
            if (s.has_coord) j << ",\"x\":" << s.x << ",\"y\":" << s.y;
            if (s.has_dy)    j << ",\"requested_dy\":" << s.dy;
            if (!s.text.empty()) j << ",\"text\":\"" << escape(s.text) << "\"";
            j << ",\"frame_w\":" << d.frame_w << ",\"frame_h\":" << d.frame_h
              << ",\"transition\":\"" << type_name(d.transition) << "\""
              << ",\"dy\":" << d.dy
              << std::setprecision(6)
              << ",\"changed_frac\":" << d.changed_frac
              << ",\"changed_frac_strong\":" << d.changed_frac_strong
              << ",\"changed_bbox\":[" << d.bbox.x << "," << d.bbox.y << ","
                                       << d.bbox.width << "," << d.bbox.height << "]"
              << ",\"changed_bbox_frac\":" << d.bbox_frac
              << ",\"components\":" << d.components
              << ",\"bands_changed\":" << d.bands_changed
              << ",\"bands_total\":" << d.bands_total
              << ",\"blocks_changed\":" << rc.changed_blocks
              << ",\"blocks_novel\":" << rc.novel_blocks
              << ",\"blocks_revisit\":" << rc.revisit_blocks
              << ",\"no_novel_run\":" << session.no_novel_run;
            if (loc.valid) {
                j << ",\"locality_radius\":" << args.radius
                  << ",\"frac_within_r\":" << loc.frac_within_r
                  << std::setprecision(1)
                  << ",\"dist_to_change\":" << loc.dist_to_change
                  << ",\"dist_to_bbox\":" << loc.dist_to_bbox
                  << ",\"click_inside_bbox\":" << (loc.inside_bbox ? "true" : "false");
            }
            if (args.verify) {
                j << ",\"verdict\":\"" << verdict_name(vr.verdict) << "\""
                  << ",\"extent\":\"" << extent_name(vr.extent) << "\""
                  << ",\"reason\":\"" << escape(vr.reason) << "\"";
            }
            j << std::setprecision(3) << ",\"ms\":" << ms << "}";
            out << j.str() << "\n";

            if (args.verbose) {
                std::cerr << traj.id << " step " << std::setw(3) << s.index << "  "
                          << std::setw(12) << s.action << "  "
                          << std::setw(11) << type_name(d.transition)
                          << "  dy=" << std::setw(5) << d.dy
                          << "  changed=" << std::setprecision(4) << d.changed_frac;
                if (args.verify)
                    std::cerr << "  " << std::setw(10) << verdict_name(vr.verdict)
                              << "  " << vr.reason;
                std::cerr << "\n";
            }
        }
    }

    out.close();
    std::cout << "steps        " << steps << "  (skipped " << skipped << ")\n"
              << "identical    " << tally[0] << "\n"
              << "scroll       " << tally[1] << "\n"
              << "page_change  " << tally[2] << "\n"
              << "unstable     " << tally[3] << "\n";
    if (args.verify)
        std::cout << "\nexpected     " << verdicts[0] << "\n"
                  << "unexpected   " << verdicts[1] << "\n"
                  << "unknown      " << verdicts[2] << "\n";
    if (steps > 0)
        std::cout << "\nmean         " << std::fixed << std::setprecision(2)
                  << (total_ms / steps) << " ms/step\n";
    std::cout << "wrote        " << args.out << "\n";
    return 0;
}
