// WebGL 2 renderer: the cushion and sunburst shaders of src/gpu/Shaders.metal in GLSL.
// Every scene is one instanced triangle-strip draw; the quad comes from gl_VertexID.

const DIMMED = 1;
const REST = 2;
const CLOUDED = 4;
const IS_HUB = 8;

const COMMON = `#version 300 es
precision highp float;
precision highp int;
uniform vec4 uViewport;    // width, height, backing scale, progress
uniform vec4 uBackground;
uniform vec4 uLight;       // direction to the light, ambient
uniform vec4 uLightParams; // diffuse, specular, shininess, dim strength
uniform vec4 uBorder;
uniform vec4 uAccent;
uniform vec4 uHoverRect;
uniform vec4 uSelRect;
uniform ivec4 uIds;        // hovered instance, selected instance
uniform vec4 uSun;         // centre.xy, divider width
vec3 dimColor(vec3 rgb) {
  float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
  return mix(rgb, mix(vec3(luma), uBackground.rgb, 0.62), uLightParams.w);
}
vec4 toClip(vec2 p) { return vec4(p.x / uViewport.x * 2.0 - 1.0, 1.0 - p.y / uViewport.y * 2.0, 0.0, 1.0); }
`;

const TREEMAP_VERTEX = `${COMMON}
layout(location = 0) in vec4 aFrom;
layout(location = 1) in vec4 aTo;
layout(location = 2) in vec4 aCoeffsFrom;
layout(location = 3) in vec4 aCoeffsTo;
layout(location = 4) in vec4 aColor;   // rgb, alpha at progress 0
layout(location = 5) in vec2 aMisc;    // alpha at progress 1, flags
out vec2 vLocal;
flat out vec4 vRect;
flat out vec4 vCoeffs;
flat out vec4 vColor;
flat out int vFlags;
flat out int vId;
void main() {
  float t = uViewport.w;
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vRect = mix(aFrom, aTo, t);
  vCoeffs = mix(aCoeffsFrom, aCoeffsTo, t);
  vColor = vec4(aColor.rgb, mix(aColor.a, aMisc.x, t));
  vFlags = int(aMisc.y);
  vId = gl_InstanceID;
  vLocal = corner;
  gl_Position = toClip(vRect.xy + corner * vRect.zw);
}`;

const TREEMAP_FRAGMENT = `${COMMON}
in vec2 vLocal;
flat in vec4 vRect;
flat in vec4 vCoeffs;
flat in vec4 vColor;
flat in int vFlags;
flat in int vId;
out vec4 fragment;
float inset(vec2 p, vec4 r) {
  vec2 lo = p - r.xy;
  vec2 hi = r.xy + r.zw - p;
  return min(min(lo.x, lo.y), min(hi.x, hi.y));
}
void main() {
  // van Wijk cushion: the surface is analytic, so its normal is too.
  vec3 normal = normalize(vec3(-(2.0 * vCoeffs.x * vLocal.x + vCoeffs.y), -(2.0 * vCoeffs.z * vLocal.y + vCoeffs.w), 1.0));
  float diffuse = max(dot(normal, uLight.xyz), 0.0);
  vec3 halfVector = normalize(uLight.xyz + vec3(0.0, 0.0, 1.0));
  float specular = pow(max(dot(normal, halfVector), 0.0), uLightParams.z) * uLightParams.y;
  vec3 rgb = vColor.rgb * (uLight.w + uLightParams.x * diffuse) + specular;

  vec2 p = vRect.xy + vLocal * vRect.zw;
  float scale = uViewport.z;
  if ((vFlags & ${CLOUDED}) != 0) {
    float stripe = step(0.5, fract((p.x + p.y) / (7.0 * scale)));
    rgb = mix(rgb, mix(rgb, uBackground.rgb, 0.5), stripe);
  }
  if ((vFlags & ${DIMMED}) != 0) rgb = dimColor(rgb);

  vec2 far = (1.0 - vLocal) * vRect.zw;
  float border = step(min(far.x, far.y), 1.0) * clamp((min(vRect.z, vRect.w) - 2.0) / 4.0, 0.0, 1.0);
  rgb = mix(rgb, uBorder.rgb, uBorder.a * border);

  if (vId == uIds.x) rgb = mix(rgb, vec3(1.0), 0.20) + 0.03;
  if (uHoverRect.z > 0.0) {
    float d = inset(p, uHoverRect);
    if (d >= 0.0) rgb = mix(rgb, vec3(1.0), 0.05 + 0.70 * (1.0 - smoothstep(scale, scale + 1.0, d)));
  }
  if (uSelRect.z > 0.0) {
    float d = inset(p, uSelRect);
    if (d >= 0.0) {
      float w = 1.5 * scale;
      float frame = 1.0 - smoothstep(w, w + 1.0, d);
      rgb = mix(rgb, vec3(0.0), 0.45 * (1.0 - smoothstep(w + 1.0, w + 2.0, d)) * (1.0 - frame));
      rgb = mix(rgb, uAccent.rgb, uAccent.a * frame);
    }
  }
  fragment = vec4(clamp(rgb, 0.0, 1.0), vColor.a);
}`;

