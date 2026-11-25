// Treemap and sunburst layout geometry tests.
#include "TestHarness.hpp"

#include "FlatArena.hpp"
#include "NodeFilter.hpp"
#include "SunburstLayout.hpp"
#include "SunburstTransition.hpp"
#include "TreemapLayout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

using namespace diskprism;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t below(uint64_t n) { return n == 0 ? 0 : next() % n; }
    double unit() { return static_cast<double>(next() >> 11) / static_cast<double>(1ull << 53); }
};

NodeInit dirInit() {
    NodeInit init;
    init.flags = kNodeDir;
    return init;
}

NodeInit fileInit(uint64_t bytes) {
    NodeInit init;
    init.allocated_bytes = bytes;
    init.logical_bytes = bytes;
    return init;
}

/// Root directory holding one file per entry of `sizes`.
std::unique_ptr<FlatArena> flatTree(const std::vector<uint64_t>& sizes) {
    auto arena = std::make_unique<FlatArena>(ArenaConfig{static_cast<uint32_t>(sizes.size() + 8), 16u << 20});
    const uint32_t root = arena->addRoot("/flat");
    for (size_t i = 0; i < sizes.size(); ++i) arena->addChild(root, "f" + std::to_string(i) + ".bin", fileInit(sizes[i]));
    arena->finalize(SizeMetric::Physical);
    return arena;
}

/// Random nested tree with a heavy-tailed size distribution (like a real disk).
std::unique_ptr<FlatArena> randomTree(uint64_t seed, int nodes) {
    static const char* const kExtensions[] = {".mov", ".jpg", ".txt", ".dylib", "", ".zip", ".log", ".o", ".png", ".db"};
    Rng rng(seed);
    auto arena = std::make_unique<FlatArena>(ArenaConfig{static_cast<uint32_t>(nodes + 8), 64u << 20});
    std::vector<uint32_t> dirs{arena->addRoot("/random")};
    for (int i = 0; i < nodes; ++i) {
        const uint32_t parent = dirs[rng.below(dirs.size())];
        if (rng.below(6) == 0) {
            dirs.push_back(arena->addChild(parent, "dir" + std::to_string(i), dirInit()));
        } else {
            const double u = rng.unit();
            const auto bytes = static_cast<uint64_t>(std::pow(2.0, 2.0 + 24.0 * u * u));  // 4 B … 64 MiB, skewed small
            arena->addChild(parent, "f" + std::to_string(i) + kExtensions[rng.below(10)], fileInit(bytes));
        }
    }
    arena->finalize(SizeMetric::Physical);
    return arena;
}

TreemapParams treemapParams(float width, float height, bool snap = true, float minExtent = 3.0f) {
    TreemapParams p;
    p.width = width;
    p.height = height;
    p.snap_to_pixels = snap;
    p.min_extent = minExtent;
    return p;
}

double area(const TreemapRect& r) {
    return static_cast<double>(r.w) * static_cast<double>(r.h);
}

bool finite(const TreemapRect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && std::isfinite(r.ax) &&
           std::isfinite(r.bx) && std::isfinite(r.ay) && std::isfinite(r.by);
}

bool insideViewport(const TreemapRect& r, float width, float height, float eps = 1e-3f) {
    return r.x >= -eps && r.y >= -eps && r.w > 0 && r.h > 0 && r.x + r.w <= width + eps && r.y + r.h <= height + eps;
}

/// Rasterises a pixel-snapped layout and counts how often each pixel is covered.
/// A valid tiling covers every pixel exactly once: no overlap, no gap, total area conserved.
struct Coverage {
    long uncovered = 0;
    long overlapped = 0;
};
Coverage pixelCoverage(const TreemapLayout& layout, int width, int height) {
    std::vector<uint8_t> grid(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    Coverage c;
    for (const TreemapRect& r : layout.rects()) {
        const int x0 = static_cast<int>(r.x), y0 = static_cast<int>(r.y);
        const int x1 = static_cast<int>(r.x + r.w), y1 = static_cast<int>(r.y + r.h);
        for (int y = std::max(y0, 0); y < std::min(y1, height); ++y) {
            for (int x = std::max(x0, 0); x < std::min(x1, width); ++x) {
                uint8_t& cell = grid[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                if (cell != 0) ++c.overlapped;
                if (cell < 255) ++cell;
            }
        }
    }
    for (const uint8_t cell : grid) {
        if (cell == 0) ++c.uncovered;
    }
    return c;
}

bool overlaps(const TreemapRect& a, const TreemapRect& b, float eps) {
    return a.x + eps < b.x + b.w && b.x + eps < a.x + a.w && a.y + eps < b.y + b.h && b.y + eps < a.y + a.h;
}

int32_t bruteForceHit(const TreemapLayout& layout, float x, float y) {
    const auto& rects = layout.rects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const TreemapRect& r = rects[i];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return static_cast<int32_t>(i);
    }
    return -1;
}

SunburstParams sunburstParams(float width, float height) {
    SunburstParams p;
    p.width = width;
    p.height = height;
    return p;
}

bool finite(const SunburstArc& a) {
    return std::isfinite(a.start_angle) && std::isfinite(a.end_angle) && std::isfinite(a.inner_radius) &&
           std::isfinite(a.outer_radius);
}

}  // namespace

// ----------------------------------------------------------------------------------------
// Squarified treemap
// ----------------------------------------------------------------------------------------

DP_TEST(treemap_reproduces_the_worked_example_of_the_paper) {
    // Bruls, Huizing, van Wijk, "Squarified Treemaps", section 3: areas 6,6,4,3,2,2,1 in a
    // 6 x 4 rectangle.
    const auto arena = flatTree({6, 6, 4, 3, 2, 2, 1});
    TreemapLayout layout;
    layout.build(*arena, arena->root(), treemapParams(6.0f, 4.0f, false, 0.0f));
    const auto& rects = layout.rects();
    REQUIRE(rects.size() == 7);

    struct Expected {
        double x, y, w, h;
    };
    const Expected expected[7] = {
        {0.0, 0.0, 3.0, 2.0},                        // 6
        {0.0, 2.0, 3.0, 2.0},                        // 6
        {3.0, 0.0, 12.0 / 7.0, 7.0 / 3.0},           // 4
        {3.0 + 12.0 / 7.0, 0.0, 9.0 / 7.0, 7.0 / 3.0},  // 3
        {3.0, 7.0 / 3.0, 6.0 / 5.0, 5.0 / 3.0},      // 2
        {4.2, 7.0 / 3.0, 6.0 / 5.0, 5.0 / 3.0},      // 2
        {5.4, 7.0 / 3.0, 3.0 / 5.0, 5.0 / 3.0},      // 1
    };
    for (int i = 0; i < 7; ++i) {
        CHECK_NEAR(rects[i].x, expected[i].x, 1e-5);
        CHECK_NEAR(rects[i].y, expected[i].y, 1e-5);
        CHECK_NEAR(rects[i].w, expected[i].w, 1e-5);
        CHECK_NEAR(rects[i].h, expected[i].h, 1e-5);
        CHECK_EQ(rects[i].flags, static_cast<uint32_t>(kTreemapFile));
        CHECK_EQ(rects[i].depth, uint16_t{1});
    }
}

