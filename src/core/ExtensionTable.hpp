// DiskPrism core — interned file-extension table with per-extension usage histogram.
#pragma once

#include "CoreTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace diskprism {

/// Aggregate usage of every live file that carries one extension.
struct ExtensionStats {
    uint64_t allocated_bytes = 0;
    uint64_t logical_bytes = 0;
    uint64_t file_count = 0;

    [[nodiscard]] uint64_t bytes(SizeMetric metric) const noexcept {
        return metric == SizeMetric::Physical ? allocated_bytes : logical_bytes;
    }
    constexpr bool operator==(const ExtensionStats&) const = default;
};

/// Maps lowercased extensions ("mov", "dylib", "<none>") to compact 16-bit IDs so that a
/// node stores two bytes instead of a string, and keeps the usage histogram and the
/// display color for each ID.
///
/// Thread-safety: `intern*()` and `find()` may be called concurrently from scanner
/// threads. Everything else (statistics, colors, enumeration) is meant for the single
/// thread that owns the finished arena.
class ExtensionTable {
public:
    static constexpr uint16_t kNone = 0;   ///< "<none>": the file name has no extension
    static constexpr uint16_t kOther = 1;  ///< "<other>": overflow once 65 535 IDs exist
    static constexpr size_t kMaxExtensionLength = 20;
    static constexpr size_t kMaxEntries = 65535;

    ExtensionTable();

    ExtensionTable(const ExtensionTable&) = delete;
    ExtensionTable& operator=(const ExtensionTable&) = delete;

    /// Extracts the lowercased extension of `filename` into `scratch` and returns a view
    /// of it; returns an empty view when the name has no usable extension (no dot, a
    /// leading-dot "hidden" name, an over-long suffix, or a suffix with spaces or
    /// punctuation that is clearly prose rather than a type).
    [[nodiscard]] static std::string_view parseExtension(
        std::string_view filename, std::array<char, kMaxExtensionLength>& scratch) noexcept;

    /// Returns the ID for an already-lowercased extension, creating it if needed.
    [[nodiscard]] uint16_t intern(std::string_view lowercased);

    /// Convenience: `parseExtension` + `intern`.
    [[nodiscard]] uint16_t internFilename(std::string_view filename);

    /// Looks up an extension without creating it.
    [[nodiscard]] std::optional<uint16_t> find(std::string_view lowercased) const;

    [[nodiscard]] size_t size() const;
    [[nodiscard]] std::string_view name(uint16_t id) const;

    // --- Histogram (single-threaded) --------------------------------------------------

    void resetStats();
    void addFile(uint16_t id, uint64_t allocated, uint64_t logical);
    void removeFile(uint16_t id, uint64_t allocated, uint64_t logical) noexcept;
    [[nodiscard]] ExtensionStats stats(uint16_t id) const noexcept;
    [[nodiscard]] ExtensionStats totals() const noexcept;

    /// IDs that currently own at least one file, largest first (ties broken by name).
    [[nodiscard]] std::vector<uint16_t> rankedBySize(SizeMetric metric) const;

    // --- Colors -----------------------------------------------------------------------

    /// Gives the extensions that occupy the most physical space the most distinct
    /// palette entries. Deterministic for a given histogram.
    void assignColors();

    [[nodiscard]] Color color(uint16_t id) const noexcept;

    /// Deterministic palette: rank 0 is the largest extension. The first 12 ranks are
    /// evenly spaced OKLCH hues, later tiers vary lightness/chroma, and the long tail
    /// fades to low-chroma tones so it never competes with the dominant types.
    [[nodiscard]] static Color paletteColor(uint32_t rank) noexcept;

    /// Neutral tone used for aggregated blocks and as the color of empty types.
    [[nodiscard]] static Color neutralColor() noexcept;

private:
    [[nodiscard]] uint16_t internLocked(std::string_view lowercased);
    [[nodiscard]] static Color fallbackColor(std::string_view name) noexcept;

    mutable std::shared_mutex mutex_;
    std::deque<std::string> names_;  // deque: element addresses stay valid on growth
    std::unordered_map<std::string_view, uint16_t> lookup_;

    std::vector<ExtensionStats> stats_;
    std::vector<Color> colors_;
};

/// Small direct-mapped cache each scanner thread keeps in front of the shared table.
/// After warm-up virtually every lookup is a hit, so interning costs one hash and one
/// short compare with no lock and no shared-cache-line traffic.
class ExtensionCache {
public:
    explicit ExtensionCache(ExtensionTable& table) noexcept : table_(table) {}

    /// Returns the ID for the extension of `filename`.
    [[nodiscard]] uint16_t idForFilename(std::string_view filename);

private:
    static constexpr size_t kSlots = 256;
    struct Slot {
        std::array<char, ExtensionTable::kMaxExtensionLength> key{};
        uint8_t length = 0;
        bool used = false;
        uint16_t id = 0;
    };

    ExtensionTable& table_;
    std::array<Slot, kSlots> slots_{};
};

}  // namespace diskprism
