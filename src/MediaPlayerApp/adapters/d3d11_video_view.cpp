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
#include <QTimer>
#include <mutex>
#include <cstring>
#include <algorithm>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

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

    UINT width = 0;
    UINT height = 0;
    bool shadersReady = false;

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

        safeRelease(vsBlob);
        safeRelease(psNv12Blob);
        safeRelease(psBgraBlob);
        shadersReady = vs && psNv12 && psBgra && sampler;
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

    void drawTexture(ID3D11Texture2D* texture, int slice)
    {
        if (!texture || !context || !rtv || !compileShaders()) return;

        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);

        const bool isNv12 = desc.Format == DXGI_FORMAT_NV12;
        const bool isBgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                            desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2D.MipLevels = 1;
        if (desc.ArraySize > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(slice);
            srvDesc.Texture2DArray.ArraySize = 1;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            (void)slice;
        }

        ID3D11ShaderResourceView* srv0 = nullptr;
        ID3D11ShaderResourceView* srv1 = nullptr;

        if (isNv12) {
            srvDesc.Format = DXGI_FORMAT_R8_UNORM;
            device->CreateShaderResourceView(texture, &srvDesc, &srv0);
            srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
            device->CreateShaderResourceView(texture, &srvDesc, &srv1);
        } else if (isBgra) {
            srvDesc.Format = desc.Format;
            device->CreateShaderResourceView(texture, &srvDesc, &srv0);
        } else {
            return;
        }

        const float clear[4] = {0.f, 0.f, 0.f, 1.f};
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        if (isNv12) {
            context->PSSetShader(psNv12, nullptr, 0);
            context->PSSetShaderResources(0, 1, &srv0);
            context->PSSetShaderResources(1, 1, &srv1);
        } else {
            context->PSSetShader(psBgra, nullptr, 0);
            context->PSSetShaderResources(0, 1, &srv0);
        }
        context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, nullSrv);
        swapChain->Present(1, 0);

        safeRelease(srv0);
        safeRelease(srv1);
    }
};

D3d11VideoViewHost::D3d11VideoViewHost(QWidget* parent)
    : QWidget(parent)
    , impl_(std::make_unique<Impl>())
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setStyleSheet(QStringLiteral("background:#000;"));
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

    ID3D11Device* texDevice = nullptr;
    tex->GetDevice(&texDevice);
    if (!texDevice) return;

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT w = static_cast<UINT>(std::max(width(), 1));
    const UINT h = static_cast<UINT>(std::max(height(), 1));

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->swapChain || impl_->device != texDevice || impl_->width != w || impl_->height != h) {
            impl_->createSwapChain(hwnd, texDevice, w, h);
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
        impl_->drawTexture(tex, slice);
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
