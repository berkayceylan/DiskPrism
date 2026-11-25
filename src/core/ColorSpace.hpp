// DiskPrism core — perceptual color helpers (OKLab / OKLCH, Björn Ottosson 2020).
#pragma once

#include "CoreTypes.hpp"

namespace diskprism {

/// Converts an OKLCH color to 8-bit sRGB.
///
/// OKLCH is perceptually uniform: equal steps in hue or lightness look equally large,
/// which is what makes a generated palette "perceptually distinct". If the requested
/// chroma lies outside the sRGB gamut it is reduced until the color fits, so the result
/// is always a valid, un-clipped color.
[[nodiscard]] Color oklchToSrgb(float lightness, float chroma, float hueDegrees) noexcept;

/// Euclidean distance between two sRGB colors measured in OKLab.
/// Roughly 0.02 is a just-noticeable difference; 0.1 is clearly distinct.
[[nodiscard]] float oklabDistance(Color a, Color b) noexcept;

/// Component-wise linear interpolation in (gamma-encoded) sRGB.
[[nodiscard]] Color mixColors(Color a, Color b, float t) noexcept;

}  // namespace diskprism
