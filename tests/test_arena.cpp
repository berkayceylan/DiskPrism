// Flat arena, string pool, extension table, incremental deletion and filter tests.
#include "TestHarness.hpp"

#include "ColorSpace.hpp"
#include "ExtensionTable.hpp"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"
#include "StringPool.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace diskprism;

namespace {

/// SplitMix64: tiny deterministic PRNG so the property tests are reproducible.
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
};

NodeInit dirInit() {
    NodeInit init;
    init.flags = kNodeDir;
    return init;
}

NodeInit fileInit(uint64_t allocated, uint64_t logical, int64_t mtime = 1700000000) {
    NodeInit init;
    init.allocated_bytes = allocated;
    init.logical_bytes = logical;
    init.mtime = mtime;
    return init;
}

struct SampleTree {
    FlatArena arena{ArenaConfig{4096, 1 << 20}};
    uint32_t root, docs, a_txt, b_pdf, nested, deep_mov, notes, media, clip_mov, song_mp3, empty, readme;

    SampleTree() {
        root = arena.addRoot("/vol");
        docs = arena.addChild(root, "docs", dirInit());
        a_txt = arena.addChild(docs, "a.txt", fileInit(4096, 100));
        b_pdf = arena.addChild(docs, "b.PDF", fileInit(8192, 7000));
        nested = arena.addChild(docs, "nested", dirInit());
        deep_mov = arena.addChild(nested, "deep.mov", fileInit(1048576, 1000000));
        notes = arena.addChild(nested, "notes", fileInit(4096, 10));
        media = arena.addChild(root, "media", dirInit());
        clip_mov = arena.addChild(media, "clip.mov", fileInit(2097152, 2000000));
        song_mp3 = arena.addChild(media, "song.mp3", fileInit(524288, 500000));
        empty = arena.addChild(root, "empty", dirInit());
        readme = arena.addChild(root, "README", fileInit(4096, 1200));
    }
};

struct Totals {
    uint64_t allocated = 0;
    uint64_t logical = 0;
    uint64_t files = 0;
    bool operator==(const Totals&) const = default;
};

/// Reference implementation: recursive sum over live children.
Totals bruteForce(const FlatArena& arena, uint32_t idx) {
    const FsNode& n = arena.node(idx);
    if (!n.is_dir()) return Totals{n.allocated_bytes, n.logical_bytes, 1};
    Totals total;
    arena.forEachChild(idx, [&](uint32_t c) {
        const Totals t = bruteForce(arena, c);
        total.allocated += t.allocated;
        total.logical += t.logical;
        total.files += t.files;
    });
    return total;
}

/// Every directory's stored totals must equal the recursive sum of its live children.
bool totalsConsistent(const FlatArena& arena) {
    for (uint32_t i = 0; i < arena.size(); ++i) {
        const FsNode& n = arena.node(i);
        if (n.is_deleted() || !n.is_dir()) continue;
        const Totals t = bruteForce(arena, i);
        if (t.allocated != n.allocated_bytes || t.logical != n.logical_bytes || t.files != n.file_count) return false;
    }
    return true;
}

/// Extension histogram recomputed independently of the arena's own bookkeeping.
std::map<std::string, ExtensionStats> referenceHistogram(const FlatArena& arena) {
    std::map<std::string, ExtensionStats> histogram;
    for (uint32_t i = 0; i < arena.size(); ++i) {
        const FsNode& n = arena.node(i);
        if (n.is_deleted() || n.is_dir()) continue;
        ExtensionStats& s = histogram[std::string(arena.extensions().name(n.ext_id))];
        s.allocated_bytes += n.allocated_bytes;
        s.logical_bytes += n.logical_bytes;
        s.file_count += 1;
    }
    return histogram;
}

std::map<std::string, ExtensionStats> storedHistogram(const FlatArena& arena) {
    std::map<std::string, ExtensionStats> histogram;
    const ExtensionTable& table = arena.extensions();
    for (size_t id = 0; id < table.size(); ++id) {
        const ExtensionStats s = table.stats(static_cast<uint16_t>(id));
        if (s.file_count > 0 || s.allocated_bytes > 0 || s.logical_bytes > 0) {
            histogram[std::string(table.name(static_cast<uint16_t>(id)))] = s;
        }
    }
    return histogram;
}

std::vector<std::string> childNames(const FlatArena& arena, uint32_t idx) {
    std::vector<std::string> names;
    arena.forEachChild(idx, [&](uint32_t c) { names.emplace_back(arena.name(c)); });
    return names;
}

