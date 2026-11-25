// DiskPrism core — shared vocabulary types.
// Pure C++20; no AppKit / Metal dependencies anywhere under src/core.
#pragma once

#include <cstdint>

namespace diskprism {

/// Sentinel for "no node". Node identifiers are dense 32-bit indices into the arena.
inline constexpr uint32_t kInvalidNode = UINT32_MAX;

/// Which byte count drives layouts, sorting and percentages.
enum class SizeMetric : uint8_t {
    Physical = 0,  ///< blocks actually allocated on the local disk (default)
    Logical = 1,   ///< apparent file length (what `ls -l` shows)
};

/// 8-bit-per-channel sRGB color.
struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    constexpr bool operator==(const Color&) const = default;

    /// Packs as 0xAABBGGRR (little-endian byte order r,g,b,a).
    [[nodiscard]] constexpr uint32_t packed() const {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
    }

    [[nodiscard]] static constexpr Color fromPacked(uint32_t v) {
        return Color{static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                     static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
    }
};

}  // namespace diskprism
