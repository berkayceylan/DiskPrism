#include "SunburstTransition.hpp"

#include "FlatArena.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace diskprism {

namespace {

constexpr float kTwoPi = 6.283185307179586476925286766559f;

/// The slice that becomes (zoom in) or was (zoom out) the full circle.
struct Pivot {
    float start = 0;
    float end = kTwoPi;
    int ring = 0;

    /// Outer-layout angle -> inner-layout angle (the slice opens to the whole circle).
    [[nodiscard]] float expand(float angle) const noexcept {
        const float span = end - start;
        if (!(span > 0)) return 0.0f;
        return std::clamp((angle - start) / span, 0.0f, 1.0f) * kTwoPi;
    }
    /// Inner-layout angle -> outer-layout angle (the circle folds back into the slice).
    [[nodiscard]] float fold(float angle) const noexcept { return start + angle / kTwoPi * (end - start); }
};

void setGeometry(float out[4], float start, float end, float inner, float outer) noexcept {
    out[0] = start;
    out[1] = end;
    out[2] = inner;
    out[3] = outer;
}

void setGeometry(float out[4], const SunburstArc& arc) noexcept {
    setGeometry(out, arc.start_angle, arc.end_angle, arc.inner_radius, arc.outer_radius);
}

SunburstTweenArc hubArc(const SunburstLayout& layout, Color color, float alphaFrom) {
    SunburstTweenArc hub{};
    setGeometry(hub.from, 0.0f, kTwoPi, 0.0f, layout.hubRadius());
    setGeometry(hub.to, 0.0f, kTwoPi, 0.0f, layout.hubRadius());
    hub.color_from = color;
    hub.color_to = color;
    hub.alpha_from = alphaFrom;
    hub.alpha_to = 1.0f;
    hub.node = layout.focus();
    hub.flags = 0;
    hub.layout_index = -1;
    hub.is_hub = true;
    return hub;
}

/// Key that identifies "the same thing" in two layouts: the node plus whether the arc is
/// the node itself or the aggregate of its small children.
uint64_t arcKey(const SunburstArc& arc) noexcept {
    return (static_cast<uint64_t>(arc.node) << 1) | ((arc.flags & kArcAggregate) != 0 ? 1u : 0u);
}

std::unordered_map<uint64_t, int32_t> indexArcs(const SunburstLayout& layout) {
    std::unordered_map<uint64_t, int32_t> index;
    const auto& arcs = layout.arcs();
    index.reserve(arcs.size());
    for (size_t i = 0; i < arcs.size(); ++i) index.emplace(arcKey(arcs[i]), static_cast<int32_t>(i));
    return index;
}

}  // namespace

SunburstTransition buildSunburstSteadyState(const SunburstLayout& layout, Color hubColor) {
    SunburstTransition result;
    const auto& arcs = layout.arcs();
    result.arcs.reserve(arcs.size() + 1);
    result.first_layout_arc = 0;
    for (size_t i = 0; i < arcs.size(); ++i) {
        SunburstTweenArc tween{};
        setGeometry(tween.from, arcs[i]);
        setGeometry(tween.to, arcs[i]);
        tween.color_from = arcs[i].color;
        tween.color_to = arcs[i].color;
        tween.alpha_from = 1.0f;
        tween.alpha_to = 1.0f;
        tween.node = arcs[i].node;
        tween.flags = arcs[i].flags;
        tween.layout_index = static_cast<int32_t>(i);
        tween.is_hub = false;
        result.arcs.push_back(tween);
    }
    result.hub_index = result.arcs.size();
    result.arcs.push_back(hubArc(layout, hubColor, 1.0f));
    return result;
}

