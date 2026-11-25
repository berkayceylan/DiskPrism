#include "TreemapLayout.hpp"

#include "ColorSpace.hpp"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace diskprism {

namespace {

constexpr uint32_t kMaxNestingDepth = 512;  // bounds recursion on pathological trees

struct RectD {
    double x, y, w, h;
};

/// Cushion surface coefficients in absolute viewport coordinates (double precision
/// while they accumulate; converted to rect-relative floats when a block is emitted).
struct Surface {
    double ax = 0, bx = 0, ay = 0, by = 0;
};

/// Worst aspect ratio in a row whose areas sum to `sum`, with smallest / largest member
/// `rmin` / `rmax`, laid out along a side of length `side` (Bruls et al., "Squarified
/// Treemaps": max(w²·r⁺ / s², s² / (w²·r⁻))).
double worstAspect(double sum, double rmin, double rmax, double side) noexcept {
    const double s2 = sum * sum;
    const double w2 = side * side;
    return std::max(w2 * rmax / s2, s2 / (w2 * rmin));
}

/// van Wijk & van de Wetering, "Cushion Treemaps": adds a parabolic ridge of the given
/// height spanning the rectangle in both directions. For x in [x1, x2] the ridge is
/// 4h/(x2−x1) · (x − x1)(x2 − x), i.e. −f·x² + f·(x1 + x2)·x with f = 4h/(x2−x1).
void addRidge(Surface& s, const RectD& r, double height) noexcept {
    if (r.w > 0) {
        const double f = 4.0 * height / r.w;
        s.bx += f * (2.0 * r.x + r.w);
        s.ax -= f;
    }
    if (r.h > 0) {
        const double f = 4.0 * height / r.h;
        s.by += f * (2.0 * r.y + r.h);
        s.ay -= f;
    }
}

class Builder {
public:
    Builder(const FlatArena& arena, const TreemapParams& params, std::vector<TreemapRect>& rects,
            std::vector<TreemapFrame>& frames)
        : arena_(arena),
          params_(params),
          rects_(rects),
          frames_(frames),
          metric_(params.metric),
          sorted_(arena.isSorted() && arena.sortedMetric() == params.metric),
          min_extent_(std::max(static_cast<double>(params.min_extent), 0.0)),
          min_area_(min_extent_ * min_extent_) {}

    void run(uint32_t focus) {
        const RectD full{0.0, 0.0, static_cast<double>(params_.width), static_cast<double>(params_.height)};
        const FsNode& root = arena_.node(focus);
        if (!root.is_dir()) {
            Surface surface;
            addRidge(surface, full, params_.ridge_height);
            emitLeaf(focus, full, surface, 0);
            return;
        }
        if (root.bytes(metric_) == 0) return;
        frames_.push_back(TreemapFrame{focus, 0.0f, 0.0f, params_.width, params_.height, 0});
        layoutDirectory(focus, full, Surface{}, 1, params_.ridge_height);
    }

private:
    struct RowItem {
        uint32_t node;
        double area;
    };

