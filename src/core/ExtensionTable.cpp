#include "ExtensionTable.hpp"

#include "ColorSpace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace diskprism {

namespace {

constexpr char asciiLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Characters that can plausibly appear in a real file-type suffix. Anything else
/// (spaces, brackets, commas…) means the "extension" is really the tail of a sentence,
/// e.g. "Notes for Dr. Smith".
constexpr bool isExtensionChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80) return true;  // UTF-8 continuation / lead bytes
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return c == '_' || c == '-' || c == '+' || c == '~' || c == '$' || c == '#' || c == '@' || c == '!';
}

uint32_t fnv1a(std::string_view text) noexcept {
    uint32_t hash = 2166136261u;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

/// Extensions that exist on practically every Mac. Interning them up front gives them
/// stable IDs across scans regardless of the order in which threads encounter files.
constexpr std::string_view kWellKnown[] = {
    "mov",  "mp4",   "m4v",  "mkv",    "avi",     "jpg",    "jpeg",  "png",   "heic",   "gif",   "tiff",
    "tif",  "psd",   "dng",  "raw",    "webp",    "svg",    "icns",  "mp3",   "m4a",    "wav",   "aiff",
    "aif",  "flac",  "caf",  "pdf",    "doc",     "docx",   "xls",   "xlsx",  "ppt",    "pptx",  "key",
    "pages", "numbers", "txt", "md",   "rtf",     "csv",    "html",  "css",   "js",     "ts",    "json",
    "xml",  "plist", "yaml", "yml",    "c",       "h",      "cpp",   "hpp",   "m",      "mm",    "swift",
    "py",   "pyc",   "rb",   "go",     "rs",      "java",   "kt",    "sh",    "o",      "a",     "dylib",
    "so",   "tbd",   "dmg",  "pkg",    "zip",     "gz",     "tgz",   "tar",   "bz2",    "xz",    "7z",
    "rar",  "iso",   "img",  "sparseimage", "car", "nib",   "strings", "ttf", "otf",    "ttc",   "db",
    "sqlite", "log", "dat",  "bin",    "cache",   "tmp",    "map",   "node",  "wasm",   "jar",   "class",
    "metallib", "mlmodel", "ipa", "app", "framework", "bundle", "lproj", "storyboardc", "swiftmodule",
};

}  // namespace

ExtensionTable::ExtensionTable() {
    names_.emplace_back("<none>");
    lookup_.emplace(std::string_view(names_.back()), kNone);
    names_.emplace_back("<other>");
    lookup_.emplace(std::string_view(names_.back()), kOther);
    for (const std::string_view ext : kWellKnown) {
        if (lookup_.find(ext) != lookup_.end()) continue;
        names_.emplace_back(ext);
        lookup_.emplace(std::string_view(names_.back()), static_cast<uint16_t>(names_.size() - 1));
    }
}

std::string_view ExtensionTable::parseExtension(
    std::string_view filename, std::array<char, kMaxExtensionLength>& scratch) noexcept {
    const size_t dot = filename.rfind('.');
    // No dot, a dotfile such as ".zshrc", or a trailing dot: no extension.
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= filename.size()) return {};
    const std::string_view suffix = filename.substr(dot + 1);
    if (suffix.size() > kMaxExtensionLength) return {};
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (!isExtensionChar(suffix[i])) return {};
        scratch[i] = asciiLower(suffix[i]);
    }
    return std::string_view(scratch.data(), suffix.size());
}

uint16_t ExtensionTable::intern(std::string_view lowercased) {
    if (lowercased.empty()) return kNone;
    {
        std::shared_lock lock(mutex_);
        const auto it = lookup_.find(lowercased);
        if (it != lookup_.end()) return it->second;
    }
    std::unique_lock lock(mutex_);
    return internLocked(lowercased);
}

uint16_t ExtensionTable::internLocked(std::string_view lowercased) {
    const auto it = lookup_.find(lowercased);
    if (it != lookup_.end()) return it->second;  // another thread won the race
    if (names_.size() >= kMaxEntries) return kOther;
    names_.emplace_back(lowercased);
    const auto id = static_cast<uint16_t>(names_.size() - 1);
    lookup_.emplace(std::string_view(names_.back()), id);
    return id;
}

uint16_t ExtensionTable::internFilename(std::string_view filename) {
    std::array<char, kMaxExtensionLength> scratch;
    return intern(parseExtension(filename, scratch));
}

std::optional<uint16_t> ExtensionTable::find(std::string_view lowercased) const {
    std::shared_lock lock(mutex_);
    const auto it = lookup_.find(lowercased);
    if (it == lookup_.end()) return std::nullopt;
    return it->second;
}

size_t ExtensionTable::size() const {
    std::shared_lock lock(mutex_);
    return names_.size();
}

std::string_view ExtensionTable::name(uint16_t id) const {
    std::shared_lock lock(mutex_);
    if (id >= names_.size()) return {};
    return names_[id];
}

void ExtensionTable::resetStats() {
    stats_.assign(size(), ExtensionStats{});
}

void ExtensionTable::addFile(uint16_t id, uint64_t allocated, uint64_t logical) {
    if (id >= stats_.size()) stats_.resize(static_cast<size_t>(id) + 1);
    ExtensionStats& s = stats_[id];
    s.allocated_bytes += allocated;
    s.logical_bytes += logical;
    s.file_count += 1;
}

void ExtensionTable::removeFile(uint16_t id, uint64_t allocated, uint64_t logical) noexcept {
    if (id >= stats_.size()) return;
    ExtensionStats& s = stats_[id];
    s.allocated_bytes -= std::min(s.allocated_bytes, allocated);
    s.logical_bytes -= std::min(s.logical_bytes, logical);
    if (s.file_count > 0) s.file_count -= 1;
}