bool childrenSortedDescending(const FlatArena& arena, SizeMetric metric) {
    for (uint32_t i = 0; i < arena.size(); ++i) {
        const FsNode& n = arena.node(i);
        if (n.is_deleted() || !n.is_dir()) continue;
        uint64_t previous = UINT64_MAX;
        uint32_t counted = 0;
        bool ok = true;
        arena.forEachChild(i, [&](uint32_t c) {
            const uint64_t size = arena.node(c).bytes(metric);
            if (size > previous) ok = false;
            previous = size;
            ++counted;
        });
        if (!ok || counted != n.child_count) return false;
    }
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------------------
// Memory layout
// ----------------------------------------------------------------------------------------

DP_TEST(node_is_compact_plain_data) {
    CHECK(sizeof(FsNode) <= 64);
    CHECK_EQ(sizeof(FsNode), size_t{56});
    CHECK(std::is_trivially_copyable_v<FsNode>);
    CHECK(std::is_standard_layout_v<FsNode>);
    CHECK_EQ(kInvalidNode, UINT32_MAX);
}

DP_TEST(virtual_region_reserves_lazily) {
    // 64 GiB of address space must cost (almost) no memory until it is touched.
    VirtualRegion region(size_t{64} << 30);
    REQUIRE(region.data() != nullptr);
    CHECK(region.size() >= (size_t{64} << 30));
    auto* bytes = static_cast<unsigned char*>(region.data());
    CHECK_EQ(bytes[0], 0);  // zero-filled
    bytes[0] = 7;
    bytes[region.size() - 1] = 9;
    CHECK_EQ(bytes[0], 7);
    CHECK_EQ(bytes[region.size() - 1], 9);

    VirtualRegion moved = std::move(region);
    CHECK(region.data() == nullptr);
    CHECK_EQ(static_cast<unsigned char*>(moved.data())[0], 7);
}

// ----------------------------------------------------------------------------------------
// String pool
// ----------------------------------------------------------------------------------------

DP_TEST(string_pool_append_and_view) {
    StringPool pool(1 << 16);
    const uint32_t a = pool.append("hello");
    const uint32_t b = pool.append("");
    const uint32_t c = pool.append("wörld.txt");
    CHECK_EQ(a, 0u);
    CHECK_EQ(b, 5u);
    CHECK_EQ(c, 5u);
    CHECK_EQ(pool.view(a, 5), "hello");
    CHECK_EQ(pool.view(c, 10), "wörld.txt");
    CHECK_EQ(pool.size(), uint64_t{15});
}

DP_TEST(string_pool_reports_exhaustion) {
    StringPool pool(64);
    CHECK(pool.capacity() >= 64);
    const auto capacity = static_cast<uint32_t>(pool.capacity());
    CHECK(pool.allocate(capacity - 8) != StringPool::kInvalidOffset);
    CHECK_EQ(pool.allocate(9), StringPool::kInvalidOffset);  // does not fit
    CHECK_EQ(pool.size(), uint64_t{capacity - 8});            // and reserved nothing
    CHECK(pool.allocate(8) != StringPool::kInvalidOffset);
    CHECK_EQ(pool.allocate(1), StringPool::kInvalidOffset);
}

DP_TEST(string_pool_concurrent_appends_are_disjoint) {
    StringPool pool(8 << 20);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 4000;
    std::vector<std::vector<std::pair<uint32_t, std::string>>> written(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                std::string text = "thread" + std::to_string(t) + "-item" + std::to_string(i);
                const uint32_t offset = pool.append(text);
                written[t].emplace_back(offset, std::move(text));
            }
        });
    }
    for (std::thread& th : threads) th.join();

    uint64_t total = 0;
    bool intact = true;
    for (const auto& perThread : written) {
        for (const auto& [offset, text] : perThread) {
            total += text.size();
            if (offset == StringPool::kInvalidOffset || pool.view(offset, static_cast<uint32_t>(text.size())) != text) {
                intact = false;
            }
        }
    }
    CHECK(intact);
    CHECK_EQ(pool.size(), total);
}

// ----------------------------------------------------------------------------------------
// Extension table
// ----------------------------------------------------------------------------------------

DP_TEST(extension_parsing_rules) {
    std::array<char, ExtensionTable::kMaxExtensionLength> scratch;
    CHECK_EQ(ExtensionTable::parseExtension("Movie.MOV", scratch), "mov");
    CHECK_EQ(ExtensionTable::parseExtension("archive.tar.gz", scratch), "gz");
    CHECK_EQ(ExtensionTable::parseExtension("libfoo.1.2.dylib", scratch), "dylib");
    CHECK_EQ(ExtensionTable::parseExtension("Makefile", scratch), "");
    CHECK_EQ(ExtensionTable::parseExtension(".zshrc", scratch), "");          // dotfile
    CHECK_EQ(ExtensionTable::parseExtension(".config.json", scratch), "json"); // dotfile with a real suffix
    CHECK_EQ(ExtensionTable::parseExtension("trailing.", scratch), "");
    CHECK_EQ(ExtensionTable::parseExtension("Letter to Dr. Smith", scratch), "");  // prose, not a type
    CHECK_EQ(ExtensionTable::parseExtension("a.b c", scratch), "");
    CHECK_EQ(ExtensionTable::parseExtension("x.thisextensionismuchtoolong", scratch), "");
    CHECK_EQ(ExtensionTable::parseExtension("x.12345678901234567890", scratch), "12345678901234567890");
    CHECK_EQ(ExtensionTable::parseExtension("", scratch), "");
    CHECK_EQ(ExtensionTable::parseExtension("photo.HEIC", scratch), "heic");
}

DP_TEST(extension_ids_are_stable_and_shared) {
    ExtensionTable table;
    CHECK_EQ(table.name(ExtensionTable::kNone), "<none>");
    CHECK_EQ(table.name(ExtensionTable::kOther), "<other>");
    CHECK_EQ(table.internFilename("README"), ExtensionTable::kNone);

    const uint16_t mov = table.internFilename("a.mov");
    CHECK_EQ(table.internFilename("B.MOV"), mov);
    CHECK_EQ(table.intern("mov"), mov);
    CHECK_EQ(table.name(mov), "mov");
    CHECK(table.find("mov").has_value());
    CHECK(!table.find("definitely-not-seen").has_value());

    // Well-known extensions are pre-interned: two fresh tables agree on their IDs.
    ExtensionTable other;
    CHECK_EQ(other.intern("mov"), mov);
    CHECK_EQ(other.intern("dylib"), table.intern("dylib"));
    CHECK_EQ(other.intern("dmg"), table.intern("dmg"));

    const size_t before = table.size();
    const uint16_t fresh = table.intern("zzqq");
    CHECK_EQ(table.size(), before + 1);
    CHECK_EQ(table.intern("zzqq"), fresh);
    CHECK_EQ(table.size(), before + 1);
}

