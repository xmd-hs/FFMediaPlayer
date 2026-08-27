#include "metal_video_view.h"

#ifdef Q_OS_MAC

#include <QResizeEvent>
#include <QShowEvent>
#include <mutex>

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <CoreVideo/CoreVideo.h>
#import <AppKit/AppKit.h>

namespace {

const char* kShaderSourceUtf8 = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertex_main(uint vid [[vertex_id]]) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    float2 uvs[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };
    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    out.uv = uvs[vid];
    return out;
}

fragment float4 fragment_bgra(VertexOut in [[stage_in]],
                              texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    return tex.sample(s, in.uv);
}

fragment float4 fragment_nv12(VertexOut in [[stage_in]],
                              texture2d<float> texY [[texture(0)]],
                              texture2d<float> texUV [[texture(1)]]) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    float y = texY.sample(s, in.uv).r;
    float2 uv = texUV.sample(s, in.uv).rg - float2(0.5, 0.5);
    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;
    return float4(r, g, b, 1.0);
}
)";

} // namespace

@interface FFMetalRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view;
- (void)updatePixelBuffer:(CVPixelBufferRef)buffer;
- (void)clearFrame;
@end

@implementation FFMetalRenderer {
    MTKView* _view;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipelineBgra;
    id<MTLRenderPipelineState> _pipelineNv12;
    CVMetalTextureCacheRef _textureCache;
    CVPixelBufferRef _pending;
    std::mutex* _mutex;
}

- (instancetype)initWithView:(MTKView*)view
{
    self = [super init];
    if (!self) return nil;
    _view = view;
    _device = view.device;
    _queue = [_device newCommandQueue];
    _pending = nullptr;
    _mutex = new std::mutex();

    CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, _device, nil, &_textureCache);

    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:kShaderSourceUtf8];
    id<MTLLibrary> library = [_device newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        NSLog(@"Metal shader compile failed: %@", error);
        return self;
    }
    id<MTLFunction> vertexFn = [library newFunctionWithName:@"vertex_main"];
    id<MTLFunction> bgraFn = [library newFunctionWithName:@"fragment_bgra"];
    id<MTLFunction> nv12Fn = [library newFunctionWithName:@"fragment_nv12"];

    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vertexFn;
    desc.colorAttachments[0].pixelFormat = view.colorPixelFormat;

    desc.fragmentFunction = bgraFn;
    _pipelineBgra = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    desc.fragmentFunction = nv12Fn;
    _pipelineNv12 = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    return self;
}

- (void)dealloc
{
    [self clearFrame];
    if (_textureCache) {
        CFRelease(_textureCache);
        _textureCache = nullptr;
    }
    delete _mutex;
    _mutex = nullptr;
}

- (void)updatePixelBuffer:(CVPixelBufferRef)buffer
{
    if (!buffer) return;
    CVBufferRetain(buffer);
    CVPixelBufferRef old = nullptr;
    {
        std::lock_guard<std::mutex> lock(*_mutex);
        old = _pending;
        _pending = buffer;
    }
    if (old) CVBufferRelease(old);
    dispatch_async(dispatch_get_main_queue(), ^{
        [_view setNeedsDisplay:YES];
        [_view draw];
    });
}

- (void)clearFrame
{
    CVPixelBufferRef old = nullptr;
    {
        std::lock_guard<std::mutex> lock(*_mutex);
        old = _pending;
        _pending = nullptr;
    }
    if (old) CVBufferRelease(old);
}