DP_TEST(treemap_areas_are_proportional_and_conserved) {
    for (uint64_t seed : {11ull, 12ull, 13ull}) {
        const auto arena = randomTree(seed, 600);
        const float width = 1400.0f, height = 900.0f;
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(width, height, false, 0.0f));
        const auto& rects = layout.rects();
        REQUIRE(!rects.empty());

        const double total = static_cast<double>(arena->node(arena->root()).allocated_bytes);
        const double scale = static_cast<double>(width) * height / total;
        double areaSum = 0;
        bool allFinite = true, allInside = true, proportional = true;
        for (const TreemapRect& r : rects) {
            allFinite = allFinite && finite(r);
            allInside = allInside && insideViewport(r, width, height);
            areaSum += area(r);
            // Without culling every block is a file and its area is its share of the bytes.
            const double expected = static_cast<double>(arena->node(r.node).allocated_bytes) * scale;
            if (std::fabs(area(r) - expected) > 1e-3 * expected + 0.05) proportional = false;
            if ((r.flags & kTreemapFile) == 0) proportional = false;
        }
        CHECK(allFinite);
        CHECK(allInside);
        CHECK(proportional);
        CHECK_NEAR(areaSum, static_cast<double>(width) * height, 1e-4 * width * height);

        // Every file with a size appears exactly once — except slivers so thin that they
        // have no extent in 32-bit float coordinates, which are (correctly) dropped.
        std::vector<int> blocksPerNode(arena->size(), 0);
        for (const TreemapRect& r : rects) ++blocksPerNode[r.node];
        bool exactlyOnce = true;
        uint32_t dropped = 0;
        for (uint32_t i = 0; i < arena->size(); ++i) {
            const FsNode& n = arena->node(i);
            if (n.is_dir() || n.allocated_bytes == 0) {
                if (blocksPerNode[i] != 0) exactlyOnce = false;
                continue;
            }
            if (blocksPerNode[i] > 1) exactlyOnce = false;
            if (blocksPerNode[i] == 0) {
                ++dropped;
                // Only blocks far below one pixel may vanish (a full-width strip one float
                // step high is still under a tenth of a pixel in area).
                if (static_cast<double>(n.allocated_bytes) * scale > 0.25) exactlyOnce = false;
            }
        }
        CHECK(exactlyOnce);
        CHECK(dropped <= 5);

        bool disjoint = true;
        for (size_t i = 0; i < rects.size() && disjoint; ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                if (overlaps(rects[i], rects[j], 2e-3f)) {
                    disjoint = false;
                    break;
                }
            }
        }
        CHECK(disjoint);
    }
}

DP_TEST(treemap_snapped_layout_covers_every_pixel_exactly_once) {
    struct Case {
        uint64_t seed;
        int nodes;
        int width, height;
    };
    const Case cases[] = {
        {21, 400, 640, 400}, {22, 3000, 1024, 640}, {23, 3000, 1600, 300}, {24, 3000, 250, 1200},
        {25, 50, 64, 64},    {26, 5000, 1, 1},      {27, 5000, 3, 2},      {28, 8000, 1919, 1079},
    };
    for (const Case& c : cases) {
        const auto arena = randomTree(c.seed, c.nodes);
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(static_cast<float>(c.width), static_cast<float>(c.height)));
        const auto& rects = layout.rects();
        REQUIRE(!rects.empty());

        bool integral = true, inside = true, allFinite = true;
        double areaSum = 0;
        for (const TreemapRect& r : rects) {
            integral = integral && r.x == std::floor(r.x) && r.y == std::floor(r.y) && r.w == std::floor(r.w) &&
                       r.h == std::floor(r.h);
            inside = inside && insideViewport(r, static_cast<float>(c.width), static_cast<float>(c.height), 0.0f);
            allFinite = allFinite && finite(r);
            areaSum += area(r);
        }
        CHECK(integral);
        CHECK(inside);
        CHECK(allFinite);
        const Coverage coverage = pixelCoverage(layout, c.width, c.height);
        CHECK_EQ(coverage.overlapped, 0l);  // rectangles never overlap
        CHECK_EQ(coverage.uncovered, 0l);   // and leave no hole
        CHECK_NEAR(areaSum, static_cast<double>(c.width) * c.height, 0.5);  // total area conserved
    }
}

DP_TEST(treemap_blocks_are_close_to_square) {
    const auto aspect = [](const TreemapRect& r) { return static_cast<double>(std::max(r.w, r.h)) / std::min(r.w, r.h); };

    // 64 equal files in a square: squarification must find the 8 x 8 grid.
    {
        const auto arena = flatTree(std::vector<uint64_t>(64, 1000));
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(800.0f, 800.0f, false, 0.0f));
        REQUIRE(layout.rects().size() == 64);
        double worst = 0;
        for (const TreemapRect& r : layout.rects()) worst = std::max(worst, aspect(r));
        CHECK(worst < 1.001);
    }
    // Heavy-tailed sizes in a wide viewport: still far from the slivers a slice-and-dice
    // layout would give (which reach aspect ratios in the hundreds here).
    {
        std::vector<uint64_t> sizes;
        for (int i = 1; i <= 200; ++i) sizes.push_back(static_cast<uint64_t>(1e9 / std::pow(i, 1.1)));
        const auto arena = flatTree(sizes);
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(1600.0f, 900.0f, false, 0.0f));
        REQUIRE(layout.rects().size() == 200);
        double sum = 0, worst = 0;
        for (const TreemapRect& r : layout.rects()) {
            sum += aspect(r);
            worst = std::max(worst, aspect(r));
        }
        const double mean = sum / 200.0;
        CHECK(mean < 1.6);
        CHECK(worst < 6.0);
        std::printf("      power-law sizes: mean aspect ratio %.2f, worst %.2f\n", mean, worst);
    }
}

DP_TEST(treemap_culls_subpixel_children_into_aggregates) {
    // One big file and 300 000 one-byte files: individually far below a pixel.
    constexpr uint32_t kTiny = 300000;
    FlatArena arena(ArenaConfig{kTiny + 16, 16u << 20});
    const uint32_t root = arena.addRoot("/cull");
    const uint32_t big = arena.addChild(root, "big.mov", fileInit(kTiny));  // exactly half of the bytes
    const uint32_t crowd = arena.addChild(root, "crowd", dirInit());
    for (uint32_t i = 0; i < kTiny; ++i) arena.addChild(crowd, "t.dat", fileInit(1));
    const uint32_t speck = arena.addChild(root, "speck-dir", dirInit());
    arena.addChild(speck, "s1.txt", fileInit(2));
    arena.addChild(speck, "s2.txt", fileInit(1));
    arena.finalize(SizeMetric::Physical);

    TreemapLayout layout;
    const auto start = std::chrono::steady_clock::now();
    layout.build(arena, root, treemapParams(1200.0f, 800.0f));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto& rects = layout.rects();

    // Three blocks instead of 300 003: the big file, the crowd (one aggregate) and the
    // remaining sliver of the root.
    CHECK(rects.size() <= 4);
    REQUIRE(rects.size() >= 2);
    const int32_t bigIndex = layout.rectIndexForNode(big);
    REQUIRE(bigIndex >= 0);
    CHECK_NEAR(area(rects[static_cast<size_t>(bigIndex)]), 0.5 * 1200.0 * 800.0, 0.01 * 1200.0 * 800.0);

    bool sawCrowdAggregate = false;
    for (const TreemapRect& r : rects) {
        if (r.node == crowd && (r.flags & kTreemapAggregate) != 0) {
            sawCrowdAggregate = true;
            CHECK_NEAR(area(r), 0.5 * 1200.0 * 800.0, 0.01 * 1200.0 * 800.0);
        }
        CHECK(insideViewport(r, 1200.0f, 800.0f, 0.0f));
    }
    CHECK(sawCrowdAggregate);
    CHECK_EQ(layout.rectIndexForNode(crowd), -1);  // an aggregate is not "the node itself"
    const Coverage coverage = pixelCoverage(layout, 1200, 800);
    CHECK_EQ(coverage.overlapped, 0l);
    CHECK_EQ(coverage.uncovered, 0l);
    // The cost must not depend on the 300 000 culled children (sorted lists allow an
    // early exit); allow generous slack for sanitizer builds.
    CHECK(ms < 250.0);
    std::printf("      300 003 nodes -> %zu blocks in %.3f ms\n", rects.size(), ms);

    // A directory that is itself below the threshold becomes one block, not a subdivision.
    FlatArena small(ArenaConfig{64, 1 << 16});
    const uint32_t sroot = small.addRoot("/small");
    small.addChild(sroot, "huge.bin", fileInit(1'000'000));
    const uint32_t thin = small.addChild(sroot, "thin-dir", dirInit());
    small.addChild(thin, "a.txt", fileInit(700));
    small.addChild(thin, "b.jpg", fileInit(500));
    small.finalize();
    TreemapLayout smallLayout;
    smallLayout.build(small, sroot, treemapParams(1000.0f, 1000.0f));
    const int32_t thinIndex = smallLayout.rectIndexForNode(thin);
    REQUIRE(thinIndex >= 0);
    CHECK((smallLayout.rects()[static_cast<size_t>(thinIndex)].flags & kTreemapDirectory) != 0);
    CHECK(std::min(smallLayout.rects()[static_cast<size_t>(thinIndex)].w, smallLayout.rects()[static_cast<size_t>(thinIndex)].h) < 3.0f);
    CHECK_EQ(smallLayout.rects().size(), size_t{2});
    // It takes the color of its dominant file.
    CHECK(smallLayout.rects()[static_cast<size_t>(thinIndex)].color == small.extensions().color(*small.extensions().find("txt")));
}

