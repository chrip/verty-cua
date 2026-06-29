#include <iostream>
#include <string>
#include <chrono>
#include <format>
#include <filesystem>

#include <opencv2/opencv.hpp>

#include "types.hpp"
#include "scroll_detector.hpp"
#include "frame_classifier.hpp"
#include "global_canvas.hpp"
#include "renderer.hpp"

namespace fs = std::filesystem;

// ── CLI args ──────────────────────────────────────────────────────────────────
struct Args {
    std::string input;
    std::string output     = "output.png";
    bool        verbose    = false;
    bool        debug_grid = false;
    int         frame_skip = 1;   // process every N-th frame
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog
              << " <input.mp4> [-o output.png] [-v] [-g] [-s skip]\n"
              << "  -o  output image path      (default: output.png)\n"
              << "  -v  verbose logging\n"
              << "  -g  draw 16×16 debug grid\n"
              << "  -s  process every Nth frame (default: 1)\n";
}

static Args parse_args(int argc, char* argv[]) {
    Args a;
    if (argc < 2) { print_usage(argv[0]); std::exit(1); }
    a.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v") { a.verbose = true; }
        else if (arg == "-g") { a.debug_grid = true; }
        else if (arg == "-o" && i + 1 < argc) { a.output = argv[++i]; }
        else if (arg == "-s" && i + 1 < argc) { a.frame_skip = std::stoi(argv[++i]); }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return a;
}

// ── progress bar ──────────────────────────────────────────────────────────────
static void print_progress(int frame, int total, int scroll_dy,
                            int new_blocks, int fixed_blocks) {
    int pct = total > 0 ? (frame * 100 / total) : 0;
    std::cout << std::format("\r[{:3d}%] frame {:5d}/{:5d}  scroll {:+4d}px  "
                             "new {:4d}  fixed {:3d}   ",
                             pct, frame, total, scroll_dy, new_blocks, fixed_blocks)
              << std::flush;
}

