// Draws the command menu on top of the game. Hooks the swap chain's Present
// calls through the vtable and renders with its own small D3D11 pipeline, so
// nothing here depends on the game's layout system.
//
// The game presents from a D3D12 swap chain, so the D3D11 pipeline normally
// runs on a D3D11On12 device that shares the game's command queue. A plain
// D3D11 swap chain is handled too, which is what the test harness can use.
#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct Vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct Glyph {
    float u0, v0, u1, v1;
    float w;   // pixels at kFontPx
};

constexpr int kAtlas = 512;
constexpr int kFontPx = 40;
constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr size_t kMaxVerts = 4096;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

PresentFn g_realPresent = nullptr;
Present1Fn g_realPresent1 = nullptr;
OverlayState g_state;
void (*g_log)(const char* fmt, ...) = nullptr;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11On12Device* g_on12 = nullptr;   // set when the swap chain is D3D12
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_vb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11ShaderResourceView* g_font = nullptr;
IDXGISwapChain* g_target = nullptr;
bool g_ready = false;
bool g_failed = false;

Glyph g_glyphs[kLastChar - kFirstChar + 1];
float g_whiteU = 0, g_whiteV = 0;   // a solid texel in the atlas, for flat quads
std::vector<Vertex> g_verts;
float g_w = 1, g_h = 1;

const char* kShader = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
VSOut vs(VSIn i) { VSOut o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.col = i.col; return o; }
float4 ps(VSOut i) : SV_TARGET { return float4(i.col.rgb, i.col.a * tex.Sample(smp, i.uv).a); }
)";

bool step(const char* what, HRESULT hr) {
    if (SUCCEEDED(hr)) return true;
    if (g_log) g_log("overlay: %s failed (%08X)", what, hr);
    return false;
}

// White text on a transparent atlas, drawn with GDI once at startup.
bool build_font() {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kAtlas;
    bi.bmiHeader.biHeight = -kAtlas;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return false; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(kFontPx, 0, 0, 0, FW_BOLD, TRUE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    memset(bits, 0, kAtlas * kAtlas * 4);

    int x = 2, y = 2;
    const int pad = 4;
    for (int c = kFirstChar; c <= kLastChar; ++c) {
        char ch = static_cast<char>(c);
        SIZE sz{};
        GetTextExtentPoint32A(dc, &ch, 1, &sz);
        int cw = sz.cx + pad;   // room for the italic overhang
        if (x + cw + 8 > kAtlas) { x = 2; y += kFontPx + 4; }
        TextOutA(dc, x, y, &ch, 1);
        Glyph& g = g_glyphs[c - kFirstChar];
        g.u0 = static_cast<float>(x) / kAtlas;
        g.v0 = static_cast<float>(y) / kAtlas;
        g.u1 = static_cast<float>(x + cw) / kAtlas;
        g.v1 = static_cast<float>(y + kFontPx) / kAtlas;
        g.w = static_cast<float>(sz.cx);
        x += cw + 8;
    }
    GdiFlush();

    // GDI leaves alpha at zero: take coverage from a colour channel, and keep a
    // solid block in the bottom right corner for untextured quads.
    std::vector<uint32_t> px(kAtlas * kAtlas);
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    for (int i = 0; i < kAtlas * kAtlas; ++i) px[i] = ((src[i] & 0xFF) << 24) | 0x00FFFFFF;
    for (int yy = kAtlas - 8; yy < kAtlas; ++yy)
        for (int xx = kAtlas - 8; xx < kAtlas; ++xx) px[yy * kAtlas + xx] = 0xFFFFFFFF;
    g_whiteU = g_whiteV = (kAtlas - 4.0f) / kAtlas;

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kAtlas;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), kAtlas * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (!step("font texture", g_dev->CreateTexture2D(&td, &sd, &tex))) return false;
    HRESULT hr = g_dev->CreateShaderResourceView(tex, nullptr, &g_font);
    tex->Release();
    return step("font view", hr);
}

bool compile(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) {
        if (g_log) g_log("overlay: compiling %s failed (%08X) %s", entry, hr,
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        if (err) err->Release();
        return false;
    }
    return true;
}

