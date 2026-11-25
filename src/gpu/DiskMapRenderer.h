// DiskPrism GPU — shader library, pipelines and draw submission (no view dependencies).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "MetalTypes.h"

NS_ASSUME_NONNULL_BEGIN

extern NSErrorDomain const DPRendererErrorDomain;

typedef NS_ERROR_ENUM(DPRendererErrorDomain, DPRendererError) {
    DPRendererErrorShaderSourceMissing = 1,   ///< Shaders.metal could not be located or read
    DPRendererErrorShaderCompileFailed = 2,   ///< the Metal compiler rejected the source
    DPRendererErrorEntryPointMissing = 3,     ///< a vertex / fragment function is absent
    DPRendererErrorPipelineFailed = 4,        ///< the render pipeline could not be created
};

/// Compiles Shaders.metal at run time and owns the two render pipelines.
///
/// Kept separate from the view so the exact code path that draws on screen can also be
/// exercised headlessly (off-screen textures) by the test suite.
@interface DPMetalRenderer : NSObject

/// Reads the shader source: `path` if given, otherwise `Shaders.metal` from the main
/// bundle's Resources. The error names every location that was tried.
+ (nullable NSString *)shaderSourceAtPath:(nullable NSString *)path error:(NSError **)error;

/// Compiles `source` with -[MTLDevice newLibraryWithSource:options:error:] and builds
/// both pipelines for `pixelFormat`. On failure `error` carries the compiler's full
/// diagnostics (file, line, column) in its localized description.
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                           shaderSource:(NSString *)source
                            pixelFormat:(MTLPixelFormat)pixelFormat
                                  error:(NSError **)error NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@property (nonatomic, readonly) id<MTLDevice> device;
@property (nonatomic, readonly) id<MTLCommandQueue> commandQueue;
@property (nonatomic, readonly) id<MTLLibrary> library;
@property (nonatomic, readonly) MTLPixelFormat pixelFormat;
@property (nonatomic, readonly) id<MTLRenderPipelineState> treemapPipeline;
@property (nonatomic, readonly) id<MTLRenderPipelineState> sunburstPipeline;
@property (nonatomic, readonly) MTLRenderPipelineReflection *treemapReflection;
@property (nonatomic, readonly) MTLRenderPipelineReflection *sunburstReflection;
/// Wall-clock time spent compiling the library and both pipelines.
@property (nonatomic, readonly) double setupMilliseconds;

/// Copies `length` bytes into a new shared-storage buffer.
- (nullable id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length label:(NSString *)label;

/// Draws every block with one instanced call: a 4-vertex triangle strip x `count`.
- (void)drawTreemapWithEncoder:(id<MTLRenderCommandEncoder>)encoder
                     instances:(id<MTLBuffer>)instances
                         count:(NSUInteger)count
                      uniforms:(const diskprism::gpu::FrameUniforms &)uniforms;

/// Draws every sector with one instanced call: a 4-vertex triangle strip x `count`.
- (void)drawSunburstWithEncoder:(id<MTLRenderCommandEncoder>)encoder
                      instances:(id<MTLBuffer>)instances
                          count:(NSUInteger)count
                       uniforms:(const diskprism::gpu::FrameUniforms &)uniforms;

/// Creates a texture that can be rendered to and read back (for snapshots and tests).
- (nullable id<MTLTexture>)newReadableRenderTargetWithWidth:(NSUInteger)width height:(NSUInteger)height;

/// Copies a render target's pixels (BGRA, 8 bits per channel, top row first) into
/// `outBytes`, which must hold `width * height * 4` bytes. Blocks until the GPU is done.
- (BOOL)readPixelsFromTexture:(id<MTLTexture>)texture intoBytes:(void *)outBytes;

@end

NS_ASSUME_NONNULL_END
