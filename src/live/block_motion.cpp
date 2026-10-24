#include "live/block_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>

#include <opencv2/imgproc.hpp>

#include "perception/types.hpp"

namespace {

// A block repeated more often than this in `before` -- a glyph in running
// text, a grid cell, a toolbar icon -- cannot be placed: any of its copies
// would do. Counted as ambiguous rather than guessed.
constexpr int kMaxCandidates = 8;

// Polynomial hash of a BLOCK_SIZE x BLOCK_SIZE window, modulo 2^64. Rows are
// hashed with P1, the row hashes combined down the column with P2. Both are
// odd, so the arithmetic is invertible and the window can be rolled one pixel
// at a time with exact results -- the rolled value and the direct value are the
// same integer, which is what lets a query hashed directly meet the scan.
constexpr uint64_t P1 = 1000003ULL;
constexpr uint64_t P2 = 2654435761ULL;

constexpr uint64_t pow_u64(uint64_t b, int e)
{
    uint64_t r = 1;
    while (e-- > 0) r *= b;
    return r;
}
constexpr uint64_t P1_TOP = pow_u64(P1, BLOCK_SIZE - 1);
constexpr uint64_t P2_TOP = pow_u64(P2, BLOCK_SIZE - 1);

// Polynomial hashes are weak in their low bits; the table indexes by low bits.
inline uint64_t mix(uint64_t h)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

uint64_t direct_hash(const cv::Mat& g, int x, int y)
{
    uint64_t v = 0;
    for (int r = 0; r < BLOCK_SIZE; ++r) {
        const uint8_t* p = g.ptr<uint8_t>(y + r) + x;
        uint64_t h = 0;
        for (int c = 0; c < BLOCK_SIZE; ++c) h = h * P1 + p[c];
        v = v * P2 + h;
    }
    return v;
}

bool same_pixels(const cv::Mat& a, int ax, int ay, const cv::Mat& b, int bx, int by)
{
    for (int r = 0; r < BLOCK_SIZE; ++r)
        if (std::memcmp(a.ptr<uint8_t>(ay + r) + ax, b.ptr<uint8_t>(by + r) + bx,
                        BLOCK_SIZE) != 0)
            return false;
    return true;
}

// Open-addressing set of the query hashes. The scan probes it once per pixel
// position, so it has to be a few cache lines, not a node-based map.
class QuerySet {
public:
    explicit QuerySet(size_t n)
    {
        size_t cap = 64;
        while (cap < n * 4) cap <<= 1;
        mask_ = cap - 1;
        slots_.assign(cap, 0);
        used_.assign(cap, 0);
    }
    void insert(uint64_t h)
    {
        for (size_t i = mix(h) & mask_;; i = (i + 1) & mask_) {
            if (!used_[i]) { used_[i] = 1; slots_[i] = h; return; }
            if (slots_[i] == h) return;
        }
    }
    bool contains(uint64_t h) const
    {
        for (size_t i = mix(h) & mask_;; i = (i + 1) & mask_) {
            if (!used_[i]) return false;
            if (slots_[i] == h) return true;
        }
    }

private:
    size_t mask_ = 0;
    std::vector<uint64_t> slots_;
    std::vector<uint8_t>  used_;
};

struct Query {
    cv::Point at;                      // grid position in `after`
    std::vector<cv::Point> found;      // exact copies in `before`
    bool overflow = false;             // more than kMaxCandidates copies
};

}  // namespace