DP_TEST(extension_interning_is_thread_safe) {
    ExtensionTable table;
    constexpr int kThreads = 8;
    constexpr int kDistinct = 601;  // prime, so every thread's stride visits all names
    std::vector<std::vector<uint16_t>> ids(kThreads, std::vector<uint16_t>(kDistinct));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ExtensionCache cache(table);
            for (int round = 0; round < 3; ++round) {
                for (int i = 0; i < kDistinct; ++i) {
                    // Different threads walk the names in different orders.
                    const int k = (i * (2 * t + 1) + t * 17) % kDistinct;
                    ids[t][k] = cache.idForFilename("file.x" + std::to_string(k));
                }
            }
        });
    }
    for (std::thread& th : threads) th.join();

    bool agree = true;
    std::set<uint16_t> distinct;
    for (int k = 0; k < kDistinct; ++k) {
        distinct.insert(ids[0][k]);
        for (int t = 1; t < kThreads; ++t) agree = agree && ids[t][k] == ids[0][k];
        if (table.name(ids[0][k]) != "x" + std::to_string(k)) agree = false;
    }
    CHECK(agree);
    CHECK_EQ(distinct.size(), size_t{kDistinct});
}

DP_TEST(palette_is_deterministic_and_perceptually_distinct) {
    // Deterministic: a pure function of the rank.
    for (uint32_t rank = 0; rank < 200; ++rank) {
        CHECK(ExtensionTable::paletteColor(rank) == ExtensionTable::paletteColor(rank));
    }
    // The dozen largest file types must be clearly distinguishable from one another
    // (OKLab distance; ~0.02 is a just-noticeable difference).
    float minTop12 = 1e9f;
    for (uint32_t i = 0; i < 12; ++i) {
        for (uint32_t j = i + 1; j < 12; ++j) {
            minTop12 = std::min(minTop12, oklabDistance(ExtensionTable::paletteColor(i), ExtensionTable::paletteColor(j)));
        }
    }
    CHECK(minTop12 > 0.07f);
    // The two largest types in particular.
    CHECK(oklabDistance(ExtensionTable::paletteColor(0), ExtensionTable::paletteColor(1)) > 0.2f);
    // And the first four tiers never collide.
    float minTop48 = 1e9f;
    for (uint32_t i = 0; i < 48; ++i) {
        for (uint32_t j = i + 1; j < 48; ++j) {
            minTop48 = std::min(minTop48, oklabDistance(ExtensionTable::paletteColor(i), ExtensionTable::paletteColor(j)));
        }
    }
    CHECK(minTop48 > 0.025f);
    std::printf("      palette min OKLab distance: top-12 = %.3f, top-48 = %.3f\n", minTop12, minTop48);
}

DP_TEST(oklch_conversion_is_sane) {
    const Color white = oklchToSrgb(1.0f, 0.0f, 0.0f);
    const Color black = oklchToSrgb(0.0f, 0.0f, 0.0f);
    CHECK(white.r >= 254 && white.g >= 254 && white.b >= 254);
    CHECK(black.r <= 1 && black.g <= 1 && black.b <= 1);
    const Color gray = oklchToSrgb(0.6f, 0.0f, 123.0f);
    CHECK(std::abs(gray.r - gray.g) <= 1 && std::abs(gray.g - gray.b) <= 1);
    // Out-of-gamut chroma is reduced, never clipped into a different hue: still valid.
    const Color vivid = oklchToSrgb(0.7f, 0.9f, 250.0f);
    CHECK(vivid.b > vivid.r);
    CHECK_NEAR(oklabDistance(gray, gray), 0.0, 1e-6);
    CHECK(oklabDistance(white, black) > 0.95f);
}

// ----------------------------------------------------------------------------------------
// Insertion and traversal
// ----------------------------------------------------------------------------------------

