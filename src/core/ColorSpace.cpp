#include "ColorSpace.hpp"

#include <algorithm>
#include <cmath>

namespace diskprism {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct LinearRgb {
    float r, g, b;
};
struct Lab {
    float L, a, b;
};

float srgbEncode(float linear) noexcept {
    if (linear <= 0.0031308f) return 12.92f * linear;
    return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

float srgbDecode(float encoded) noexcept {
    if (encoded <= 0.04045f) return encoded / 12.92f;
    return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

LinearRgb oklabToLinear(const Lab& lab) noexcept {
    const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;
    return LinearRgb{
        +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
    };
}

Lab linearToOklab(const LinearRgb& c) noexcept {
    const float l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    const float m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    const float s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;
    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);
    return Lab{
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

bool inGamut(const LinearRgb& c) noexcept {
    constexpr float kEps = 1e-4f;
    return c.r >= -kEps && c.r <= 1.0f + kEps && c.g >= -kEps && c.g <= 1.0f + kEps && c.b >= -kEps &&
           c.b <= 1.0f + kEps;
}

uint8_t toByte(float linear) noexcept {
    const float encoded = srgbEncode(std::clamp(linear, 0.0f, 1.0f));
    return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

Lab toOklab(Color c) noexcept {
    return linearToOklab(LinearRgb{srgbDecode(static_cast<float>(c.r) / 255.0f),
                                   srgbDecode(static_cast<float>(c.g) / 255.0f),
                                   srgbDecode(static_cast<float>(c.b) / 255.0f)});
}

}  // namespace

Color oklchToSrgb(float lightness, float chroma, float hueDegrees) noexcept {
    const float L = std::clamp(lightness, 0.0f, 1.0f);
    const float hue = hueDegrees * kPi / 180.0f;
    const float cosH = std::cos(hue);
    const float sinH = std::sin(hue);
    float C = std::max(chroma, 0.0f);

    // Gamut mapping by chroma reduction: keeps lightness and hue (the perceptually
    // important attributes for telling categories apart) and gives up saturation.
    LinearRgb rgb = oklabToLinear(Lab{L, C * cosH, C * sinH});
    for (int i = 0; i < 48 && !inGamut(rgb); ++i) {
        C *= 0.93f;
        rgb = oklabToLinear(Lab{L, C * cosH, C * sinH});
    }
    return Color{toByte(rgb.r), toByte(rgb.g), toByte(rgb.b), 255};
}

float oklabDistance(Color a, Color b) noexcept {
    const Lab x = toOklab(a);
    const Lab y = toOklab(b);
    const float dL = x.L - y.L;
    const float da = x.a - y.a;
    const float db = x.b - y.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

Color mixColors(Color a, Color b, float t) noexcept {
    const float u = std::clamp(t, 0.0f, 1.0f);
    const auto lerp = [u](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(
            std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * u));
    };
    return Color{lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

}  // namespace diskprism
