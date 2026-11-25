#import "DiskMapRenderer.h"

#include <chrono>

using diskprism::gpu::FrameUniforms;

NSErrorDomain const DPRendererErrorDomain = @"com.berkayceylan.diskprism.renderer";

namespace {

/// The compiler's message, or a placeholder when Metal did not provide one.
NSString *diagnosticsOf(NSError *_Nullable error) {
    NSString *text = error.localizedDescription;
    return text.length > 0 ? text : @"(no diagnostics)";
}

NSError *makeError(DPRendererError code, NSString *description, NSError *_Nullable underlying) {
    NSMutableDictionary<NSErrorUserInfoKey, id> *info = [NSMutableDictionary dictionary];
    info[NSLocalizedDescriptionKey] = description;
    if (underlying != nil) info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:DPRendererErrorDomain code:code userInfo:info];
}

}  // namespace

@implementation DPMetalRenderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    id<MTLLibrary> _library;
    id<MTLRenderPipelineState> _treemapPipeline;
    id<MTLRenderPipelineState> _sunburstPipeline;
    MTLRenderPipelineReflection *_treemapReflection;
    MTLRenderPipelineReflection *_sunburstReflection;
    MTLPixelFormat _pixelFormat;
    double _setupMilliseconds;
}

@synthesize device = _device;
@synthesize commandQueue = _commandQueue;
@synthesize library = _library;
@synthesize pixelFormat = _pixelFormat;
@synthesize treemapPipeline = _treemapPipeline;
@synthesize sunburstPipeline = _sunburstPipeline;
@synthesize treemapReflection = _treemapReflection;
@synthesize sunburstReflection = _sunburstReflection;
@synthesize setupMilliseconds = _setupMilliseconds;