void* read_ptr(const void* at) {
    __try {
        return *static_cast<void* const*>(at);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// DXGI will not hand back the command queue a D3D12 swap chain was created
// with, but the chain keeps a pointer to it. Every queue of a device has the
// same vtable, so make one of our own and look through the chain object for a
// pointer to something with that vtable.
ID3D12CommandQueue* find_queue(IDXGISwapChain* sc, ID3D12Device* dev12) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* probe = nullptr;
    if (FAILED(dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&probe))))
        return nullptr;
    void* vtbl = *reinterpret_cast<void**>(probe);
    probe->Release();

    const uint8_t* obj = reinterpret_cast<const uint8_t*>(sc);
    for (size_t off = 0; off < 0x1000; off += sizeof(void*)) {
        void* cand = read_ptr(obj + off);
        if (!cand || read_ptr(cand) != vtbl) continue;
        ID3D12CommandQueue* queue = nullptr;
        if (FAILED(static_cast<IUnknown*>(cand)->QueryInterface(__uuidof(ID3D12CommandQueue),
                                                                reinterpret_cast<void**>(&queue))))
            continue;
        if (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            if (g_log) g_log("overlay: command queue %p at swap chain +0x%X", queue, static_cast<unsigned>(off));
            return queue;
        }
        queue->Release();
    }
    return nullptr;
}

// Finds the D3D11 device to draw with: the swap chain's own if it has one,
// otherwise a D3D11On12 device on top of the D3D12 queue the chain presents from.
bool acquire_device(IDXGISwapChain* sc) {
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev)))) {
        g_dev->GetImmediateContext(&g_ctx);
        if (g_log) g_log("overlay: swap chain is d3d11");
        return true;
    }

    ID3D12Device* dev12 = nullptr;
    if (!step("GetDevice(d3d12)", sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev12))))
        return false;
    ID3D12CommandQueue* queue = find_queue(sc, dev12);
    if (!queue) {
        if (g_log) g_log("overlay: could not find the swap chain's command queue");
        dev12->Release();
        return false;
    }
    IUnknown* queues[1] = {queue};
    HRESULT hr = D3D11On12CreateDevice(dev12, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &g_dev,
                                       &g_ctx, nullptr);
    dev12->Release();
    queue->Release();
    if (!step("D3D11On12CreateDevice", hr)) return false;
    if (!step("ID3D11On12Device", g_dev->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&g_on12))))
        return false;
    if (g_log) g_log("overlay: swap chain is d3d12, drawing through d3d11on12");
    return true;
}

bool init(IDXGISwapChain* sc) {
    if (!acquire_device(sc)) return false;

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    if (!compile("vs", "vs_4_0", &vsb)) return false;
    if (!compile("ps", "ps_4_0", &psb)) return false;
    if (!step("CreateVertexShader",
              g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)))
        return false;
    if (!step("CreatePixelShader",
              g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps)))
        return false;
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (!step("CreateInputLayout",
              g_dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout)))
        return false;
    vsb->Release();
    psb->Release();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(kMaxVerts * sizeof(Vertex));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!step("CreateBuffer", g_dev->CreateBuffer(&bd, nullptr, &g_vb))) return false;

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!step("CreateBlendState", g_dev->CreateBlendState(&bl, &g_blend))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (!step("CreateRasterizerState", g_dev->CreateRasterizerState(&rd, &g_raster))) return false;

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    if (!step("CreateDepthStencilState", g_dev->CreateDepthStencilState(&dd, &g_depth))) return false;

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (!step("CreateSamplerState", g_dev->CreateSamplerState(&smp, &g_sampler))) return false;

    if (!build_font()) {
        if (g_log) g_log("overlay: building the font atlas failed");
        return false;
    }
    return true;
}

// --- drawing, coordinates in backbuffer pixels ---

void vert(float x, float y, float u, float v, const float c[4]) {
    if (g_verts.size() >= kMaxVerts) return;
    g_verts.push_back({x / g_w * 2 - 1, 1 - y / g_h * 2, u, v, c[0], c[1], c[2], c[3]});
}

