// DiskPrism UI — text formatting and small Foundation / C++ bridging helpers.
#import <AppKit/AppKit.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace diskprism {
class FlatArena;
}

NS_ASSUME_NONNULL_BEGIN

/// "12.3 GB" — decimal units, the way Finder shows sizes.
NSString *DPFormatBytes(uint64_t bytes);

/// "1,234,567" in the user's locale.
NSString *DPFormatCount(uint64_t count);

/// "1.2M", "845K", "312" — for tight spaces such as throughput read-outs.
NSString *DPFormatCompactCount(double count);

/// "12.3%", "<0.1%", "100%".
NSString *DPFormatPercent(double fraction);

/// "850 ms", "3.21 s", "2 min 5 s".
NSString *DPFormatDuration(double seconds);

/// Compact date ("24 Nov 26" in the user's locale), or an em dash when unknown.
NSString *DPFormatDate(int64_t unixSeconds);

/// File-system bytes -> NSString. Names on disk are not guaranteed to be valid UTF-8,
/// so this never returns nil.
NSString *DPStringFromFileSystemBytes(std::string_view bytes);

/// Display name of a node (the last path component for the scan root).
NSString *DPNodeDisplayName(const diskprism::FlatArena &arena, uint32_t node);

/// Absolute path / file URL of a node.
NSString *DPNodePath(const diskprism::FlatArena &arena, uint32_t node);
NSURL *DPNodeURL(const diskprism::FlatArena &arena, uint32_t node);

/// An SF Symbol configured for small inline use, or nil if the symbol does not exist on
/// this system (callers then simply show no glyph).
NSImage *_Nullable DPSymbol(NSString *name, NSString *accessibilityDescription);

/// A text label that never wraps and truncates in the middle.
NSTextField *DPMakeLabel(NSString *text, NSFont *font, NSColor *color);

NS_ASSUME_NONNULL_END
