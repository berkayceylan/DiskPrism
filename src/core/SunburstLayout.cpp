#include "SunburstLayout.hpp"

#include "ColorSpace.hpp"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"

#include <algorithm>
#include <cmath>

namespace diskprism {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Directories are tinted by where they sit on the circle, so a subtree keeps one hue
/// family all the way out and neighbouring subtrees are easy to tell apart. Rings get
/// lighter and calmer toward the outside.
Color directoryColor(double midAngle, int ring) noexcept {
    const float hue = static_cast<float>(std::fmod(midAngle / kTwoPi * 360.0 + 205.0, 360.0));
    const float step = static_cast<float>(std::min(ring - 1, 7));
    const float lightness = std::min(0.60f + 0.034f * step, 0.82f);
    const float chroma = std::max(0.170f - 0.013f * step, 0.07f);
    return oklchToSrgb(lightness, chroma, hue);
}

struct PendingParent {
    uint32_t node;
    double start;
    double end;
    int32_t arcIndex;
};

}  // namespace

void SunburstLayout::clear() {
    arcs_.clear();
    ring_begin_.clear();
    arc_by_node_.clear();
    focus_ = kInvalidNode;
    center_x_ = 0;
    center_y_ = 0;
    hub_radius_ = 0;
    outer_radius_ = 0;
    ring_unit_ = 0;
    max_depth_ = 0;
}

float SunburstLayout::ringOuterRadius(int ring) const noexcept {
    if (ring <= 0) return ring == 0 ? hub_radius_ : 0.0f;
    // Geometric series: ring k has width unit·decay^(k−1).
    const double decay = ring_decay_;
    const double sum = std::abs(1.0 - decay) < 1e-9 ? static_cast<double>(ring)
                                                    : (1.0 - std::pow(decay, ring)) / (1.0 - decay);
    return hub_radius_ + static_cast<float>(static_cast<double>(ring_unit_) * sum);
}

void SunburstLayout::ringBounds(int ring, float& inner, float& outer) const noexcept {
    if (ring < 0) {
        inner = 0.0f;
        outer = 0.0f;
    } else if (ring == 0) {
        inner = 0.0f;
        outer = hub_radius_;
    } else {
        inner = ringOuterRadius(ring - 1);
        outer = ringOuterRadius(ring);
    }
}

std::pair<size_t, size_t> SunburstLayout::ringRange(int ring) const noexcept {
    if (ring < 1 || static_cast<size_t>(ring) >= ring_begin_.size()) return {0, 0};
    return {ring_begin_[static_cast<size_t>(ring) - 1], ring_begin_[static_cast<size_t>(ring)]};
}