const SUNBURST_VERTEX = `${COMMON}
layout(location = 0) in vec4 aFrom;    // start angle, end angle, inner radius, outer radius
layout(location = 1) in vec4 aTo;
layout(location = 2) in vec4 aColorFrom;
layout(location = 3) in vec4 aColorTo;
layout(location = 4) in float aFlags;
out vec2 vP;
flat out vec4 vArc;
flat out vec4 vColor;
flat out int vFlags;
flat out int vId;
const float PI = 3.14159265358979;
void main() {
  float t = uViewport.w;
  vec4 arc = mix(aFrom, aTo, t);
  vColor = mix(aColorFrom, aColorTo, t);
  vFlags = int(aFlags);
  vId = gl_InstanceID;
  float mid = 0.5 * (arc.x + arc.y);
  float halfSweep = clamp(0.5 * (arc.y - arc.x), 0.0, PI);
  float inner = max(arc.z, 0.0);
  float outer = max(arc.w, inner);
  vArc = vec4(arc.xy, inner, outer);
  // A quad aligned with the sector's bisector stays tight at any orientation.
  vec2 u = vec2(sin(mid), -cos(mid));
  vec2 v = vec2(cos(mid), sin(mid));
  float uMin, vMax;
  if (halfSweep < 0.5 * PI) {
    uMin = inner * cos(halfSweep) - 1.5;
    vMax = outer * sin(halfSweep) + 1.5;
  } else {
    uMin = outer * cos(halfSweep) - 1.5;
    vMax = outer + 1.5;
  }
  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
  vec2 p = u * mix(uMin, outer + 1.5, corner.x) + v * mix(-vMax, vMax, corner.y);
  if (outer - inner <= 0.0 || halfSweep <= 0.0 || vColor.a <= 0.0) p = vec2(0.0);
  vP = p;
  gl_Position = toClip(uSun.xy + p);
}`;

const SUNBURST_FRAGMENT = `${COMMON}
in vec2 vP;
flat in vec4 vArc;
flat in vec4 vColor;
flat in int vFlags;
flat in int vId;
out vec4 fragment;
const float PI = 3.14159265358979;
void main() {
  float inner = vArc.z;
  float outer = vArc.w;
  float halfSweep = 0.5 * (vArc.y - vArc.x);
  bool hub = (vFlags & ${IS_HUB}) != 0;
  float radius = length(vP);
  float delta = atan(vP.x, -vP.y) - 0.5 * (vArc.x + vArc.y);
  delta -= 2.0 * PI * floor(delta / (2.0 * PI) + 0.5);

  // Signed distance to the sector's four edges, in pixels.
  float gap = hub ? 0.0 : 0.5 * uSun.z;
  float sd = outer - radius - gap;
  if (inner > 0.0) sd = min(sd, radius - inner - gap);
  if (halfSweep < PI - 1.0e-4) sd = min(sd, radius * sin(clamp(halfSweep - abs(delta), -0.5 * PI, 0.5 * PI)) - gap);
  float coverage = clamp(sd / max(fwidth(sd), 1.0e-4) + 0.5, 0.0, 1.0);
  if (coverage <= 0.0) discard;

  vec3 rgb = vColor.rgb;
  float scale = uViewport.z;
  if (hub) {
    float rim = 1.0 - smoothstep(0.0, 2.5 * scale, outer - radius);
    rgb = mix(rgb, uBorder.rgb, 0.35 * rim);
    if (vId == uIds.x) rgb = mix(rgb, uAccent.rgb, 0.12);
  } else {
    float across = clamp((radius - inner) / max(outer - inner, 1.0e-3), 0.0, 1.0);
    rgb *= mix(1.07, 0.90, across * across);
    if ((vFlags & ${REST}) != 0) rgb = mix(rgb, mix(rgb, uBackground.rgb, 0.35), step(0.5, fract(radius / (5.0 * scale))));
    if ((vFlags & ${CLOUDED}) != 0) rgb = mix(rgb, mix(rgb, uBackground.rgb, 0.5), step(0.5, fract((vP.x + vP.y) / (7.0 * scale))));
    if ((vFlags & ${DIMMED}) != 0) rgb = dimColor(rgb);
    if (vId == uIds.x) rgb = mix(rgb, vec3(1.0), 0.24) + 0.03;
    if (vId == uIds.y) {
      float w = 1.5 * scale;
      rgb = mix(rgb, uAccent.rgb, uAccent.a * (1.0 - smoothstep(w, w + 1.0, sd)));
    }
  }
  fragment = vec4(clamp(rgb, 0.0, 1.0), coverage * vColor.a);
}`;

