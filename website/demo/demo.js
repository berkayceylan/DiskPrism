// The live sandbox: wires the layouts and the renderer to the page.
// Like the app, it draws only when something changes; an idle page requests no frames.

import { CLOUD, LINK, LINK_DUP, SPARSE, Tree, formatBytes, formatCount, formatPercent } from "./model.js";
import { css } from "./color.js";
import { hitTreemap, layoutTreemap, nearestRect, steadyTreemap, tweenTreemap } from "./treemap.js";
import { HUB, hitSunburst, layoutSunburst, nearestArc, steadySunburst, tweenSunburst } from "./sunburst.js";
import { createRenderer, themeFor } from "./renderer.js";

const $ = (selector, root = document) => root.querySelector(selector);
const el = (tag, className, text) => {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
};
const icon = (name) => {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("class", "icon");
  svg.setAttribute("aria-hidden", "true");
  const use = document.createElementNS("http://www.w3.org/2000/svg", "use");
  use.setAttribute("href", `#i-${name}`);
  svg.append(use);
  return svg;
};

const easeOut = (t) => 1 - (1 - t) ** 3;
const DURATION = { zoom: 280, sunburst: 260, bloom: 440, morph: 700, fade: 260 };
const MAX_ROWS = 80;

const STATUS = [
  [CLOUD, "cloud", "In iCloud only. It takes no space on this disk."],
  [LINK_DUP, "link", "Hard link. Its data is counted once, at another name of the file."],
  [LINK, "link", "Hard-linked file. This name carries the on-disk size."],
  [SPARSE, "sparse", "Sparse file. Only the blocks that were written take space."],
];

export class Demo {
  constructor(root, json) {
    this.root = root;
    this.tree = new Tree(json);
    this.state = { mode: "treemap", metric: "physical", focus: 0, type: -1 };
    this.frames = 0;
    this.hover = null; // {index, node, rest, hub}
    this.external = -1; // node hovered in a list
    this.selected = -1;
    this.matches = null;
    this.keyboardRow = -1;

    this.stage = $("#stage", root);
    this.canvas = $("#map", root);
    this.renderer = createRenderer(this.canvas);
    this.renderer.onrestore = () => this.#upload(this.#steady());
    this.reducedMotion = matchMedia("(prefers-reduced-motion: reduce)");
    this.#updateTheme();
    this.#bind();
    new ResizeObserver(() => this.#resize()).observe(this.stage);
    this.#resize();
    this.#refreshPanels();
  }

  // --- scene ---------------------------------------------------------------------------

  #updateTheme() {
    this.theme = themeFor(document.documentElement.dataset.theme !== "light");
  }