    void layoutDirectory(uint32_t dir, const RectD& rect, const Surface& surface, uint32_t depth, double ridge) {
        const uint64_t total = arena_.node(dir).bytes(metric_);
        if (total == 0 || !(rect.w > 0) || !(rect.h > 0)) return;

        // Children, largest first. Normally that is simply the arena's child list.
        std::vector<uint32_t> ordered;
        size_t orderedPos = 0;
        uint32_t child;
        if (sorted_) {
            child = arena_.firstChild(dir);
        } else {
            arena_.forEachChild(dir, [&ordered](uint32_t c) { ordered.push_back(c); });
            std::sort(ordered.begin(), ordered.end(), [this](uint32_t a, uint32_t b) {
                const uint64_t sa = arena_.node(a).bytes(metric_);
                const uint64_t sb = arena_.node(b).bytes(metric_);
                return sa != sb ? sa > sb : a < b;
            });
            child = ordered.empty() ? kInvalidNode : ordered.front();
        }
        const auto advance = [&](uint32_t current) -> uint32_t {
            if (sorted_) return arena_.nextSibling(current);
            ++orderedPos;
            return orderedPos < ordered.size() ? ordered[orderedPos] : kInvalidNode;
        };

        const double scale = (rect.w * rect.h) / static_cast<double>(total);
        RectD remaining = rect;
        bool tail = false;

        while (child != kInvalidNode) {
            const double side = std::min(remaining.w, remaining.h);
            if (!(side > 0)) break;

            // Grow a row while adding the next child does not worsen its worst aspect ratio.
            const size_t rowBegin = row_.size();
            double sum = 0;
            double rmin = std::numeric_limits<double>::infinity();
            double rmax = 0;
            while (child != kInvalidNode) {
                const uint64_t bytes = arena_.node(child).bytes(metric_);
                if (bytes == 0) {
                    child = kInvalidNode;  // sorted: only empty items remain
                    break;
                }
                const double area = static_cast<double>(bytes) * scale;
                if (area < min_area_) {
                    tail = true;  // sorted: every remaining child is at most this small
                    break;
                }
                const double newSum = sum + area;
                const double newMin = std::min(rmin, area);
                const double newMax = std::max(rmax, area);
                if (row_.size() > rowBegin &&
                    worstAspect(newSum, newMin, newMax, side) > worstAspect(sum, rmin, rmax, side)) {
                    break;
                }
                row_.push_back(RowItem{child, area});
                sum = newSum;
                rmin = newMin;
                rmax = newMax;
                child = advance(child);
            }
            const size_t rowEnd = row_.size();
            if (rowEnd == rowBegin) break;

            // Lay the row out as a strip along the shorter side of what is left.
            const bool vertical = remaining.w >= remaining.h;  // strip hugs the left edge
            const double available = vertical ? remaining.w : remaining.h;
            const bool lastRow = child == kInvalidNode && !tail;
            const double idealThickness = sum / side;
            const double thickness = lastRow ? available : std::min(idealThickness, available);

            double pos = vertical ? remaining.y : remaining.x;
            const double posEnd = pos + side;
            for (size_t i = rowBegin; i < rowEnd; ++i) {
                const RowItem item = row_[i];  // by value: recursion below may grow row_
                const double next = (i + 1 == rowEnd) ? posEnd : std::min(pos + item.area / idealThickness, posEnd);
                const RectD r = vertical ? RectD{remaining.x, pos, thickness, next - pos}
                                         : RectD{pos, remaining.y, next - pos, thickness};
                place(item.node, r, surface, depth, ridge);
                pos = next;
            }
            row_.resize(rowBegin);

            if (vertical) {
                remaining.x += thickness;
                remaining.w -= thickness;
            } else {
                remaining.y += thickness;
                remaining.h -= thickness;
            }
        }

        // Sub-pixel culling: all remaining children are too small to draw individually.
        // Their combined area is exactly what is left, so they become one block.
        if (tail && remaining.w > 0 && remaining.h > 0) {
            emitAggregate(dir, child, remaining, surface, depth, ridge);
        }
    }

    void place(uint32_t nodeIdx, const RectD& r, const Surface& parentSurface, uint32_t depth, double ridge) {
        const FsNode& n = arena_.node(nodeIdx);
        Surface surface = parentSurface;
        addRidge(surface, r, ridge);

        if (n.is_dir() && std::min(r.w, r.h) >= min_extent_ && n.bytes(metric_) > 0 && depth < kMaxNestingDepth) {
            float snapped[4];
            if (snap(r, snapped)) {
                frames_.push_back(TreemapFrame{nodeIdx, snapped[0], snapped[1], snapped[2], snapped[3],
                                               static_cast<uint16_t>(std::min<uint32_t>(depth, UINT16_MAX))});
            }
            layoutDirectory(nodeIdx, r, surface, depth + 1, ridge * params_.ridge_decay);
            return;
        }
        emitLeaf(nodeIdx, r, surface, depth);
    }

