// Flat file tree: parallel arrays indexed by node number, parents always before children.
// The browser cousin of src/core/FlatArena: totals are one reverse sweep over the arrays.

import { NEUTRAL, paletteColor } from "./color.js";

export const CLOUD = 1; // SF_DATALESS: not stored on this disk
export const LINK = 2; // hard-linked, this name carries the on-disk size
export const LINK_DUP = 4; // further name of a hard-linked file
export const SPARSE = 8;

const NO_EXTENSION = "(none)";

function extensionOf(name) {
  const dot = name.lastIndexOf(".");
  if (dot <= 0 || dot + 1 >= name.length) return NO_EXTENSION;
  const suffix = name.slice(dot + 1);
  return suffix.length <= 20 && !/[\s()[\],;:'"]/.test(suffix) ? suffix.toLowerCase() : NO_EXTENSION;
}

export class Tree {
  names = [];
  parent = [];
  isDir = [];
  flags = [];
  ext = []; // index into types, -1 for folders
  physical = [];
  logical = [];
  files = [];
  children = [];
  types = []; // {name, physical, logical, count, color}
  #sorted = new Map();

  constructor(json) {
    const typeIds = new Map();
    const add = (entry, parent) => {
      const node = this.names.length;
      const isDir = Array.isArray(entry[1]);
      this.names.push(entry[0]);
      this.parent.push(parent);
      this.isDir.push(isDir);
      this.children.push([]);
      if (parent >= 0) this.children[parent].push(node);
      if (isDir) {
        this.flags.push(0);
        this.ext.push(-1);
        this.physical.push(0);
        this.logical.push(0);
        this.files.push(0);
        for (const child of entry[1]) add(child, node);
        return;
      }
      const [name, logical, physical = Math.ceil(logical / 4096) * 4096, flags = 0] = entry;
      const typeName = extensionOf(name);
      let type = typeIds.get(typeName);
      if (type === undefined) {
        type = this.types.length;
        typeIds.set(typeName, type);
        this.types.push({ name: typeName, physical: 0, logical: 0, count: 0, color: NEUTRAL });
      }
      const stats = this.types[type];
      stats.physical += physical;
      stats.logical += logical;
      stats.count += 1;
      this.flags.push(flags);
      this.ext.push(type);
      this.physical.push(physical);
      this.logical.push(logical);
      this.files.push(1);
    };
    add(json, -1);

    for (let i = this.names.length - 1; i > 0; i--) {
      const p = this.parent[i];
      this.physical[p] += this.physical[i];
      this.logical[p] += this.logical[i];
      this.files[p] += this.files[i];
    }
    this.rankedTypes("physical").forEach((id, rank) => (this.types[id].color = paletteColor(rank)));
  }

  get size() {
    return this.names.length;
  }

  bytes(node, metric) {
    return metric === "physical" ? this.physical[node] : this.logical[node];
  }

  rankedTypes(metric) {
    const ids = this.types.map((_, id) => id);
    return ids.sort(
      (a, b) =>
        this.types[b][metric] - this.types[a][metric] ||
        this.types[b].count - this.types[a].count ||
        (this.types[a].name < this.types[b].name ? -1 : 1),
    );
  }

  /** Children, largest first (cached per metric). */
  sorted(node, metric) {
    const key = `${metric}:${node}`;
    let list = this.#sorted.get(key);
    if (!list) {
      list = [...this.children[node]].sort((a, b) => this.bytes(b, metric) - this.bytes(a, metric) || a - b);
      this.#sorted.set(key, list);
    }
    return list;
  }

  isAncestorOrSelf(ancestor, node) {
    while (node > ancestor) node = this.parent[node];
    return node === ancestor;
  }

  /** The child of `ancestor` on the way to `node`, or -1. */
  childToward(ancestor, node) {
    while (node > ancestor) {
      const parent = this.parent[node];
      if (parent === ancestor) return node;
      node = parent;
    }
    return -1;
  }

  chain(node) {
    const out = [];
    for (let n = node; n >= 0; n = this.parent[n]) out.unshift(n);
    return out;
  }

  path(node) {
    return this.chain(node)
      .map((n) => this.names[n])
      .join("/");
  }

  /** Largest file reached by always taking the largest child: gives a merged block its color. */
  representative(node, metric) {
    let current = node;
    while (this.isDir[current]) {
      const next = this.sorted(current, metric).find((c) => !this.isDir[c] || this.files[c] > 0);
      if (next === undefined) return -1;
      current = next;
    }
    return current;
  }

  /** 1 = matches the type, 2 = contains a match. Two linear sweeps, like NodeMatchSet. */
  matchType(type) {
    const bits = new Uint8Array(this.size);
    for (let i = 0; i < this.size; i++) if (this.ext[i] === type) bits[i] = 1;
    for (let i = this.size - 1; i > 0; i--) if (bits[i]) bits[this.parent[i]] |= 2;
    return bits;
  }
}

/** Decimal units with Finder's number of digits. */
export function formatBytes(bytes) {
  if (bytes < 1000) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let value = bytes / 1000;
  let unit = 0;
  while (value >= 999.5 && unit < units.length - 1) {
    value /= 1000;
    unit++;
  }
  const digits = unit === 0 ? 0 : unit === 1 ? 1 : 2;
  return `${value.toLocaleString("en-US", { maximumFractionDigits: digits })} ${units[unit]}`;
}

export function formatPercent(fraction) {
  if (!(fraction > 0)) return "0%";
  if (fraction < 0.001) return "<0.1%";
  return `${(fraction * 100).toFixed(fraction >= 0.9995 ? 0 : 1)}%`;
}

export const formatCount = (n) => n.toLocaleString("en-US");
