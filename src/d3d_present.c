/* Direct3D 11 display path (plain C, COM macros). Shaders are compiled at run time with D3DCompile. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>
#include "d3d_present.h"

static ID3D11Device *dev; static ID3D11DeviceContext *ctx; static IDXGISwapChain *sc; static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex; static ID3D11ShaderResourceView *srv; static ID3D11SamplerState *smp;
static ID3D11VertexShader *vsh; static ID3D11PixelShader *ps_plain, *ps_crt; static ID3D11Buffer *cbuf;
static int SW, SH, DH, BW, BH;
typedef struct { float srcW, srcH, dstW, dstH, scale, pad0, pad1, pad2; } CB;

static const char *HLSL =
"cbuffer CB : register(b0) { float2 srcSize; float2 dstSize; float scale; float3 pad; };\n"
"Texture2D tex : register(t0);\n"
"SamplerState lin : register(s0);\n"
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VSOut vs(uint id : SV_VertexID) {\n"
"  VSOut o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
/* sharp bilinear: nearest-like pixels with only the edges smoothed */
"float4 ps_plain(VSOut i) : SV_Target {\n"
"  float2 p = i.uv * srcSize; float2 pi = floor(p); float2 f = p - pi;\n"
"  float2 rr = 0.5 - 0.5 / scale; float2 cd = f - 0.5;\n"
"  float2 f2 = (cd - clamp(cd, -rr, rr)) * scale + 0.5;\n"
"  return float4(tex.SampleLevel(lin, (pi + f2) / srcSize, 0).rgb, 1); }\n"
"float3 L(int2 c) { c = clamp(c, int2(0, 0), int2(srcSize) - 1); return pow(tex.Load(int3(c, 0)).rgb, 2.4); }\n"
/* CRT: gaussian scanline beams that widen with brightness, RGB aperture grille, halation, linear-light blending */
"float4 ps_crt(VSOut i) : SV_Target {\n"
"  float2 s = i.uv * srcSize - 0.5;\n"
"  float iy = floor(s.y + 0.5); float dy = s.y - iy;\n"
"  float ix = floor(s.x); float fx = s.x - ix;\n"
"  float3 a = lerp(L(int2(ix, iy)), L(int2(ix + 1, iy)), fx);\n"
"  float2 uv0 = (float2(ix + fx, iy) + 0.5) / srcSize; float3 b = 0; float2 st = 2.0 / srcSize;\n"
"  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) b += tex.SampleLevel(lin, uv0 + float2(x, y) * st, 0).rgb;\n"
"  b = pow(b / 9.0, 2.4);\n"
"  float3 sg = 0.26 + 0.17 * a; float3 beam = exp(-(dy * dy) / (2.0 * sg * sg)) * 1.18;\n"
"  float3 rgb = a * beam + 0.22 * b;\n"
"  int ph = (int)i.pos.x % 3; float3 m = float3(ph == 0 ? 1.0 : 0.62, ph == 1 ? 1.0 : 0.62, ph == 2 ? 1.0 : 0.62);\n"
"  rgb *= m * 1.28;\n"
"  return float4(pow(saturate(rgb), 1.0 / 2.2), 1); }\n";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3DCompile(HLSL, strlen(HLSL), NULL, NULL, NULL, entry, target, 0, 0, &blob, &err);
    if (FAILED(hr)) { if (err) { OutputDebugStringA((const char *)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); } return NULL; }
    if (err) ID3D10Blob_Release(err);
    return blob;
}
static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return 0;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return SUCCEEDED(hr);
}

void d3d_shutdown(void) {
#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
    if (ctx) ID3D11DeviceContext_ClearState(ctx);
    REL(cbuf); REL(ps_crt); REL(ps_plain); REL(vsh); REL(smp); REL(srv); REL(tex); REL(rtv); REL(sc); REL(ctx); REL(dev);
#undef REL
}

int d3d_init(HWND hw, int srcw, int srch, int dish) {
    SW = srcw; SH = srch; DH = dish;
    RECT rc; GetClientRect(hw, &rc); BW = rc.right > 0 ? rc.right : 1; BH = rc.bottom > 0 ? rc.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2; sd.BufferDesc.Width = BW; sd.BufferDesc.Height = BH; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { d3d_shutdown(); return 0; }
    IDXGIFactory *fac = NULL; IDXGIDevice *dd = NULL;                                    /* we handle Alt+Enter ourselves (we do not) */
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dd))) {
        IDXGIAdapter *ad = NULL;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad))) { if (SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac))) { IDXGIFactory_MakeWindowAssociation(fac, hw, DXGI_MWA_NO_ALT_ENTER); IDXGIFactory_Release(fac); } IDXGIAdapter_Release(ad); }
        IDXGIDevice_Release(dd);
    }
    if (!make_rtv()) { d3d_shutdown(); return 0; }
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
    td.Width = SW; td.Height = SH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) { d3d_shutdown(); return 0; }
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) { d3d_shutdown(); return 0; }
    D3D11_SAMPLER_DESC sm; memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sm, &smp))) { d3d_shutdown(); return 0; }
    ID3DBlob *vb = compile("vs", "vs_4_0"), *pb = compile("ps_plain", "ps_4_0"), *cb2 = compile("ps_crt", "ps_4_0");
    int ok = vb && pb && cb2
        && SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vsh))
        && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps_plain))
        && SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(cb2), ID3D10Blob_GetBufferSize(cb2), NULL, &ps_crt));
    if (vb) ID3D10Blob_Release(vb); if (pb) ID3D10Blob_Release(pb); if (cb2) ID3D10Blob_Release(cb2);
    if (!ok) { d3d_shutdown(); return 0; }
    D3D11_BUFFER_DESC bd; memset(&bd, 0, sizeof bd); bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf))) { d3d_shutdown(); return 0; }
    return 1;
}

void d3d_resize(int w, int h) {
    if (!sc || w < 1 || h < 1) return;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
    if (FAILED(IDXGISwapChain_ResizeBuffers(sc, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) return;
    BW = w; BH = h; make_rtv();
}

void d3d_present(const uint32_t *fb, int mode) {
    if (!sc || !rtv) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        for (int y = 0; y < SH; y++) memcpy((char *)ms.pData + (size_t)y * ms.RowPitch, fb + (size_t)y * SW, (size_t)SW * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0);
    }
    int vw = BW, vh = BW * DH / SW; if (vh > BH) { vh = BH; vw = BH * SW / DH; }                 /* letterbox to the 320x240 shape */
    int vx = (BW - vw) / 2, vy = (BH - vh) / 2;
    CB cb = { (float)SW, (float)SH, (float)vw, (float)vh, (float)vh / (float)SH, 0, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &cb, 0, 0);
    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    D3D11_VIEWPORT vp = { (float)vx, (float)vy, (float)vw, (float)vh, 0.0f, 1.0f };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vsh, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, mode ? ps_crt : ps_plain, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    IDXGISwapChain_Present(sc, 1, 0);
}