SunburstTransition buildSunburstTransition(const FlatArena& arena, const SunburstLayout& oldLayout,
                                           const SunburstLayout& newLayout, Color hubColor) {
    const auto& oldArcs = oldLayout.arcs();
    const auto& newArcs = newLayout.arcs();
    const uint32_t oldFocus = oldLayout.focus();
    const uint32_t newFocus = newLayout.focus();

    // Which kind of move is it, and where is the pivot slice?
    SunburstTransitionKind kind = SunburstTransitionKind::Bloom;
    Pivot pivot;
    const bool sameGeometry = oldFocus != kInvalidNode && newFocus != kInvalidNode &&
                              std::fabs(oldLayout.outerRadius() - newLayout.outerRadius()) < 0.5f &&
                              oldLayout.ringCount() == newLayout.ringCount();
    const bool comparable = sameGeometry && oldFocus != newFocus;
    if (sameGeometry && oldFocus == newFocus && !oldArcs.empty()) {
        kind = SunburstTransitionKind::Morph;
    } else if (comparable && arena.isAncestorOrSelf(oldFocus, newFocus)) {
        const int32_t index = oldLayout.arcIndexForNode(newFocus);
        if (index >= 0) {
            const SunburstArc& arc = oldArcs[static_cast<size_t>(index)];
            pivot = Pivot{arc.start_angle, arc.end_angle, arc.depth};
            kind = SunburstTransitionKind::ZoomIn;
        }
    } else if (comparable && arena.isAncestorOrSelf(newFocus, oldFocus)) {
        const int32_t index = newLayout.arcIndexForNode(oldFocus);
        if (index >= 0) {
            const SunburstArc& arc = newArcs[static_cast<size_t>(index)];
            pivot = Pivot{arc.start_angle, arc.end_angle, arc.depth};
            kind = SunburstTransitionKind::ZoomOut;
        }
    }

    SunburstTransition result;
    result.kind = kind;
    result.arcs.reserve(oldArcs.size() + newArcs.size() + 1);

    const std::unordered_map<uint64_t, int32_t> oldIndex =
        kind == SunburstTransitionKind::Bloom ? std::unordered_map<uint64_t, int32_t>{} : indexArcs(oldLayout);
    const std::unordered_map<uint64_t, int32_t> newIndex =
        kind == SunburstTransitionKind::Bloom ? std::unordered_map<uint64_t, int32_t>{} : indexArcs(newLayout);

    // --- Arcs that only exist in the old layout: they leave ----------------------------------
    if (kind != SunburstTransitionKind::Bloom) {
        for (const SunburstArc& arc : oldArcs) {
            if (newIndex.find(arcKey(arc)) != newIndex.end()) continue;  // it persists; handled below
            SunburstTweenArc tween{};
            setGeometry(tween.from, arc);
            tween.color_from = arc.color;
            tween.color_to = arc.color;
            tween.alpha_from = 1.0f;
            tween.alpha_to = 0.0f;
            tween.node = arc.node;
            tween.flags = arc.flags;
            tween.layout_index = -1;
            tween.is_hub = false;

            float inner = 0, outer = 0;
            if (kind == SunburstTransitionKind::Morph) {
                setGeometry(tween.to, arc);  // it simply fades out where it is
            } else if (kind == SunburstTransitionKind::ZoomIn) {
                newLayout.ringBounds(static_cast<int>(arc.depth) - pivot.ring, inner, outer);
                setGeometry(tween.to, pivot.expand(arc.start_angle), pivot.expand(arc.end_angle), inner, outer);
                if (arc.node == newFocus && (arc.flags & kArcAggregate) == 0) {
                    // The slice that was clicked becomes the hub itself.
                    tween.color_to = hubColor;
                    tween.alpha_to = 1.0f;
                }
            } else {
                newLayout.ringBounds(static_cast<int>(arc.depth) + pivot.ring, inner, outer);
                setGeometry(tween.to, pivot.fold(arc.start_angle), pivot.fold(arc.end_angle), inner, outer);
            }
            result.arcs.push_back(tween);
        }
    }

    // --- The new layout's arcs: they arrive (or simply move) ---------------------------------
    result.first_layout_arc = result.arcs.size();
    for (size_t i = 0; i < newArcs.size(); ++i) {
        const SunburstArc& arc = newArcs[i];
        SunburstTweenArc tween{};
        setGeometry(tween.to, arc);
        tween.color_from = arc.color;
        tween.color_to = arc.color;
        tween.alpha_from = 0.0f;
        tween.alpha_to = 1.0f;
        tween.node = arc.node;
        tween.flags = arc.flags;
        tween.layout_index = static_cast<int32_t>(i);
        tween.is_hub = false;

        float inner = 0, outer = 0;
        switch (kind) {
            case SunburstTransitionKind::Bloom:
                // Unfurl clockwise from 12 o'clock.
                setGeometry(tween.from, 0.0f, 0.0f, arc.inner_radius, arc.outer_radius);
                break;
            case SunburstTransitionKind::Morph:
                setGeometry(tween.from, arc);  // newcomers fade in at their final place
                break;
            case SunburstTransitionKind::ZoomIn:
                oldLayout.ringBounds(static_cast<int>(arc.depth) + pivot.ring, inner, outer);
                setGeometry(tween.from, pivot.fold(arc.start_angle), pivot.fold(arc.end_angle), inner, outer);
                break;
            case SunburstTransitionKind::ZoomOut:
                oldLayout.ringBounds(static_cast<int>(arc.depth) - pivot.ring, inner, outer);
                setGeometry(tween.from, pivot.expand(arc.start_angle), pivot.expand(arc.end_angle), inner, outer);
                if (arc.node == oldFocus && (arc.flags & kArcAggregate) == 0) {
                    // The old hub grows back into its slice.
                    tween.color_from = hubColor;
                    tween.alpha_from = 1.0f;
                }
                break;
        }

        if (kind != SunburstTransitionKind::Bloom) {
            const auto it = oldIndex.find(arcKey(arc));
            if (it != oldIndex.end()) {
                // Already on screen: start exactly where it is drawn now, fully opaque.
                const SunburstArc& previous = oldArcs[static_cast<size_t>(it->second)];
                setGeometry(tween.from, previous);
                tween.color_from = previous.color;
                tween.alpha_from = 1.0f;
            } else if (kind == SunburstTransitionKind::ZoomOut && !(tween.from[1] > tween.from[0])) {
                // A sibling of the old focus: it sweeps in from zero width, no fade needed.
                tween.alpha_from = 1.0f;
            }
        }
        result.arcs.push_back(tween);
    }

    const bool hubStays = kind == SunburstTransitionKind::Bloom || kind == SunburstTransitionKind::Morph;
    result.hub_index = result.arcs.size();
    result.arcs.push_back(hubArc(newLayout, hubColor, hubStays ? 1.0f : 0.0f));
    return result;
}

}  // namespace diskprism