const UNIFORMS = ["uViewport", "uBackground", "uLight", "uLightParams", "uBorder", "uAccent", "uHoverRect", "uSelRect", "uIds", "uSun"];
const TREEMAP_STRIDE = 22;
const SUNBURST_STRIDE = 17;

/** Lighting and colors per appearance, as in src/gpu/GpuInstances.cpp. */
export function themeFor(dark) {
  const length = Math.hypot(-0.42, -0.58, 1);
  const light = [-0.42 / length, -0.58 / length, 1 / length];
  return dark
    ? { dark, background: [0.11, 0.11, 0.118], light: [...light, 0.38], params: [0.76, 0.05, 14, 0.86], border: [0, 0, 0, 0.34], hub: [46, 46, 50] }
    : { dark, background: [0.925, 0.925, 0.937], light: [...light, 0.56], params: [0.58, 0.04, 14, 0.84], border: [0.12, 0.12, 0.16, 0.26], hub: [252, 252, 253] };
}

const rgb01 = (c) => [c[0] / 255, c[1] / 255, c[2] / 255];

export function createRenderer(canvas) {
  const gl = canvas.getContext("webgl2", { alpha: false, antialias: false, depth: false, stencil: false, powerPreference: "high-performance" });
  return gl ? new GlRenderer(canvas, gl) : new CanvasRenderer(canvas);
}

class GlRenderer {
  name = "WebGL 2";
  count = 0;

  constructor(canvas, gl) {
    this.canvas = canvas;
    this.gl = gl;
    this.#setUp();
    canvas.addEventListener("webglcontextlost", (event) => event.preventDefault());
    canvas.addEventListener("webglcontextrestored", () => {
      this.#setUp();
      this.onrestore?.();
    });
  }

  #setUp() {
    const { gl } = this;
    this.programs = {
      treemap: this.#program(TREEMAP_VERTEX, TREEMAP_FRAGMENT, TREEMAP_STRIDE, [4, 4, 4, 4, 4, 2]),
      sunburst: this.#program(SUNBURST_VERTEX, SUNBURST_FRAGMENT, SUNBURST_STRIDE, [4, 4, 4, 4, 1]),
    };
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
  }

  #program(vertex, fragment, stride, sizes) {
    const { gl } = this;
    const compile = (type, source) => {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS) && !gl.isContextLost()) throw new Error(gl.getShaderInfoLog(shader));
      return shader;
    };
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, vertex));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragment));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS) && !gl.isContextLost()) throw new Error(gl.getProgramInfoLog(program));
    const array = gl.createVertexArray();
    const buffer = gl.createBuffer();
    gl.bindVertexArray(array);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    let offset = 0;
    sizes.forEach((size, location) => {
      gl.enableVertexAttribArray(location);
      gl.vertexAttribPointer(location, size, gl.FLOAT, false, stride * 4, offset * 4);
      gl.vertexAttribDivisor(location, 1);
      offset += size;
    });
    gl.bindVertexArray(null);
    const uniforms = Object.fromEntries(UNIFORMS.map((name) => [name, gl.getUniformLocation(program, name)]));
    return { program, array, buffer, uniforms };
  }

  /** Uploads the scene: one buffer write, however many blocks there are. */
  setScene(kind, items, isDimmed) {
    const data = kind === "treemap" ? packTreemap(items, isDimmed) : packSunburst(items, isDimmed);
    const { gl } = this;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.programs[kind].buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
    this.kind = kind;
    this.count = items.length;
  }

  draw({ theme, scale, progress, hover = -1, selected = -1, hoverRect, selectedRect, centre = [0, 0] }) {
    const { gl, canvas } = this;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clearColor(...theme.background, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    if (!this.count) return;
    const { program, array, uniforms: u } = this.programs[this.kind];
    gl.useProgram(program);
    gl.uniform4f(u.uViewport, canvas.width, canvas.height, scale, progress);
    gl.uniform4f(u.uBackground, ...theme.background, 1);
    gl.uniform4f(u.uLight, ...theme.light);
    gl.uniform4f(u.uLightParams, ...theme.params);
    gl.uniform4f(u.uBorder, ...theme.border);
    gl.uniform4f(u.uAccent, ...(theme.dark || this.kind === "treemap" ? [1, 1, 1] : [0.1, 0.1, 0.12]), 0.96);
    gl.uniform4f(u.uHoverRect, ...(hoverRect ?? [0, 0, 0, 0]));
    gl.uniform4f(u.uSelRect, ...(selectedRect ?? [0, 0, 0, 0]));
    gl.uniform4i(u.uIds, hover, selected, 0, 0);
    gl.uniform4f(u.uSun, centre[0], centre[1], Math.max(1, scale), 0);
    gl.bindVertexArray(array);
    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, this.count);
    gl.bindVertexArray(null);
  }
}