DP_TEST(arena_insertion_and_traversal) {
    SampleTree t;
    FlatArena& arena = t.arena;
    CHECK_EQ(arena.root(), 0u);
    CHECK_EQ(arena.size(), 12u);
    CHECK_EQ(arena.liveNodeCount(), 12u);

    CHECK_EQ(arena.node(t.root).parent_idx, kInvalidNode);
    CHECK_EQ(arena.node(t.docs).parent_idx, t.root);
    CHECK_EQ(arena.node(t.deep_mov).parent_idx, t.nested);
    CHECK_EQ(arena.name(t.deep_mov), "deep.mov");
    CHECK_EQ(arena.name(t.root), "/vol");
    CHECK(arena.node(t.docs).is_dir());
    CHECK(!arena.node(t.a_txt).is_dir());

    // Children index > parent index: the invariant every linear sweep relies on.
    for (uint32_t i = 1; i < arena.size(); ++i) CHECK(arena.node(i).parent_idx < i);

    auto rootChildren = childNames(arena, t.root);
    std::sort(rootChildren.begin(), rootChildren.end());
    CHECK_EQ(rootChildren.size(), size_t{4});
    CHECK_EQ(rootChildren[0], "README");
    CHECK_EQ(rootChildren[1], "docs");
    CHECK_EQ(rootChildren[2], "empty");
    CHECK_EQ(rootChildren[3], "media");
    CHECK_EQ(arena.node(t.root).child_count, 4u);
    CHECK_EQ(arena.node(t.empty).child_count, 0u);
    CHECK_EQ(arena.firstChild(t.empty), kInvalidNode);
    CHECK_EQ(arena.firstChild(t.a_txt), kInvalidNode);

    CHECK_EQ(arena.path(t.root), "/vol");
    CHECK_EQ(arena.path(t.deep_mov), "/vol/docs/nested/deep.mov");
    CHECK_EQ(arena.relativePath(t.deep_mov, t.root), "docs/nested/deep.mov");
    CHECK_EQ(arena.relativePath(t.deep_mov, t.docs), "nested/deep.mov");
    CHECK_EQ(arena.relativePath(t.docs, t.docs), "docs");

    CHECK_EQ(arena.depth(t.root), 0u);
    CHECK_EQ(arena.depth(t.deep_mov), 3u);
    CHECK(arena.isAncestorOrSelf(t.root, t.deep_mov));
    CHECK(arena.isAncestorOrSelf(t.docs, t.deep_mov));
    CHECK(arena.isAncestorOrSelf(t.deep_mov, t.deep_mov));
    CHECK(!arena.isAncestorOrSelf(t.media, t.deep_mov));
    CHECK(!arena.isAncestorOrSelf(t.deep_mov, t.docs));
    CHECK_EQ(arena.childToward(t.root, t.deep_mov), t.docs);
    CHECK_EQ(arena.childToward(t.docs, t.deep_mov), t.nested);
    CHECK_EQ(arena.childToward(t.media, t.deep_mov), kInvalidNode);
    CHECK_EQ(arena.childToward(t.docs, t.docs), kInvalidNode);
}

DP_TEST(arena_root_path_slash_is_not_doubled) {
    FlatArena arena(ArenaConfig{64, 4096});
    const uint32_t root = arena.addRoot("/");
    const uint32_t usr = arena.addChild(root, "usr", dirInit());
    const uint32_t bin = arena.addChild(usr, "bin", dirInit());
    CHECK_EQ(arena.path(root), "/");
    CHECK_EQ(arena.path(usr), "/usr");
    CHECK_EQ(arena.path(bin), "/usr/bin");
    CHECK_EQ(arena.relativePath(bin, root), "usr/bin");
}

DP_TEST(arena_paths_deeper_than_inline_buffer) {
    FlatArena arena(ArenaConfig{1024, 1 << 16});
    uint32_t current = arena.addRoot("/r");
    std::string expected = "/r";
    for (int i = 0; i < 300; ++i) {
        const std::string name = "d" + std::to_string(i);
        current = arena.addChild(current, name, dirInit());
        expected += "/" + name;
    }
    CHECK_EQ(arena.depth(current), 300u);
    CHECK_EQ(arena.path(current), expected);
    CHECK(arena.isAncestorOrSelf(0, current));
}

DP_TEST(arena_capacity_limits_are_reported) {
    FlatArena arena(ArenaConfig{8, 4096});
    CHECK_EQ(arena.capacity(), 8u);
    const uint32_t root = arena.addRoot("/r");
    for (int i = 0; i < 7; ++i) CHECK(arena.addChild(root, "f" + std::to_string(i), fileInit(1, 1)) != kInvalidNode);
    CHECK_EQ(arena.size(), 8u);
    CHECK_EQ(arena.addChild(root, "overflow", fileInit(1, 1)), kInvalidNode);
    CHECK_EQ(arena.allocNodes(1), kInvalidNode);
    CHECK_EQ(arena.size(), 8u);  // a failed allocation reserves nothing
    arena.finalize();
    CHECK_EQ(arena.node(root).file_count, 7u);
}

DP_TEST(arena_concurrent_node_allocation_is_disjoint) {
    FlatArena arena(ArenaConfig{1 << 20, 4096});
    constexpr int kThreads = 8;
    constexpr int kRounds = 3000;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> ranges(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kRounds; ++i) {
                const uint32_t count = 1 + static_cast<uint32_t>((i * 7 + t) % 13);
                const uint32_t base = arena.allocNodes(count);
                if (base != kInvalidNode) ranges[t].emplace_back(base, count);
            }
        });
    }
    for (std::thread& th : threads) th.join();

    std::vector<std::pair<uint32_t, uint32_t>> all;
    for (const auto& r : ranges) all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end());
    uint32_t expectedNext = 0;
    bool contiguous = true;
    for (const auto& [base, count] : all) {
        if (base != expectedNext) contiguous = false;
        expectedNext = base + count;
    }
    CHECK(contiguous);  // no gaps, no overlaps
    CHECK_EQ(arena.size(), expectedNext);
}

// ----------------------------------------------------------------------------------------
// Aggregation, sorting, histogram
// ----------------------------------------------------------------------------------------