// Quad with separate colours on the left and right edge, and a horizontal skew
// so the bars can lean like the KH2 menu.
void quad(float x, float y, float w, float h, const float left[4], const float right[4], float skew = 0,
          float u0 = -1, float v0 = 0, float u1 = 0, float v1 = 0) {
    if (u0 < 0) { u0 = u1 = g_whiteU; v0 = v1 = g_whiteV; }
    vert(x + skew, y, u0, v0, left);
    vert(x + w + skew, y, u1, v0, right);
    vert(x, y + h, u0, v1, left);
    vert(x + w + skew, y, u1, v0, right);
    vert(x + w, y + h, u1, v1, right);
    vert(x, y + h, u0, v1, left);
}

void text(float x, float y, float px, const char* s, const float col[4]) {
    float k = px / kFontPx;
    for (; *s; ++s) {
        int c = static_cast<unsigned char>(*s);
        if (c < kFirstChar || c > kLastChar) c = '?';
        const Glyph& g = g_glyphs[c - kFirstChar];
        float gw = (g.u1 - g.u0) * kAtlas * k;
        quad(x, y, gw, px, col, col, 0, g.u0, g.v0, g.u1, g.v1);
        x += g.w * k;
    }
}

void draw_menu() {
    static const char* kItems[4] = {"Attack", "Magic", "Items", "Summon"};
    const int kRows = 4;
    const float s = g_h / 1080.0f;
    const float rowH = 46 * s, rowW = 300 * s, gap = 4 * s;
    const float x0 = 48 * s;
    const float y0 = g_h - 60 * s - kRows * (rowH + gap);

    const float headL[4] = {0.05f, 0.10f, 0.32f, 0.92f};
    const float headR[4] = {0.05f, 0.10f, 0.32f, 0.0f};
    const float rowL[4]  = {0.07f, 0.16f, 0.45f, 0.88f};
    const float rowR[4]  = {0.07f, 0.16f, 0.45f, 0.25f};
    const float selL[4]  = {0.20f, 0.42f, 0.95f, 0.95f};
    const float selR[4]  = {0.20f, 0.42f, 0.95f, 0.45f};
    const float edge[4]  = {0.75f, 0.85f, 1.00f, 0.95f};
    const float gold[4]  = {1.00f, 0.86f, 0.25f, 1.00f};
    const float white[4] = {1, 1, 1, 1};
    const float dim[4]   = {0.80f, 0.86f, 1.00f, 0.90f};
    const float faint[4] = {0.80f, 0.86f, 1.00f, 0.45f};
    const float shadow[4] = {0, 0, 0, 0.6f};

    // copy once: the game thread can change this while we draw
    const OverlayState st = g_state;
    const bool sub = st.subOpen;

    quad(x0, y0 - 30 * s, 190 * s, 26 * s, headL, headR, 8 * s);
    text(x0 + 14 * s, y0 - 30 * s, 24 * s, sub ? st.subTitle : "COMMAND", dim);

    // the submenu shows a window of kRows entries around the selection
    int first = 0;
    if (sub && st.subSelected >= kRows) first = st.subSelected - kRows + 1;

    for (int i = 0; i < kRows; ++i) {
        const char* label;
        bool sel;
        if (sub) {
            int e = first + i;
            if (e >= st.subCount) {
                if (st.subCount == 0 && i == 0) {
                    float y = y0;
                    quad(x0, y, rowW, rowH, rowL, rowR, 10 * s);
                    text(x0 + 30 * s, y + 5 * s, 36 * s, "Nothing", faint);
                }
                continue;
            }
            label = st.subNames[e] ? st.subNames[e] : "?";
            sel = e == st.subSelected;
        } else {
            label = kItems[i];
            sel = i == st.selected;
        }
        float x = x0 + (sel ? 18 * s : 0);
        float y = y0 + i * (rowH + gap);
        quad(x, y, rowW, rowH, sel ? selL : rowL, sel ? selR : rowR, 10 * s);
        quad(x, y, 6 * s, rowH, sel ? gold : edge, sel ? gold : edge, 10 * s);
        text(x + 30 * s + 2 * s, y + 5 * s + 2 * s, 36 * s, label, shadow);
        text(x + 30 * s, y + 5 * s, 36 * s, label, sel ? white : dim);
    }
}