    /// Rounds the rectangle's edges (not its size) so neighbours keep sharing an edge.
    /// Returns false if the rectangle collapses.
    bool snap(const RectD& r, float out[4]) const noexcept {
        double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
        if (params_.snap_to_pixels) {
            x0 = std::round(x0);
            y0 = std::round(y0);
            x1 = std::round(x1);
            y1 = std::round(y1);
        }
        // Compare the edges as the 32-bit floats the renderer will see: a sliver thinner
        // than one float step at its position has no extent there and is dropped.
        const auto fx0 = static_cast<float>(x0), fy0 = static_cast<float>(y0);
        const auto fx1 = static_cast<float>(x1), fy1 = static_cast<float>(y1);
        if (!(fx1 > fx0) || !(fy1 > fy0)) return false;
        out[0] = fx0;
        out[1] = fy0;
        out[2] = fx1 - fx0;
        out[3] = fy1 - fy0;
        return true;
    }

    void emit(uint32_t nodeIdx, const RectD& r, const Surface& s, uint32_t depth, uint32_t flags, Color color,
              uint16_t extId) {
        float box[4];
        if (!snap(r, box)) return;
        TreemapRect out;
        out.x = box[0];
        out.y = box[1];
        out.w = box[2];
        out.h = box[3];
        // Re-express the surface relative to the block's origin: with x = x0 + u,
        // d/dx (ax·x² + bx·x) = 2·ax·u + (2·ax·x0 + bx).
        out.ax = static_cast<float>(s.ax);
        out.bx = static_cast<float>(2.0 * s.ax * static_cast<double>(box[0]) + s.bx);
        out.ay = static_cast<float>(s.ay);
        out.by = static_cast<float>(2.0 * s.ay * static_cast<double>(box[1]) + s.by);
        out.node = nodeIdx;
        out.flags = flags;
        out.color = color;
        out.depth = static_cast<uint16_t>(std::min<uint32_t>(depth, UINT16_MAX));
        out.ext_id = extId;
        rects_.push_back(out);
    }

    void emitLeaf(uint32_t nodeIdx, const RectD& r, const Surface& surface, uint32_t depth) {
        const FsNode& n = arena_.node(nodeIdx);
        const ExtensionTable& extensions = arena_.extensions();
        if (!n.is_dir()) {
            uint32_t flags = kTreemapFile;
            if (n.is_dataless()) flags |= kTreemapDataless;
            if (params_.matches != nullptr && params_.matches->active() && !params_.matches->highlighted(nodeIdx)) {
                flags |= kTreemapDimmed;
            }
            emit(nodeIdx, r, surface, depth, flags, extensions.color(n.ext_id), n.ext_id);
            return;
        }
        // A directory too small to subdivide: one block in the color of its dominant file.
        uint32_t flags = kTreemapDirectory;
        if (params_.matches != nullptr && params_.matches->active() && !params_.matches->relevant(nodeIdx)) {
            flags |= kTreemapDimmed;
        }
        const uint32_t representative = arena_.representativeFile(nodeIdx);
        const uint16_t extId = representative != kInvalidNode ? arena_.node(representative).ext_id : ExtensionTable::kNone;
        const Color color = representative != kInvalidNode ? extensions.color(extId) : ExtensionTable::neutralColor();
        emit(nodeIdx, r, surface, depth, flags, color, extId);
    }

