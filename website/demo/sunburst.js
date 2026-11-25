// Sunburst layout and transitions — a port of src/core/SunburstLayout.cpp and SunburstTransition.cpp.

import { NEUTRAL, directoryColor } from "./color.js";
import { CLOUD } from "./model.js";

const TAU = Math.PI * 2;
const RINGS = 6;
const MIN_ANGLE = (0.4 * Math.PI) / 180;
const HUB_FRACTION = 0.25;
const RING_DECAY = 0.86;

export function layoutSunburst(tree, focus, { width, height, metric, margin }) {
  const outer = Math.max(Math.min(width, height) / 2 - margin, 0);
  const hub = outer * HUB_FRACTION;
  const unit = ((outer - hub) * (1 - RING_DECAY)) / (1 - RING_DECAY ** RINGS);
  const edge = (ring) => (ring <= 0 ? (ring === 0 ? hub : 0) : hub + (unit * (1 - RING_DECAY ** ring)) / (1 - RING_DECAY));
  const bounds = (ring) => (ring < 0 ? [0, 0] : ring === 0 ? [0, hub] : [edge(ring - 1), edge(ring)]);

  const arcs = [];
  const begin = [0];
  const byNode = new Map();
  const result = { kind: "sunburst", tree, focus, width, height, metric, arcs, begin, byNode, cx: width / 2, cy: height / 2, hub, outer, edge, bounds };
  if (focus < 0 || !(outer > 0) || tree.bytes(focus, metric) === 0) return result;

  let current = [{ node: focus, start: 0, end: TAU, arc: -1 }];
  for (let ring = 1; ring <= RINGS && current.length; ring++) {
    const [r0, r1] = bounds(ring);
    const next = [];
    for (const parent of current) {
      const total = tree.bytes(parent.node, metric);
      if (total === 0) continue;
      const perByte = (parent.end - parent.start) / total;
      let angle = parent.start;
      for (const child of tree.sorted(parent.node, metric)) {
        const bytes = tree.bytes(child, metric);
        if (bytes === 0) break;
        const sweep = bytes * perByte;
        if (sweep < MIN_ANGLE) {
          if (parent.end - angle > 1e-6) {
            arcs.push({ node: parent.node, kind: "rest", type: -1, ring, parent: parent.arc, color: NEUTRAL, geo: [angle, parent.end, r0, r1], cloud: false });
          }
          break;
        }
        const end = Math.min(angle + sweep, parent.end);
        const isDir = tree.isDir[child];
        byNode.set(child, arcs.length);
        if (isDir && ring < RINGS) next.push({ node: child, start: angle, end, arc: arcs.length });
        arcs.push({
          node: child,
          kind: isDir ? "dir" : "file",
          type: isDir ? -1 : tree.ext[child],
          ring,
          parent: parent.arc,
          color: isDir ? directoryColor((angle + end) / 2, ring) : tree.types[tree.ext[child]].color,
          geo: [angle, end, r0, r1],
          cloud: !isDir && (tree.flags[child] & CLOUD) !== 0,
        });
        angle = end;
      }
    }
    begin.push(arcs.length);
    current = next;
  }
  while (begin.length < RINGS + 1) begin.push(arcs.length);
  return result;
}

export const HUB = -2;

export function hitSunburst(layout, x, y) {
  const dx = x - layout.cx;
  const dy = y - layout.cy;
  const radius = Math.hypot(dx, dy);
  if (!(layout.outer > 0) || radius >= layout.outer) return -1;
  if (radius < layout.hub) return HUB;
  let ring = 1;
  while (ring < RINGS && radius >= layout.edge(ring)) ring++;
  let angle = Math.atan2(dx, -dy);
  if (angle < 0) angle += TAU;
  let lo = layout.begin[ring - 1];
  let hi = layout.begin[ring];
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (layout.arcs[mid].geo[0] <= angle) lo = mid + 1;
    else hi = mid;
  }
  const index = lo - 1;
  return index >= layout.begin[ring - 1] && angle < layout.arcs[index].geo[1] ? index : -1;
}

/** Index of the arc of a node or of its closest drawn ancestor below the focus, or -1. */
export function nearestArc(layout, node) {
  for (let n = node; n >= 0 && n !== layout.focus; n = layout.tree.parent[n]) {
    const index = layout.byNode.get(n);
    if (index !== undefined) return index;
  }
  return -1;
}

const keyOf = (arc) => arc.node * 2 + (arc.kind === "rest" ? 1 : 0);

const hubItem = (layout, color, alphaFrom) => ({
  hub: true,
  from: [0, TAU, 0, layout.hub],
  to: [0, TAU, 0, layout.hub],
  colorFrom: color,
  colorTo: color,
  alphaFrom,
  alphaTo: 1,
});

