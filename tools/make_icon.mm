// dp_make_icon — draws DiskPrism's app icon with Core Graphics and writes AppIcon.icns.
// Runs at build time, so the repository needs no binary assets and no external tools.
//
//   dp_make_icon <output.icns>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void appendBigEndian(NSMutableData *data, uint32_t value) {
    const uint32_t swapped = CFSwapInt32HostToBig(value);
    [data appendBytes:&swapped length:sizeof(swapped)];
}

/// One PNG chunk: length, type, payload, CRC-32 of type and payload.
void appendChunk(NSMutableData *png, const char *type, NSData *payload) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    NSMutableData *body = [NSMutableData dataWithBytes:type length:4];
    [body appendData:payload];
    uint32_t crc = 0xFFFFFFFFu;
    const auto *bytes = static_cast<const uint8_t *>(body.bytes);
    for (NSUInteger i = 0; i < body.length; ++i) crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    appendBigEndian(png, static_cast<uint32_t>(payload.length));
    [png appendData:body];
    appendBigEndian(png, crc ^ 0xFFFFFFFFu);
}

/// Encodes an image as PNG, keeping the smallest of three whole-image row filters. On flat
/// artwork like this icon that is far smaller than what NSBitmapImageRep writes.
NSData *encodePNG(CGImageRef image) {
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    const size_t stride = width * 4;
    std::vector<uint8_t> pixels(stride * height);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, stride, CGImageGetColorSpace(image),
                                                 static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));
    if (context == nullptr) return nil;
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    for (size_t i = 0; i < pixels.size(); i += 4) {  // PNG stores straight alpha
        const unsigned alpha = pixels[i + 3];
        for (size_t c = 0; c < 3; ++c) pixels[i + c] = alpha == 0 ? 0 : static_cast<uint8_t>(std::min(255u, (pixels[i + c] * 255u + alpha / 2) / alpha));
    }

    NSData *best = nil;
    for (const uint8_t filter : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {  // none, sub, up
        NSMutableData *raw = [NSMutableData dataWithCapacity:(stride + 1) * height];
        std::vector<uint8_t> line(stride);
        uint32_t a = 1, b = 0;  // Adler-32
        const auto append = [&](const uint8_t *bytes, size_t count) {
            [raw appendBytes:bytes length:count];
            for (size_t i = 0; i < count; ++i) {
                a = (a + bytes[i]) % 65521;
                b = (b + a) % 65521;
            }
        };
        for (size_t y = 0; y < height; ++y) {
            const uint8_t *row = &pixels[y * stride];
            for (size_t x = 0; x < stride; ++x) {
                const uint8_t left = x >= 4 ? row[x - 4] : 0;
                const uint8_t above = y > 0 ? pixels[(y - 1) * stride + x] : 0;
                line[x] = static_cast<uint8_t>(row[x] - (filter == 1 ? left : filter == 2 ? above : 0));
            }
            append(&filter, 1);
            append(line.data(), stride);
        }
        // Foundation emits a raw DEFLATE stream; PNG wants it in a zlib envelope.
        NSData *deflated = [raw compressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:nil];
        if (deflated == nil) return nil;
        const uint8_t zlibHeader[2] = {0x78, 0x9C};
        NSMutableData *idat = [NSMutableData dataWithBytes:zlibHeader length:2];
        [idat appendData:deflated];
        appendBigEndian(idat, (b << 16) | a);
        if (best == nil || idat.length < best.length) best = idat;
    }

    const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    NSMutableData *png = [NSMutableData dataWithBytes:signature length:8];
    NSMutableData *header = [NSMutableData data];
    appendBigEndian(header, static_cast<uint32_t>(width));
    appendBigEndian(header, static_cast<uint32_t>(height));
    const uint8_t format[5] = {8, 6, 0, 0, 0};  // 8 bits per channel, RGBA, deflate, adaptive filters, no interlace
    [header appendBytes:format length:5];
    const uint8_t renderingIntent = 0;
    appendChunk(png, "IHDR", header);
    appendChunk(png, "sRGB", [NSData dataWithBytes:&renderingIntent length:1]);
    appendChunk(png, "IDAT", best);
    appendChunk(png, "IEND", [NSData data]);
    return png;
}