+ (nullable NSString *)shaderSourceAtPath:(nullable NSString *)path error:(NSError **)error {
    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    if (path.length > 0) [candidates addObject:path];
    NSString *bundled = [NSBundle.mainBundle pathForResource:@"Shaders" ofType:@"metal"];
    if (bundled != nil) [candidates addObject:bundled];

    NSError *lastError = nil;
    for (NSString *candidate in candidates) {
        NSError *readError = nil;
        NSString *source = [NSString stringWithContentsOfFile:candidate encoding:NSUTF8StringEncoding error:&readError];
        if (source.length > 0) return source;
        lastError = readError;
    }
    if (error != nil) {
        NSString *tried = candidates.count > 0 ? [candidates componentsJoinedByString:@", "]
                                               : @"(no path given and no Shaders.metal in the bundle's Resources)";
        *error = makeError(DPRendererErrorShaderSourceMissing,
                           [NSString stringWithFormat:@"Shaders.metal could not be read. Tried: %@", tried], lastError);
    }
    return nil;
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                           shaderSource:(NSString *)source
                            pixelFormat:(MTLPixelFormat)pixelFormat
                                  error:(NSError **)error {
    self = [super init];
    if (self == nil) return nil;

    const auto start = std::chrono::steady_clock::now();
    _device = device;
    _pixelFormat = pixelFormat;
    _commandQueue = [device newCommandQueue];
    _commandQueue.label = @"DiskPrism";

    // Run-time compilation: the app carries the shader as text and never needs the
    // offline `metal` tool (which standalone Command Line Tools do not include).
    MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
    NSError *compileError = nil;
    _library = [device newLibraryWithSource:source options:options error:&compileError];
    if (_library == nil) {
        if (error != nil) {
            *error = makeError(DPRendererErrorShaderCompileFailed,
                               [NSString stringWithFormat:@"Shaders.metal failed to compile:\n%@", diagnosticsOf(compileError)],
                               compileError);
        }
        return nil;
    }
    _library.label = @"DiskPrism shaders";

    NSError *pipelineError = nil;
    MTLRenderPipelineReflection *reflection = nil;
    _treemapPipeline = [self pipelineWithVertex:@(diskprism::gpu::kTreemapVertexFunction)
                                       fragment:@(diskprism::gpu::kTreemapFragmentFunction)
                                          label:@"Treemap"
                                     reflection:&reflection
                                          error:&pipelineError];
    _treemapReflection = reflection;
    if (_treemapPipeline != nil) {
        reflection = nil;
        _sunburstPipeline = [self pipelineWithVertex:@(diskprism::gpu::kSunburstVertexFunction)
                                            fragment:@(diskprism::gpu::kSunburstFragmentFunction)
                                               label:@"Sunburst"
                                          reflection:&reflection
                                               error:&pipelineError];
        _sunburstReflection = reflection;
    }
    if (_treemapPipeline == nil || _sunburstPipeline == nil) {
        if (error != nil) *error = pipelineError;
        return nil;
    }

    _setupMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return self;
}

- (nullable id<MTLRenderPipelineState>)pipelineWithVertex:(NSString *)vertexName
                                                 fragment:(NSString *)fragmentName
                                                    label:(NSString *)label
                                               reflection:(MTLRenderPipelineReflection *_Nullable *_Nonnull)reflection
                                                    error:(NSError **)error {
    id<MTLFunction> vertexFunction = [_library newFunctionWithName:vertexName];
    id<MTLFunction> fragmentFunction = [_library newFunctionWithName:fragmentName];
    if (vertexFunction == nil || fragmentFunction == nil) {
        if (error != nil) {
            NSString *missing = vertexFunction == nil ? vertexName : fragmentName;
            *error = makeError(DPRendererErrorEntryPointMissing,
                               [NSString stringWithFormat:@"Shaders.metal does not define the function \"%@\". It defines: %@",
                                                          missing, [_library.functionNames componentsJoinedByString:@", "]],
                               nil);
        }
        return nil;
    }

    MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = [@"DiskPrism " stringByAppendingString:label];
    descriptor.vertexFunction = vertexFunction;
    descriptor.fragmentFunction = fragmentFunction;
    MTLRenderPipelineColorAttachmentDescriptor *color = descriptor.colorAttachments[0];
    color.pixelFormat = _pixelFormat;
    // Straight-alpha "over" blending: used for anti-aliased arc edges and for
    // cross-fading layers during transitions.
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    NSError *pipelineError = nil;
    id<MTLRenderPipelineState> state =
        [_device newRenderPipelineStateWithDescriptor:descriptor
                                              options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                           reflection:reflection
                                                error:&pipelineError];
    if (state == nil && error != nil) {
        *error = makeError(DPRendererErrorPipelineFailed,
                           [NSString stringWithFormat:@"The %@ render pipeline could not be created:\n%@", label,
                                                      diagnosticsOf(pipelineError)],
                           pipelineError);
    }
    return state;
}

- (nullable id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length label:(NSString *)label {
    if (length == 0 || bytes == nullptr) return nil;
    id<MTLBuffer> buffer = [_device newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
    buffer.label = label;
    return buffer;
}

- (void)drawWithPipeline:(id<MTLRenderPipelineState>)pipeline
                 encoder:(id<MTLRenderCommandEncoder>)encoder
               instances:(id<MTLBuffer>)instances
                   count:(NSUInteger)count
                uniforms:(const FrameUniforms &)uniforms {
    if (count == 0 || instances == nil) return;
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:instances offset:0 atIndex:diskprism::gpu::kBufferIndexInstances];
    [encoder setVertexBytes:&uniforms length:sizeof(FrameUniforms) atIndex:diskprism::gpu::kBufferIndexUniforms];
    [encoder setFragmentBytes:&uniforms length:sizeof(FrameUniforms) atIndex:diskprism::gpu::kBufferIndexUniforms];
    // One draw call for everything: the quad is generated in the vertex shader from
    // vertex_id, and all per-item data comes from the instance buffer.
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:count];
}

- (void)drawTreemapWithEncoder:(id<MTLRenderCommandEncoder>)encoder
                     instances:(id<MTLBuffer>)instances
                         count:(NSUInteger)count
                      uniforms:(const FrameUniforms &)uniforms {
    [self drawWithPipeline:_treemapPipeline encoder:encoder instances:instances count:count uniforms:uniforms];
}

- (void)drawSunburstWithEncoder:(id<MTLRenderCommandEncoder>)encoder
                      instances:(id<MTLBuffer>)instances
                          count:(NSUInteger)count
                       uniforms:(const FrameUniforms &)uniforms {
    [self drawWithPipeline:_sunburstPipeline encoder:encoder instances:instances count:count uniforms:uniforms];
}

- (nullable id<MTLTexture>)newReadableRenderTargetWithWidth:(NSUInteger)width height:(NSUInteger)height {
    if (width == 0 || height == 0) return nil;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:_pixelFormat
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // Unified-memory GPUs can read the texture in place; discrete GPUs need a managed
    // texture that is synchronised back to the CPU before it is read.
    descriptor.storageMode = _device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    id<MTLTexture> texture = [_device newTextureWithDescriptor:descriptor];
    texture.label = @"DiskPrism snapshot";
    return texture;
}

- (BOOL)readPixelsFromTexture:(id<MTLTexture>)texture intoBytes:(void *)outBytes {
    if (texture == nil || outBytes == nullptr) return NO;
    if (texture.storageMode == MTLStorageModeManaged) {
        id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit synchronizeResource:texture];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    }
    [texture getBytes:outBytes
          bytesPerRow:texture.width * 4
           fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height)
          mipmapLevel:0];
    return YES;
}

@end
