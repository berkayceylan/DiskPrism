#import "Formatting.h"

#include "FlatArena.hpp"

#include <cmath>

NSString *DPFormatBytes(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:static_cast<long long>(bytes) countStyle:NSByteCountFormatterCountStyleFile];
}

NSString *DPFormatCount(uint64_t count) {
    static NSNumberFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [[NSNumberFormatter alloc] init];
        formatter.numberStyle = NSNumberFormatterDecimalStyle;
        formatter.maximumFractionDigits = 0;
    });
    return [formatter stringFromNumber:@(count)];
}

NSString *DPFormatCompactCount(double count) {
    if (!(count >= 0)) return @"0";
    if (count >= 1e9) return [NSString stringWithFormat:@"%.1fB", count / 1e9];
    if (count >= 1e6) return [NSString stringWithFormat:@"%.1fM", count / 1e6];
    if (count >= 1e4) return [NSString stringWithFormat:@"%.0fK", count / 1e3];
    if (count >= 1e3) return [NSString stringWithFormat:@"%.1fK", count / 1e3];
    return [NSString stringWithFormat:@"%.0f", count];
}

NSString *DPFormatPercent(double fraction) {
    if (!(fraction > 0)) return @"0%";
    const double percent = fraction * 100.0;
    if (percent < 0.1) return @"<0.1%";
    if (percent >= 99.95) return @"100%";
    return [NSString stringWithFormat:@"%.1f%%", percent];
}

NSString *DPFormatDuration(double seconds) {
    if (!(seconds >= 0)) return @"—";
    if (seconds < 1.0) return [NSString stringWithFormat:@"%.0f ms", seconds * 1000.0];
    if (seconds < 60.0) return [NSString stringWithFormat:@"%.2f s", seconds];
    const long whole = std::lround(seconds);
    return [NSString stringWithFormat:@"%ld min %ld s", whole / 60, whole % 60];
}

NSString *DPFormatDate(int64_t unixSeconds) {
    if (unixSeconds <= 0) return @"—";
    static NSDateFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [[NSDateFormatter alloc] init];
        // Day, abbreviated month, two-digit year, in the user's own order and language:
        // compact enough for a narrow table column.
        [formatter setLocalizedDateFormatFromTemplate:@"dMMMyy"];
    });
    return [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:static_cast<NSTimeInterval>(unixSeconds)]];
}

NSString *DPStringFromFileSystemBytes(std::string_view bytes) {
    if (bytes.empty()) return @"";
    NSString *string = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSUTF8StringEncoding];
    if (string != nil) return string;
    // Not valid UTF-8 (legacy volumes): show every byte rather than nothing.
    string = [[NSString alloc] initWithBytes:bytes.data() length:bytes.size() encoding:NSISOLatin1StringEncoding];
    return string != nil ? string : @"?";
}

NSString *DPNodePath(const diskprism::FlatArena &arena, uint32_t node) {
    return DPStringFromFileSystemBytes(arena.path(node));
}

NSURL *DPNodeURL(const diskprism::FlatArena &arena, uint32_t node) {
    const std::string path = arena.path(node);
    return [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:arena.node(node).is_dir() relativeToURL:nil];
}

NSString *DPNodeDisplayName(const diskprism::FlatArena &arena, uint32_t node) {
    NSString *name = DPStringFromFileSystemBytes(arena.name(node));
    if (node != arena.root()) return name;
    // The root's name is the whole scanned path; show the volume or folder name instead.
    if ([name isEqualToString:@"/"]) {
        NSString *volumeName = nil;
        NSURL *url = [NSURL fileURLWithPath:@"/" isDirectory:YES];
        if ([url getResourceValue:&volumeName forKey:NSURLVolumeNameKey error:nil] && volumeName.length > 0) return volumeName;
        return @"Macintosh HD";
    }
    return name.lastPathComponent;
}

NSImage *DPSymbol(NSString *name, NSString *accessibilityDescription) {
    return [NSImage imageWithSystemSymbolName:name accessibilityDescription:accessibilityDescription];
}

NSTextField *DPMakeLabel(NSString *text, NSFont *font, NSColor *color) {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = font;
    label.textColor = color;
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.maximumNumberOfLines = 1;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}