BlockMotion measure_motion(const cv::Mat& before_gray, const cv::Mat& after_gray,
                           const cv::Mat& changed)
{
    CV_Assert(before_gray.type() == CV_8UC1 && after_gray.type() == CV_8UC1
              && before_gray.size() == after_gray.size()
              && changed.size() == after_gray.size());
    BlockMotion out;
    const int W = after_gray.cols, H = after_gray.rows;
    if (W < BLOCK_SIZE || H < BLOCK_SIZE) return out;

    // ── Queries: changed, textured grid blocks of `after` ───────────────────
    std::vector<Query> queries;
    std::unordered_map<uint64_t, std::vector<int>> by_hash;
    for (int y = 0; y + BLOCK_SIZE <= H; y += BLOCK_SIZE) {
        for (int x = 0; x + BLOCK_SIZE <= W; x += BLOCK_SIZE) {
            const cv::Rect r(x, y, BLOCK_SIZE, BLOCK_SIZE);
            if (cv::countNonZero(changed(r)) == 0) continue;
            // The mask is blurred, so a change bleeds into the edge of a block
            // whose own pixels did not change. That block is not a question.
            if (same_pixels(after_gray, x, y, before_gray, x, y)) continue;
            ++out.changed_blocks;
            // Near-uniform blocks match anywhere; same gate as the identifier.
            cv::Scalar mean, stddev;
            cv::meanStdDev(after_gray(r), mean, stddev);
            if (stddev[0] < IDENT_MIN_STDDEV) continue;
            ++out.textured_blocks;
            by_hash[direct_hash(after_gray, x, y)].push_back(static_cast<int>(queries.size()));
            queries.push_back({{x, y}, {}, false});
        }
    }
    if (queries.empty()) return out;

    QuerySet set(by_hash.size());
    for (const auto& [h, ids] : by_hash) set.insert(h);

    // ── One rolling pass over every window position of `before` ─────────────
    const int nx = W - BLOCK_SIZE + 1;
    std::vector<uint64_t> ring(static_cast<size_t>(BLOCK_SIZE) * nx);   // row hashes
    std::vector<uint64_t> col(nx, 0);                                   // window hashes
    std::vector<uint64_t> next(nx);

    auto row_hashes = [&](int y, uint64_t* dst) {
        const uint8_t* p = before_gray.ptr<uint8_t>(y);
        uint64_t h = 0;
        for (int c = 0; c < BLOCK_SIZE; ++c) h = h * P1 + p[c];
        dst[0] = h;
        for (int x = 1; x < nx; ++x) {
            h = (h - p[x - 1] * P1_TOP) * P1 + p[x + BLOCK_SIZE - 1];
            dst[x] = h;
        }
    };

    for (int y = 0; y < BLOCK_SIZE; ++y) {
        uint64_t* rh = &ring[static_cast<size_t>(y) * nx];
        row_hashes(y, rh);
        for (int x = 0; x < nx; ++x) col[x] = col[x] * P2 + rh[x];
    }
    for (int y = 0;; ++y) {
        // col[x] is the hash of the window with top-left (x, y).
        for (int x = 0; x < nx; ++x) {
            if (!set.contains(col[x])) continue;
            for (int id : by_hash.find(col[x])->second) {
                Query& q = queries[id];
                if (q.at == cv::Point(x, y) || q.overflow) continue;
                if (!same_pixels(after_gray, q.at.x, q.at.y, before_gray, x, y)) continue;
                if (static_cast<int>(q.found.size()) >= kMaxCandidates) {
                    q.overflow = true;
                    q.found.clear();
                } else {
                    q.found.push_back({x, y});
                }
            }
        }
        if (y + BLOCK_SIZE >= H) break;
        // Roll down one row: drop row y, add row y + BLOCK_SIZE.
        uint64_t* slot = &ring[static_cast<size_t>(y % BLOCK_SIZE) * nx];
        row_hashes(y + BLOCK_SIZE, next.data());
        for (int x = 0; x < nx; ++x) {
            col[x] = (col[x] - slot[x] * P2_TOP) * P2 + next[x];
            slot[x] = next[x];
        }
    }

    // ── Assign each block one source, then group by vector ──────────────────
    // A block with a single copy votes for its vector outright. A block with
    // several copies takes the one whose vector the unique blocks already
    // support -- the same object moved as one -- and failing that the nearest,
    // the prior an encoder uses when it has nothing better.
    std::map<std::pair<int, int>, int> votes;
    for (const Query& q : queries)
        if (q.found.size() == 1) {
            const cv::Point mv = q.at - q.found[0];
            votes[{mv.x, mv.y}]++;
        }

    struct Placed { cv::Point at, from; };
    std::map<std::pair<int, int>, std::vector<Placed>> by_mv;
    for (const Query& q : queries) {
        if (q.overflow) { ++out.ambiguous_blocks; continue; }
        if (q.found.empty()) { ++out.new_blocks; continue; }
        ++out.moved_blocks;
        cv::Point best = q.found[0];
        int best_votes = -1;
        double best_dist = 1e18;
        for (const cv::Point& f : q.found) {
            const cv::Point mv = q.at - f;
            auto it = votes.find({mv.x, mv.y});
            const int v = it == votes.end() ? 0 : it->second;
            const double d = std::hypot(mv.x, mv.y);
            if (v > best_votes || (v == best_votes && d < best_dist)) {
                best = f; best_votes = v; best_dist = d;
            }
        }
        const cv::Point mv = q.at - best;
        by_mv[{mv.x, mv.y}].push_back({q.at, best});
    }

    // One vector is not one object: when a card leaves a column, every column
    // that closes up behind a removal moves by the same vector. So blocks
    // sharing a vector are split into spatially connected groups. Blocks count
    // as connected across a gap of up to kJoinBlocks grid cells, because the
    // flat padding inside a card or row carries no texture and is never queried.
    constexpr int kJoinBlocks = 2;
    for (auto& [k, placed] : by_mv) {
        const int n = static_cast<int>(placed.size());
        std::vector<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const cv::Point d = placed[i].at - placed[j].at;
                if (std::max(std::abs(d.x), std::abs(d.y)) <= kJoinBlocks * BLOCK_SIZE)
                    parent[find(i)] = find(j);
            }
        std::map<int, MotionCluster> comps;
        for (int i = 0; i < n; ++i) {
            MotionCluster& g = comps[find(i)];
            const cv::Rect src(placed[i].from.x, placed[i].from.y, BLOCK_SIZE, BLOCK_SIZE);
            const cv::Rect dst(placed[i].at.x, placed[i].at.y, BLOCK_SIZE, BLOCK_SIZE);
            if (g.blocks == 0) { g.mv = {k.first, k.second}; g.src = src; g.dst = dst; }
            else               { g.src |= src; g.dst |= dst; }
            ++g.blocks;
        }
        for (auto& [root, g] : comps)
            if (g.blocks >= BlockMotion::kMinClusterBlocks) out.clusters.push_back(g);
    }
    std::sort(out.clusters.begin(), out.clusters.end(),
              [](const MotionCluster& a, const MotionCluster& b) { return a.blocks > b.blocks; });
    return out;
}
