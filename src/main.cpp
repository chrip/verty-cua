#include <iostream>
#include <string>
#include <chrono>
#include <format>
#include <filesystem>

#include <opencv2/opencv.hpp>

#include "stitcher.hpp"

namespace fs = std::filesystem;

// ── CLI args ──────────────────────────────────────────────────────────────────
struct Args {
    std::string input;
    std::string output  = "output.png";
    bool        verbose = false;
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <input.mp4> [-o output.png] [-v]\n"
              << "  -o  output image path  (default: output.png)\n"
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
        else { std::cerr << "Unknown argument: " << arg << "\n"; print_usage(argv[0]); std::exit(1); }
    }
    return a;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    Args args = parse_args(argc, argv);
    auto t_start = std::chrono::steady_clock::now();

    Stitcher stitcher(args.input, args.verbose);
    if (!stitcher.ok()) {
        std::cerr << "Cannot open: " << args.input << "\n";
        return 1;
    }

    cv::Mat result = stitcher.run();
    if (result.empty()) {
        std::cerr << "[main] reconstruction produced empty image\n";
        return 1;
    }

    if (!cv::imwrite(args.output, result)) {
        std::cerr << "[main] failed to write: " << args.output << "\n";
        return 1;
    }

    double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    std::cout << "[main] done in " << std::format("{:.1f}s", secs) << "\n"
              << "[main] output: " << fs::absolute(args.output) << "\n"
              << "[main] doc size: " << result.cols << "×" << result.rows << "px\n";
    return 0;
}