    void emitAggregate(uint32_t dir, uint32_t firstTailChild, const RectD& r, const Surface& parentSurface,
                       uint32_t depth, double ridge) {
        Surface surface = parentSurface;
        addRidge(surface, r, ridge);

        const ExtensionTable& extensions = arena_.extensions();
        uint16_t extId = ExtensionTable::kNone;
        Color color = ExtensionTable::neutralColor();
        if (firstTailChild != kInvalidNode) {
            const FsNode& first = arena_.node(firstTailChild);
            const uint32_t representative = first.is_dir() ? arena_.representativeFile(firstTailChild) : firstTailChild;
            if (representative != kInvalidNode) {
                extId = arena_.node(representative).ext_id;
                // Many small files of mixed types: hint at the dominant one, muted.
                color = mixColors(extensions.color(extId), ExtensionTable::neutralColor(), 0.45f);
            }
        }

        uint32_t flags = kTreemapAggregate;
        if (params_.matches != nullptr && params_.matches->active() && !tailIsRelevant(dir, firstTailChild)) {
            flags |= kTreemapDimmed;
        }
        emit(dir, r, surface, depth, flags, color, extId);
    }

    /// Does the merged tail of `dir` contain anything the filter cares about?
    /// Exact when the directory itself matches; otherwise a bounded look at the tail.
    bool tailIsRelevant(uint32_t dir, uint32_t firstTailChild) const {
        const NodeMatchSet& matches = *params_.matches;
        if (matches.highlighted(dir)) return true;
        if (!matches.relevant(dir) || !sorted_) return matches.relevant(dir);
        int budget = 512;
        for (uint32_t c = firstTailChild; c != kInvalidNode && budget > 0; c = arena_.nextSibling(c), --budget) {
            if (matches.relevant(c)) return true;
        }
        return false;
    }

    const FlatArena& arena_;
    const TreemapParams& params_;
    std::vector<TreemapRect>& rects_;
    std::vector<TreemapFrame>& frames_;
    SizeMetric metric_;
    bool sorted_;
    double min_extent_;
    double min_area_;
    std::vector<RowItem> row_;  // scratch stack shared by all recursion levels
};

}  // namespace

void TreemapLayout::clear() {
    rects_.clear();
    frames_.clear();
    rect_by_node_.clear();
    frame_by_node_.clear();
    grid_start_.clear();
    grid_items_.clear();
    grid_cols_ = 0;
    grid_rows_ = 0;
    focus_ = kInvalidNode;
    width_ = 0;
    height_ = 0;
}

void TreemapLayout::build(const FlatArena& arena, uint32_t focus, const TreemapParams& params) {
    clear();
    focus_ = focus;
    width_ = params.width;
    height_ = params.height;
    if (focus == kInvalidNode || !arena.isValid(focus) || !(params.width > 0) || !(params.height > 0)) return;

    Builder builder(arena, params, rects_, frames_);
    builder.run(focus);
    buildIndexes();
}