void SunburstLayout::build(const FlatArena& arena, uint32_t focus, const SunburstParams& params) {
    clear();
    focus_ = focus;
    max_depth_ = std::clamp(params.max_depth, 1, 32);
    ring_decay_ = std::clamp(params.ring_decay, 0.3f, 1.0f);
    center_x_ = params.width * 0.5f;
    center_y_ = params.height * 0.5f;
    outer_radius_ = std::max(std::min(params.width, params.height) * 0.5f - std::max(params.margin, 0.0f), 0.0f);
    hub_radius_ = outer_radius_ * std::clamp(params.hub_fraction, 0.05f, 0.8f);
    {
        const double decay = ring_decay_;
        const double sum = std::abs(1.0 - decay) < 1e-9 ? static_cast<double>(max_depth_)
                                                        : (1.0 - std::pow(decay, max_depth_)) / (1.0 - decay);
        ring_unit_ = static_cast<float>(static_cast<double>(outer_radius_ - hub_radius_) / sum);
    }
    ring_begin_.push_back(0);

    if (focus == kInvalidNode || !arena.isValid(focus) || !(outer_radius_ > 0)) return;
    const SizeMetric metric = params.metric;
    const FsNode& root = arena.node(focus);
    if (!root.is_dir() || root.bytes(metric) == 0) return;

    const bool sorted = arena.isSorted() && arena.sortedMetric() == metric;
    const double minAngle = static_cast<double>(std::max(params.min_angle_degrees, 0.0f)) * kTwoPi / 360.0;
    const NodeMatchSet* matches = (params.matches != nullptr && params.matches->active()) ? params.matches : nullptr;
    const ExtensionTable& extensions = arena.extensions();

    std::vector<PendingParent> current{{focus, 0.0, kTwoPi, -1}};
    std::vector<PendingParent> next;
    std::vector<uint32_t> ordered;  // fallback when the arena is not pre-sorted

    for (int ring = 1; ring <= max_depth_ && !current.empty(); ++ring) {
        float inner = 0, outer = 0;
        ringBounds(ring, inner, outer);
        next.clear();

        for (const PendingParent& parent : current) {
            const uint64_t parentBytes = arena.node(parent.node).bytes(metric);
            if (parentBytes == 0) continue;
            const double perByte = (parent.end - parent.start) / static_cast<double>(parentBytes);

            size_t orderedPos = 0;
            uint32_t child;
            if (sorted) {
                child = arena.firstChild(parent.node);
            } else {
                ordered.clear();
                arena.forEachChild(parent.node, [&ordered](uint32_t c) { ordered.push_back(c); });
                std::sort(ordered.begin(), ordered.end(), [&arena, metric](uint32_t a, uint32_t b) {
                    const uint64_t sa = arena.node(a).bytes(metric);
                    const uint64_t sb = arena.node(b).bytes(metric);
                    return sa != sb ? sa > sb : a < b;
                });
                child = ordered.empty() ? kInvalidNode : ordered.front();
            }

            double angle = parent.start;
            for (; child != kInvalidNode;) {
                const FsNode& n = arena.node(child);
                const uint64_t bytes = n.bytes(metric);
                if (bytes == 0) break;  // sorted: only empty items remain

                const double sweep = static_cast<double>(bytes) * perByte;
                if (sweep < minAngle) {
                    // Everything from here on is thinner than the threshold. The rest of
                    // the parent's span is exactly their combined size: one "other" slice.
                    const double remaining = parent.end - angle;
                    if (remaining > 1e-6) {
                        SunburstArc arc{};
                        arc.start_angle = static_cast<float>(angle);
                        arc.end_angle = static_cast<float>(parent.end);
                        arc.inner_radius = inner;
                        arc.outer_radius = outer;
                        arc.node = parent.node;
                        arc.flags = kArcAggregate;
                        if (matches != nullptr && !matches->relevant(parent.node)) arc.flags |= kArcDimmed;
                        arc.color = ExtensionTable::neutralColor();
                        arc.depth = static_cast<uint16_t>(ring);
                        arc.ext_id = ExtensionTable::kNone;
                        arc.parent = parent.arcIndex;
                        arcs_.push_back(arc);
                    }
                    break;
                }

                const double endAngle = std::min(angle + sweep, parent.end);
                SunburstArc arc{};
                arc.start_angle = static_cast<float>(angle);
                arc.end_angle = static_cast<float>(endAngle);
                arc.inner_radius = inner;
                arc.outer_radius = outer;
                arc.node = child;
                arc.depth = static_cast<uint16_t>(ring);
                arc.parent = parent.arcIndex;
                if (n.is_dir()) {
                    arc.flags = kArcDirectory;
                    arc.ext_id = ExtensionTable::kNone;
                    arc.color = directoryColor((angle + endAngle) * 0.5, ring);
                    if (matches != nullptr && !matches->relevant(child)) arc.flags |= kArcDimmed;
                } else {
                    arc.flags = kArcFile;
                    arc.ext_id = n.ext_id;
                    arc.color = extensions.color(n.ext_id);
                    if (matches != nullptr && !matches->highlighted(child)) arc.flags |= kArcDimmed;
                }
                if (n.is_dataless()) arc.flags |= kArcDataless;

                const auto arcIndex = static_cast<int32_t>(arcs_.size());
                arcs_.push_back(arc);
                if (n.is_dir() && ring < max_depth_) next.push_back(PendingParent{child, angle, endAngle, arcIndex});

                angle = endAngle;
                if (sorted) {
                    child = arena.nextSibling(child);
                } else {
                    ++orderedPos;
                    child = orderedPos < ordered.size() ? ordered[orderedPos] : kInvalidNode;
                }
            }
        }

        ring_begin_.push_back(arcs_.size());
        current.swap(next);
    }
    while (ring_begin_.size() < static_cast<size_t>(max_depth_) + 1) ring_begin_.push_back(arcs_.size());

    arc_by_node_.reserve(arcs_.size());
    for (size_t i = 0; i < arcs_.size(); ++i) {
        if ((arcs_[i].flags & kArcAggregate) == 0) arc_by_node_.emplace_back(arcs_[i].node, static_cast<int32_t>(i));
    }
    std::sort(arc_by_node_.begin(), arc_by_node_.end());
}

int32_t SunburstLayout::hitTest(float x, float y) const noexcept {
    if (!(outer_radius_ > 0)) return kHitNone;
    const double dx = static_cast<double>(x) - center_x_;
    const double dy = static_cast<double>(y) - center_y_;
    const double radius = std::sqrt(dx * dx + dy * dy);
    if (radius < hub_radius_) return kHitHub;
    if (radius >= outer_radius_) return kHitNone;

    // Ring: at most `max_depth_` comparisons.
    int ring = 0;
    for (int k = 1; k <= max_depth_; ++k) {
        if (radius < ringOuterRadius(k)) {
            ring = k;
            break;
        }
    }
    if (ring == 0) return kHitNone;

    double angle = std::atan2(dx, -dy);  // clockwise from 12 o'clock
    if (angle < 0) angle += kTwoPi;
    const auto target = static_cast<float>(angle);

    // Arcs of a ring are sorted by start angle and never overlap: binary search for the
    // last arc that starts at or before the cursor.
    const auto [first, last] = ringRange(ring);
    const auto begin = arcs_.begin() + static_cast<std::ptrdiff_t>(first);
    const auto end = arcs_.begin() + static_cast<std::ptrdiff_t>(last);
    auto it = std::upper_bound(begin, end, target,
                               [](float value, const SunburstArc& arc) { return value < arc.start_angle; });
    if (it == begin) return kHitNone;
    --it;
    if (target >= it->start_angle && target < it->end_angle) return static_cast<int32_t>(it - arcs_.begin());
    return kHitNone;
}

int32_t SunburstLayout::arcIndexForNode(uint32_t node) const noexcept {
    const auto it = std::lower_bound(arc_by_node_.begin(), arc_by_node_.end(), std::make_pair(node, INT32_MIN));
    if (it == arc_by_node_.end() || it->first != node) return -1;
    return it->second;
}

}  // namespace diskprism
