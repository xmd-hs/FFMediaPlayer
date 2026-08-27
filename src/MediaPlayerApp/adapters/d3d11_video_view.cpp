#include "d3d11_video_view.h"

#ifdef Q_OS_WIN

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <QResizeEvent>
#include <QShowEvent>
#include <QPaintEvent>
#include <QPaintEngine>
#include <QTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <atomic>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

void writeHwLog(const QString& message)
{
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    const QString line = QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             message);
    qInfo().noquote() << line;
    QFile file(QCoreApplication::applicationDirPath() + QStringLiteral("/ffplayer_hw.log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << line << '\n';
    }
}

QString hexValue(unsigned long value)
{
    return QStringLiteral("0x%1").arg(value, 0, 16);
}

const char* kShaderHlsl = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    float2 uv[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };
    VSOut o;
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv = uv[id];
    return o;
}

Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samp0 : register(s0);

float4 ps_nv12(VSOut i) : SV_Target {
    float y = texY.Sample(samp0, i.uv).r;
    float2 uv = texUV.Sample(samp0, i.uv).rg - float2(0.5, 0.5);
    float r = y + 1.5748 * uv.y;
    float g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    float b = y + 1.8556 * uv.x;
    return float4(r, g, b, 1.0);
}

Texture2D texBgra : register(t0);
float4 ps_bgra(VSOut i) : SV_Target {
    return texBgra.Sample(samp0, i.uv);
}
)";

template<typename T>
void safeRelease(T*& ptr)
{
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

} // namespace

