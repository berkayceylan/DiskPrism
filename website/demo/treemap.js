// Squarified cushion treemap (Bruls, Huizing, van Wijk) — a port of src/core/TreemapLayout.cpp.

import { NEUTRAL, mix } from "./color.js";
import { CLOUD } from "./model.js";

const RIDGE = 0.34; // height of the first nesting level's cushion
const DECAY = 0.82; // height factor per further level

const worst = (sum, min, max, side) => Math.max((side * side * max) / (sum * sum), (sum * sum) / (side * side * min));

/**
 * Each block carries its cushion surface z = ax·u² + bx·u + ay·v² + by·v, with u and v
 * normalised to the block (0…1), so that a block can change size while it animates and
 * keep a consistent surface.
 */
export function layoutTreemap(tree, focus, { width, height, metric, minExtent }) {
  const blocks = [];
  const frames = new Map(); // folder -> rectangle it was subdivided in
  const byNode = new Map();
  const minArea = minExtent * minExtent;
  const result = { kind: "treemap", tree, focus, width, height, metric, blocks, frames, byNode };
  if (focus < 0 || !(width > 0) || !(height > 0) || tree.bytes(focus, metric) === 0) return result;

  const ridge = (s, x, y, w, h, height_) => {
    const fx = (4 * height_) / w;
    const fy = (4 * height_) / h;
    return { ax: s.ax - fx, bx: s.bx + fx * (2 * x + w), ay: s.ay - fy, by: s.by + fy * (2 * y + h) };
  };

  const snap = (x, y, w, h) => {
    const x0 = Math.round(x);
    const y0 = Math.round(y);
    const x1 = Math.round(x + w);
    const y1 = Math.round(y + h);
    return x1 > x0 && y1 > y0 ? [x0, y0, x1 - x0, y1 - y0] : null;
  };

  const emit = (node, x, y, w, h, s, kind, color, type, depth) => {
    const box = snap(x, y, w, h);
    if (!box) return;
    if (kind !== "rest") byNode.set(node, blocks.length);
    blocks.push({
      node,
      kind,
      type,
      depth,
      color,
      rect: box,
      coeffs: [s.ax * box[2], 2 * s.ax * box[0] + s.bx, s.ay * box[3], 2 * s.ay * box[1] + s.by],
      cloud: kind === "file" && (tree.flags[node] & CLOUD) !== 0,
    });
  };

  const place = (node, x, y, w, h, parentSurface, depth, ridgeHeight) => {
    const s = ridge(parentSurface, x, y, w, h, ridgeHeight);
    if (!tree.isDir[node]) {
      emit(node, x, y, w, h, s, "file", tree.types[tree.ext[node]].color, tree.ext[node], depth);
    } else if (Math.min(w, h) >= minExtent && tree.bytes(node, metric) > 0) {
      const box = snap(x, y, w, h);
      if (box) frames.set(node, box);
      folder(node, x, y, w, h, s, depth + 1, ridgeHeight * DECAY);
    } else {
      const file = tree.representative(node, metric);
      emit(node, x, y, w, h, s, "dir", file < 0 ? NEUTRAL : tree.types[tree.ext[file]].color, file < 0 ? -1 : tree.ext[file], depth);
    }
  };

  const folder = (dir, x, y, w, h, surface, depth, ridgeHeight) => {
    const total = tree.bytes(dir, metric);
    if (total === 0) return;
    const kids = tree.sorted(dir, metric);
    const scale = (w * h) / total;
    let i = 0;
    let tail = false;
    while (i < kids.length) {
      const side = Math.min(w, h);
      if (!(side > 0)) break;
      const row = [];
      let sum = 0;
      let min = Infinity;
      let max = 0;
      while (i < kids.length) {
        const bytes = tree.bytes(kids[i], metric);
        if (bytes === 0) {
          i = kids.length;
          break;
        }
        const area = bytes * scale;
        if (area < minArea) {
          tail = true;
          break;
        }
        const nextMin = Math.min(min, area);
        const nextMax = Math.max(max, area);
        if (row.length && worst(sum + area, nextMin, nextMax, side) > worst(sum, min, max, side)) break;
        row.push([kids[i], area]);
        sum += area;
        min = nextMin;
        max = nextMax;
        i++;
      }
      if (!row.length) break;

      const vertical = w >= h;
      const available = vertical ? w : h;
      const ideal = sum / side;
      const thickness = i >= kids.length && !tail ? available : Math.min(ideal, available);
      let pos = vertical ? y : x;
      const end = pos + side;
      row.forEach(([node, area], k) => {
        const next = k === row.length - 1 ? end : Math.min(pos + area / ideal, end);
        if (vertical) place(node, x, pos, thickness, next - pos, surface, depth, ridgeHeight);
        else place(node, pos, y, next - pos, thickness, surface, depth, ridgeHeight);
        pos = next;
      });
      if (vertical) {
        x += thickness;
        w -= thickness;
      } else {
        y += thickness;
        h -= thickness;
      }
      if (tail) break;
    }
    if (tail && w > 0 && h > 0) {
      // Everything left is too small to draw by itself: one block for all of it.
      const first = kids[i];
      const file = tree.isDir[first] ? tree.representative(first, metric) : first;
      const color = file < 0 ? NEUTRAL : mix(tree.types[tree.ext[file]].color, NEUTRAL, 0.45);
      emit(dir, x, y, w, h, ridge(surface, x, y, w, h, ridgeHeight), "rest", color, file < 0 ? -1 : tree.ext[file], depth);
    }
  };

  frames.set(focus, [0, 0, width, height]);
  folder(focus, 0, 0, width, height, { ax: 0, bx: 0, ay: 0, by: 0 }, 1, RIDGE);
  return result;
}