void bind_and_draw(ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp{0, 0, g_w, g_h, 0, 1};
    const FLOAT factor[4] = {0, 0, 0, 0};
    UINT stride = sizeof(Vertex), offset = 0;
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetBlendState(g_blend, factor, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_depth, 0);
    g_ctx->RSSetState(g_raster);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_font);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    g_ctx->Draw(static_cast<UINT>(g_verts.size()), 0);
}

bool fill_vertices(UINT width, UINT height) {
    g_w = static_cast<float>(width);
    g_h = static_cast<float>(height);
    g_verts.clear();
    draw_menu();
    if (g_verts.empty()) return false;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    memcpy(map.pData, g_verts.data(), g_verts.size() * sizeof(Vertex));
    g_ctx->Unmap(g_vb, 0);
    return true;
}

// The swap chain belongs to the game's own D3D11 device: put back every piece
// of state we touch.
void render_d3d11(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return;
    D3D11_TEXTURE2D_DESC desc;
    back->GetDesc(&desc);
    ID3D11RenderTargetView* rtv = nullptr;
    HRESULT hr = g_dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr)) return;
    if (!fill_vertices(desc.Width, desc.Height)) { rtv->Release(); return; }

    ID3D11RenderTargetView* oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oDsv = nullptr;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    g_ctx->RSGetViewports(&nvp, oVp);
    ID3D11BlendState* oBlend = nullptr; FLOAT oFactor[4]; UINT oMask;
    g_ctx->OMGetBlendState(&oBlend, oFactor, &oMask);
    ID3D11DepthStencilState* oDepth = nullptr; UINT oRef;
    g_ctx->OMGetDepthStencilState(&oDepth, &oRef);
    ID3D11RasterizerState* oRaster = nullptr;
    g_ctx->RSGetState(&oRaster);
    ID3D11InputLayout* oLayout = nullptr;
    g_ctx->IAGetInputLayout(&oLayout);
    D3D11_PRIMITIVE_TOPOLOGY oTopo;
    g_ctx->IAGetPrimitiveTopology(&oTopo);
    ID3D11Buffer* oVb = nullptr; UINT oStride, oOffset;
    g_ctx->IAGetVertexBuffers(0, 1, &oVb, &oStride, &oOffset);
    ID3D11VertexShader* oVs = nullptr; ID3D11PixelShader* oPs = nullptr; ID3D11GeometryShader* oGs = nullptr;
    g_ctx->VSGetShader(&oVs, nullptr, nullptr);
    g_ctx->PSGetShader(&oPs, nullptr, nullptr);
    g_ctx->GSGetShader(&oGs, nullptr, nullptr);
    ID3D11ShaderResourceView* oSrv = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &oSrv);
    ID3D11SamplerState* oSmp = nullptr;
    g_ctx->PSGetSamplers(0, 1, &oSmp);

    bind_and_draw(rtv);

    g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
    g_ctx->RSSetViewports(nvp, oVp);
    g_ctx->OMSetBlendState(oBlend, oFactor, oMask);
    g_ctx->OMSetDepthStencilState(oDepth, oRef);
    g_ctx->RSSetState(oRaster);
    g_ctx->IASetInputLayout(oLayout);
    g_ctx->IASetPrimitiveTopology(oTopo);
    g_ctx->IASetVertexBuffers(0, 1, &oVb, &oStride, &oOffset);
    g_ctx->VSSetShader(oVs, nullptr, 0);
    g_ctx->GSSetShader(oGs, nullptr, 0);
    g_ctx->PSSetShader(oPs, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &oSrv);
    g_ctx->PSSetSamplers(0, 1, &oSmp);

    for (auto* p : oRtv) if (p) p->Release();
    if (oDsv) oDsv->Release();
    if (oBlend) oBlend->Release();
    if (oDepth) oDepth->Release();
    if (oRaster) oRaster->Release();
    if (oLayout) oLayout->Release();
    if (oVb) oVb->Release();
    if (oVs) oVs->Release();
    if (oPs) oPs->Release();
    if (oGs) oGs->Release();
    if (oSrv) oSrv->Release();
    if (oSmp) oSmp->Release();
    rtv->Release();
}