struct D3d11VideoViewHost::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psNv12 = nullptr;
    ID3D11PixelShader* psBgra = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11ShaderResourceView* sampleSrv0 = nullptr;
    ID3D11ShaderResourceView* sampleSrv1 = nullptr;
    ID3D11Texture2D* srvTexture = nullptr;
    UINT srvSlice = 0;

    UINT width = 0;
    UINT height = 0;
    bool shadersReady = false;
    std::atomic_bool presentationAvailable{true};
    std::uint64_t receivedFrames = 0;
    std::uint64_t presentedFrames = 0;
    std::uint64_t failedFrames = 0;
    bool textureDescriptorLogged = false;
    bool zeroCopyLogged = false;

    ID3D11Texture2D* pendingTex = nullptr;
    int pendingSlice = 0;
    std::shared_ptr<void> pendingKeepAlive;
    std::mutex mutex;

    ~Impl() { reset(); }

    void reset()
    {
        safeRelease(pendingTex);
        pendingKeepAlive.reset();
        safeRelease(rtv);
        safeRelease(swapChain);
        safeRelease(sampler);
        safeRelease(rasterizer);
        safeRelease(sampleSrv1);
        safeRelease(sampleSrv0);
        safeRelease(srvTexture);
        safeRelease(psBgra);
        safeRelease(psNv12);
        safeRelease(vs);
        safeRelease(context);
        safeRelease(device);
        width = height = 0;
        shadersReady = false;
    }

    bool compileShaders()
    {
        if (shadersReady) return true;
        if (!device) return false;

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psNv12Blob = nullptr;
        ID3DBlob* psBgraBlob = nullptr;
        ID3DBlob* err = nullptr;

        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
        if (FAILED(D3DCompile(kShaderHlsl, strlen(kShaderHlsl), "ffplayer_vs", nullptr, nullptr,
                              "vs_main", "vs_5_0", flags, 0, &vsBlob, &err))) {
            safeRelease(err);
            return false;
        }
        if (FAILED(D3DCompile(kShaderHlsl, strlen(kShaderHlsl), "ffplayer_ps_nv12", nullptr, nullptr,
                              "ps_nv12", "ps_5_0", flags, 0, &psNv12Blob, &err))) {
            safeRelease(vsBlob);
            safeRelease(err);
            return false;
        }
        if (FAILED(D3DCompile(kShaderHlsl, strlen(kShaderHlsl), "ffplayer_ps_bgra", nullptr, nullptr,
                              "ps_bgra", "ps_5_0", flags, 0, &psBgraBlob, &err))) {
            safeRelease(vsBlob);
            safeRelease(psNv12Blob);
            safeRelease(err);
            return false;
        }

        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader(psNv12Blob->GetBufferPointer(), psNv12Blob->GetBufferSize(), nullptr, &psNv12);
        device->CreatePixelShader(psBgraBlob->GetBufferPointer(), psBgraBlob->GetBufferSize(), nullptr, &psBgra);

        D3D11_SAMPLER_DESC samp = {};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&samp, &sampler);

        D3D11_RASTERIZER_DESC rasterDesc = {};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rasterDesc, &rasterizer);

        safeRelease(vsBlob);
        safeRelease(psNv12Blob);
        safeRelease(psBgraBlob);
        shadersReady = vs && psNv12 && psBgra && sampler && rasterizer;
        return shadersReady;
    }

    bool createSwapChain(HWND hwnd, ID3D11Device* dev, UINT w, UINT h)
    {
        if (!dev || !hwnd || w == 0 || h == 0) return false;

        if (device != dev) {
            reset();
            device = dev;
            device->AddRef();
            device->GetImmediateContext(&context);
            if (context) {
                ID3D11Multithread* mt = nullptr;
                if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),
                                                        reinterpret_cast<void**>(&mt)))) {
                    mt->SetMultithreadProtected(TRUE);
                    mt->Release();
                }
            }
        } else {
            safeRelease(rtv);
            safeRelease(swapChain);
        }

        IDXGIDevice* dxgiDev = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))) {
            return false;
        }
        IDXGIAdapter* adapter = nullptr;
        dxgiDev->GetAdapter(&adapter);
        IDXGIFactory* factory = nullptr;
        adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));

        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount = 2;
        scd.BufferDesc.Width = w;
        scd.BufferDesc.Height = h;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferDesc.RefreshRate.Numerator = 60;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const HRESULT hr = factory->CreateSwapChain(device, &scd, &swapChain);
        safeRelease(factory);
        safeRelease(adapter);
        safeRelease(dxgiDev);
        if (FAILED(hr)) return false;

        ID3D11Texture2D* back = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
            return false;
        }
        device->CreateRenderTargetView(back, nullptr, &rtv);
        safeRelease(back);

        width = w;
        height = h;
        return rtv != nullptr;
    }

    bool drawTexture(ID3D11Texture2D* texture, int slice)
    {
        if (!texture || !context || !rtv || !compileShaders()) {
            presentationAvailable = false;
            writeHwLog(QStringLiteral("D3D11 present failed: renderer initialization unavailable"));
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        if (!textureDescriptorLogged) {
            textureDescriptorLogged = true;
            writeHwLog(QStringLiteral(
                "D3D11 decode texture: %1x%2 format=%3 array=%4 bind=%5 misc=%6")
                .arg(desc.Width).arg(desc.Height).arg(static_cast<unsigned>(desc.Format))
                .arg(desc.ArraySize).arg(hexValue(desc.BindFlags)).arg(hexValue(desc.MiscFlags)));
        }

        const bool isNv12 = desc.Format == DXGI_FORMAT_NV12;
        const bool isBgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                            desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;

        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            presentationAvailable = false;
            writeHwLog(QStringLiteral("D3D11 present failed: decode texture is not shader-readable"));
            return false;
        }
        if (!isNv12 && !isBgra) {
            presentationAvailable = false;
            writeHwLog(QStringLiteral("D3D11 present failed: unsupported DXGI format=%1")
                .arg(static_cast<unsigned>(desc.Format)));
            return false;
        }

        const UINT sourceSlice = desc.ArraySize > 1
            ? static_cast<UINT>(std::max(0, std::min(slice, static_cast<int>(desc.ArraySize) - 1)))
            : 0;
        if (srvTexture != texture || srvSlice != sourceSlice) {
            safeRelease(sampleSrv1);
            safeRelease(sampleSrv0);
            safeRelease(srvTexture);
            srvTexture = texture;
            srvTexture->AddRef();
            srvSlice = sourceSlice;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.ViewDimension = desc.ArraySize > 1
                ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
            if (desc.ArraySize > 1) {
                srvDesc.Texture2DArray.MostDetailedMip = 0;
                srvDesc.Texture2DArray.MipLevels = 1;
                srvDesc.Texture2DArray.FirstArraySlice = sourceSlice;
                srvDesc.Texture2DArray.ArraySize = 1;
            } else {
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;
            }
            if (isNv12) {
                srvDesc.Format = DXGI_FORMAT_R8_UNORM;
                device->CreateShaderResourceView(texture, &srvDesc, &sampleSrv0);
                srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
                device->CreateShaderResourceView(texture, &srvDesc, &sampleSrv1);
            } else {
                srvDesc.Format = desc.Format;
                device->CreateShaderResourceView(texture, &srvDesc, &sampleSrv0);
            }
            if (!zeroCopyLogged &&
                ((isNv12 && sampleSrv0 && sampleSrv1) || (isBgra && sampleSrv0))) {
                zeroCopyLogged = true;
                writeHwLog(QStringLiteral(
                    "D3D11 zero-copy verified: decoder texture sampled directly; "
                    "slice=%1 srv=1 gpu-copy=0")
                    .arg(sourceSlice));
            }
        }

        if ((isNv12 && (!sampleSrv0 || !sampleSrv1)) || (isBgra && !sampleSrv0)) {
            presentationAvailable = false;
            writeHwLog(QStringLiteral("D3D11 present failed: shader resource view creation"));
            return false;
        }

        const float clear[4] = {0.f, 0.f, 0.f, 1.f};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        if (isNv12) {
            context->PSSetShader(psNv12, nullptr, 0);
            context->PSSetShaderResources(0, 1, &sampleSrv0);
            context->PSSetShaderResources(1, 1, &sampleSrv1);
        } else {
            context->PSSetShader(psBgra, nullptr, 0);
            context->PSSetShaderResources(0, 1, &sampleSrv0);
        }
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, nullSrv);
        const HRESULT presentResult = swapChain->Present(1, 0);
        if (FAILED(presentResult)) {
            presentationAvailable = false;
            writeHwLog(QStringLiteral("D3D11 present failed: swap chain HRESULT=%1")
                .arg(hexValue(static_cast<unsigned long>(presentResult))));
            return false;
        }
        presentationAvailable = true;

        return true;
    }
};