export function steadySunburst(layout, hubColor) {
  const items = layout.arcs.map((arc, index) => ({ arc, index, from: arc.geo, to: arc.geo, colorFrom: arc.color, colorTo: arc.color, alphaFrom: 1, alphaTo: 1 }));
  items.push(hubItem(layout, hubColor, 1));
  return items;
}

/** Where an arc that has no counterpart in `other` folds to: a slit between its surviving neighbours. */
function slit(layout, other, otherIndex, arc) {
  const at = layout.arcs.indexOf(arc);
  for (let i = at - 1; i >= layout.begin[arc.ring - 1] && layout.arcs[i].parent === arc.parent; i--) {
    const twin = otherIndex.get(keyOf(layout.arcs[i]));
    if (twin) return twin.geo[1];
  }
  if (arc.parent < 0) return 0;
  const parent = layout.arcs[arc.parent];
  const twin = otherIndex.get(keyOf(parent));
  return twin ? twin.geo[0] : slit(layout, other, otherIndex, parent);
}

export function tweenSunburst(oldLayout, newLayout, hubColor) {
  const { tree } = newLayout;
  const oldIndex = new Map(oldLayout.arcs.map((a) => [keyOf(a), a]));
  const newIndex = new Map(newLayout.arcs.map((a) => [keyOf(a), a]));
  let kind = "bloom";
  let pivot = null;
  if (oldLayout.focus === newLayout.focus && oldLayout.arcs.length) {
    kind = "morph";
  } else if (oldLayout.arcs.length && tree.isAncestorOrSelf(oldLayout.focus, newLayout.focus)) {
    pivot = oldLayout.arcs[oldLayout.byNode.get(newLayout.focus)] ?? null;
    if (pivot) kind = "in";
  } else if (oldLayout.arcs.length && tree.isAncestorOrSelf(newLayout.focus, oldLayout.focus)) {
    pivot = newLayout.arcs[newLayout.byNode.get(oldLayout.focus)] ?? null;
    if (pivot) kind = "out";
  }
  const expand = (a) => Math.min(1, Math.max(0, (a - pivot.geo[0]) / (pivot.geo[1] - pivot.geo[0]))) * TAU;
  const fold = (a) => pivot.geo[0] + (a / TAU) * (pivot.geo[1] - pivot.geo[0]);

  const items = [];
  if (kind !== "bloom") {
    for (const arc of oldLayout.arcs) {
      if (newIndex.has(keyOf(arc))) continue;
      const item = { arc, index: -1, from: arc.geo, colorFrom: arc.color, colorTo: arc.color, alphaFrom: 1, alphaTo: 0 };
      if (kind === "morph") {
        const at = slit(oldLayout, newLayout, newIndex, arc);
        item.to = [at, at, arc.geo[2], arc.geo[3]];
        item.alphaTo = 1;
      } else if (kind === "in") {
        item.to = [expand(arc.geo[0]), expand(arc.geo[1]), ...newLayout.bounds(arc.ring - pivot.ring)];
        if (arc === pivot) Object.assign(item, { colorTo: hubColor, alphaTo: 1 });
      } else {
        item.to = [fold(arc.geo[0]), fold(arc.geo[1]), ...newLayout.bounds(arc.ring + pivot.ring)];
      }
      items.push(item);
    }
  }
  newLayout.arcs.forEach((arc, index) => {
    const item = { arc, index, to: arc.geo, colorFrom: arc.color, colorTo: arc.color, alphaFrom: 0, alphaTo: 1 };
    const before = kind === "bloom" ? null : oldIndex.get(keyOf(arc));
    if (before) {
      Object.assign(item, { from: before.geo, colorFrom: before.color, alphaFrom: 1 });
    } else if (kind === "bloom") {
      item.from = [0, 0, arc.geo[2], arc.geo[3]];
    } else if (kind === "morph") {
      const at = slit(newLayout, oldLayout, oldIndex, arc);
      Object.assign(item, { from: [at, at, arc.geo[2], arc.geo[3]], alphaFrom: 1 });
    } else if (kind === "in") {
      item.from = [fold(arc.geo[0]), fold(arc.geo[1]), ...oldLayout.bounds(arc.ring + pivot.ring)];
    } else {
      item.from = [expand(arc.geo[0]), expand(arc.geo[1]), ...oldLayout.bounds(arc.ring - pivot.ring)];
      if (arc === pivot) Object.assign(item, { colorFrom: hubColor, alphaFrom: 1 });
      else if (!(item.from[1] > item.from[0])) item.alphaFrom = 1;
    }
    items.push(item);
  });
  items.push(hubItem(newLayout, hubColor, kind === "bloom" || kind === "morph" ? 1 : 0));
  return { kind, items };
}
