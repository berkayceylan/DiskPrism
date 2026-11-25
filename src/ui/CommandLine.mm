#import "CommandLine.h"

#import <AppKit/AppKit.h>

#import "DiskMapMetalView.h"
#import "Formatting.h"
#include "Benchmark.hpp"
#include "BulkScanner.hpp"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace diskprism::cli {

namespace {

#ifndef DISKPRISM_VERSION
#define DISKPRISM_VERSION "1.0.0"
#endif

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "DiskPrism %s — native macOS disk space analyzer\n\n"
                 "Usage:\n"
                 "  DiskPrism [folder]                      open the app (and scan the folder)\n"
                 "  DiskPrism --benchmark <path> [options]  headless scan benchmark\n"
                 "      --runs N        number of scans (default 3)\n"
                 "      --threads N     worker threads (default: all hardware threads)\n"
                 "      --logical       sort and report by logical size\n"
                 "  DiskPrism --snapshot <path> --out <file.png> [options]\n"
                 "      --mode treemap|sunburst   (default treemap)\n"
                 "      --size WxH                pixel size (default 1600x1000)\n"
                 "      --scale S                 backing scale (default 2)\n"
                 "      --appearance dark|light   (default dark)\n"
                 "      --logical                 size blocks by logical size\n"
                 "      --filter TEXT             dim everything whose name does not contain TEXT\n"
                 "      --focus REL/PATH          drill into a sub-folder first\n"
                 "      --shader FILE             use this Shaders.metal instead of the bundled one\n"
                 "  DiskPrism --version | --help\n",
                 DISKPRISM_VERSION);
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct SnapshotOptions {
    std::string path;
    std::string out;
    std::string shader;
    std::string filter;
    std::string focus;
    bool sunburst = false;
    bool dark = true;
    bool logical = false;
    int width = 1600;
    int height = 1000;
    double scale = 2.0;
};

/// Finds a node by its path relative to the root ("Library/Caches").
uint32_t findRelative(const FlatArena& arena, const std::string& relative) {
    uint32_t current = arena.root();
    size_t pos = 0;
    while (pos < relative.size() && current != kInvalidNode) {
        const size_t slash = relative.find('/', pos);
        const std::string component = relative.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!component.empty()) {
            uint32_t found = kInvalidNode;
            arena.forEachChild(current, [&](uint32_t child) {
                if (arena.name(child) == component) found = child;
            });
            current = found;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return current;
}

/// Copies `image` and, for a sunburst, adds what the window shows as AppKit labels above the
/// centre disc: the focused folder's name and size.
CGImageRef newImageWithHubText(CGImageRef image, NSString *title, NSString *value, double hubDiameter, double scale) {
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, CGImageGetColorSpace(image),
                                                 static_cast<uint32_t>(kCGImageAlphaNoneSkipLast));
    if (context == nullptr) return CGImageRetain(image);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    if (hubDiameter > 0) {
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO];
        const double valuePoints = hubDiameter / scale >= 150 ? 21 : 15;  // same sizes as the window
        NSDictionary *titleStyle = @{
            NSFontAttributeName : [NSFont systemFontOfSize:12 * scale weight:NSFontWeightMedium],
            NSForegroundColorAttributeName : NSColor.secondaryLabelColor
        };
        NSDictionary *valueStyle = @{
            NSFontAttributeName : [NSFont systemFontOfSize:valuePoints * scale weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName : NSColor.labelColor
        };
        const NSSize titleSize = [title sizeWithAttributes:titleStyle];
        const NSSize valueSize = [value sizeWithAttributes:valueStyle];
        const double bottom = (height - titleSize.height - valueSize.height) / 2;
        [value drawAtPoint:NSMakePoint((width - valueSize.width) / 2, bottom) withAttributes:valueStyle];
        [title drawAtPoint:NSMakePoint((width - titleSize.width) / 2, bottom + valueSize.height) withAttributes:titleStyle];
        NSGraphicsContext.currentContext = nil;
    }
    CGImageRef result = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    return result;
}

bool writePNG(CGImageRef image, const std::string& path) {
    NSURL *url = [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:NO relativeToURL:nil];
    NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
    NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return png != nil && [png writeToURL:url atomically:YES];
}

