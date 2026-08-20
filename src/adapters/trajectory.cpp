#include "trajectory.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

std::vector<std::string> split_tabs(const std::string& line)
{
    std::vector<std::string> out;
    std::string cell;
    std::istringstream is(line);
    while (std::getline(is, cell, '\t')) out.push_back(cell);
    return out;
}

// An empty cell means "absent"; anything else must parse cleanly. Silently
// treating a malformed number as 0 would turn a broken adapter into a
// plausible-looking measurement, which is the one failure mode this whole
// exercise cannot tolerate.
bool parse_int(const std::string& cell, int& out, const std::string& col, int line_no)
{
    if (cell.empty()) return false;
    try {
        size_t used = 0;
        out = std::stoi(cell, &used);
        if (used != cell.size()) throw std::invalid_argument("trailing");
    } catch (const std::exception&) {
        throw std::runtime_error("manifest line " + std::to_string(line_no)
                                 + ": column '" + col + "' is not an integer: '" + cell + "'");
    }
    return true;
}

}  // namespace

Trajectory load_manifest(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open manifest: " + path);

    Trajectory traj;
    traj.id = std::filesystem::path(path).parent_path().filename().string();

    std::string line;
    std::vector<std::string> cols;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // '#' lines carry adapter metadata: "# source=mcp_bridge".
        if (line[0] == '#') {
            const auto eq = line.find("source=");
            if (eq != std::string::npos) traj.source = line.substr(eq + 7);
            continue;
        }
        if (cols.empty()) { cols = split_tabs(line); continue; }   // header

        const auto cells = split_tabs(line);
        std::unordered_map<std::string, std::string> row;
        for (size_t i = 0; i < cols.size(); ++i)
            row[cols[i]] = i < cells.size() ? cells[i] : std::string();

        TrajectoryStep s;
        parse_int(row["index"], s.index, "index", line_no);
        s.action    = row["action"];
        s.has_coord = parse_int(row["x"], s.x, "x", line_no)
                    & parse_int(row["y"], s.y, "y", line_no);
        s.has_dy    = parse_int(row["dy"], s.dy, "dy", line_no);
        s.text      = row["text"];
        s.before    = row["before"];
        s.after     = row["after"];
        if (s.before.empty() || s.after.empty())
            throw std::runtime_error("manifest line " + std::to_string(line_no)
                                     + ": step has no before/after frame");
        // A step that compares a frame with itself measures nothing, and it does
        // so silently -- changed_frac 0.0 on every row, which reads as "the
        // action changed nothing" rather than as a broken adapter. Both
        // trajectory sources produced exactly this bug at first (see
        // both trajectory adapters produced it at first), so it is refused here
        // rather than trusted.
        if (s.before == s.after)
            throw std::runtime_error("manifest line " + std::to_string(line_no)
                                     + ": before and after are the same frame ("
                                     + s.before + ") -- the adapter is pairing "
                                     "each frame with itself");
        traj.steps.push_back(std::move(s));
    }
    if (cols.empty()) throw std::runtime_error("manifest has no header: " + path);
    return traj;
}