DP_TEST(finalize_aggregates_bytes_and_counts) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize(SizeMetric::Physical);

    CHECK_EQ(arena.node(t.nested).allocated_bytes, uint64_t{1048576 + 4096});
    CHECK_EQ(arena.node(t.nested).logical_bytes, uint64_t{1000000 + 10});
    CHECK_EQ(arena.node(t.nested).file_count, 2u);
    CHECK_EQ(arena.node(t.docs).allocated_bytes, uint64_t{4096 + 8192 + 1048576 + 4096});
    CHECK_EQ(arena.node(t.docs).file_count, 4u);
    CHECK_EQ(arena.node(t.media).allocated_bytes, uint64_t{2097152 + 524288});
    CHECK_EQ(arena.node(t.media).file_count, 2u);
    CHECK_EQ(arena.node(t.empty).allocated_bytes, uint64_t{0});
    CHECK_EQ(arena.node(t.empty).file_count, 0u);
    CHECK_EQ(arena.node(t.root).allocated_bytes, uint64_t{4096 + 8192 + 1048576 + 4096 + 2097152 + 524288 + 4096});
    CHECK_EQ(arena.node(t.root).logical_bytes, uint64_t{100 + 7000 + 1000000 + 10 + 2000000 + 500000 + 1200});
    CHECK_EQ(arena.node(t.root).file_count, 7u);
    CHECK_EQ(arena.node(t.a_txt).file_count, 1u);
    CHECK(totalsConsistent(arena));

    // Idempotent.
    arena.finalize(SizeMetric::Physical);
    CHECK_EQ(arena.node(t.root).allocated_bytes, uint64_t{4096 + 8192 + 1048576 + 4096 + 2097152 + 524288 + 4096});
    CHECK_EQ(arena.node(t.root).file_count, 7u);
}

DP_TEST(children_are_sorted_largest_first) {
    SampleTree t;
    FlatArena& arena = t.arena;
    CHECK(!arena.isSorted());
    arena.finalize(SizeMetric::Physical);
    CHECK(arena.isSorted());
    CHECK(arena.sortedMetric() == SizeMetric::Physical);
    CHECK(childrenSortedDescending(arena, SizeMetric::Physical));

    const auto rootChildren = childNames(arena, t.root);
    REQUIRE(rootChildren.size() == 4);
    CHECK_EQ(rootChildren[0], "media");
    CHECK_EQ(rootChildren[1], "docs");
    CHECK_EQ(rootChildren[2], "README");
    CHECK_EQ(rootChildren[3], "empty");

    const auto docsChildren = childNames(arena, t.docs);
    REQUIRE(docsChildren.size() == 3);
    CHECK_EQ(docsChildren[0], "nested");
    CHECK_EQ(docsChildren[1], "b.PDF");
    CHECK_EQ(docsChildren[2], "a.txt");

    // Switching the metric re-sorts every list.
    FlatArena other(ArenaConfig{64, 4096});
    const uint32_t root = other.addRoot("/r");
    other.addChild(root, "sparse.img", fileInit(4096, 1'000'000'000));
    other.addChild(root, "dense.bin", fileInit(50'000'000, 50'000'000));
    other.finalize(SizeMetric::Physical);
    CHECK_EQ(other.name(other.firstChild(root)), "dense.bin");
    other.sortChildren(SizeMetric::Logical);
    CHECK(other.sortedMetric() == SizeMetric::Logical);
    CHECK_EQ(other.name(other.firstChild(root)), "sparse.img");
    CHECK(childrenSortedDescending(other, SizeMetric::Logical));
}

DP_TEST(extension_histogram_is_accurate) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    const ExtensionTable& table = arena.extensions();

    const auto mov = table.find("mov");
    const auto pdf = table.find("pdf");
    const auto txt = table.find("txt");
    const auto mp3 = table.find("mp3");
    REQUIRE(mov && pdf && txt && mp3);

    CHECK(table.stats(*mov) == (ExtensionStats{1048576 + 2097152, 1000000 + 2000000, 2}));
    CHECK(table.stats(*pdf) == (ExtensionStats{8192, 7000, 1}));  // "b.PDF" is folded to "pdf"
    CHECK(table.stats(*txt) == (ExtensionStats{4096, 100, 1}));
    CHECK(table.stats(*mp3) == (ExtensionStats{524288, 500000, 1}));
    CHECK(table.stats(ExtensionTable::kNone) == (ExtensionStats{4096 + 4096, 10 + 1200, 2}));  // notes, README

    const ExtensionStats totals = table.totals();
    CHECK_EQ(totals.allocated_bytes, arena.node(t.root).allocated_bytes);
    CHECK_EQ(totals.logical_bytes, arena.node(t.root).logical_bytes);
    CHECK_EQ(totals.file_count, uint64_t{arena.node(t.root).file_count});

    const auto ranked = table.rankedBySize(SizeMetric::Physical);
    REQUIRE(ranked.size() == 5);
    CHECK_EQ(ranked[0], *mov);
    CHECK_EQ(ranked[1], *mp3);
    CHECK_EQ(ranked[2], ExtensionTable::kNone);  // 8192 bytes in two files…
    CHECK_EQ(ranked[3], *pdf);                   // …outranks 8192 bytes in one
    CHECK_EQ(ranked[4], *txt);
    // The largest extension gets the first palette color; types never share one here.
    CHECK(table.color(*mov) == ExtensionTable::paletteColor(0));
    CHECK(table.color(*mp3) == ExtensionTable::paletteColor(1));
    // Files without an extension are ranked like any other type (third largest here).
    CHECK(table.color(ExtensionTable::kNone) == ExtensionTable::paletteColor(2));
    CHECK(table.color(*pdf) == ExtensionTable::paletteColor(3));
    CHECK(table.color(*txt) == ExtensionTable::paletteColor(4));
    CHECK(storedHistogram(arena) == referenceHistogram(arena));
}

// ----------------------------------------------------------------------------------------
// Incremental deletion
// ----------------------------------------------------------------------------------------

DP_TEST(remove_file_rolls_back_exactly_along_ancestor_chain) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    std::vector<FsNode> before;
    for (uint32_t i = 0; i < arena.size(); ++i) before.push_back(arena.node(i));

    const RemovalResult result = arena.removeNode(t.deep_mov);
    REQUIRE(result.removed);
    CHECK_EQ(result.allocated_bytes, uint64_t{1048576});
    CHECK_EQ(result.logical_bytes, uint64_t{1000000});
    CHECK_EQ(result.file_count, 1u);
    CHECK_EQ(result.node_count, 1u);

    // Exactly the ancestors changed — and only in their totals. No list was relinked and
    // no unrelated node was touched: that is what makes the rollback O(depth).
    const std::set<uint32_t> ancestors{t.nested, t.docs, t.root};
    for (uint32_t i = 0; i < arena.size(); ++i) {
        const FsNode& now = arena.node(i);
        const FsNode& was = before[i];
        CHECK_EQ(now.first_child_idx, was.first_child_idx);
        CHECK_EQ(now.next_sibling_idx, was.next_sibling_idx);
        CHECK_EQ(now.parent_idx, was.parent_idx);
        if (i == t.deep_mov) {
            CHECK(now.is_deleted());
            CHECK_EQ(now.allocated_bytes, was.allocated_bytes);
        } else if (ancestors.count(i) != 0) {
            CHECK_EQ(now.allocated_bytes, was.allocated_bytes - 1048576);
            CHECK_EQ(now.logical_bytes, was.logical_bytes - 1000000);
            CHECK_EQ(now.file_count, was.file_count - 1);
            CHECK_EQ(now.flags, was.flags);
        } else {
            CHECK_EQ(now.allocated_bytes, was.allocated_bytes);
            CHECK_EQ(now.logical_bytes, was.logical_bytes);
            CHECK_EQ(now.file_count, was.file_count);
            CHECK_EQ(now.child_count, was.child_count);
            CHECK_EQ(now.flags, was.flags);
        }
    }
    CHECK_EQ(arena.node(t.nested).child_count, 1u);
    CHECK_EQ(arena.liveNodeCount(), 11u);
    CHECK(!arena.isValid(t.deep_mov));
    CHECK(totalsConsistent(arena));

    // Traversal no longer sees the tombstone.
    const auto nestedChildren = childNames(arena, t.nested);
    REQUIRE(nestedChildren.size() == 1);
    CHECK_EQ(nestedChildren[0], "notes");

    // The histogram was updated in step: identical to a from-scratch recomputation.
    const auto incremental = storedHistogram(arena);
    CHECK(incremental == referenceHistogram(arena));
    const auto mov = arena.extensions().find("mov");
    REQUIRE(mov.has_value());
    CHECK(arena.extensions().stats(*mov) == (ExtensionStats{2097152, 2000000, 1}));
    arena.rebuildExtensionStats();
    CHECK(storedHistogram(arena) == incremental);

    // Removing it again, removing the root or a bogus index: refused, nothing changes.
    CHECK(!arena.removeNode(t.deep_mov).removed);
    CHECK(!arena.removeNode(t.root).removed);
    CHECK(!arena.removeNode(kInvalidNode).removed);
    CHECK(!arena.removeNode(9999).removed);
    CHECK(totalsConsistent(arena));
}