/// Scans a folder and renders it with the same view class the window uses, into an
/// off-screen texture. No window is created and no run loop is started.
int runSnapshot(const SnapshotOptions& options) {
    @autoreleasepool {
        [NSApplication sharedApplication];  // appearances and fonts need the app object

        FlatArena arena;
        BulkScanner scanner;
        ScanOptions scanOptions;
        scanOptions.sort_metric = options.logical ? SizeMetric::Logical : SizeMetric::Physical;
        const ScanResult result = scanner.scan(options.path, arena, scanOptions);
        if (result.status == ScanStatus::InvalidRoot) {
            std::fprintf(stderr, "snapshot: %s\n", result.error_message.c_str());
            return 1;
        }

        NodeMatchSet matches;
        if (!options.filter.empty()) {
            FilterQuery query;
            query.name_substring = options.filter;
            matches.compute(arena, query);
        }
        uint32_t focus = arena.root();
        if (!options.focus.empty()) {
            focus = findRelative(arena, options.focus);
            if (focus == kInvalidNode) {
                std::fprintf(stderr, "snapshot: no such folder below the scan root: %s\n", options.focus.c_str());
                return 1;
            }
        }

        NSString *shaderPath = options.shader.empty() ? nil : @(options.shader.c_str());
        const NSRect frame = NSMakeRect(0, 0, options.width / options.scale, options.height / options.scale);
        DiskMapMetalView *view = [[DiskMapMetalView alloc] initWithFrame:frame shaderPath:shaderPath];
        if (view.shaderErrorMessage != nil) {
            std::fprintf(stderr, "snapshot: %s\n", view.shaderErrorMessage.UTF8String);
            return 1;
        }
        view.appearance = [NSAppearance appearanceNamed:options.dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        view.sizeMetric = scanOptions.sort_metric;
        if (options.sunburst) [view setViewMode:DPViewModeSunburst animated:NO];
        [view setArena:&arena matches:&matches focus:focus animated:NO];

        CGImageRef image = [view newSnapshotWithPixelWidth:static_cast<NSUInteger>(options.width)
                                                    height:static_cast<NSUInteger>(options.height)
                                                     scale:options.scale];
        const NSUInteger instances = view.instanceCount;
        const double layoutMs = view.lastLayoutMilliseconds;
        NSString *hubTitle = DPNodeDisplayName(arena, focus);
        NSString *hubValue = DPFormatBytes(arena.node(focus).bytes(scanOptions.sort_metric));
        const double hubDiameter = view.hubRect.size.width * options.scale;
        [view setArena:nullptr matches:nullptr focus:kInvalidNode animated:NO];  // the arena dies with this scope
        if (image == nullptr) {
            std::fprintf(stderr, "snapshot: rendering failed\n");
            return 1;
        }
        __block CGImageRef finished = nullptr;
        [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
            finished = newImageWithHubText(image, hubTitle, hubValue, options.sunburst ? hubDiameter : 0, options.scale);
        }];
        CGImageRelease(image);
        const bool written = writePNG(finished, options.out);
        CGImageRelease(finished);
        if (!written) {
            std::fprintf(stderr, "snapshot: could not write %s\n", options.out.c_str());
            return 1;
        }
        std::printf("snapshot: %llu files, %llu directories scanned in %.1f ms\n", static_cast<unsigned long long>(result.totals.files),
                    static_cast<unsigned long long>(result.totals.directories), result.scan_seconds * 1e3);
        std::printf("snapshot: %s, %dx%d px, %lu instances in one draw call, layout + upload %.2f ms -> %s\n",
                    options.sunburst ? "sunburst" : "treemap", options.width, options.height, static_cast<unsigned long>(instances),
                    layoutMs, options.out.c_str());
        return 0;
    }
}

}  // namespace