function packTreemap(items, isDimmed) {
  const data = new Float32Array(items.length * TREEMAP_STRIDE);
  items.forEach((item, i) => {
    const flags = (isDimmed(item.block) ? DIMMED : 0) | (item.block.kind === "rest" ? REST : 0) | (item.block.cloud ? CLOUDED : 0);
    data.set([...item.from, ...item.to, ...item.coeffsFrom, ...item.coeffsTo, ...rgb01(item.block.color), item.alphaFrom, item.alphaTo, flags], i * TREEMAP_STRIDE);
  });
  return data;
}

function packSunburst(items, isDimmed) {
  const data = new Float32Array(items.length * SUNBURST_STRIDE);
  items.forEach((item, i) => {
    const flags = item.hub
      ? IS_HUB
      : (isDimmed(item.arc) ? DIMMED : 0) | (item.arc.kind === "rest" ? REST : 0) | (item.arc.cloud ? CLOUDED : 0);
    data.set([...item.from, ...item.to, ...rgb01(item.colorFrom), item.alphaFrom, ...rgb01(item.colorTo), item.alphaTo, flags], i * SUNBURST_STRIDE);
  });
  return data;
}

/** Without WebGL 2: the same scenes with plain 2D fills (a gradient stands in for the cushion). */
class CanvasRenderer {
  name = "Canvas 2D";
  count = 0;

  constructor(canvas) {
    this.canvas = canvas;
    this.context = canvas.getContext("2d", { alpha: false });
  }

  setScene(kind, items, isDimmed) {
    this.kind = kind;
    this.items = items.map((item) => ({ item, dimmed: !item.hub && isDimmed(item.block ?? item.arc) }));
    this.count = items.length;
  }

  draw({ theme, progress, hover = -1, selected = -1, hoverRect, selectedRect, centre = [0, 0] }) {
    const { context: c, canvas } = this;
    if (!c) return;
    const paint = (v, a = 1) => `rgb(${v.map((x) => Math.round(x)).join(" ")} / ${a})`;
    const lerp = (a, b) => a.map((x, i) => x + (b[i] - x) * progress);
    c.fillStyle = paint(theme.background.map((x) => x * 255));
    c.fillRect(0, 0, canvas.width, canvas.height);
    this.items?.forEach(({ item, dimmed }, i) => {
      const alpha = (item.alphaFrom + (item.alphaTo - item.alphaFrom) * progress) * (dimmed ? 0.18 : 1);
      if (alpha <= 0) return;
      c.globalAlpha = alpha;
      const geo = lerp(item.from, item.to);
      if (this.kind === "treemap") {
        const [x, y, w, h] = geo;
        if (w <= 0 || h <= 0) return;
        const gradient = c.createLinearGradient(x, y, x + w, y + h);
        gradient.addColorStop(0, paint(item.block.color));
        gradient.addColorStop(1, paint(item.block.color.map((v) => v * 0.45)));
        c.fillStyle = i === hover ? paint(item.block.color.map((v) => Math.min(255, v + 45))) : gradient;
        c.fillRect(x, y, Math.max(w - 1, 0.5), Math.max(h - 1, 0.5));
      } else {
        const [a0, a1, r0, r1] = geo;
        c.fillStyle = paint(lerp(item.colorFrom, item.colorTo).map((v) => (i === hover ? Math.min(255, v + 40) : v)));
        c.beginPath();
        c.arc(centre[0], centre[1], r1, a0 - Math.PI / 2, a1 - Math.PI / 2);
        c.arc(centre[0], centre[1], Math.max(r0, 0), a1 - Math.PI / 2, a0 - Math.PI / 2, true);
        c.fill();
        c.strokeStyle = paint(theme.background.map((x) => x * 255));
        c.stroke();
      }
    });
    c.globalAlpha = 1;
    for (const [rect, width] of [[hoverRect, 1], [selectedRect, 3]]) {
      if (!rect) continue;
      c.strokeStyle = "white";
      c.lineWidth = width;
      c.strokeRect(rect[0] + width / 2, rect[1] + width / 2, rect[2] - width, rect[3] - width);
    }
    if (selected >= 0 && this.kind === "sunburst") {
      const [a0, a1, r0, r1] = this.items[selected].item.to;
      c.strokeStyle = theme.dark ? "white" : "black";
      c.lineWidth = 3;
      c.beginPath();
      c.arc(centre[0], centre[1], r1 - 2, a0 - Math.PI / 2, a1 - Math.PI / 2);
      c.arc(centre[0], centre[1], r0 + 2, a1 - Math.PI / 2, a0 - Math.PI / 2, true);
      c.closePath();
      c.stroke();
      c.lineWidth = 1;
    }
  }
}