DP_TEST(remove_subtree_rolls_back_and_tombstones_descendants) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    const uint64_t rootAllocated = arena.node(t.root).allocated_bytes;
    const uint64_t rootLogical = arena.node(t.root).logical_bytes;
    const uint64_t docsAllocated = arena.node(t.docs).allocated_bytes;
    const uint64_t docsLogical = arena.node(t.docs).logical_bytes;

    const RemovalResult result = arena.removeNode(t.docs);
    REQUIRE(result.removed);
    CHECK_EQ(result.allocated_bytes, docsAllocated);
    CHECK_EQ(result.logical_bytes, docsLogical);
    CHECK_EQ(result.file_count, 4u);
    CHECK_EQ(result.node_count, 6u);  // docs, a.txt, b.PDF, nested, deep.mov, notes

    for (uint32_t idx : {t.docs, t.a_txt, t.b_pdf, t.nested, t.deep_mov, t.notes}) CHECK(arena.node(idx).is_deleted());
    for (uint32_t idx : {t.root, t.media, t.clip_mov, t.song_mp3, t.empty, t.readme}) CHECK(!arena.node(idx).is_deleted());

    CHECK_EQ(arena.node(t.root).allocated_bytes, rootAllocated - docsAllocated);
    CHECK_EQ(arena.node(t.root).logical_bytes, rootLogical - docsLogical);
    CHECK_EQ(arena.node(t.root).file_count, 3u);
    CHECK_EQ(arena.node(t.root).child_count, 3u);
    CHECK_EQ(arena.liveNodeCount(), 6u);
    CHECK(totalsConsistent(arena));
    CHECK(storedHistogram(arena) == referenceHistogram(arena));
    CHECK(!arena.extensions().find("txt").has_value() || arena.extensions().stats(*arena.extensions().find("txt")).file_count == 0);

    // Removing something inside an already removed subtree is a no-op.
    CHECK(!arena.removeNode(t.deep_mov).removed);
    CHECK_EQ(arena.node(t.root).file_count, 3u);

    // finalize() after deletions must not resurrect anything.
    arena.finalize();
    CHECK_EQ(arena.node(t.root).allocated_bytes, rootAllocated - docsAllocated);
    CHECK_EQ(arena.node(t.root).file_count, 3u);
    CHECK(storedHistogram(arena) == referenceHistogram(arena));
}