D3d11VideoViewHost::D3d11VideoViewHost(QWidget* parent)
    : QWidget(parent)
    , impl_(std::make_unique<Impl>())
{
    QFile::remove(QCoreApplication::applicationDirPath() + QStringLiteral("/ffplayer_hw.log"));
    writeHwLog(QStringLiteral("D3D11 diagnostics started"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

D3d11VideoViewHost::~D3d11VideoViewHost() = default;

void D3d11VideoViewHost::ensureInitialized()
{
    winId();
}

void D3d11VideoViewHost::setDecodeTexture(void* texture, int subresourceIndex,
                                          std::shared_ptr<void> keepAlive)
{
    if (!texture) return;
    ensureInitialized();
    auto* tex = static_cast<ID3D11Texture2D*>(texture);
    ++impl_->receivedFrames;

    ID3D11Device* texDevice = nullptr;
    tex->GetDevice(&texDevice);
    if (!texDevice) return;

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT w = static_cast<UINT>(std::max(width(), 1));
    const UINT h = static_cast<UINT>(std::max(height(), 1));

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->swapChain || impl_->device != texDevice || impl_->width != w || impl_->height != h) {
            if (!impl_->createSwapChain(hwnd, texDevice, w, h)) {
                impl_->presentationAvailable = false;
            }
        }
        safeRelease(impl_->pendingTex);
        impl_->pendingKeepAlive = std::move(keepAlive);
        impl_->pendingTex = tex;
        impl_->pendingTex->AddRef();
        impl_->pendingSlice = subresourceIndex;
    }
    texDevice->Release();

    QTimer::singleShot(0, this, [this] { presentPending(); });
}

bool D3d11VideoViewHost::hardwarePresentationAvailable() const
{
    return impl_ && impl_->presentationAvailable.load();
}

QPaintEngine* D3d11VideoViewHost::paintEngine() const
{
    return nullptr;
}

void D3d11VideoViewHost::paintEvent(QPaintEvent* event)
{
    if (event) event->accept();
    presentPending();
}

void D3d11VideoViewHost::presentPending()
{
    ID3D11Texture2D* tex = nullptr;
    int slice = 0;
    std::shared_ptr<void> keepAlive;
    ID3D11Device* texDevice = nullptr;

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->pendingTex) return;
        tex = impl_->pendingTex;
        slice = impl_->pendingSlice;
        keepAlive = impl_->pendingKeepAlive;
        tex->GetDevice(&texDevice);
    }

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT w = static_cast<UINT>(std::max(width(), 1));
    const UINT h = static_cast<UINT>(std::max(height(), 1));

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (texDevice && (!impl_->swapChain || impl_->device != texDevice ||
                          impl_->width != w || impl_->height != h)) {
            impl_->createSwapChain(hwnd, texDevice, w, h);
        }
        if (impl_->drawTexture(tex, slice)) ++impl_->presentedFrames;
        else ++impl_->failedFrames;
        const std::uint64_t attempts = impl_->presentedFrames + impl_->failedFrames;
        if (attempts % 60 == 0 || impl_->failedFrames > 0) {
            const double successRate = attempts > 0
                ? 100.0 * static_cast<double>(impl_->presentedFrames) /
                    static_cast<double>(attempts)
                : 0.0;
            writeHwLog(QStringLiteral(
                "D3D11 stats: received=%1 presented=%2 failed=%3 success=%4%")
                .arg(impl_->receivedFrames).arg(impl_->presentedFrames)
                .arg(impl_->failedFrames).arg(successRate, 0, 'f', 2));
        }
    }

    if (texDevice) texDevice->Release();
    (void)keepAlive;
}

void D3d11VideoViewHost::clearFrame()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    safeRelease(impl_->pendingTex);
    impl_->pendingKeepAlive.reset();
    if (impl_->context && impl_->rtv) {
        const float clear[4] = {0.f, 0.f, 0.f, 1.f};
        impl_->context->ClearRenderTargetView(impl_->rtv, clear);
        if (impl_->swapChain) impl_->swapChain->Present(0, 0);
    }
}

void D3d11VideoViewHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT w = static_cast<UINT>(std::max(width(), 1));
    const UINT h = static_cast<UINT>(std::max(height(), 1));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->device && hwnd) {
        impl_->createSwapChain(hwnd, impl_->device, w, h);
    }
}

void D3d11VideoViewHost::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ensureInitialized();
}

#endif // Q_OS_WIN
