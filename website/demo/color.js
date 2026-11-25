// OKLCH palette — the same math as src/core/ColorSpace.cpp and ExtensionTable.cpp.

const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));

const encode = (linear) => (linear <= 0.0031308 ? 12.92 * linear : 1.055 * linear ** (1 / 2.4) - 0.055);

function oklabToLinear(L, a, b) {
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  ];
}

const inGamut = (rgb) => rgb.every((c) => c >= -1e-4 && c <= 1 + 1e-4);

/** OKLCH to 8-bit sRGB. Out-of-gamut colors give up chroma, never lightness or hue. */
export function oklchToSrgb(lightness, chroma, hueDegrees) {
  const L = clamp(lightness, 0, 1);
  const hue = (hueDegrees * Math.PI) / 180;
  let C = Math.max(chroma, 0);
  let rgb = oklabToLinear(L, C * Math.cos(hue), C * Math.sin(hue));
  for (let i = 0; i < 48 && !inGamut(rgb); i++) {
    C *= 0.93;
    rgb = oklabToLinear(L, C * Math.cos(hue), C * Math.sin(hue));
  }
  return rgb.map((c) => Math.round(clamp(encode(clamp(c, 0, 1)), 0, 1) * 255));
}

const HUE_ORDER = [0, 6, 3, 9, 1, 7, 4, 10, 2, 8, 5, 11];
const TIERS = [
  [0.7, 0.17, 0],
  [0.56, 0.14, 15],
  [0.82, 0.105, 7],
  [0.45, 0.11, 22],
];

/** Color of the file type with the given size rank (0 is the largest). */
export function paletteColor(rank) {
  const tier = TIERS[Math.floor(rank / 12)];
  const slot = rank % 12;
  if (tier) {
    const [lightness, chroma, shift] = tier;
    const hue = (255 + HUE_ORDER[slot] * 30 + shift) % 360;
    const towardYellow = Math.cos(((hue - 105) * Math.PI) / 180);
    const natural = (towardYellow > 0 ? 0.13 : 0.06) * towardYellow;
    const alternate = HUE_ORDER[slot] % 2 === 0 ? 0.018 : -0.018;
    return oklchToSrgb(lightness + natural + alternate, chroma, hue);
  }
  return oklchToSrgb(0.52 + 0.045 * ((rank * 7) % 5), 0.04, (rank * 137.508) % 360);
}

export const NEUTRAL = oklchToSrgb(0.66, 0.012, 250);

/** Sunburst folders are tinted by where they sit on the circle. */
export function directoryColor(midAngle, ring) {
  const hue = ((midAngle / (2 * Math.PI)) * 360 + 205) % 360;
  const step = Math.min(ring - 1, 7);
  return oklchToSrgb(Math.min(0.6 + 0.034 * step, 0.82), Math.max(0.17 - 0.013 * step, 0.07), hue);
}

export const mix = (a, b, t) => a.map((x, i) => x + (b[i] - x) * t);
export const css = (rgb) => `rgb(${rgb.map(Math.round).join(" ")})`;