ExtensionStats ExtensionTable::stats(uint16_t id) const noexcept {
    return id < stats_.size() ? stats_[id] : ExtensionStats{};
}

ExtensionStats ExtensionTable::totals() const noexcept {
    ExtensionStats total;
    for (const ExtensionStats& s : stats_) {
        total.allocated_bytes += s.allocated_bytes;
        total.logical_bytes += s.logical_bytes;
        total.file_count += s.file_count;
    }
    return total;
}

std::vector<uint16_t> ExtensionTable::rankedBySize(SizeMetric metric) const {
    std::vector<uint16_t> ids;
    ids.reserve(stats_.size());
    for (size_t i = 0; i < stats_.size(); ++i) {
        if (stats_[i].file_count > 0) ids.push_back(static_cast<uint16_t>(i));
    }
    std::shared_lock lock(mutex_);
    std::sort(ids.begin(), ids.end(), [&](uint16_t a, uint16_t b) {
        const uint64_t sa = stats_[a].bytes(metric);
        const uint64_t sb = stats_[b].bytes(metric);
        if (sa != sb) return sa > sb;
        if (stats_[a].file_count != stats_[b].file_count) return stats_[a].file_count > stats_[b].file_count;
        return names_[a] < names_[b];
    });
    return ids;
}

Color ExtensionTable::neutralColor() noexcept {
    return oklchToSrgb(0.66f, 0.012f, 250.0f);
}

Color ExtensionTable::fallbackColor(std::string_view name) noexcept {
    const uint32_t hash = fnv1a(name);
    const float hue = static_cast<float>(hash % 3600u) / 10.0f;
    return oklchToSrgb(0.68f, 0.07f, hue);
}

Color ExtensionTable::paletteColor(uint32_t rank) noexcept {
    // Twelve hues 30° apart, visited in an order that keeps consecutive ranks far apart
    // on the hue circle (so the two largest file types never look alike).
    static constexpr int kHueOrder[12] = {0, 6, 3, 9, 1, 7, 4, 10, 2, 8, 5, 11};
    struct Tier {
        float lightness;
        float chroma;
        float hueShift;
    };
    static constexpr Tier kTiers[4] = {
        {0.70f, 0.170f, 0.0f},
        {0.56f, 0.140f, 15.0f},
        {0.82f, 0.105f, 7.0f},
        {0.45f, 0.110f, 22.0f},
    };
    constexpr float kBaseHue = 255.0f;  // rank 0 is a calm blue
    constexpr float kPi = 3.14159265358979323846f;

    const uint32_t tierIndex = rank / 12;
    const uint32_t slot = rank % 12;
    if (tierIndex < 4) {
        const Tier& tier = kTiers[tierIndex];
        const float hue = std::fmod(kBaseHue + static_cast<float>(kHueOrder[slot]) * 30.0f + tier.hueShift, 360.0f);
        // Yellows and greens read as "clean" only when they are light, blues and violets
        // only when they are darker; follow the natural lightness of each hue.
        const float towardYellow = std::cos((hue - 105.0f) * kPi / 180.0f);
        const float natural = (towardYellow > 0.0f ? 0.13f : 0.06f) * towardYellow;
        // Alternate lightness between neighbouring hues to widen their distance further.
        const float alternate = (kHueOrder[slot] % 2 == 0) ? 0.018f : -0.018f;
        return oklchToSrgb(tier.lightness + natural + alternate, tier.chroma, hue);
    }
    // Long tail: deterministic but deliberately quiet.
    const float hue = std::fmod(static_cast<float>(rank) * 137.508f, 360.0f);
    const float lightness = 0.52f + 0.045f * static_cast<float>((rank * 7u) % 5u);
    return oklchToSrgb(lightness, 0.040f, hue);
}

void ExtensionTable::assignColors() {
    const size_t count = size();
    colors_.assign(count, Color{});
    {
        std::shared_lock lock(mutex_);
        for (size_t i = 0; i < count; ++i) colors_[i] = fallbackColor(names_[i]);
    }
    colors_[kNone] = neutralColor();
    colors_[kOther] = oklchToSrgb(0.58f, 0.012f, 250.0f);

    // Files without an extension (executables, Mach-O binaries, data blobs) are often the
    // largest group of all, so they take part in the ranking like any other type; only
    // the overflow bucket keeps a neutral tone.
    // A tree of purely sparse or cloud-only files occupies nothing; rank it by length instead.
    const SizeMetric metric = totals().allocated_bytes > 0 ? SizeMetric::Physical : SizeMetric::Logical;
    uint32_t rank = 0;
    for (const uint16_t id : rankedBySize(metric)) {
        if (id == kOther) continue;
        colors_[id] = paletteColor(rank++);
    }
}

Color ExtensionTable::color(uint16_t id) const noexcept {
    if (id < colors_.size()) return colors_[id];
    return neutralColor();
}

uint16_t ExtensionCache::idForFilename(std::string_view filename) {
    std::array<char, ExtensionTable::kMaxExtensionLength> scratch;
    const std::string_view ext = ExtensionTable::parseExtension(filename, scratch);
    if (ext.empty()) return ExtensionTable::kNone;

    Slot& slot = slots_[fnv1a(ext) % kSlots];
    if (slot.used && slot.length == ext.size() && std::memcmp(slot.key.data(), ext.data(), ext.size()) == 0) {
        return slot.id;
    }
    const uint16_t id = table_.intern(ext);
    std::memcpy(slot.key.data(), ext.data(), ext.size());
    slot.length = static_cast<uint8_t>(ext.size());
    slot.id = id;
    slot.used = true;
    return id;
}

}  // namespace diskprism