  #resize() {
    const box = this.stage.getBoundingClientRect();
    if (box.width < 2 || box.height < 2) return;
    this.scale = Math.min(window.devicePixelRatio || 1, 3);
    const width = Math.round(box.width * this.scale);
    const height = Math.round(box.height * this.scale);
    if (width === this.canvas.width && height === this.canvas.height && this.layout) return;
    this.canvas.width = width;
    this.canvas.height = height;
    this.#finish();
    this.layout = this.#buildLayout();
    this.#upload(this.#steady());
  }

  #buildLayout() {
    const { mode, metric, focus } = this.state;
    const size = { width: this.canvas.width, height: this.canvas.height, metric };
    const start = performance.now();
    const layout =
      mode === "treemap"
        ? layoutTreemap(this.tree, focus, { ...size, minExtent: 1.5 * this.scale })
        : layoutSunburst(this.tree, focus, { ...size, margin: 14 * this.scale });
    this.layoutMs = performance.now() - start;
    return layout;
  }

  #steady() {
    return this.layout.kind === "treemap" ? steadyTreemap(this.layout) : steadySunburst(this.layout, this.theme.hub);
  }

  #dimmed = (shape) => {
    if (!this.matches) return false;
    const bits = this.matches[shape.node];
    return shape.kind === "file" ? !(bits & 1) : !bits;
  };

  #upload(items) {
    this.items = items;
    this.renderer.setScene(this.layout.kind, items, this.#dimmed);
    this.firstArc = this.layout.kind === "sunburst" ? items.findIndex((item) => item.index === 0) : 0;
    this.#requestDraw();
  }

  /** Applies a change of state and animates from the old scene to the new one. */
  #change(patch, { duration } = {}) {
    this.#finish();
    const before = this.layout;
    Object.assign(this.state, patch);
    this.hover = null;
    this.external = -1;
    this.#hideHud();
    this.layout = this.#buildLayout();
    this.#refreshPanels();

    let items = this.#steady();
    let ms = duration ?? 0;
    if (before && !this.reducedMotion.matches) {
      if (before.kind === "treemap" && this.layout.kind === "treemap") {
        items = tweenTreemap(before, this.layout);
        ms ||= DURATION.zoom;
      } else if (this.layout.kind === "sunburst") {
        const from = before.kind === "sunburst" ? before : { ...this.layout, arcs: [], byNode: new Map(), focus: -1 };
        const tween = tweenSunburst(from, this.layout, this.theme.hub);
        items = tween.items;
        ms ||= tween.kind === "bloom" ? DURATION.bloom : DURATION.sunburst;
      } else {
        items = items.map((item) => ({ ...item, alphaFrom: 0 }));
        ms ||= DURATION.fade;
      }
    } else {
      ms = 0;
    }
    if (ms > 0) this.animation = { start: performance.now(), duration: ms };
    this.#upload(items);
  }

  #finish() {
    if (!this.animation) return;
    this.animation = null;
    if (this.layout) this.#upload(this.#steady());
  }

  // --- drawing -------------------------------------------------------------------------

  #requestDraw() {
    this.raf ||= requestAnimationFrame((now) => this.#frame(now));
  }

  #frame(now) {
    this.raf = 0;
    let progress = 1;
    if (this.animation) {
      const t = (now - this.animation.start) / this.animation.duration;
      if (t >= 1) this.#finish();
      else progress = easeOut(Math.max(t, 0));
      this.raf ||= 0;
    }
    this.renderer.draw({ theme: this.theme, scale: this.scale, progress, centre: [this.layout.cx ?? 0, this.layout.cy ?? 0], ...(this.animation ? {} : this.#highlights()) });
    this.frames++;
    if (this.animation) this.#requestDraw();
    this.#showStatus();
  }

  #highlights() {
    const { layout, tree } = this;
    if (layout.kind === "treemap") {
      const out = { hover: this.hover?.index ?? -1 };
      const node = this.external >= 0 ? this.external : this.hover ? tree.childToward(layout.focus, this.hover.node) : -1;
      if (this.external >= 0) out.hover = layout.byNode.get(this.external) ?? -1;
      if (node >= 0 && (this.external >= 0 || tree.isDir[node])) {
        const rect = nearestRect(layout, node);
        if (rect && (rect[2] < layout.width || rect[3] < layout.height)) out.hoverRect = rect;
      }
      if (this.selected >= 0 && this.selected !== layout.focus && tree.isAncestorOrSelf(layout.focus, this.selected)) out.selectedRect = nearestRect(layout, this.selected) ?? undefined;
      return out;
    }
    const instance = (arc) => (arc >= 0 ? this.firstArc + arc : -1);
    const out = { hover: -1, selected: -1 };
    if (this.external >= 0) out.hover = instance(nearestArc(layout, this.external));
    else if (this.hover?.hub) out.hover = this.items.length - 1;
    else if (this.hover) out.hover = instance(this.hover.index);
    if (this.selected >= 0 && this.selected !== layout.focus && tree.isAncestorOrSelf(layout.focus, this.selected)) out.selected = instance(nearestArc(layout, this.selected));
    return out;
  }

  #showStatus() {
    const shapes = this.layout.kind === "treemap" ? this.layout.blocks.length : this.layout.arcs.length;
    const noun = this.layout.kind === "treemap" ? "blocks" : "arcs";
    $("#rig-status", this.root).textContent = `${formatCount(shapes)} ${noun} · 1 draw call · ${formatCount(this.frames)} ${this.frames === 1 ? "frame" : "frames"} drawn`;
    $("#rig-dot", this.root).classList.toggle("active", Boolean(this.animation));
  }

  // --- panels ----------------------------------------------------------------------------

  #refreshPanels() {
    const { tree, state } = this;
    for (const button of this.root.querySelectorAll("[data-mode]")) button.setAttribute("aria-pressed", String(button.dataset.mode === state.mode));
    for (const button of this.root.querySelectorAll("[data-metric]")) button.setAttribute("aria-pressed", String(button.dataset.metric === state.metric));

    const crumbs = $("#crumbs", this.root);
    crumbs.replaceChildren(
      ...tree.chain(state.focus).map((node) => {
        const item = el("li");
        const button = el("button", "crumb", tree.names[node]);
        button.type = "button";
        if (node === state.focus) button.setAttribute("aria-current", "location");
        button.addEventListener("click", () => this.focusOn(node));
        item.append(button);
        return item;
      }),
    );
    crumbs.scrollLeft = crumbs.scrollWidth;

    const physical = tree.physical[state.focus];
    const logical = tree.logical[state.focus];
    $("#rig-total", this.root).textContent = `${formatBytes(tree.bytes(state.focus, state.metric))} · ${formatCount(tree.files[state.focus])} files`;
    $("#rig-note", this.root).textContent =
      logical > physical
        ? `${formatBytes(physical)} on disk, ${formatBytes(logical)} logical. The difference is cloud-only, sparse and hard-linked data.`
        : `${formatBytes(physical)} on disk, ${formatBytes(logical)} logical.`;

    this.#refreshContents();
    this.#refreshTypes();

    const empty = tree.bytes(state.focus, state.metric) === 0;
    $("#map-empty", this.root).hidden = !empty;
    const hub = $("#hub", this.root);
    hub.hidden = state.mode !== "sunburst" || empty;
    if (!hub.hidden) {
      const parent = tree.parent[state.focus];
      $("#hub-name", hub).textContent = tree.names[state.focus];
      $("#hub-size", hub).textContent = formatBytes(tree.bytes(state.focus, state.metric));
      hub.disabled = parent < 0;
      hub.setAttribute("aria-label", parent < 0 ? `${tree.names[state.focus]}, top of the tree` : `Zoom out to ${tree.names[parent]}`);
      const diameter = (this.layout ? this.layout.hub / this.scale : 60) * 2;
      hub.style.setProperty("--diameter", `${Math.max(diameter, 40)}px`);
    }
  }

  #refreshContents() {
    const { tree, state } = this;
    const total = tree.bytes(state.focus, state.metric);
    const kids = tree.sorted(state.focus, state.metric);
    this.rows = kids.slice(0, MAX_ROWS);
    this.keyboardRow = -1;
    const list = $("#contents", this.root);
    list.replaceChildren(
      ...this.rows.map((node) => {
        const item = el("li");
        const button = el("button", "row");
        button.type = "button";
        const size = tree.bytes(node, state.metric);
        const flags = tree.flags[node];
        button.append(icon(tree.isDir[node] ? "folder" : "file"), el("span", "row-name", tree.names[node]));
        const badge = flags & CLOUD ? "cloud" : flags & (LINK | LINK_DUP) ? "link" : flags & SPARSE ? "sparse" : null;
        if (badge) button.append(icon(badge));
        button.append(el("span", "row-size", formatBytes(size)));
        const bar = el("span", "row-bar");
        bar.style.setProperty("--fraction", total > 0 ? size / total : 0);
        button.append(bar);
        button.title = STATUS.find(([bit]) => flags & bit)?.[2] ?? tree.names[node];
        button.addEventListener("click", () => this.#open(node));
        button.addEventListener("pointerenter", () => this.#hoverFromList(node));
        button.addEventListener("pointerleave", () => this.#hoverFromList(-1));
        button.addEventListener("focus", () => this.#hoverFromList(node));
        button.addEventListener("blur", () => this.#hoverFromList(-1));
        item.append(button);
        return item;
      }),
    );
    if (kids.length > MAX_ROWS) list.append(el("li", "more", `and ${formatCount(kids.length - MAX_ROWS)} more`));
  }

  #refreshTypes() {
    const { tree, state } = this;
    const total = tree.bytes(0, state.metric);
    const list = $("#types", this.root);
    this.typeIds = tree.rankedTypes(state.metric);
    list.replaceChildren(
      ...this.typeIds.map((id) => {
        const type = tree.types[id];
        const item = el("li");
        const button = el("button", "type");
        button.type = "button";
        const dot = el("span", "dot");
        dot.style.setProperty("--dot", css(type.color));
        button.append(
          dot,
          el("span", "type-name", type.name.startsWith("(") ? type.name : `.${type.name}`),
          el("span", "type-share", formatPercent(type[state.metric] / total)),
          el("span", "type-size", formatBytes(type[state.metric])),
        );
        button.title = `${formatCount(type.count)} files`;
        button.addEventListener("click", () => this.filterBy(id === this.state.type ? -1 : id));
        item.append(button);
        return item;
      }),
    );
    this.#showFilter();
  }

  #showFilter() {
    const { type } = this.state;
    this.root.querySelectorAll("#types .type").forEach((button, i) => button.setAttribute("aria-pressed", String(this.typeIds[i] === type)));
    $("#types-clear", this.root).hidden = type < 0;
    const name = type < 0 ? "" : this.tree.types[type].name;
    $("#types-title", this.root).textContent = type < 0 ? "File types" : name.startsWith("(") ? "No extension" : `Only .${name}`;
  }

  // --- actions ---------------------------------------------------------------------------

  focusOn(node) {
    if (node < 0 || node === this.state.focus || !this.tree.isDir[node]) return;
    this.selected = -1;
    this.#change({ focus: node });
  }

  setMode(mode) {
    if (mode !== this.state.mode) this.#change({ mode });
  }

  setMetric(metric) {
    if (metric !== this.state.metric) this.#change({ metric }, { duration: DURATION.morph });
  }

  filterBy(type) {
    this.state.type = type;
    this.matches = type < 0 ? null : this.tree.matchType(type);
    this.#finish();
    this.#upload(this.#steady());
    this.#showFilter();
  }

  #open(node) {
    if (this.tree.isDir[node]) {
      this.focusOn(node);
    } else {
      this.selected = this.selected === node ? -1 : node;
      this.#requestDraw();
    }
  }

  /** What a click on a shape opens: one level at a time in the treemap, the folder itself in the sunburst. */
  #target(hit) {
    if (!hit || hit.hub) return -1;
    if (this.layout.kind === "treemap") return hit.node === this.layout.focus ? -1 : this.tree.childToward(this.layout.focus, hit.node);
    return hit.rest ? -1 : hit.node;
  }

  #hoverFromList(node) {
    this.external = node;
    this.#requestDraw();
  }

  // --- pointer and keyboard -----------------------------------------------------------------

  #hit(event) {
    const box = this.canvas.getBoundingClientRect();
    const x = (event.clientX - box.left) * this.scale;
    const y = (event.clientY - box.top) * this.scale;
    if (this.layout.kind === "treemap") {
      const index = hitTreemap(this.layout, x, y);
      return index < 0 ? null : { index, node: this.layout.blocks[index].node, rest: this.layout.blocks[index].kind === "rest" };
    }
    const index = hitSunburst(this.layout, x, y);
    if (index === HUB) return { index, node: this.layout.focus, hub: true };
    return index < 0 ? null : { index, node: this.layout.arcs[index].node, rest: this.layout.arcs[index].kind === "rest" };
  }

  #setHover(hit, event) {
    const same = hit === this.hover || (hit && this.hover && hit.index === this.hover.index && hit.node === this.hover.node);
    this.hover = hit;
    if (!hit) this.#hideHud();
    else {
      if (!same) this.#fillHud(hit);
      this.#placeHud(event);
    }
    if (!same) {
      this.canvas.style.cursor = hit && this.tree.isDir[this.#target(hit)] ? "pointer" : hit?.hub && this.tree.parent[this.state.focus] >= 0 ? "pointer" : "default";
      const top = hit ? this.tree.childToward(this.state.focus, hit.node) : -1;
      this.root.querySelectorAll("#contents .row").forEach((row, i) => row.classList.toggle("lit", this.rows[i] === top));
      this.#requestDraw();
    }
  }

  #bind() {
    for (const button of this.root.querySelectorAll("[data-mode]")) button.addEventListener("click", () => this.setMode(button.dataset.mode));
    for (const button of this.root.querySelectorAll("[data-metric]")) button.addEventListener("click", () => this.setMetric(button.dataset.metric));
    $("#types-clear", this.root).addEventListener("click", () => this.filterBy(-1));
    $("#empty-logical", this.root).addEventListener("click", () => this.setMetric("logical"));
    const hub = $("#hub", this.root);
    hub.addEventListener("click", () => this.focusOn(this.tree.parent[this.state.focus]));
    hub.addEventListener("pointerenter", () => this.#setHover({ index: HUB, node: this.state.focus, hub: true }, null));
    hub.addEventListener("pointerleave", () => this.#setHover(null));

    const { canvas } = this;
    canvas.addEventListener("pointermove", (event) => {
      if (event.pointerType !== "touch" && !this.animation) this.#setHover(this.#hit(event), event);
    });
    canvas.addEventListener("pointerleave", (event) => {
      if (event.pointerType !== "touch") this.#setHover(null);
    });
    canvas.addEventListener("click", (event) => {
      if (this.animation) return;
      const hit = this.#hit(event);
      const target = this.#target(hit);
      if (event.pointerType === "touch" || this.lastPointer === "touch") {
        // Touch has no hover: the first tap shows what is there, a second one opens it.
        const known = hit && this.hover && this.#target(this.hover) === target && target >= 0;
        if (!known) {
          this.#setHover(hit, event);
          return;
        }
      }
      if (hit?.hub) this.focusOn(this.tree.parent[this.state.focus]);
      else if (target >= 0) this.#open(target);
      else if (hit) this.#open(hit.node);
    });
    canvas.addEventListener("pointerdown", (event) => (this.lastPointer = event.pointerType));
    canvas.addEventListener("keydown", (event) => {
      const step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[event.key];
      if (step) {
        event.preventDefault();
        if (!this.rows.length) return;
        this.keyboardRow = (this.keyboardRow + step + this.rows.length) % this.rows.length;
        this.#hoverFromList(this.rows[this.keyboardRow]);
        $("#map-live", this.root).textContent = `${this.tree.names[this.rows[this.keyboardRow]]}, ${formatBytes(this.tree.bytes(this.rows[this.keyboardRow], this.state.metric))}`;
      } else if (event.key === "Enter" && this.keyboardRow >= 0) {
        this.#open(this.rows[this.keyboardRow]);
      } else if (event.key === "Escape" || event.key === "Backspace") {
        event.preventDefault();
        this.focusOn(this.tree.parent[this.state.focus]);
      }
    });
    canvas.addEventListener("blur", () => this.#hoverFromList(-1));
    document.addEventListener("themechange", () => {
      this.#updateTheme();
      this.#finish();
      this.#upload(this.#steady());
    });
  }

  // --- floating read-out ------------------------------------------------------------------------

  #fillHud(hit) {
    const { tree, state } = this;
    const { node } = hit;
    const set = (id, text) => ($(`#hud-${id}`, this.root).textContent = text);
    set("title", hit.rest ? `Smaller items in “${tree.names[node]}”` : tree.names[node]);
    set("path", tree.path(node));
    set("physical", formatBytes(tree.physical[node]));
    set("logical", formatBytes(tree.logical[node]));
    const parent = tree.parent[node];
    set("share", parent < 0 ? "—" : formatPercent(tree.bytes(node, state.metric) / Math.max(tree.bytes(parent, state.metric), 1)));
    set("kind-label", tree.isDir[node] ? "Contains" : "Type");
    set("kind", tree.isDir[node] ? `${formatCount(tree.files[node])} files` : tree.types[tree.ext[node]].name === "(none)" ? "no extension" : `.${tree.types[tree.ext[node]].name}`);
    let status = hit.rest ? ["", "", "Too small to draw one by one. Open the folder to see them."] : STATUS.find(([bit]) => tree.flags[node] & bit);
    const target = this.#target(hit);
    if (!status && this.lastPointer === "touch" && target >= 0 && tree.isDir[target]) status = ["", "", `Tap again to open ${tree.names[target]}.`];
    if (hit.hub) status = ["", "", parent < 0 ? "The top of the tree." : `Click to zoom out to ${tree.names[parent]}.`];
    const line = $("#hud-status", this.root);
    line.hidden = !status;
    if (status) line.replaceChildren(...(status[1] ? [icon(status[1])] : []), el("span", "", status[2]));
  }

  #placeHud(event) {
    const hud = $("#hud", this.root);
    hud.hidden = false;
    const bounds = this.stage.getBoundingClientRect();
    const size = hud.getBoundingClientRect();
    let x = bounds.width / 2 - size.width / 2;
    let y = bounds.height - size.height - 12;
    if (event && event.clientY - bounds.top > bounds.height / 2) y = 12; // keep clear of the finger
    if (event && this.lastPointer !== "touch" && bounds.width > 520) {
      x = event.clientX - bounds.left + 18;
      y = event.clientY - bounds.top + 18;
      if (x + size.width > bounds.width - 8) x = event.clientX - bounds.left - 18 - size.width;
      if (y + size.height > bounds.height - 8) y = event.clientY - bounds.top - 18 - size.height;
      x = Math.max(8, Math.min(x, bounds.width - size.width - 8));
      y = Math.max(8, Math.min(y, bounds.height - size.height - 8));
    }
    hud.style.setProperty("transform", `translate(${Math.round(x)}px, ${Math.round(y)}px)`);
  }

  #hideHud() {
    $("#hud", this.root).hidden = true;
  }
}

export async function start(root) {
  const response = await fetch(new URL("../assets/demo-tree.json", import.meta.url));
  if (!response.ok) throw new Error(`demo data: HTTP ${response.status}`);
  return new Demo(root, await response.json());
}