DP_TEST(remove_partially_deleted_subtree_does_not_double_subtract) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    const uint64_t rootAllocated = arena.node(t.root).allocated_bytes;

    REQUIRE(arena.removeNode(t.deep_mov).removed);  // first a file deep inside docs…
    const RemovalResult second = arena.removeNode(t.docs);  // …then docs itself
    REQUIRE(second.removed);
    CHECK_EQ(second.node_count, 5u);  // deep.mov was already gone
    CHECK_EQ(second.file_count, 3u);
    CHECK_EQ(second.allocated_bytes, uint64_t{4096 + 8192 + 4096});
    CHECK_EQ(arena.node(t.root).allocated_bytes, rootAllocated - (4096 + 8192 + 1048576 + 4096));
    CHECK(totalsConsistent(arena));
    CHECK(storedHistogram(arena) == referenceHistogram(arena));
    CHECK_EQ(arena.liveNodeCount(), 6u);
}

DP_TEST(resort_restores_order_after_removal) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    CHECK_EQ(arena.name(arena.firstChild(t.root)), "media");

    // media shrinks below docs once its 2 MiB clip is gone.
    REQUIRE(arena.removeNode(t.clip_mov).removed);
    CHECK(!arena.isSorted());                                  // honest about the stale order
    CHECK_EQ(arena.name(arena.firstChild(t.root)), "media");  // lists are not touched by removeNode

    arena.resortAncestorChain(arena.node(t.clip_mov).parent_idx);
    CHECK(arena.isSorted());
    CHECK(childrenSortedDescending(arena, SizeMetric::Physical));
    const auto rootChildren = childNames(arena, t.root);
    REQUIRE(rootChildren.size() == 4);
    CHECK_EQ(rootChildren[0], "docs");
    CHECK_EQ(rootChildren[1], "media");
    // The tombstone has been unlinked from media's list for good.
    CHECK_EQ(arena.node(t.media).first_child_idx, t.song_mp3);
    CHECK_EQ(arena.node(t.song_mp3).next_sibling_idx, kInvalidNode);
    CHECK(totalsConsistent(arena));
}

DP_TEST(deep_chain_removal_updates_every_ancestor) {
    constexpr int kDepth = 2000;
    FlatArena arena(ArenaConfig{kDepth + 16, 1 << 20});
    uint32_t current = arena.addRoot("/r");
    std::vector<uint32_t> chain{current};
    for (int i = 0; i < kDepth; ++i) {
        current = arena.addChild(current, "d", dirInit());
        chain.push_back(current);
    }
    const uint32_t leaf = arena.addChild(current, "leaf.bin", fileInit(4096, 4000));
    const uint32_t keep = arena.addChild(chain[kDepth / 2], "keep.bin", fileInit(8192, 8000));
    arena.finalize();
    CHECK_EQ(arena.node(0).allocated_bytes, uint64_t{4096 + 8192});

    REQUIRE(arena.removeNode(leaf).removed);
    for (int i = 0; i <= kDepth; ++i) {
        const uint64_t expected = i <= kDepth / 2 ? 8192u : 0u;
        if (arena.node(chain[i]).allocated_bytes != expected) {
            CHECK_EQ(arena.node(chain[i]).allocated_bytes, expected);
            break;
        }
    }
    CHECK_EQ(arena.node(0).file_count, 1u);
    CHECK(arena.isValid(keep));
}

DP_TEST(random_trees_stay_consistent_under_random_deletions) {
    for (uint64_t seed : {1ull, 2ull, 3ull, 42ull}) {
        Rng rng(seed);
        FlatArena arena(ArenaConfig{1 << 16, 4 << 20});
        std::vector<uint32_t> dirs{arena.addRoot("/root")};
        static const char* const kExtensions[] = {".mov", ".jpg", ".txt", ".dylib", "", ".zip", ".LOG", ".o"};
        const int nodeCount = 6000;
        for (int i = 0; i < nodeCount; ++i) {
            const uint32_t parent = dirs[rng.below(dirs.size())];
            if (rng.below(5) == 0) {
                dirs.push_back(arena.addChild(parent, "dir" + std::to_string(i), dirInit()));
            } else {
                const uint64_t logical = rng.below(1 << 22);
                const uint64_t allocated = rng.below(8) == 0 ? 0 : ((logical + 4095) / 4096) * 4096;
                arena.addChild(parent, "f" + std::to_string(i) + kExtensions[rng.below(8)], fileInit(allocated, logical));
            }
        }
        arena.finalize(SizeMetric::Physical);
        CHECK(totalsConsistent(arena));
        CHECK(childrenSortedDescending(arena, SizeMetric::Physical));
        CHECK(storedHistogram(arena) == referenceHistogram(arena));
        CHECK_EQ(arena.extensions().totals().allocated_bytes, arena.node(0).allocated_bytes);

        uint32_t removedNodes = 0;
        for (int round = 0; round < 150; ++round) {
            const uint32_t victim = 1 + static_cast<uint32_t>(rng.below(arena.size() - 1));
            const bool wasLive = arena.isValid(victim);
            const Totals expected = wasLive ? bruteForce(arena, victim) : Totals{};
            const Totals rootBefore{arena.node(0).allocated_bytes, arena.node(0).logical_bytes, arena.node(0).file_count};

            const RemovalResult result = arena.removeNode(victim);
            CHECK_EQ(result.removed, wasLive);
            if (!wasLive) continue;
            removedNodes += result.node_count;
            CHECK_EQ(result.allocated_bytes, expected.allocated);
            CHECK_EQ(result.logical_bytes, expected.logical);
            CHECK_EQ(uint64_t{result.file_count}, expected.files);
            CHECK_EQ(arena.node(0).allocated_bytes, rootBefore.allocated - expected.allocated);
            CHECK_EQ(arena.node(0).logical_bytes, rootBefore.logical - expected.logical);
            CHECK_EQ(uint64_t{arena.node(0).file_count}, rootBefore.files - expected.files);

            arena.resortAncestorChain(arena.node(victim).parent_idx);
            if (round % 10 == 0) {
                CHECK(totalsConsistent(arena));
                CHECK(storedHistogram(arena) == referenceHistogram(arena));
                CHECK(arena.isSorted());
                CHECK(childrenSortedDescending(arena, SizeMetric::Physical));
            }
        }
        CHECK(totalsConsistent(arena));
        CHECK(storedHistogram(arena) == referenceHistogram(arena));
        CHECK(childrenSortedDescending(arena, SizeMetric::Physical));
        CHECK_EQ(arena.liveNodeCount(), arena.size() - removedNodes);
        CHECK_EQ(arena.extensions().totals().allocated_bytes, arena.node(0).allocated_bytes);
        CHECK_EQ(arena.extensions().totals().file_count, uint64_t{arena.node(0).file_count});
    }
}