- (void)drawInMTKView:(MTKView*)view
{
    CVPixelBufferRef buffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(*_mutex);
        if (_pending) {
            buffer = _pending;
            CVBufferRetain(buffer);
        }
    }
    if (!buffer || !_queue || !_textureCache) {
        if (buffer) CVBufferRelease(buffer);
        return;
    }

    id<MTLCommandBuffer> cmd = [_queue commandBuffer];
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (!pass) {
        CVBufferRelease(buffer);
        return;
    }
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
    const OSType format = CVPixelBufferGetPixelFormatType(buffer);
    const size_t width = CVPixelBufferGetWidth(buffer);
    const size_t height = CVPixelBufferGetHeight(buffer);

    auto makeTexture = [&](size_t plane, MTLPixelFormat pixelFormat, size_t w, size_t h) -> id<MTLTexture> {
        CVMetalTextureRef cvTex = nullptr;
        if (CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault, _textureCache, buffer, nil, pixelFormat,
                w, h, plane, &cvTex) != kCVReturnSuccess || !cvTex) {
            return nil;
        }
        id<MTLTexture> tex = CVMetalTextureGetTexture(cvTex);
        CFRelease(cvTex);
        return tex;
    };

    if (format == kCVPixelFormatType_32BGRA) {
        id<MTLTexture> tex = makeTexture(0, MTLPixelFormatBGRA8Unorm, width, height);
        if (tex && _pipelineBgra) {
            [enc setRenderPipelineState:_pipelineBgra];
            [enc setFragmentTexture:tex atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    } else {
        id<MTLTexture> texY = makeTexture(0, MTLPixelFormatR8Unorm, width, height);
        id<MTLTexture> texUV = makeTexture(1, MTLPixelFormatRG8Unorm, width / 2, height / 2);
        if (texY && texUV && _pipelineNv12) {
            [enc setRenderPipelineState:_pipelineNv12];
            [enc setFragmentTexture:texY atIndex:0];
            [enc setFragmentTexture:texUV atIndex:1];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    }

    [enc endEncoding];
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (drawable) [cmd presentDrawable:drawable];
    [cmd commit];
    CVBufferRelease(buffer);
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    (void)size;
}
@end

struct MetalVideoViewHost::Native {
    MTKView* view = nil;
    FFMetalRenderer* renderer = nil;
};

MetalVideoViewHost::MetalVideoViewHost(QWidget* parent)
    : QWidget(parent)
    , native_(std::make_unique<Native>())
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral("background:#000;"));
}

MetalVideoViewHost::~MetalVideoViewHost()
{
    if (native_ && native_->renderer) {
        [native_->renderer clearFrame];
    }
    native_.reset();
}

void MetalVideoViewHost::syncNativeFrame()
{
    if (!native_ || native_->view) return;
    winId();
    NSView* parent = (__bridge NSView*)reinterpret_cast<void*>(winId());
    if (!parent) return;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;

    MTKView* view = [[MTKView alloc] initWithFrame:parent.bounds device:device];
    view.framebufferOnly = YES;
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.clearColor = MTLClearColorMake(0, 0, 0, 1);
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    view.paused = YES;
    view.enableSetNeedsDisplay = YES;

    FFMetalRenderer* renderer = [[FFMetalRenderer alloc] initWithView:view];
    view.delegate = renderer;
    [parent addSubview:view];

    native_->view = view;
    native_->renderer = renderer;
}

void MetalVideoViewHost::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    syncNativeFrame();
}

void MetalVideoViewHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    syncNativeFrame();
    if (native_ && native_->view) {
        NSView* parent = (__bridge NSView*)reinterpret_cast<void*>(winId());
        if (parent) native_->view.frame = parent.bounds;
    }
}

void MetalVideoViewHost::setPixelBuffer(void* cvPixelBufferRetainAlreadyHeld)
{
    syncNativeFrame();
    if (!native_ || !native_->renderer || !cvPixelBufferRetainAlreadyHeld) return;
    auto* buffer = static_cast<CVPixelBufferRef>(cvPixelBufferRetainAlreadyHeld);
    [native_->renderer updatePixelBuffer:buffer];
}

void MetalVideoViewHost::clearFrame()
{
    if (native_ && native_->renderer) [native_->renderer clearFrame];
}

#endif // Q_OS_MAC