// ── debug: visualise classification ─────────────────────────────────────────
static void show_debug_frame(const cv::Mat& bgr, const Grid& grid, int frame_no) {
    cv::Mat vis = bgr.clone();
    for (const auto& row : grid) {
        for (const Block& b : row) {
            cv::Scalar colour;
            switch (b.cls) {
                case BlockClass::STATIC:   colour = {0,   200, 0};   break;  // green
                case BlockClass::SCROLLED: colour = {200, 100, 0};   break;  // blue-ish
                case BlockClass::NEW:      colour = {0,   0,   220}; break;  // red
                case BlockClass::FIXED:    colour = {220, 0,   220}; break;  // magenta
                default:                   colour = {128, 128, 128}; break;  // grey
            }
            int px = b.gx * BLOCK_SIZE, py = b.gy * BLOCK_SIZE;
            cv::rectangle(vis, {px+1, py+1},
                          {px+BLOCK_SIZE-2, py+BLOCK_SIZE-2},
                          colour, 1);
        }
    }
    std::string win = "frame " + std::to_string(frame_no);
    cv::imshow(win, vis);
    cv::waitKey(1);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    Args args = parse_args(argc, argv);

    // ── Open video ────────────────────────────────────────────────────────────
    cv::VideoCapture cap(args.input);
    if (!cap.isOpened()) {
        std::cerr << "Cannot open: " << args.input << "\n";
        return 1;
    }

    const int total_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));
    const double fps       = cap.get(cv::CAP_PROP_FPS);
    const int W            = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int H            = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

    if (args.verbose) {
        std::cout << "[main] video: " << args.input
                  << "  " << W << "×" << H
                  << "  " << total_frames << " frames @ " << fps << " fps\n";
    }

    // ── Read first frame ──────────────────────────────────────────────────────
    cv::Mat curr_bgr;
    if (!cap.read(curr_bgr) || curr_bgr.empty()) {
        std::cerr << "Failed to read first frame\n";
        return 1;
    }

    GlobalCanvas   canvas(W, H);
    ScrollDetector scroll_det;
    FrameClassifier classifier;
    RendererOptions render_opts{.draw_grid = args.debug_grid,
                                .verbose   = args.verbose};
    Renderer renderer(render_opts);

    // Grayscale for block matching
    cv::Mat curr_gray, prev_gray;
    cv::cvtColor(curr_bgr, curr_gray, cv::COLOR_BGR2GRAY);

    canvas.ingest_first(curr_bgr);

    cv::Mat prev_bgr = curr_bgr.clone();
    prev_gray        = curr_gray.clone();

    auto t_start = std::chrono::steady_clock::now();
    int  frame_no = 1;

    // ── Main frame loop ───────────────────────────────────────────────────────
    while (cap.read(curr_bgr)) {
        ++frame_no;

        // Optional frame skip (reduces processing for long recordings)
        if ((frame_no % args.frame_skip) != 0) continue;

        cv::cvtColor(curr_bgr, curr_gray, cv::COLOR_BGR2GRAY);

        // Pre-filter: skip visually identical frames.
        // Consecutive identical (or near-identical) frames occur at session
        // start before user interaction, during reading pauses, or from
        // duplicate-frame encoding artefacts.  They do not carry scroll
        // information but CAN produce false peaks in the scroll histogram
        // (repeating page layout matches at large dy offsets), which leads to
        // spurious fixed-element promotion.  Threshold 2.0 MAD is safely
        // below any real scroll (even 1 px of scroll changes many pixels).
        {
            cv::Mat diff_img;
            cv::absdiff(prev_gray, curr_gray, diff_img);
            if (cv::mean(diff_img)[0] < 2.0) {
                prev_bgr  = curr_bgr.clone();
                prev_gray = curr_gray.clone();
                if (args.verbose)
                    print_progress(frame_no, total_frames, 0, 0,
                                   static_cast<int>(
                                       cv::countNonZero(canvas.fixed_mask())));
                continue;
            }
        }

        // Detect dominant scroll
        ScrollResult scroll = scroll_det.detect(prev_gray, curr_gray);

        // Classify blocks (two-pass: seed + BFS grow)
        Grid grid = classifier.classify(
            prev_gray, curr_gray,
            scroll.valid ? scroll.dy : 0,
            canvas.fixed_mask(),
            canvas.viewport_top() + (scroll.valid ? scroll.dy : 0)
        );

        // Count stats for progress display
        int new_count = 0, fixed_count = 0;
        for (const auto& row : grid)
            for (const Block& b : row) {
                if (b.cls == BlockClass::NEW)   ++new_count;
                if (b.cls == BlockClass::FIXED) ++fixed_count;
            }

        // Update global canvas
        canvas.update(curr_bgr, prev_gray, curr_gray,
                      grid, scroll.valid ? scroll.dy : 0);

        if (args.verbose)
            print_progress(frame_no, total_frames,
                           scroll.valid ? scroll.dy : 0,
                           new_count, fixed_count);

        if (args.debug_grid)
            show_debug_frame(curr_bgr, grid, frame_no);

        prev_bgr  = curr_bgr.clone();
        prev_gray = curr_gray.clone();
    }

    std::cout << "\n";

    // ── Render final composite ────────────────────────────────────────────────
    if (args.verbose)
        std::cout << "[main] rendering…\n";

    cv::Mat result = renderer.render(canvas);
    if (result.empty()) {
        std::cerr << "[main] render produced empty image\n";
        return 1;
    }

    if (!cv::imwrite(args.output, result)) {
        std::cerr << "[main] failed to write: " << args.output << "\n";
        return 1;
    }

    auto t_end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t_end - t_start).count();

    std::cout << "[main] done in " << std::format("{:.1f}s", secs) << "\n"
              << "[main] output: " << fs::absolute(args.output) << "\n"
              << "[main] doc size: " << result.cols << "×" << result.rows << "px\n";

    return 0;
}