constexpr CGFloat kPi = 3.14159265358979323846;

/// Fills an annular sector. Angles in radians, clockwise from 12 o'clock (y-up canvas).
void fillSector(CGContextRef context, CGPoint centre, CGFloat inner, CGFloat outer, CGFloat start, CGFloat end, CGColorRef color) {
    // Core Graphics measures counter-clockwise from the x axis: 12 o'clock is +pi/2.
    const CGFloat a0 = kPi / 2 - start;
    const CGFloat a1 = kPi / 2 - end;
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathAddArc(path, nullptr, centre.x, centre.y, outer, a0, a1, true);
    CGPathAddArc(path, nullptr, centre.x, centre.y, inner, a1, a0, false);
    CGPathCloseSubpath(path);
    CGContextAddPath(context, path);
    CGContextSetFillColorWithColor(context, color);
    CGContextFillPath(context);
    CGPathRelease(path);
}

/// Hue (0…1) to a pleasant, slightly soft sRGB color.
CGColorRef createHueColor(CGColorSpaceRef space, CGFloat hue, CGFloat saturation, CGFloat value) {
    const CGFloat h = (hue - std::floor(hue)) * 6;
    const int sextant = static_cast<int>(h);
    const CGFloat f = h - sextant;
    const CGFloat p = value * (1 - saturation);
    const CGFloat q = value * (1 - saturation * f);
    const CGFloat t = value * (1 - saturation * (1 - f));
    CGFloat r = value, g = t, b = p;
    switch (sextant % 6) {
        case 1: r = q; g = value; b = p; break;
        case 2: r = p; g = value; b = t; break;
        case 3: r = p; g = q; b = value; break;
        case 4: r = t; g = p; b = value; break;
        case 5: r = value; g = p; b = q; break;
        default: break;
    }
    const CGFloat components[4] = {r, g, b, 1};
    return CGColorCreate(space, components);
}