// D3D12 swap chain: wrap the current back buffer for our D3D11On12 device,
// draw, and hand it back in the present state. Nothing is kept between frames
// so the game can still resize its buffers.
void render_d3d12(IDXGISwapChain* sc) {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3)))) return;
    UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();

    ID3D12Resource* back12 = nullptr;
    if (FAILED(sc->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back12)))) return;
    D3D12_RESOURCE_DESC desc = back12->GetDesc();

    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Resource* wrapped = nullptr;
    HRESULT hr = g_on12->CreateWrappedResource(back12, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                               D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource),
                                               reinterpret_cast<void**>(&wrapped));
    back12->Release();
    if (FAILED(hr)) {
        static bool once = false;
        if (!once && g_log) g_log("overlay: CreateWrappedResource failed (%08X)", hr);
        once = true;
        return;
    }

    g_on12->AcquireWrappedResources(&wrapped, 1);
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_dev->CreateRenderTargetView(wrapped, nullptr, &rtv))) {
        if (fill_vertices(static_cast<UINT>(desc.Width), desc.Height)) bind_and_draw(rtv);
        ID3D11RenderTargetView* none = nullptr;
        g_ctx->OMSetRenderTargets(1, &none, nullptr);
        rtv->Release();
    }
    g_on12->ReleaseWrappedResources(&wrapped, 1);
    wrapped->Release();
    g_ctx->Flush();
}

void render(IDXGISwapChain* sc) {
    if (g_on12) render_d3d12(sc);
    else render_d3d11(sc);
}

void on_present(IDXGISwapChain* sc, UINT flags) {
    if (g_failed || !g_state.visible || (flags & DXGI_PRESENT_TEST)) return;
    if (g_target && g_target != sc) return;
    __try {
        if (!g_ready) {
            DXGI_SWAP_CHAIN_DESC d{};
            sc->GetDesc(&d);
            if (g_log) g_log("overlay: swap chain %p, %ux%u, window %p", sc, d.BufferDesc.Width, d.BufferDesc.Height, d.OutputWindow);
            g_ready = init(sc);
            if (g_ready) g_target = sc;
            else g_failed = true;
            if (g_log) g_log(g_ready ? "overlay: ready" : "overlay: init failed");
        }
        if (g_ready) render(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
        if (g_log) g_log("overlay: exception %08X, disabled", GetExceptionCode());
    }
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    on_present(sc, flags);
    return g_realPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* params) {
    on_present(sc, flags);
    return g_realPresent1(sc, sync, flags, params);
}

bool patch_slot(void** slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

}  // namespace

OverlayState& overlay_state() { return g_state; }

bool overlay_draw_now(IDXGISwapChain* sc, void (*log)(const char* fmt, ...)) {
    g_log = log;
    if (!g_ready) g_ready = init(sc);
    if (g_ready) render(sc);
    return g_ready;
}

// Makes a throwaway device and swap chain just to find the Present slots, then
// swaps the pointers in the vtable every swap chain of that class shares.
bool overlay_install(void (*log)(const char* fmt, ...)) {
    g_log = log;
    HWND wnd = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
    if (!wnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        DestroyWindow(wnd);
        if (log) log("overlay: could not create a probe swap chain (%08X)", hr);
        return false;
    }

    // Present is slot 8 of IDXGISwapChain, Present1 slot 22 of IDXGISwapChain1.
    void** vtbl = *reinterpret_cast<void***>(sc);
    g_realPresent = reinterpret_cast<PresentFn>(vtbl[8]);
    bool ok = patch_slot(&vtbl[8], reinterpret_cast<void*>(&present_hook));

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
        void** vtbl1 = *reinterpret_cast<void***>(sc1);
        g_realPresent1 = reinterpret_cast<Present1Fn>(vtbl1[22]);
        patch_slot(&vtbl1[22], reinterpret_cast<void*>(&present1_hook));
        sc1->Release();
    }

    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(wnd);
    if (log) log(ok ? "overlay: present hooked" : "overlay: could not patch the vtable");
    return ok;
}