export function hitTreemap(layout, x, y) {
  const { blocks } = layout;
  for (let i = 0; i < blocks.length; i++) {
    const r = blocks[i].rect;
    if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return i;
  }
  return -1;
}

/** Rectangle of a node: its own block, or the frame it was subdivided in. */
export function rectOfNode(layout, node) {
  const index = layout.byNode.get(node);
  return index !== undefined ? layout.blocks[index].rect : (layout.frames.get(node) ?? null);
}

/** Rectangle of the node or of its closest drawn ancestor below the focus. */
export function nearestRect(layout, node) {
  for (let n = node; n >= 0 && n !== layout.focus; n = layout.tree.parent[n]) {
    const rect = rectOfNode(layout, n);
    if (rect) return rect;
  }
  return null;
}

const keyOf = (block) => block.node * 2 + (block.kind === "rest" ? 1 : 0);
const centre = (r) => [r[0] + r[2] / 2, r[1] + r[3] / 2, 0, 0];
const still = (block) => ({ block, from: block.rect, to: block.rect, coeffsFrom: block.coeffs, coeffsTo: block.coeffs, alphaFrom: 1, alphaTo: 1 });

export const steadyTreemap = (layout) => layout.blocks.map(still);

/**
 * Blocks of both layouts with their geometry at either end of a transition.
 * Zooming carries the pivot rectangle onto the whole canvas; a change of size moves every
 * block to its new place and shrinks what no longer takes any room to a point.
 */
export function tweenTreemap(oldLayout, newLayout) {
  const { tree } = newLayout;
  const oldIndex = new Map(oldLayout.blocks.map((b) => [keyOf(b), b]));
  const newKeys = new Set(newLayout.blocks.map(keyOf));
  const full = [0, 0, newLayout.width, newLayout.height];

  let pivot = null;
  let zoomIn = false;
  if (oldLayout.focus !== newLayout.focus) {
    zoomIn = tree.isAncestorOrSelf(oldLayout.focus, newLayout.focus);
    pivot = zoomIn ? rectOfNode(oldLayout, newLayout.focus) : tree.isAncestorOrSelf(newLayout.focus, oldLayout.focus) ? rectOfNode(newLayout, oldLayout.focus) : null;
  }
  const map = (from, to) => (r) => [
    to[0] + ((r[0] - from[0]) / from[2]) * to[2],
    to[1] + ((r[1] - from[1]) / from[3]) * to[3],
    (r[2] / from[2]) * to[2],
    (r[3] / from[3]) * to[3],
  ];
  const forward = pivot ? (zoomIn ? map(pivot, full) : map(full, pivot)) : null; // old scene -> new scene
  const backward = pivot ? (zoomIn ? map(full, pivot) : map(pivot, full)) : null; // new scene -> old scene

  const leaving = [];
  for (const block of oldLayout.blocks) {
    if (newKeys.has(keyOf(block))) continue;
    leaving.push({
      block,
      from: block.rect,
      to: forward ? forward(block.rect) : centre(block.rect),
      coeffsFrom: block.coeffs,
      coeffsTo: block.coeffs,
      alphaFrom: 1,
      alphaTo: forward ? 1 : 0,
      leaving: true,
    });
  }
  const arriving = newLayout.blocks.map((block) => {
    const before = oldIndex.get(keyOf(block));
    if (before) return { block, from: before.rect, to: block.rect, coeffsFrom: before.coeffs, coeffsTo: block.coeffs, alphaFrom: 1, alphaTo: 1 };
    const from = backward ? backward(block.rect) : centre(block.rect);
    return { block, from, to: block.rect, coeffsFrom: block.coeffs, coeffsTo: block.coeffs, alphaFrom: 0, alphaTo: 1 };
  });
  // Zooming: the old scene lies underneath. Resizing: what shrinks away stays visible on top.
  return pivot ? [...leaving, ...arriving] : [...arriving, ...leaving];
}