void TreemapLayout::buildIndexes() {
    rect_by_node_.reserve(rects_.size());
    for (size_t i = 0; i < rects_.size(); ++i) {
        if ((rects_[i].flags & kTreemapAggregate) == 0) {
            rect_by_node_.emplace_back(rects_[i].node, static_cast<int32_t>(i));
        }
    }
    std::sort(rect_by_node_.begin(), rect_by_node_.end());

    frame_by_node_.reserve(frames_.size());
    for (size_t i = 0; i < frames_.size(); ++i) frame_by_node_.emplace_back(frames_[i].node, static_cast<int32_t>(i));
    std::sort(frame_by_node_.begin(), frame_by_node_.end());

    // Uniform grid sized so that a cell holds a handful of average blocks.
    if (rects_.empty()) return;
    const double averageArea = static_cast<double>(width_) * static_cast<double>(height_) / static_cast<double>(rects_.size());
    cell_size_ = static_cast<float>(std::clamp(std::sqrt(averageArea) * 2.0, 8.0, 128.0));
    grid_cols_ = std::max(1, static_cast<int>(std::ceil(width_ / cell_size_)));
    grid_rows_ = std::max(1, static_cast<int>(std::ceil(height_ / cell_size_)));
    const size_t cellCount = static_cast<size_t>(grid_cols_) * static_cast<size_t>(grid_rows_);

    const auto cellRange = [this](const TreemapRect& r, int& cx0, int& cy0, int& cx1, int& cy1) {
        cx0 = std::clamp(static_cast<int>(std::floor(r.x / cell_size_)), 0, grid_cols_ - 1);
        cy0 = std::clamp(static_cast<int>(std::floor(r.y / cell_size_)), 0, grid_rows_ - 1);
        cx1 = std::clamp(static_cast<int>(std::ceil((r.x + r.w) / cell_size_)) - 1, cx0, grid_cols_ - 1);
        cy1 = std::clamp(static_cast<int>(std::ceil((r.y + r.h) / cell_size_)) - 1, cy0, grid_rows_ - 1);
    };

    // Counting pass, prefix sum, fill pass: a compact CSR layout with no per-cell vectors.
    grid_start_.assign(cellCount + 1, 0);
    for (const TreemapRect& r : rects_) {
        int cx0, cy0, cx1, cy1;
        cellRange(r, cx0, cy0, cx1, cy1);
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) ++grid_start_[static_cast<size_t>(cy) * grid_cols_ + cx + 1];
        }
    }
    for (size_t c = 0; c < cellCount; ++c) grid_start_[c + 1] += grid_start_[c];

    grid_items_.assign(grid_start_[cellCount], 0);
    std::vector<uint32_t> cursor(grid_start_.begin(), grid_start_.end() - 1);
    for (size_t i = 0; i < rects_.size(); ++i) {
        int cx0, cy0, cx1, cy1;
        cellRange(rects_[i], cx0, cy0, cx1, cy1);
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                grid_items_[cursor[static_cast<size_t>(cy) * grid_cols_ + cx]++] = static_cast<uint32_t>(i);
            }
        }
    }
}

int32_t TreemapLayout::hitTest(float x, float y) const noexcept {
    if (grid_cols_ == 0 || !(x >= 0) || !(y >= 0) || !(x < width_) || !(y < height_)) return -1;
    const int cx = std::min(static_cast<int>(x / cell_size_), grid_cols_ - 1);
    const int cy = std::min(static_cast<int>(y / cell_size_), grid_rows_ - 1);
    const size_t cell = static_cast<size_t>(cy) * grid_cols_ + cx;
    for (uint32_t k = grid_start_[cell]; k < grid_start_[cell + 1]; ++k) {
        const TreemapRect& r = rects_[grid_items_[k]];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return static_cast<int32_t>(grid_items_[k]);
    }
    return -1;
}

int32_t TreemapLayout::rectIndexForNode(uint32_t node) const noexcept {
    const auto it = std::lower_bound(rect_by_node_.begin(), rect_by_node_.end(), std::make_pair(node, INT32_MIN));
    if (it == rect_by_node_.end() || it->first != node) return -1;
    return it->second;
}

bool TreemapLayout::rectForNode(uint32_t node, float outXYWH[4]) const noexcept {
    const int32_t rectIndex = rectIndexForNode(node);
    if (rectIndex >= 0) {
        const TreemapRect& r = rects_[static_cast<size_t>(rectIndex)];
        outXYWH[0] = r.x;
        outXYWH[1] = r.y;
        outXYWH[2] = r.w;
        outXYWH[3] = r.h;
        return true;
    }
    const auto it = std::lower_bound(frame_by_node_.begin(), frame_by_node_.end(), std::make_pair(node, INT32_MIN));
    if (it == frame_by_node_.end() || it->first != node) return false;
    const TreemapFrame& f = frames_[static_cast<size_t>(it->second)];
    outXYWH[0] = f.x;
    outXYWH[1] = f.y;
    outXYWH[2] = f.w;
    outXYWH[3] = f.h;
    return true;
}

}  // namespace diskprism
