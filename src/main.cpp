#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <format>
#include <filesystem>

#include <opencv2/opencv.hpp>

#include "session_processor.hpp"
#include "session_report.hpp"

namespace fs = std::filesystem;

// ── CLI args ──────────────────────────────────────────────────────────────────
struct Args {
    std::string input;
    std::string output  = "output.png";
    std::string report;                  // session-report JSON path ("" = off)
    bool        verbose = false;
    Sensitivity sensitivity = Sensitivity::Normal;
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <input.mp4> [-o output.png] [-r report.json] [-v]\n"
              << "  -o  output image path (default: output.png). If the session\n"
              << "      contains several pages, page N is written with a _pageN\n"
              << "      suffix (e.g. output_page0.png, output_page1.png).\n"
              << "  -r  write session report (JSON): pages, navigations, scroll\n"
              << "      events + timestamps, fixed-element rects, transition tally\n"
              << "  -S  sensitivity: normal|high (default normal). normal starts a\n"
              << "      new page only on a real navigation; high starts a new\n"
              << "      stitched screenshot on every bigger mutation and never\n"
              << "      merges revisits\n"
              << "  -v  verbose logging\n";
}

static Args parse_args(int argc, char* argv[]) {
    Args a;
    if (argc < 2) { print_usage(argv[0]); std::exit(1); }
    a.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "-v")                     a.verbose = true;
        else if (arg == "-o" && i + 1 < argc)     a.output  = argv[++i];
        else if (arg == "-r" && i + 1 < argc)     a.report  = argv[++i];
        else if (arg == "-S" && i + 1 < argc) {
            const std::string v = argv[++i];
            if      (v == "normal") a.sensitivity = Sensitivity::Normal;
            else if (v == "high")   a.sensitivity = Sensitivity::High;
            else { std::cerr << "invalid -S value: " << v << " (use normal|high)\n"; std::exit(1); }
        }
        else { std::cerr << "Unknown argument: " << arg << "\n"; print_usage(argv[0]); std::exit(1); }
    }
    return a;
}

// output.png → output_page2.png (single-page sessions keep the name verbatim).
// ── Output names ──────────────────────────────────────────────────────────────
// out.png → out_page000.png, and everything belonging to that page carries its
// number: out_page000_sub000_state000.png. Zero-padded and page-first so that a
// file browser sorting by name puts a page and its subframes together, in order,
// which is how a person actually looks at them. Padding to three digits because
// two is not enough for a long session (one real recording produced eleven pages,
// and a page can hold several subframes with dozens of states each).
static std::string page_token(size_t page)
{
    std::ostringstream t;
    t << "_page" << std::setfill('0') << std::setw(3) << page;
    return t.str();
}

static std::string page_output_path(const std::string& base, size_t page, size_t page_count) {
    if (page_count <= 1) return base;
    fs::path p(base);
    fs::path named = p.parent_path() /
        (p.stem().string() + page_token(page) + p.extension().string());
    return named.string();
}

// One image belonging to a page: a settled STATE of a subframe, or an inner
// DOCUMENT it scrolled through (taller than the subframe). Numbered, not named,
// because neither has a name the pixels can supply — the identity of each is its
// position in the sequence.
std::string subframe_output_path(const std::string& base, size_t page, size_t sub,
                                 size_t n, const char* what)
{
    fs::path p(base);
    const std::string ext = p.extension().empty() ? ".png" : p.extension().string();
    std::ostringstream name;
    name << p.stem().string() << page_token(page)
         << "_sub" << std::setfill('0') << std::setw(3) << sub
         << "_" << what << std::setfill('0') << std::setw(3) << n << ext;
    return (p.parent_path() / name.str()).string();
}

int main(int argc, char* argv[])
{
    Args args = parse_args(argc, argv);
    auto t_start = std::chrono::steady_clock::now();

    SessionProcessor session(args.input, args.verbose, args.sensitivity);
    if (!session.ok()) {
        std::cerr << "Cannot open: " << args.input << "\n";
        return 1;
    }
    if (!session.run() || session.pages().empty()) {
        std::cerr << "[main] session produced no pages\n";
        return 1;
    }

    const auto& pages = session.pages();
    std::vector<std::string> stitch_paths;
    for (size_t i = 0; i < pages.size(); ++i) {
        const std::string path = page_output_path(args.output, i, pages.size());
        if (pages[i].image.empty() || !cv::imwrite(path, pages[i].image)) {
            std::cerr << "[main] failed to write: " << path << "\n";
            return 1;
        }
        stitch_paths.push_back(path);
    }

    // ── Subframe states ──────────────────────────────────────────────────────
    // A screen flow's states are written as their own files rather than composed
    // into the page: they are mutually exclusive appearances of the same
    // rectangle, so there is no single image that shows them all, and a consumer
    // that wants to lay them back over the page has their frame position in the
    // report.
    std::vector<std::vector<std::vector<std::string>>> state_paths, doc_paths;
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        state_paths.emplace_back();
        doc_paths.emplace_back();
        for (size_t si = 0; si < pages[pi].subframes.size(); ++si) {
            const auto& sf = pages[pi].subframes[si];
            state_paths.back().emplace_back();
            doc_paths.back().emplace_back();
            if (!sf.screenflow) continue;   // noise keeps nothing
            for (size_t k = 0; k < sf.inner.size(); ++k) {
                const std::string p = subframe_output_path(args.output, pi, si, k, "doc");
                if (sf.inner[k].image.empty() || !cv::imwrite(p, sf.inner[k].image)) {
                    std::cerr << "[main] failed to write: " << p << "\n";
                    return 1;
                }
                doc_paths.back().back().push_back(p);
            }
            for (size_t k = 0; k < sf.states.size(); ++k) {
                const std::string p = subframe_output_path(args.output, pi, si, k, "state");
                if (sf.states[k].image.empty() || !cv::imwrite(p, sf.states[k].image)) {
                    std::cerr << "[main] failed to write: " << p << "\n";
                    return 1;
                }
                state_paths.back().back().push_back(p);
            }
            std::cout << "[main] page " << pi << " subframe " << si << ": "
                      << sf.states.size() << " states, " << sf.inner.size()
                      << " inner documents → "
                      << fs::absolute(subframe_output_path(args.output, pi, si, 0,
                                                           sf.inner.empty() ? "state" : "doc"))
                      << " …\n";
        }
    }

    if (!args.report.empty()) {
        if (!SessionReport::write(args.report, args.input, session, stitch_paths,
                                  state_paths, doc_paths)) {
            std::cerr << "[main] failed to write report: " << args.report << "\n";
            return 1;
        }
        std::cout << "[main] report: " << fs::absolute(args.report) << "\n";
    }

    double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "[main] done in " << std::format("{:.1f}s", secs) << "\n";
    for (size_t i = 0; i < pages.size(); ++i)
        std::cout << "[main] page " << i << ": " << fs::absolute(stitch_paths[i])
                  << "  (" << pages[i].image.cols << "×" << pages[i].image.rows << "px)\n";
    return 0;
}