DP_TEST(treemap_block_count_is_bounded_by_the_viewport) {
    const auto arena = randomTree(31, 60000);
    for (const float minExtent : {1.0f, 3.0f, 8.0f}) {
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(800.0f, 600.0f, true, minExtent));
        // Every individually drawn child has an area of at least min_extent²; aggregates
        // add at most one block per subdivided directory.
        const double bound = 800.0 * 600.0 / (static_cast<double>(minExtent) * minExtent) + static_cast<double>(layout.frames().size());
        CHECK(static_cast<double>(layout.rects().size()) <= bound);
        const Coverage coverage = pixelCoverage(layout, 800, 600);
        CHECK_EQ(coverage.overlapped, 0l);
        CHECK_EQ(coverage.uncovered, 0l);
    }
}

DP_TEST(treemap_hit_testing_matches_brute_force) {
    for (const bool snap : {true, false}) {
        const auto arena = randomTree(41, 9000);
        const float width = 1280.0f, height = 720.0f;
        TreemapLayout layout;
        layout.build(*arena, arena->root(), treemapParams(width, height, snap));
        const auto& rects = layout.rects();
        REQUIRE(rects.size() > 500);

        // The centre of every block hits that block.
        bool centresHit = true;
        for (size_t i = 0; i < rects.size(); ++i) {
            const TreemapRect& r = rects[i];
            // Without pixel snapping a block may be a sliver only a few float steps wide,
            // whose computed "centre" rounds onto its own edge; skip those.
            if (std::min(r.w, r.h) < 0.01f) continue;
            const int32_t hit = layout.hitTest(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
            if (hit != static_cast<int32_t>(i)) {
                if (centresHit) {
                    std::printf("      snap=%d: centre of block %zu (%.9g, %.9g, %.9g x %.9g) hit %d\n", snap ? 1 : 0, i,
                                static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.w),
                                static_cast<double>(r.h), hit);
                }
                centresHit = false;
            }
        }
        CHECK(centresHit);

        // Random points (including the exact corners of blocks) agree with a linear search.
        Rng rng(snap ? 7 : 8);
        bool agree = true;
        int hits = 0;
        for (int i = 0; i < 20000; ++i) {
            float x, y;
            if (i % 4 == 0) {
                const TreemapRect& r = rects[rng.below(rects.size())];
                x = r.x + (rng.below(2) != 0 ? r.w : 0.0f);
                y = r.y + (rng.below(2) != 0 ? r.h : 0.0f);
            } else {
                x = static_cast<float>(rng.unit() * (width + 40.0) - 20.0);
                y = static_cast<float>(rng.unit() * (height + 40.0) - 20.0);
            }
            const int32_t fast = layout.hitTest(x, y);
            const bool outside = x < 0 || y < 0 || x >= width || y >= height;
            const int32_t slow = outside ? -1 : bruteForceHit(layout, x, y);
            if (fast != slow) agree = false;
            if (fast >= 0) ++hits;
            if (outside && fast != -1) agree = false;
        }
        CHECK(agree);
        CHECK(hits > 15000);
    }
    TreemapLayout empty;
    CHECK_EQ(empty.hitTest(10.0f, 10.0f), -1);

    // Cost of a hit test: grid lookup (treemap) and ring + binary search (sunburst).
    const auto big = randomTree(42, 60000);
    TreemapLayout treemap;
    treemap.build(*big, big->root(), treemapParams(2880.0f, 1800.0f));
    SunburstLayout sunburst;
    sunburst.build(*big, big->root(), sunburstParams(1800.0f, 1800.0f));
    Rng rng(3);
    constexpr int kQueries = 200000;
    long sum = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kQueries; ++i) sum += treemap.hitTest(static_cast<float>(rng.unit() * 2880.0), static_cast<float>(rng.unit() * 1800.0));
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kQueries; ++i) sum += sunburst.hitTest(static_cast<float>(rng.unit() * 1800.0), static_cast<float>(rng.unit() * 1800.0));
    const auto t2 = std::chrono::steady_clock::now();
    const double treemapNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / kQueries;
    const double sunburstNs = std::chrono::duration<double, std::nano>(t2 - t1).count() / kQueries;
    CHECK(sum != 0);
    CHECK(treemapNs < 100000.0);
    CHECK(sunburstNs < 100000.0);
    std::printf("      hit test: treemap %.0f ns over %zu blocks, sunburst %.0f ns over %zu arcs\n", treemapNs,
                treemap.rects().size(), sunburstNs, sunburst.arcs().size());
}

DP_TEST(treemap_cushion_surface_coefficients) {
    // One file filling the viewport: a single ridge of height h over [0, W] x [0, H].
    const auto arena = flatTree({1000});
    TreemapParams params = treemapParams(400.0f, 200.0f, false, 0.0f);
    params.ridge_height = 0.5f;
    TreemapLayout layout;
    layout.build(*arena, arena->root(), params);
    REQUIRE(layout.rects().size() == 1);
    const TreemapRect& r = layout.rects()[0];
    // z(u) = −(4h/W)·u² + 4h·u  →  slope 4h at the left edge, 0 in the middle, −4h at the right.
    CHECK_NEAR(r.ax, -4.0 * 0.5 / 400.0, 1e-7);
    CHECK_NEAR(r.bx, 4.0 * 0.5, 1e-5);
    CHECK_NEAR(r.ay, -4.0 * 0.5 / 200.0, 1e-7);
    CHECK_NEAR(r.by, 4.0 * 0.5, 1e-5);
    const auto slopeX = [&r](float u) { return 2.0f * r.ax * u + r.bx; };
    CHECK_NEAR(slopeX(0.0f), 2.0, 1e-5);
    CHECK_NEAR(slopeX(200.0f), 0.0, 1e-5);
    CHECK_NEAR(slopeX(400.0f), -2.0, 1e-5);

    // Nested: root/dir/file, each level adds its own ridge with decaying height. The
    // coefficients are stored relative to the block, so they stay small and exact even
    // for a block far from the origin.
    FlatArena nested(ArenaConfig{64, 1 << 16});
    const uint32_t root = nested.addRoot("/n");
    nested.addChild(root, "left.bin", fileInit(3000));
    const uint32_t dir = nested.addChild(root, "dir", dirInit());
    const uint32_t inner = nested.addChild(dir, "inner.bin", fileInit(1000));
    nested.finalize();
    TreemapParams np = treemapParams(4000.0f, 1000.0f, false, 0.0f);
    np.ridge_height = 0.5f;
    np.ridge_decay = 0.5f;
    TreemapLayout nl;
    nl.build(nested, root, np);
    const int32_t innerIndex = nl.rectIndexForNode(inner);
    REQUIRE(innerIndex >= 0);
    const TreemapRect& ir = nl.rects()[static_cast<size_t>(innerIndex)];
    CHECK_NEAR(ir.x, 3000.0, 1e-3);
    CHECK_NEAR(ir.w, 1000.0, 1e-3);
    // Two ridges over the same [3000, 4000] span: heights 0.5 (dir) and 0.25 (file).
    CHECK_NEAR(ir.ax, -4.0 * (0.5 + 0.25) / 1000.0, 1e-7);
    CHECK_NEAR(ir.bx, 4.0 * (0.5 + 0.25), 1e-4);
    CHECK_NEAR(2.0f * ir.ax * 500.0f + ir.bx, 0.0, 1e-4);  // flat on top, in the middle of the block
    CHECK(ir.depth == 2);
}