DP_TEST(representative_file_follows_largest_children) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    CHECK_EQ(arena.representativeFile(t.root), t.clip_mov);
    CHECK_EQ(arena.representativeFile(t.docs), t.deep_mov);
    CHECK_EQ(arena.representativeFile(t.empty), kInvalidNode);
    CHECK_EQ(arena.representativeFile(t.readme), t.readme);
}

// ----------------------------------------------------------------------------------------
// Filtering
// ----------------------------------------------------------------------------------------

DP_TEST(case_insensitive_substring_search) {
    CHECK(containsCaseInsensitive("Holiday Movie.MOV", "movie"));
    CHECK(containsCaseInsensitive("Holiday Movie.MOV", ".mov"));
    CHECK(containsCaseInsensitive("abc", ""));
    CHECK(!containsCaseInsensitive("abc", "abcd"));
    CHECK(!containsCaseInsensitive("", "a"));
    CHECK(containsCaseInsensitive("aab", "ab"));
    CHECK(containsCaseInsensitive("naïve.txt", "ïve"));
    CHECK_EQ(toLowerAscii("MiXeD.Ext"), "mixed.ext");
}

DP_TEST(filter_by_name_marks_self_inherited_and_containing) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    NodeMatchSet matches;
    CHECK(!matches.active());

    FilterQuery query;
    query.name_substring = "NEST";  // matches the directory "nested"
    REQUIRE(matches.compute(arena, query));
    CHECK(matches.active());
    CHECK_EQ(matches.matchCount(), uint64_t{1});
    CHECK_EQ(matches.bits(t.nested) & NodeMatchSet::kSelf, NodeMatchSet::kSelf);
    // Everything inside a matching folder is highlighted with it…
    CHECK(matches.highlighted(t.deep_mov));
    CHECK(matches.highlighted(t.notes));
    CHECK_EQ(matches.bits(t.deep_mov), uint8_t{NodeMatchSet::kInherited});
    // …its ancestors know they contain a match…
    CHECK(matches.relevant(t.docs));
    CHECK(!matches.highlighted(t.docs));
    CHECK(matches.relevant(t.root));
    // …and unrelated branches are neither.
    CHECK(!matches.relevant(t.media));
    CHECK(!matches.relevant(t.a_txt));
    CHECK_EQ(matches.matchedFiles(), uint64_t{2});
    CHECK_EQ(matches.matchedAllocatedBytes(), uint64_t{1048576 + 4096});
}

DP_TEST(filter_by_extension_and_combined) {
    SampleTree t;
    FlatArena& arena = t.arena;
    arena.finalize();
    const auto mov = arena.extensions().find("mov");
    REQUIRE(mov.has_value());

    NodeMatchSet matches;
    FilterQuery query;
    query.extension_id = *mov;
    REQUIRE(matches.compute(arena, query));
    CHECK_EQ(matches.matchCount(), uint64_t{2});
    CHECK(matches.highlighted(t.deep_mov));
    CHECK(matches.highlighted(t.clip_mov));
    CHECK(!matches.highlighted(t.song_mp3));
    CHECK(!matches.highlighted(t.media));  // a directory is not "of type mov"…
    CHECK(matches.relevant(t.media));      // …but it contains one
    CHECK(!matches.relevant(t.empty));
    CHECK_EQ(matches.matchedAllocatedBytes(), uint64_t{1048576 + 2097152});
    CHECK_EQ(matches.matchedLogicalBytes(), uint64_t{1000000 + 2000000});

    query.name_substring = "clip";  // AND
    REQUIRE(matches.compute(arena, query));
    CHECK_EQ(matches.matchCount(), uint64_t{1});
    CHECK(matches.highlighted(t.clip_mov));
    CHECK(!matches.highlighted(t.deep_mov));
    CHECK(!matches.relevant(t.docs));

    // Deleted nodes never match.
    REQUIRE(arena.removeNode(t.clip_mov).removed);
    REQUIRE(matches.compute(arena, query));
    CHECK_EQ(matches.matchCount(), uint64_t{0});
    CHECK(!matches.relevant(t.root));

    // An empty query deactivates the set; a pre-set cancel flag aborts the evaluation.
    REQUIRE(matches.compute(arena, FilterQuery{}));
    CHECK(!matches.active());
    std::atomic<bool> cancel{true};
    query.name_substring = "a";
    CHECK(!matches.compute(arena, query, &cancel));
    CHECK(!matches.active());
}

DP_TEST_MAIN()