CGImageRef createIcon(size_t pixels) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, pixels, pixels, 8, 0, space, static_cast<uint32_t>(kCGImageAlphaPremultipliedLast));
    if (context == nullptr) {
        CGColorSpaceRelease(space);
        return nullptr;
    }
    const CGFloat unit = static_cast<CGFloat>(pixels) / 1024;

    // macOS icon grid: an 824-point rounded square centred on a 1024-point canvas.
    const CGRect body = CGRectMake(100 * unit, 100 * unit, 824 * unit, 824 * unit);
    CGPathRef shape = CGPathCreateWithRoundedRect(body, 186 * unit, 186 * unit, nullptr);

    CGContextSaveGState(context);
    CGContextAddPath(context, shape);
    CGContextClip(context);
    // Vertical gradient, one row at a time. CGGradient would dither it, and that noise is
    // what makes a PNG of an otherwise smooth icon several times larger.
    const CGFloat top[3] = {0.20, 0.22, 0.30};
    const CGFloat bottom[3] = {0.07, 0.08, 0.11};
    const auto rows = static_cast<int>(std::ceil(body.size.height));
    for (int row = 0; row < rows; ++row) {
        const CGFloat t = rows > 1 ? static_cast<CGFloat>(row) / (rows - 1) : 0;  // 0 at the bottom (y-up canvas)
        CGContextSetRGBFillColor(context, bottom[0] + (top[0] - bottom[0]) * t, bottom[1] + (top[1] - bottom[1]) * t,
                                 bottom[2] + (top[2] - bottom[2]) * t, 1);
        CGContextFillRect(context, CGRectMake(CGRectGetMinX(body), std::floor(CGRectGetMinY(body)) + row, body.size.width, 1));
    }

    // A three-ring sunburst: every slice of a ring is split further on the next one.
    const CGPoint centre = CGPointMake(512 * unit, 512 * unit);
    const CGFloat gap = 0.022;  // radians between slices
    const CGFloat rings[3][2] = {{150, 246}, {258, 330}, {342, 396}};
    const CGFloat shares[6] = {0.30, 0.22, 0.17, 0.13, 0.10, 0.08};
    CGFloat angle = 0;
    for (int i = 0; i < 6; ++i) {
        const CGFloat sweep = shares[i] * 2 * kPi;
        const CGFloat hue = 0.58 + (angle + sweep / 2) / (2 * kPi);
        CGColorRef color = createHueColor(space, hue, 0.62, 0.96);
        fillSector(context, centre, rings[0][0] * unit, rings[0][1] * unit, angle + gap / 2, angle + sweep - gap / 2, color);
        CGColorRelease(color);

        // Second ring: two or three children; third ring: only the larger slices continue.
        const int children = i < 3 ? 3 : 2;
        const CGFloat childShares[3] = {children == 3 ? 0.5 : 0.62, children == 3 ? 0.3 : 0.38, 0.2};
        CGFloat childAngle = angle;
        for (int c = 0; c < children; ++c) {
            const CGFloat childSweep = sweep * childShares[c];
            const CGFloat childHue = 0.58 + (childAngle + childSweep / 2) / (2 * kPi);
            CGColorRef childColor = createHueColor(space, childHue, 0.48, 0.98);
            fillSector(context, centre, rings[1][0] * unit, rings[1][1] * unit, childAngle + gap / 2, childAngle + childSweep - gap / 2, childColor);
            CGColorRelease(childColor);
            if (c == 0 && childSweep > 0.3) {
                CGColorRef outerColor = createHueColor(space, childHue, 0.34, 1.0);
                fillSector(context, centre, rings[2][0] * unit, rings[2][1] * unit, childAngle + gap / 2,
                           childAngle + childSweep * 0.68 - gap / 2, outerColor);
                CGColorRelease(outerColor);
            }
            childAngle += childSweep;
        }
        angle += sweep;
    }

    // Hub.
    const CGFloat hubComponents[4] = {0.93, 0.94, 0.97, 1};
    CGColorRef hub = CGColorCreate(space, hubComponents);
    CGContextSetFillColorWithColor(context, hub);
    CGContextFillEllipseInRect(context, CGRectMake(centre.x - 118 * unit, centre.y - 118 * unit, 236 * unit, 236 * unit));
    CGColorRelease(hub);
    CGContextRestoreGState(context);

    CGPathRelease(shape);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return image;
}

}  // namespace

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) {
            std::fprintf(stderr, "usage: dp_make_icon <output.icns>\n");
            return 2;
        }
        // An .icns file is a tagged container: "icns" + total length, then one element per
        // size (4-byte type, 4-byte length including this header, PNG data), big-endian.
        struct Element {
            const char *type;
            size_t pixels;
        };
        const Element elements[] = {{"icp4", 16}, {"icp5", 32}, {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}};
        NSMutableData *body = [NSMutableData data];
        for (const Element &element : elements) {
            CGImageRef image = createIcon(element.pixels);
            NSData *png = nil;
            if (image != nullptr) {
                png = encodePNG(image);
                CGImageRelease(image);
            }
            if (png == nil) {
                std::fprintf(stderr, "dp_make_icon: rendering %zu px failed\n", element.pixels);
                return 1;
            }
            [body appendBytes:element.type length:4];
            appendBigEndian(body, static_cast<uint32_t>(png.length + 8));
            [body appendData:png];
        }
        NSMutableData *file = [NSMutableData dataWithBytes:"icns" length:4];
        appendBigEndian(file, static_cast<uint32_t>(body.length + 8));
        [file appendData:body];
        if (![file writeToFile:@(argv[1]) atomically:YES]) {
            std::fprintf(stderr, "dp_make_icon: writing %s failed\n", argv[1]);
            return 1;
        }
    }
    return 0;
}