Invocation handleCommandLine(int argc, const char* const* argv) {
    Invocation invocation;
    std::vector<std::string> args(argv + (argc > 0 ? 1 : 0), argv + argc);

    enum class Command { None, Benchmark, Snapshot } command = Command::None;
    BenchmarkOptions benchmark;
    SnapshotOptions snapshot;
    std::vector<std::string> positional;

    const auto fail = [&invocation](const std::string& message) {
        std::fprintf(stderr, "DiskPrism: %s\n\n", message.c_str());
        printUsage(stderr);
        invocation.handled = true;
        invocation.exit_code = 2;
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&](std::string& out) -> bool {
            if (i + 1 >= args.size()) {
                fail("missing value for " + arg);
                return false;
            }
            out = args[++i];
            return true;
        };
        std::string text;
        if (arg == "--help" || arg == "-h") {
            printUsage(stdout);
            invocation.handled = true;
            return invocation;
        }
        if (arg == "--version") {
            std::printf("DiskPrism %s\n", DISKPRISM_VERSION);
            invocation.handled = true;
            return invocation;
        }
        if (arg == "--benchmark") {
            command = Command::Benchmark;
            if (!value(benchmark.path)) return invocation;
        } else if (arg == "--snapshot") {
            command = Command::Snapshot;
            if (!value(snapshot.path)) return invocation;
        } else if (arg == "--runs") {
            if (!value(text)) return invocation;
            benchmark.runs = std::atoi(text.c_str());
        } else if (arg == "--threads") {
            if (!value(text)) return invocation;
            benchmark.threads = static_cast<unsigned>(std::max(0, std::atoi(text.c_str())));
        } else if (arg == "--logical") {
            benchmark.metric = SizeMetric::Logical;
            snapshot.logical = true;
        } else if (arg == "--out") {
            if (!value(snapshot.out)) return invocation;
        } else if (arg == "--mode") {
            if (!value(text)) return invocation;
            if (text != "treemap" && text != "sunburst") {
                fail("--mode must be treemap or sunburst");
                return invocation;
            }
            snapshot.sunburst = text == "sunburst";
        } else if (arg == "--appearance") {
            if (!value(text)) return invocation;
            if (text != "dark" && text != "light") {
                fail("--appearance must be dark or light");
                return invocation;
            }
            snapshot.dark = text == "dark";
        } else if (arg == "--size") {
            if (!value(text)) return invocation;
            if (std::sscanf(text.c_str(), "%dx%d", &snapshot.width, &snapshot.height) != 2 || snapshot.width < 16 ||
                snapshot.height < 16 || snapshot.width > 16384 || snapshot.height > 16384) {
                fail("--size must look like 1600x1000");
                return invocation;
            }
        } else if (arg == "--scale") {
            if (!value(text)) return invocation;
            snapshot.scale = std::atof(text.c_str());
            if (!(snapshot.scale >= 1.0 && snapshot.scale <= 4.0)) {
                fail("--scale must be between 1 and 4");
                return invocation;
            }
        } else if (arg == "--filter") {
            if (!value(snapshot.filter)) return invocation;
        } else if (arg == "--focus") {
            if (!value(snapshot.focus)) return invocation;
        } else if (arg == "--shader") {
            if (!value(snapshot.shader)) return invocation;
            invocation.shader_path = snapshot.shader;
        } else if (!arg.empty() && arg[0] == '-') {
            // Launch Services and AppKit pass their own switches (-psn_…, -NSDocument…,
            // -AppleLanguages (…)); they are not ours to reject. Skip a value if one follows.
            if (arg.rfind("-NS", 0) == 0 || arg.rfind("-Apple", 0) == 0) {
                if (i + 1 < args.size() && (args[i + 1].empty() || args[i + 1][0] != '-')) ++i;
            }
        } else {
            positional.push_back(arg);
        }
    }

    if (command == Command::Benchmark) {
        invocation.handled = true;
        invocation.exit_code = runBenchmark(benchmark, stdout);
        return invocation;
    }
    if (command == Command::Snapshot) {
        invocation.handled = true;
        if (snapshot.out.empty()) {
            fail("--snapshot needs --out <file.png>");
            return invocation;
        }
        invocation.exit_code = runSnapshot(snapshot);
        return invocation;
    }

    for (const std::string& candidate : positional) {
        if (isDirectory(candidate)) {
            invocation.initial_path = candidate;
            break;
        }
    }
    return invocation;
}

}  // namespace diskprism::cli