DP_TEST(treemap_frames_and_node_lookup) {
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/lookup");
    const uint32_t docs = arena.addChild(root, "docs", dirInit());
    const uint32_t a = arena.addChild(docs, "a.txt", fileInit(400'000));
    const uint32_t b = arena.addChild(docs, "b.txt", fileInit(200'000));
    const uint32_t media = arena.addChild(root, "media", dirInit());
    const uint32_t clip = arena.addChild(media, "clip.mov", fileInit(600'000));
    const uint32_t empty = arena.addChild(root, "empty", dirInit());
    arena.finalize();

    TreemapLayout layout;
    layout.build(arena, root, treemapParams(1200.0f, 800.0f));
    CHECK_EQ(layout.focus(), root);
    CHECK_EQ(layout.rects().size(), size_t{3});

    float box[4];
    REQUIRE(layout.rectForNode(root, box));  // the focused directory spans the viewport
    CHECK_NEAR(box[2] * box[3], 1200.0 * 800.0, 1.0);
    REQUIRE(layout.rectForNode(docs, box));  // a subdivided directory has a frame…
    CHECK_NEAR(box[2] * box[3], 0.5 * 1200.0 * 800.0, 2000.0);
    CHECK_EQ(layout.rectIndexForNode(docs), -1);  // …but no block of its own
    const float docsBox[4] = {box[0], box[1], box[2], box[3]};

    for (uint32_t file : {a, b}) {
        REQUIRE(layout.rectForNode(file, box));
        CHECK(layout.rectIndexForNode(file) >= 0);
        // A file's block lies inside its directory's frame.
        CHECK(box[0] >= docsBox[0] && box[1] >= docsBox[1]);
        CHECK(box[0] + box[2] <= docsBox[0] + docsBox[2] && box[1] + box[3] <= docsBox[1] + docsBox[3]);
    }
    CHECK(layout.rectForNode(clip, box));
    CHECK(layout.rectForNode(media, box));
    CHECK(!layout.rectForNode(empty, box));  // zero bytes: no geometry
    CHECK(!layout.rectForNode(12345, box));

    // Focusing a sub-directory lays out only that subtree.
    layout.build(arena, docs, treemapParams(600.0f, 600.0f));
    CHECK_EQ(layout.rects().size(), size_t{2});
    CHECK(!layout.rectForNode(clip, box));
    REQUIRE(layout.rectForNode(a, box));
    CHECK_NEAR(box[2] * box[3], 600.0 * 600.0 * 2.0 / 3.0, 400.0);
}

DP_TEST(treemap_metric_switch_and_unsorted_fallback) {
    FlatArena arena(ArenaConfig{4096, 1 << 20});
    const uint32_t root = arena.addRoot("/metric");
    Rng rng(99);
    std::vector<uint32_t> dirs{root};
    for (int i = 0; i < 1500; ++i) {
        const uint32_t parent = dirs[rng.below(dirs.size())];
        if (rng.below(7) == 0) {
            dirs.push_back(arena.addChild(parent, "d" + std::to_string(i), dirInit()));
        } else {
            NodeInit init;
            init.logical_bytes = 1 + rng.below(5'000'000);
            init.allocated_bytes = rng.below(3) == 0 ? 4096 : init.logical_bytes;  // sparse / compressed files
            arena.addChild(parent, "f" + std::to_string(i), init);
        }
    }
    arena.finalize(SizeMetric::Physical);

    TreemapParams params = treemapParams(900.0f, 700.0f);
    params.metric = SizeMetric::Logical;

    // The lists are sorted by physical size: the layout must notice and still be right.
    REQUIRE(arena.sortedMetric() == SizeMetric::Physical);
    TreemapLayout fallback;
    fallback.build(arena, root, params);

    arena.sortChildren(SizeMetric::Logical);
    TreemapLayout sorted;
    sorted.build(arena, root, params);

    REQUIRE(fallback.rects().size() == sorted.rects().size());
    bool identical = true;
    for (size_t i = 0; i < sorted.rects().size(); ++i) {
        const TreemapRect& x = fallback.rects()[i];
        const TreemapRect& y = sorted.rects()[i];
        if (x.node != y.node || x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h || x.flags != y.flags) identical = false;
    }
    CHECK(identical);
    const Coverage coverage = pixelCoverage(sorted, 900, 700);
    CHECK_EQ(coverage.overlapped, 0l);
    CHECK_EQ(coverage.uncovered, 0l);

    // And the two metrics genuinely produce different pictures.
    params.metric = SizeMetric::Physical;
    arena.sortChildren(SizeMetric::Physical);
    TreemapLayout physical;
    physical.build(arena, root, params);
    bool differs = physical.rects().size() != sorted.rects().size();
    for (size_t i = 0; !differs && i < physical.rects().size(); ++i) {
        differs = physical.rects()[i].node != sorted.rects()[i].node || physical.rects()[i].w != sorted.rects()[i].w;
    }
    CHECK(differs);
}

DP_TEST(treemap_marks_blocks_outside_the_filter_as_dimmed) {
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/filter");
    const uint32_t movie = arena.addChild(root, "movie.mov", fileInit(500'000));
    const uint32_t photo = arena.addChild(root, "photo.jpg", fileInit(300'000));
    const uint32_t dir = arena.addChild(root, "clips", dirInit());
    const uint32_t clip = arena.addChild(dir, "clip.MOV", fileInit(200'000));
    const uint32_t note = arena.addChild(dir, "note.txt", fileInit(100'000));
    arena.finalize();

    NodeMatchSet matches;
    FilterQuery query;
    query.extension_id = *arena.extensions().find("mov");
    REQUIRE(matches.compute(arena, query));

    TreemapParams params = treemapParams(1000.0f, 1000.0f);
    params.matches = &matches;
    TreemapLayout layout;
    layout.build(arena, root, params);
    const auto dimmed = [&layout](uint32_t node) {
        const int32_t index = layout.rectIndexForNode(node);
        return index >= 0 && (layout.rects()[static_cast<size_t>(index)].flags & kTreemapDimmed) != 0;
    };
    CHECK(!dimmed(movie));
    CHECK(!dimmed(clip));
    CHECK(dimmed(photo));
    CHECK(dimmed(note));

    // No filter: nothing is dimmed.
    params.matches = nullptr;
    layout.build(arena, root, params);
    for (const TreemapRect& r : layout.rects()) CHECK((r.flags & kTreemapDimmed) == 0);
    // Blocks carry the color of their file type.
    const int32_t movieIndex = layout.rectIndexForNode(movie);
    const int32_t clipIndex = layout.rectIndexForNode(clip);
    REQUIRE(movieIndex >= 0 && clipIndex >= 0);
    CHECK(layout.rects()[static_cast<size_t>(movieIndex)].color == layout.rects()[static_cast<size_t>(clipIndex)].color);
    CHECK(!(layout.rects()[static_cast<size_t>(movieIndex)].color ==
            layout.rects()[static_cast<size_t>(layout.rectIndexForNode(photo))].color));
}

DP_TEST(treemap_degenerate_inputs_are_safe) {
    const auto arena = randomTree(51, 200);
    TreemapLayout layout;

    layout.build(*arena, arena->root(), treemapParams(0.0f, 500.0f));
    CHECK(layout.rects().empty());
    layout.build(*arena, arena->root(), treemapParams(500.0f, -3.0f));
    CHECK(layout.rects().empty());
    layout.build(*arena, kInvalidNode, treemapParams(500.0f, 500.0f));
    CHECK(layout.rects().empty());
    layout.build(*arena, 999999, treemapParams(500.0f, 500.0f));
    CHECK(layout.rects().empty());
    CHECK_EQ(layout.hitTest(1.0f, 1.0f), -1);

    // An empty directory, and one whose files are all zero bytes.
    FlatArena empty(ArenaConfig{64, 1 << 16});
    const uint32_t root = empty.addRoot("/e");
    const uint32_t hollow = empty.addChild(root, "hollow", dirInit());
    empty.addChild(hollow, "zero.bin", fileInit(0));
    const uint32_t file = empty.addChild(root, "only.bin", fileInit(10));
    empty.finalize();
    layout.build(empty, hollow, treemapParams(300.0f, 300.0f));
    CHECK(layout.rects().empty());
    // Focusing a file gives one block covering the viewport.
    layout.build(empty, file, treemapParams(300.0f, 200.0f));
    REQUIRE(layout.rects().size() == 1);
    CHECK_NEAR(area(layout.rects()[0]), 300.0 * 200.0, 1e-3);
    CHECK_EQ(layout.hitTest(150.0f, 100.0f), 0);
    // A removed node is not laid out.
    REQUIRE(empty.removeNode(file).removed);
    layout.build(empty, file, treemapParams(300.0f, 200.0f));
    CHECK(layout.rects().empty());
    layout.build(empty, root, treemapParams(300.0f, 200.0f));
    CHECK(layout.rects().empty());  // only zero-byte content is left

    // Very deep nesting must not overflow the stack.
    FlatArena deep(ArenaConfig{4096, 1 << 20});
    uint32_t current = deep.addRoot("/deep");
    for (int i = 0; i < 1500; ++i) current = deep.addChild(current, "d", dirInit());
    deep.addChild(current, "leaf.bin", fileInit(100));
    deep.finalize();
    layout.build(deep, 0, treemapParams(800.0f, 600.0f));
    REQUIRE(layout.rects().size() == 1);
    CHECK(finite(layout.rects()[0]));
    CHECK_NEAR(area(layout.rects()[0]), 800.0 * 600.0, 1e-2);
}

// ----------------------------------------------------------------------------------------
// Sunburst
// ----------------------------------------------------------------------------------------

DP_TEST(sunburst_ring_one_spans_the_full_circle_proportionally) {
    const auto arena = flatTree({500, 300, 150, 50});
    SunburstLayout layout;
    layout.build(*arena, arena->root(), sunburstParams(800.0f, 600.0f));
    const auto& arcs = layout.arcs();
    REQUIRE(arcs.size() == 4);

    CHECK_NEAR(layout.centerX(), 400.0, 1e-4);
    CHECK_NEAR(layout.centerY(), 300.0, 1e-4);
    CHECK(layout.outerRadius() > 0 && layout.outerRadius() <= 300.0f);
    CHECK(layout.hubRadius() > 0 && layout.hubRadius() < layout.outerRadius());

    const double expectedShare[4] = {0.5, 0.3, 0.15, 0.05};
    double angle = 0, total = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(finite(arcs[i]));
        CHECK_NEAR(arcs[i].start_angle, angle, 1e-5);  // largest first, clockwise, no gaps
        const double sweep = static_cast<double>(arcs[i].end_angle) - arcs[i].start_angle;
        CHECK_NEAR(sweep, expectedShare[i] * kTwoPi, 1e-5);
        CHECK_EQ(arcs[i].depth, uint16_t{1});
        CHECK_EQ(arcs[i].parent, -1);
        CHECK_NEAR(arcs[i].inner_radius, layout.hubRadius(), 1e-4);
        CHECK(arcs[i].outer_radius > arcs[i].inner_radius);
        CHECK_EQ(arcs[i].flags, static_cast<uint32_t>(kArcFile));
        angle += sweep;
        total += sweep;
    }
    CHECK_NEAR(total, kTwoPi, 1e-5);
    CHECK_NEAR(arcs[3].end_angle, kTwoPi, 1e-5);
}

DP_TEST(sunburst_children_subdivide_their_parents_span) {
    for (uint64_t seed : {61ull, 62ull, 63ull}) {
        const auto arena = randomTree(seed, 4000);
        SunburstParams params = sunburstParams(1000.0f, 1000.0f);
        params.max_depth = 6;
        SunburstLayout layout;
        layout.build(*arena, arena->root(), params);
        const auto& arcs = layout.arcs();
        REQUIRE(arcs.size() > 20);

        const double rootBytes = static_cast<double>(arena->node(arena->root()).allocated_bytes);
        const double minAngle = 0.4 * kTwoPi / 360.0;
        bool allFinite = true, ordered = true, nested = true, proportional = true, thick = true, radial = true;
        std::vector<double> childSweep(arcs.size(), 0.0);
        std::vector<int> aggregates(arcs.size(), 0);
        double ringOneSweep = 0;

        for (size_t i = 0; i < arcs.size(); ++i) {
            const SunburstArc& a = arcs[i];
            allFinite = allFinite && finite(a);
            const double sweep = static_cast<double>(a.end_angle) - a.start_angle;
            if (!(sweep > 0) || a.start_angle < -1e-6 || a.end_angle > kTwoPi + 1e-5) ordered = false;
            if (!(a.outer_radius > a.inner_radius) || a.inner_radius < layout.hubRadius() - 1e-3f ||
                a.outer_radius > layout.outerRadius() + 1e-3f) {
                radial = false;
            }
            if (a.depth < 1 || a.depth > params.max_depth) radial = false;

            if (a.parent >= 0) {
                const SunburstArc& p = arcs[static_cast<size_t>(a.parent)];
                // Inside the parent's angular span, exactly one ring further out.
                if (a.start_angle < p.start_angle - 1e-5f || a.end_angle > p.end_angle + 1e-5f) nested = false;
                if (a.depth != p.depth + 1 || std::fabs(a.inner_radius - p.outer_radius) > 1e-3f) nested = false;
                if ((p.flags & kArcDirectory) == 0) nested = false;
                childSweep[static_cast<size_t>(a.parent)] += sweep;
                if ((a.flags & kArcAggregate) != 0) ++aggregates[static_cast<size_t>(a.parent)];
            } else {
                if (a.depth != 1) nested = false;
                ringOneSweep += sweep;
            }

            if ((a.flags & kArcAggregate) == 0) {
                // A real item: its angle is its share of the whole disc, and it is never
                // thinner than the aggregation threshold.
                const double expected = static_cast<double>(arena->node(a.node).allocated_bytes) / rootBytes * kTwoPi;
                if (std::fabs(sweep - expected) > 2e-4) proportional = false;
                if (sweep < minAngle - 1e-5) thick = false;
            }
        }
        CHECK(allFinite);
        CHECK(ordered);
        CHECK(radial);
        CHECK(nested);
        CHECK(proportional);
        CHECK(thick);
        CHECK_NEAR(ringOneSweep, kTwoPi, 1e-4);  // ring 1 always closes the circle

        // Children (plus their "other" slice) fill their parent exactly — on every ring
        // that is not the last one.
        bool filled = true, singleAggregate = true;
        for (size_t i = 0; i < arcs.size(); ++i) {
            const SunburstArc& a = arcs[i];
            if (aggregates[i] > 1) singleAggregate = false;
            if ((a.flags & kArcDirectory) == 0 || a.depth >= params.max_depth) continue;
            if (arena->node(a.node).allocated_bytes == 0) continue;
            const double sweep = static_cast<double>(a.end_angle) - a.start_angle;
            if (std::fabs(childSweep[i] - sweep) > 5e-4) filled = false;
        }
        CHECK(filled);
        CHECK(singleAggregate);

        // Each ring is sorted by angle and free of overlaps (what the binary search needs).
        bool sortedRings = true;
        for (int ring = 1; ring <= layout.ringCount(); ++ring) {
            const auto [first, last] = layout.ringRange(ring);
            for (size_t i = first; i < last; ++i) {
                if (arcs[i].depth != ring) sortedRings = false;
                if (i > first && arcs[i].start_angle < arcs[i - 1].end_angle - 1e-5f) sortedRings = false;
            }
        }
        CHECK(sortedRings);
    }
}

DP_TEST(sunburst_merges_micro_slices_per_parent) {
    // Two real slices and 5 000 files of one byte each: 0.0007 degrees apiece.
    FlatArena arena(ArenaConfig{8192, 1 << 20});
    const uint32_t root = arena.addRoot("/micro");
    arena.addChild(root, "big.mov", fileInit(600'000));
    const uint32_t dir = arena.addChild(root, "dir", dirInit());
    arena.addChild(dir, "half.bin", fileInit(197'500));
    for (int i = 0; i < 2500; ++i) arena.addChild(dir, "tiny-in-dir.txt", fileInit(1));
    for (int i = 0; i < 5000; ++i) arena.addChild(root, "tiny.txt", fileInit(1));
    arena.addChild(root, "medium.jpg", fileInit(195'000));
    arena.finalize();
    const double total = static_cast<double>(arena.node(root).allocated_bytes);

    SunburstLayout layout;
    layout.build(arena, root, sunburstParams(900.0f, 900.0f));
    const auto& arcs = layout.arcs();

    // Ring 1: big.mov, dir, medium.jpg and one "other smaller items" slice.
    const auto [first, last] = layout.ringRange(1);
    REQUIRE(last - first == 4);
    CHECK_EQ(arcs[first + 3].flags & kArcAggregate, static_cast<uint32_t>(kArcAggregate));
    CHECK_EQ(arcs[first + 3].node, root);
    CHECK_NEAR(static_cast<double>(arcs[first + 3].end_angle) - arcs[first + 3].start_angle, 5000.0 / total * kTwoPi, 1e-5);
    CHECK_NEAR(arcs[first + 3].end_angle, kTwoPi, 1e-5);
    CHECK_EQ(layout.arcIndexForNode(root), -1);

    // Ring 2: half.bin and the directory's own "other" slice.
    const auto [first2, last2] = layout.ringRange(2);
    REQUIRE(last2 - first2 == 2);
    CHECK_EQ(arcs[first2].flags, static_cast<uint32_t>(kArcFile));
    CHECK_EQ(arcs[first2 + 1].flags & kArcAggregate, static_cast<uint32_t>(kArcAggregate));
    CHECK_EQ(arcs[first2 + 1].node, dir);
    CHECK_NEAR(static_cast<double>(arcs[first2 + 1].end_angle) - arcs[first2 + 1].start_angle, 2500.0 / total * kTwoPi, 1e-5);
    CHECK_EQ(arcs.size(), size_t{6});  // 7 505 nodes -> 6 arcs

    // With the threshold switched off nothing is merged (and nothing is lost).
    SunburstParams all = sunburstParams(900.0f, 900.0f);
    all.min_angle_degrees = 0.0f;
    layout.build(arena, root, all);
    CHECK_EQ(layout.arcs().size(), size_t{1 + 1 + 1 + 5000 + 1 + 2500});
    for (const SunburstArc& a : layout.arcs()) CHECK((a.flags & kArcAggregate) == 0);
}

DP_TEST(sunburst_depth_limit_and_ring_radii) {
    // A chain 12 directories deep with a file at every level.
    FlatArena arena(ArenaConfig{256, 1 << 16});
    uint32_t current = arena.addRoot("/rings");
    for (int i = 0; i < 12; ++i) {
        arena.addChild(current, "file.bin", fileInit(1000));
        current = arena.addChild(current, "next", dirInit());
    }
    arena.addChild(current, "last.bin", fileInit(1000));
    arena.finalize();

    for (int depth : {1, 3, 6, 9}) {
        SunburstParams params = sunburstParams(1000.0f, 800.0f);
        params.max_depth = depth;
        params.min_angle_degrees = 0.0f;
        SunburstLayout layout;
        layout.build(arena, 0, params);
        CHECK_EQ(layout.ringCount(), depth);
        uint16_t deepest = 0;
        for (const SunburstArc& a : layout.arcs()) deepest = std::max(deepest, a.depth);
        CHECK_EQ(static_cast<int>(deepest), depth);
        CHECK_EQ(layout.arcs().size(), static_cast<size_t>(2 * depth));

        // Rings are contiguous, strictly growing outward, each thinner than the last,
        // and the outermost ends exactly at the outer radius.
        float previousOuter = layout.hubRadius();
        float previousWidth = 1e9f;
        for (int ring = 1; ring <= depth; ++ring) {
            float inner, outer;
            layout.ringBounds(ring, inner, outer);
            CHECK_NEAR(inner, previousOuter, 1e-3);
            CHECK(outer > inner);
            CHECK(outer - inner <= previousWidth + 1e-3f);
            previousWidth = outer - inner;
            previousOuter = outer;
        }
        CHECK_NEAR(previousOuter, layout.outerRadius(), 1e-2);
        CHECK_NEAR(layout.outerRadius(), 400.0f - params.margin, 1e-3);

        // Ring 0 is the hub; rings beyond the visible ones keep growing (used to fly
        // arcs in from outside); negative rings collapse to the centre.
        float inner, outer;
        layout.ringBounds(0, inner, outer);
        CHECK_NEAR(inner, 0.0, 1e-6);
        CHECK_NEAR(outer, layout.hubRadius(), 1e-6);
        layout.ringBounds(depth + 1, inner, outer);
        CHECK_NEAR(inner, layout.outerRadius(), 1e-2);
        CHECK(outer > inner);
        layout.ringBounds(-3, inner, outer);
        CHECK_NEAR(outer, 0.0, 1e-6);
        CHECK(std::isfinite(inner) && std::isfinite(outer));
    }
}

DP_TEST(sunburst_hit_testing) {
    const auto arena = randomTree(71, 5000);
    SunburstLayout layout;
    layout.build(*arena, arena->root(), sunburstParams(1100.0f, 900.0f));
    const auto& arcs = layout.arcs();
    REQUIRE(arcs.size() > 30);
    const float cx = layout.centerX(), cy = layout.centerY();

    // The middle of every arc hits that arc.
    bool middlesHit = true;
    for (size_t i = 0; i < arcs.size(); ++i) {
        const SunburstArc& a = arcs[i];
        const double theta = (static_cast<double>(a.start_angle) + a.end_angle) * 0.5;
        const double radius = (static_cast<double>(a.inner_radius) + a.outer_radius) * 0.5;
        const auto x = static_cast<float>(cx + radius * std::sin(theta));
        const auto y = static_cast<float>(cy - radius * std::cos(theta));
        if (layout.hitTest(x, y) != static_cast<int32_t>(i)) middlesHit = false;
    }
    CHECK(middlesHit);

    CHECK_EQ(layout.hitTest(cx, cy), SunburstLayout::kHitHub);
    CHECK_EQ(layout.hitTest(cx + layout.hubRadius() * 0.7f, cy), SunburstLayout::kHitHub);
    CHECK_EQ(layout.hitTest(cx + layout.outerRadius() + 5.0f, cy), SunburstLayout::kHitNone);
    CHECK_EQ(layout.hitTest(0.0f, 0.0f), SunburstLayout::kHitNone);
    CHECK_EQ(layout.hitTest(-50.0f, 5000.0f), SunburstLayout::kHitNone);
    // 12 o'clock is angle 0: just right of the vertical is inside the first (largest) arc.
    CHECK_EQ(layout.hitTest(cx + 0.5f, cy - (layout.hubRadius() + 2.0f)), 0);

    // Random points agree with a linear scan in polar coordinates.
    Rng rng(5);
    bool agree = true;
    for (int i = 0; i < 20000; ++i) {
        const auto x = static_cast<float>(rng.unit() * 1100.0);
        const auto y = static_cast<float>(rng.unit() * 900.0);
        const double dx = static_cast<double>(x) - cx, dy = static_cast<double>(y) - cy;
        const double radius = std::sqrt(dx * dx + dy * dy);
        double theta = std::atan2(dx, -dy);
        if (theta < 0) theta += kTwoPi;
        int32_t expected = SunburstLayout::kHitNone;
        if (radius < layout.hubRadius()) {
            expected = SunburstLayout::kHitHub;
        } else {
            for (size_t k = 0; k < arcs.size(); ++k) {
                const SunburstArc& a = arcs[k];
                if (radius >= a.inner_radius && radius < a.outer_radius && static_cast<float>(theta) >= a.start_angle &&
                    static_cast<float>(theta) < a.end_angle) {
                    expected = static_cast<int32_t>(k);
                    break;
                }
            }
        }
        const int32_t actual = layout.hitTest(x, y);
        if (actual != expected) {
            // Only tolerate disagreements within float rounding of a ring boundary.
            bool nearBoundary = false;
            for (int ring = 0; ring <= layout.ringCount(); ++ring) {
                float inner, outer;
                layout.ringBounds(ring, inner, outer);
                if (std::fabs(radius - outer) < 1e-3) nearBoundary = true;
            }
            if (!nearBoundary) agree = false;
        }
    }
    CHECK(agree);

    // Node lookup.
    const int32_t index = layout.arcIndexForNode(arcs[5].node);
    if ((arcs[5].flags & kArcAggregate) == 0) CHECK_EQ(index, 5);
    CHECK_EQ(layout.arcIndexForNode(987654), -1);
}

DP_TEST(sunburst_degenerate_inputs_are_safe) {
    SunburstLayout layout;
    const auto arena = randomTree(81, 300);

    layout.build(*arena, arena->root(), sunburstParams(0.0f, 0.0f));
    CHECK(layout.arcs().empty());
    CHECK_EQ(layout.hitTest(0.0f, 0.0f), SunburstLayout::kHitNone);
    layout.build(*arena, arena->root(), sunburstParams(10.0f, 10.0f));  // smaller than the margin
    CHECK(layout.arcs().empty());
    layout.build(*arena, kInvalidNode, sunburstParams(500.0f, 500.0f));
    CHECK(layout.arcs().empty());
    CHECK(std::isfinite(layout.hubRadius()) && std::isfinite(layout.outerRadius()));

    // A single child owns the whole circle: start 0, end 2π, no NaN.
    FlatArena single(ArenaConfig{64, 1 << 16});
    const uint32_t root = single.addRoot("/single");
    const uint32_t only = single.addChild(root, "only", dirInit());
    single.addChild(only, "inner.bin", fileInit(42));
    const uint32_t hollow = single.addChild(root, "hollow", dirInit());
    single.addChild(root, "zero.bin", fileInit(0));
    single.finalize();
    layout.build(single, root, sunburstParams(600.0f, 600.0f));
    REQUIRE(layout.arcs().size() == 2);
    CHECK_NEAR(layout.arcs()[0].start_angle, 0.0, 1e-7);
    CHECK_NEAR(layout.arcs()[0].end_angle, kTwoPi, 1e-5);
    CHECK_NEAR(layout.arcs()[1].end_angle - layout.arcs()[1].start_angle, kTwoPi, 1e-5);
    CHECK_EQ(layout.arcs()[1].parent, 0);
    for (const SunburstArc& a : layout.arcs()) CHECK(finite(a));

    // Empty focus, file focus: a hub and nothing else.
    layout.build(single, hollow, sunburstParams(600.0f, 600.0f));
    CHECK(layout.arcs().empty());
    CHECK_EQ(layout.hitTest(300.0f, 300.0f), SunburstLayout::kHitHub);
    layout.build(single, 2, sunburstParams(600.0f, 600.0f));  // "inner.bin"
    CHECK(layout.arcs().empty());
}

DP_TEST(sunburst_filter_and_colors) {
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/colors");
    const uint32_t movies = arena.addChild(root, "movies", dirInit());
    const uint32_t clip = arena.addChild(movies, "clip.mov", fileInit(400'000));
    const uint32_t docs = arena.addChild(root, "docs", dirInit());
    const uint32_t note = arena.addChild(docs, "note.txt", fileInit(300'000));
    const uint32_t loose = arena.addChild(root, "loose.mov", fileInit(200'000));
    arena.finalize();

    NodeMatchSet matches;
    FilterQuery query;
    query.extension_id = *arena.extensions().find("mov");
    REQUIRE(matches.compute(arena, query));
    SunburstParams params = sunburstParams(800.0f, 800.0f);
    params.matches = &matches;
    SunburstLayout layout;
    layout.build(arena, root, params);

    const auto dimmed = [&layout](uint32_t node) {
        const int32_t index = layout.arcIndexForNode(node);
        return index >= 0 && (layout.arcs()[static_cast<size_t>(index)].flags & kArcDimmed) != 0;
    };
    CHECK(!dimmed(clip));
    CHECK(!dimmed(loose));
    CHECK(!dimmed(movies));  // contains a match
    CHECK(dimmed(docs));
    CHECK(dimmed(note));

    // Files carry their type color; directories on opposite sides get different hues.
    const auto& arcs = layout.arcs();
    const Color clipColor = arcs[static_cast<size_t>(layout.arcIndexForNode(clip))].color;
    const Color looseColor = arcs[static_cast<size_t>(layout.arcIndexForNode(loose))].color;
    CHECK(clipColor == looseColor);
    CHECK(clipColor == arena.extensions().color(*arena.extensions().find("mov")));
    const Color moviesColor = arcs[static_cast<size_t>(layout.arcIndexForNode(movies))].color;
    const Color docsColor = arcs[static_cast<size_t>(layout.arcIndexForNode(docs))].color;
    CHECK(!(moviesColor == docsColor));
}

// ----------------------------------------------------------------------------------------
// Sunburst drill-down transition
// ----------------------------------------------------------------------------------------

namespace {

bool sameGeometry(const float g[4], const SunburstArc& arc, float eps = 1e-4f) {
    return std::fabs(g[0] - arc.start_angle) <= eps && std::fabs(g[1] - arc.end_angle) <= eps &&
           std::fabs(g[2] - arc.inner_radius) <= eps && std::fabs(g[3] - arc.outer_radius) <= eps;
}

bool tweenIsFinite(const SunburstTweenArc& t) {
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(t.from[i]) || !std::isfinite(t.to[i])) return false;
    }
    return std::isfinite(t.alpha_from) && std::isfinite(t.alpha_to);
}

/// Largest directory arc on `ring` of a layout, or -1.
int32_t largestDirectoryArc(const SunburstLayout& layout, int ring) {
    const auto [first, last] = layout.ringRange(ring);
    for (size_t i = first; i < last; ++i) {
        if ((layout.arcs()[i].flags & kArcDirectory) != 0) return static_cast<int32_t>(i);
    }
    return -1;
}

}  // namespace

DP_TEST(sunburst_steady_state_has_no_motion) {
    const auto arena = randomTree(91, 3000);
    SunburstLayout layout;
    layout.build(*arena, arena->root(), sunburstParams(900.0f, 900.0f));
    const Color hub{40, 40, 44, 255};
    const SunburstTransition steady = buildSunburstSteadyState(layout, hub);

    REQUIRE(steady.arcs.size() == layout.arcs().size() + 1);
    CHECK_EQ(steady.first_layout_arc, size_t{0});
    CHECK_EQ(steady.hub_index, layout.arcs().size());
    for (size_t i = 0; i < layout.arcs().size(); ++i) {
        const SunburstTweenArc& t = steady.arcs[i];
        CHECK(sameGeometry(t.from, layout.arcs()[i]) && sameGeometry(t.to, layout.arcs()[i]));
        CHECK_EQ(t.layout_index, static_cast<int32_t>(i));
        CHECK(t.alpha_from == 1.0f && t.alpha_to == 1.0f && !t.is_hub);
    }
    const SunburstTweenArc& hubArc = steady.arcs[steady.hub_index];
    CHECK(hubArc.is_hub);
    CHECK_NEAR(hubArc.to[0], 0.0, 1e-6);
    CHECK_NEAR(hubArc.to[1], kTwoPi, 1e-5);
    CHECK_NEAR(hubArc.to[2], 0.0, 1e-6);
    CHECK_NEAR(hubArc.to[3], layout.hubRadius(), 1e-5);
    CHECK(hubArc.color_to == hub);
}

DP_TEST(sunburst_zoom_in_and_out_are_consistent_maps) {
    for (uint64_t seed : {101ull, 102ull, 103ull}) {
        const auto arena = randomTree(seed, 6000);
        const SunburstParams params = sunburstParams(1000.0f, 1000.0f);
        SunburstLayout outer;
        outer.build(*arena, arena->root(), params);
        for (int ring : {1, 2}) {
            const int32_t pivotIndex = largestDirectoryArc(outer, ring);
            if (pivotIndex < 0) continue;
            const SunburstArc pivot = outer.arcs()[static_cast<size_t>(pivotIndex)];
            SunburstLayout inner;
            inner.build(*arena, pivot.node, params);
            if (inner.arcs().empty()) continue;
            const Color hub{250, 250, 250, 255};
            const float span = pivot.end_angle - pivot.start_angle;

            // ---------------- zoom in: outer -> inner ----------------
            const SunburstTransition in = buildSunburstTransition(*arena, outer, inner, hub);
            CHECK(in.kind == SunburstTransitionKind::ZoomIn);
            REQUIRE(in.arcs.size() >= inner.arcs().size() + 1);
            CHECK_EQ(in.hub_index, in.arcs.size() - 1);
            CHECK(in.arcs[in.hub_index].is_hub);
            CHECK(in.arcs[in.hub_index].alpha_from == 0.0f && in.arcs[in.hub_index].alpha_to == 1.0f);

            bool finite = true, endsOnLayout = true, startsOnScreen = true, leavingFades = true, monotonic = true;
            bool pivotBecomesHub = false;
            size_t persisted = 0;
            for (size_t i = 0; i < in.arcs.size(); ++i) {
                const SunburstTweenArc& t = in.arcs[i];
                finite = finite && tweenIsFinite(t);
                if (t.is_hub) continue;
                // Angles stay ordered at every moment of the animation (the map is monotonic).
                for (float p : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                    const float a0 = t.from[0] + (t.to[0] - t.from[0]) * p;
                    const float a1 = t.from[1] + (t.to[1] - t.from[1]) * p;
                    const float r0 = t.from[2] + (t.to[2] - t.from[2]) * p;
                    const float r1 = t.from[3] + (t.to[3] - t.from[3]) * p;
                    if (a1 < a0 - 1e-5f || r1 < r0 - 1e-4f || a0 < -1e-4f || a1 > kTwoPi + 1e-3) monotonic = false;
                }
                if (t.layout_index >= 0) {
                    const SunburstArc& target = inner.arcs()[static_cast<size_t>(t.layout_index)];
                    if (i != in.first_layout_arc + static_cast<size_t>(t.layout_index)) endsOnLayout = false;
                    if (!sameGeometry(t.to, target) || t.alpha_to != 1.0f) endsOnLayout = false;
                    const int32_t previous = (target.flags & kArcAggregate) == 0 ? outer.arcIndexForNode(target.node) : -1;
                    if (previous >= 0) {
                        // It was already visible: the tween starts exactly where it is
                        // drawn now, and that agrees with the analytic "fold" map.
                        ++persisted;
                        const SunburstArc& was = outer.arcs()[static_cast<size_t>(previous)];
                        if (!sameGeometry(t.from, was) || t.alpha_from != 1.0f) startsOnScreen = false;
                        const float foldedStart = pivot.start_angle + target.start_angle / static_cast<float>(kTwoPi) * span;
                        const float foldedEnd = pivot.start_angle + target.end_angle / static_cast<float>(kTwoPi) * span;
                        if (std::fabs(foldedStart - was.start_angle) > 2e-3f || std::fabs(foldedEnd - was.end_angle) > 2e-3f) {
                            startsOnScreen = false;
                        }
                        if (was.depth != target.depth + pivot.depth) startsOnScreen = false;
                    }
                } else if (t.node == pivot.node && (t.flags & kArcAggregate) == 0) {
                    // The clicked slice opens to a full disc and turns into the hub.
                    pivotBecomesHub = sameGeometry(t.from, pivot) && std::fabs(t.to[0]) < 1e-5f &&
                                      std::fabs(t.to[1] - static_cast<float>(kTwoPi)) < 1e-4f && std::fabs(t.to[2]) < 1e-5f &&
                                      std::fabs(t.to[3] - inner.hubRadius()) < 1e-4f && t.alpha_to == 1.0f && t.color_to == hub;
                } else {
                    if (t.alpha_from != 1.0f || t.alpha_to != 0.0f) leavingFades = false;
                }
            }
            CHECK(finite);
            CHECK(monotonic);
            CHECK(endsOnLayout);
            CHECK(startsOnScreen);
            CHECK(leavingFades);
            CHECK(pivotBecomesHub);
            CHECK(persisted > 0);

            // ---------------- zoom out: inner -> outer ----------------
            const SunburstTransition out = buildSunburstTransition(*arena, inner, outer, hub);
            CHECK(out.kind == SunburstTransitionKind::ZoomOut);
            bool outFinite = true, outEnds = true, hubBecomesSlice = false, outStarts = true;
            for (const SunburstTweenArc& t : out.arcs) {
                outFinite = outFinite && tweenIsFinite(t);
                if (t.is_hub || t.layout_index < 0) continue;
                const SunburstArc& target = outer.arcs()[static_cast<size_t>(t.layout_index)];
                if (!sameGeometry(t.to, target)) outEnds = false;
                if (target.node == pivot.node && (target.flags & kArcAggregate) == 0) {
                    hubBecomesSlice = std::fabs(t.from[0]) < 1e-5f && std::fabs(t.from[1] - static_cast<float>(kTwoPi)) < 1e-4f &&
                                      std::fabs(t.from[2]) < 1e-5f && std::fabs(t.from[3] - inner.hubRadius()) < 1e-4f &&
                                      t.color_from == hub && t.alpha_from == 1.0f;
                }
                const int32_t previous = (target.flags & kArcAggregate) == 0 ? inner.arcIndexForNode(target.node) : -1;
                if (previous >= 0 && !sameGeometry(t.from, inner.arcs()[static_cast<size_t>(previous)])) outStarts = false;
            }
            CHECK(outFinite);
            CHECK(outEnds);
            CHECK(outStarts);
            CHECK(hubBecomesSlice);
        }
    }
}

DP_TEST(sunburst_unrelated_focus_change_blooms) {
    // Two sibling directories: neither contains the other, so there is no zoom relation.
    FlatArena arena(ArenaConfig{4096, 1 << 20});
    const uint32_t root = arena.addRoot("/bloom");
    const uint32_t left = arena.addChild(root, "left", dirInit());
    const uint32_t right = arena.addChild(root, "right", dirInit());
    for (int i = 0; i < 40; ++i) {
        arena.addChild(left, "l" + std::to_string(i) + ".mov", fileInit(1000 + 37 * static_cast<uint64_t>(i)));
        arena.addChild(right, "r" + std::to_string(i) + ".jpg", fileInit(500 + 91 * static_cast<uint64_t>(i)));
    }
    arena.finalize();
    const SunburstParams params = sunburstParams(800.0f, 800.0f);
    SunburstLayout a, b, empty;
    a.build(arena, left, params);
    b.build(arena, right, params);
    REQUIRE(!a.arcs().empty() && !b.arcs().empty());
    const Color hub{30, 30, 30, 255};

    // From a sibling, or from nothing at all (first scan): the new arcs unfurl.
    for (const SunburstLayout* from : {&a, &empty}) {
        const SunburstTransition t = buildSunburstTransition(arena, *from, b, hub);
        CHECK(t.kind == SunburstTransitionKind::Bloom);
        REQUIRE(t.arcs.size() == b.arcs().size() + 1);  // nothing from the old layout lingers
        CHECK_EQ(t.first_layout_arc, size_t{0});
        bool unfurls = true;
        for (size_t i = 0; i < b.arcs().size(); ++i) {
            const SunburstTweenArc& arc = t.arcs[i];
            if (arc.from[0] != 0.0f || arc.from[1] != 0.0f || !sameGeometry(arc.to, b.arcs()[i])) unfurls = false;
            if (arc.alpha_from != 0.0f || arc.alpha_to != 1.0f || !tweenIsFinite(arc)) unfurls = false;
        }
        CHECK(unfurls);
        CHECK(t.arcs[t.hub_index].is_hub && t.arcs[t.hub_index].alpha_from == 1.0f);
    }

    // A different viewport: also no geometric tween.
    SunburstLayout resized;
    resized.build(arena, root, sunburstParams(500.0f, 500.0f));
    CHECK(buildSunburstTransition(arena, resized, b, hub).kind == SunburstTransitionKind::Bloom);
}

DP_TEST(sunburst_same_focus_change_morphs_in_place) {
    // Removing an item (or switching the metric) keeps the focus: arcs that survive slide
    // to their new position, the removed one fades out where it was.
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/morph");
    const uint32_t a = arena.addChild(root, "a.mov", fileInit(500));
    const uint32_t b = arena.addChild(root, "b.jpg", fileInit(300));
    const uint32_t c = arena.addChild(root, "c.txt", fileInit(200));
    arena.finalize();
    const SunburstParams params = sunburstParams(600.0f, 600.0f);
    SunburstLayout before;
    before.build(arena, root, params);
    REQUIRE(before.arcs().size() == 3);

    REQUIRE(arena.removeNode(b).removed);
    arena.resortAncestorChain(root);
    SunburstLayout after;
    after.build(arena, root, params);
    REQUIRE(after.arcs().size() == 2);

    const Color hub{20, 20, 20, 255};
    const SunburstTransition t = buildSunburstTransition(arena, before, after, hub);
    CHECK(t.kind == SunburstTransitionKind::Morph);
    REQUIRE(t.arcs.size() == 4);  // b leaving, a and c staying, hub
    CHECK_EQ(t.first_layout_arc, size_t{1});

    const SunburstTweenArc& leaving = t.arcs[0];
    CHECK_EQ(leaving.node, b);
    CHECK_EQ(leaving.layout_index, -1);
    CHECK(sameGeometry(leaving.from, before.arcs()[1]) && sameGeometry(leaving.to, before.arcs()[1]));
    CHECK(leaving.alpha_from == 1.0f && leaving.alpha_to == 0.0f);

    for (size_t i = 0; i < 2; ++i) {
        const SunburstTweenArc& staying = t.arcs[t.first_layout_arc + i];
        const uint32_t node = i == 0 ? a : c;
        CHECK_EQ(staying.node, node);
        CHECK(sameGeometry(staying.from, before.arcs()[static_cast<size_t>(before.arcIndexForNode(node))]));
        CHECK(sameGeometry(staying.to, after.arcs()[i]));
        CHECK(staying.alpha_from == 1.0f && staying.alpha_to == 1.0f);
    }
    // a grew from 50 % to 5/7 of the circle; c slid along behind it.
    CHECK_NEAR(t.arcs[1].to[1] - t.arcs[1].to[0], kTwoPi * 5.0 / 7.0, 1e-4);
    CHECK_NEAR(t.arcs[2].from[0], kTwoPi * 0.8, 1e-4);
    CHECK_NEAR(t.arcs[2].to[0], kTwoPi * 5.0 / 7.0, 1e-4);
    CHECK(t.arcs[t.hub_index].is_hub && t.arcs[t.hub_index].alpha_from == 1.0f);

    // An identical layout morphs into itself without any motion.
    const SunburstTransition same = buildSunburstTransition(arena, after, after, hub);
    CHECK(same.kind == SunburstTransitionKind::Morph);
    for (const SunburstTweenArc& arc : same.arcs) {
        for (int k = 0; k < 4; ++k) CHECK(arc.from[k] == arc.to[k]);
    }
}

DP_TEST_MAIN()
