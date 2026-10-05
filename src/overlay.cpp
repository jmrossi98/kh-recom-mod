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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
    float mode;   // 1 = normal blend, 0 = additive
};

struct Glyph {
    float u0, v0, u1, v1;
    float w;   // pixels at kFontPx
};

constexpr int kAtlas = 512;
constexpr int kFontPx = 40;
constexpr int kFirstChar = 32;
constexpr int kLastChar = 126;
constexpr size_t kMaxVerts = 16384;

// KH2's gauge sheet. Sprite coordinates in the layout data are for the
// original 512x256 sheet; the remastered one is twice that.
constexpr int kKh2SheetW = 1024;
constexpr int kKh2SheetH = 512;
constexpr float kKh2Texel = 2.0f;
const float kPi = 3.14159265f;

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
ID3D11ShaderResourceView* g_face = nullptr;   // Sora's portrait, optional
ID3D11ShaderResourceView* g_kh2 = nullptr;    // KH2's gauge sheet, optional
ID3D11ShaderResourceView* g_cmd = nullptr;    // KH2's command menu sheet, optional
IDXGISwapChain* g_target = nullptr;
bool g_ready = false;
bool g_failed = false;

Glyph g_glyphs[kLastChar - kFirstChar + 1];
float g_whiteU = 0, g_whiteV = 0;   // a solid texel in the atlas, for flat quads
// Layers, drawn in this order, one texture each.
std::vector<Vertex> g_cmdVerts;    // KH2 command menu sheet
std::vector<Vertex> g_gaugeVerts;  // KH2 gauge sheet
std::vector<Vertex> g_faceVerts;   // portrait
std::vector<Vertex> g_verts;       // font atlas: text and flat shapes
std::vector<Vertex>* g_out = &g_verts;
float g_mode = 1;                  // blend mode for the vertices being added
unsigned g_frameNo = 0;            // counts presented frames, drives looping animations
float g_w = 1, g_h = 1;

const char* kShader = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float mode : MODE0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float mode : MODE0; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
VSOut vs(VSIn i) { VSOut o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.col = i.col; o.mode = i.mode; return o; }
// Premultiplied output: with the blend set to ONE / INV_SRC_ALPHA an alpha of
// zero leaves the colour to be added on, which is how additive sprites work.
float4 ps(VSOut i) : SV_TARGET {
    float4 t = tex.Sample(smp, i.uv);
    float a = saturate(i.col.a * t.a);
    return float4(i.col.rgb * t.rgb * a, a * i.mode);
}
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
    HFONT font = CreateFontA(kFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Georgia");
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

// Art is built from the player's own game files by tools_dev/make_assets.py
// and sits in an assets folder next to this module, as raw RGBA. Anything
// missing just falls back to plain shapes.
ID3D11ShaderResourceView* load_rgba(const char* name, int width, int height) {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&load_rgba), &self);
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') --n;
    if (n == 0 || n + 40 >= MAX_PATH) return nullptr;
    sprintf_s(path + n, MAX_PATH - n, "assets\\%s", name);

    std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        if (g_log) g_log("overlay: no %s", path);
        return nullptr;
    }
    size_t got = fread(px.data(), 1, px.size(), f);
    fclose(f);
    if (got != px.size()) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), static_cast<UINT>(width * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

void vert(float x, float y, float u, float v, const float c[4]);

// ---- KH2 sequence data ----
//
// A .sqd from a KH2 .2dd, flattened to a binary by tools_dev/make_assets.py.
// Sprites are rectangles on the sheet, sprite groups place a few of them
// around an origin, and an animation group is a list of timed animations that
// each move, scale, tint and fade one sprite group. Format and behaviour follow
// OpenKH's documentation of the file.

struct SeqSprite { float u0, v0, u1, v1; float col[4][4]; };   // corner colours: left, top, right, bottom
struct SeqPart { float x0, y0, x1, y1; int sprite; };
struct SeqRange { int first, count; };
struct SeqAnim {
    int flags, group, frame0, frame1, blend;
    float tx0, tx1, ty0, ty1, px0, px1, py0, py1, rz0, rz1;
    float s0, s1, sx0, sx1, sy0, sy1, bx0, bx1, by0, by1, bxn, byn;
    float c0[4], c1[4];
};
struct SeqAnimGroup { int first, count, noLoop, loopStart, loopEnd, last; };

struct Seq {
    std::vector<SeqSprite> sprites;
    std::vector<SeqPart> parts;
    std::vector<SeqRange> groups;
    std::vector<SeqAnim> anims;
    std::vector<SeqAnimGroup> animGroups;
};
Seq g_cmdSeq;

template <typename T>
bool read_vec(FILE* f, std::vector<T>& v, uint32_t n) {
    v.resize(n);
    return n == 0 || fread(v.data(), sizeof(T), n, f) == n;
}

bool load_seq(const char* name, Seq& seq) {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&load_seq), &self);
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') --n;
    if (n == 0 || n + 40 >= MAX_PATH) return false;
    sprintf_s(path + n, MAX_PATH - n, "assets\\%s", name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    uint32_t head[6] = {};
    bool ok = fread(head, sizeof(uint32_t), 6, f) == 6 && head[0] == 0x31514553 /* "SEQ1" */ &&
              read_vec(f, seq.sprites, head[1]) && read_vec(f, seq.parts, head[2]) &&
              read_vec(f, seq.groups, head[3]) && read_vec(f, seq.anims, head[4]) &&
              read_vec(f, seq.animGroups, head[5]);
    fclose(f);
    if (!ok && g_log) g_log("overlay: %s is not a sequence file this build understands", name);
    return ok;
}

namespace seqflag {
constexpr int kLinear = 0x1, kNoBounce = 0x10, kNoRotation = 0x20, kNoScale = 0x40, kFixedColor = 0x80;
constexpr int kNoColor = 0x400, kFixedTranslate = 0x1000, kNoPivot = 0x4000;
}

float mix(float t, float a, float b) { return a * (1 - t) + b * t; }

// Draws one animation group at a frame. (ox, oy) is the group's origin in
// pixels and unit the size of one KH2 unit. Frames past the end hold the last
// frame for one-shot groups and wrap for looping ones.
void seq_draw(const Seq& seq, int group, int frame, float ox, float oy, float unit, float alpha = 1) {
    if (group < 0 || group >= static_cast<int>(seq.animGroups.size())) return;
    const SeqAnimGroup& ag = seq.animGroups[group];
    if (ag.noLoop) {
        if (frame > ag.last) frame = ag.last;
    } else {
        int end = ag.loopEnd ? ag.loopEnd : ag.last;
        if (frame >= end) frame = end <= ag.loopStart ? ag.loopStart : ag.loopStart + (frame - ag.loopStart) % (end - ag.loopStart);
    }
    for (int i = 0; i < ag.count; ++i) {
        const SeqAnim& a = seq.anims[ag.first + i];
        if (frame < a.frame0 || frame > a.frame1) continue;
        float d = a.frame1 > a.frame0 ? static_cast<float>(frame - a.frame0) / (a.frame1 - a.frame0) : 0;
        float t = (a.flags & seqflag::kLinear) ? d : (sinf(d * kPi - kPi / 2) + 1) / 2;

        float x = (a.flags & seqflag::kFixedTranslate) ? a.tx0 : mix(t, a.tx0, a.tx1);
        float y = (a.flags & seqflag::kFixedTranslate) ? a.ty0 : mix(t, a.ty0, a.ty1);
        float sx = 1, sy = 1;
        if (!(a.flags & seqflag::kNoScale)) {
            float sc = mix(t, a.s0, a.s1);
            sx = sc * mix(t, a.sx0, a.sx1);
            sy = sc * mix(t, a.sy0, a.sy1);
        }
        float col[4] = {1, 1, 1, 1};
        if (!(a.flags & seqflag::kNoColor))
            for (int k = 0; k < 4; ++k) col[k] = (a.flags & seqflag::kFixedColor) ? a.c0[k] : mix(t, a.c0[k], a.c1[k]);
        col[3] *= alpha;
        float rz = (a.flags & seqflag::kNoRotation) ? 0 : mix(t, a.rz0, a.rz1);
        float pvx = 0, pvy = 0;
        if (!(a.flags & seqflag::kNoPivot)) {
            pvx = mix(t, a.px0, a.px1);
            pvy = mix(t, a.py0, a.py1);
        }
        if (!(a.flags & seqflag::kNoBounce)) {
            x += sinf(d * a.bxn * kPi) * mix(t, a.bx0, a.bx1);
            y += sinf(d * a.byn * kPi) * mix(t, a.by0, a.by1);
        }
        const float cs = cosf(-rz), sn = sinf(-rz);

        g_mode = a.blend == 1 ? 0.0f : 1.0f;
        const SeqRange& sg = seq.groups[a.group];
        for (int p = 0; p < sg.count; ++p) {
            const SeqPart& part = seq.parts[sg.first + p];
            const SeqSprite& sp = seq.sprites[part.sprite];
            // corners: top-left, top-right, bottom-left, bottom-right
            const float lx[4] = {part.x0, part.x1, part.x0, part.x1};
            const float ly[4] = {part.y0, part.y0, part.y1, part.y1};
            const float lu[4] = {sp.u0, sp.u1, sp.u0, sp.u1};
            const float lv[4] = {sp.v0, sp.v0, sp.v1, sp.v1};
            float px[4], py[4], pc[4][4];
            for (int k = 0; k < 4; ++k) {
                float vx = (lx[k] + pvx) * sx, vy = (ly[k] + pvy) * sy;
                px[k] = ox + (vx * cs - vy * sn + x) * unit;
                py[k] = oy + (vx * sn + vy * cs + y) * unit;
                for (int c = 0; c < 4; ++c) pc[k][c] = sp.col[k][c] * col[c];
            }
            const int order[6] = {0, 1, 2, 1, 3, 2};
            for (int k : order)
                vert(px[k], py[k], lu[k] * kKh2Texel / kKh2SheetW, lv[k] * kKh2Texel / kKh2SheetH, pc[k]);
        }
    }
    g_mode = 1;
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
        {"MODE", 0, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (!step("CreateInputLayout",
              g_dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout)))
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
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;   // premultiplied, see the shader
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
    g_face = load_rgba("face.rgba", 256, 256);
    g_kh2 = load_rgba("kh2_gauge.rgba", kKh2SheetW, kKh2SheetH);
    g_cmd = load_rgba("kh2_command.rgba", kKh2SheetW, kKh2SheetH);
    if (g_cmd && !load_seq("kh2_command.seq", g_cmdSeq)) {
        g_cmd->Release();
        g_cmd = nullptr;
    }
    return true;
}

// --- drawing, coordinates in backbuffer pixels ---

void vert(float x, float y, float u, float v, const float c[4]) {
    if (g_verts.size() + g_faceVerts.size() + g_gaugeVerts.size() + g_cmdVerts.size() >= kMaxVerts) return;
    g_out->push_back({x / g_w * 2 - 1, 1 - y / g_h * 2, u, v, c[0], c[1], c[2], c[3], g_mode});
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

void draw_menu_plain() {
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

// Ring segment between two angles (radians, 0 = right, clockwise on screen),
// inner colour on the inside edge and outer on the outside.
void arc(float cx, float cy, float r0, float r1, float a0, float a1, const float inner[4], const float outer[4]) {
    const int steps = 48;
    const float u = g_whiteU, v = g_whiteV;
    for (int i = 0; i < steps; ++i) {
        float t0 = a0 + (a1 - a0) * i / steps;
        float t1 = a0 + (a1 - a0) * (i + 1) / steps;
        float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
        vert(cx + r0 * c0, cy + r0 * s0, u, v, inner);
        vert(cx + r1 * c0, cy + r1 * s0, u, v, outer);
        vert(cx + r0 * c1, cy + r0 * s1, u, v, inner);
        vert(cx + r1 * c0, cy + r1 * s0, u, v, outer);
        vert(cx + r1 * c1, cy + r1 * s1, u, v, outer);
        vert(cx + r0 * c1, cy + r0 * s1, u, v, inner);
    }
}

void disc(float cx, float cy, float r, const float col[4]) { arc(cx, cy, 0, r, 0, 2 * kPi, col, col); }

void rect(float x0, float y0, float x1, float y1, const float top[4], const float bottom[4]) {
    const float u = g_whiteU, v = g_whiteV;
    vert(x0, y0, u, v, top);
    vert(x1, y0, u, v, top);
    vert(x0, y1, u, v, bottom);
    vert(x1, y0, u, v, top);
    vert(x1, y1, u, v, bottom);
    vert(x0, y1, u, v, bottom);
}

// One KH2 style gauge: a bar that runs in from the left under the portrait and
// then wraps up around its right side to the top. `fill` is 0..1 and empties
// from the far left end first. `grow` pushes every edge out by that much, which
// is how the outline is drawn: the same shape, a little bigger, underneath.
void gauge(float cx, float cy, float r0, float r1, float barLen, float fill, float grow, const float inner[4],
           const float outer[4]) {
    const float arcLen = kPi * (r0 + r1) / 2;
    const float filled = (arcLen + barLen) * fill;
    if (filled <= 0) return;
    // the outline's cap at the top of the ring is a small sweep past vertical
    const float capAngle = grow > 0 ? grow / ((r0 + r1) / 2) : 0;
    if (filled >= arcLen) {
        arc(cx, cy, r0 - grow, r1 + grow, -kPi / 2 - capAngle, kPi / 2, inner, outer);
        float w = filled - arcLen;
        if (w > 0) rect(cx - w - grow, cy + r0 - grow, cx, cy + r1 + grow, inner, outer);
    } else {
        arc(cx, cy, r0 - grow, r1 + grow, -kPi / 2 - capAngle, -kPi / 2 + kPi * filled / arcLen, inner, outer);
    }
}

// Status HUD in KH2's place, bottom right: portrait, HP wrapped around it and
// MP as a straight bar sitting on the HP bar. Everything shares one outline
// width, and shapes that touch share a single line between them.
void draw_status_plain() {
    const OverlayState st = g_state;
    const float s = g_h / 1080.0f;
    const float e = 3 * s;                       // outline width, used everywhere
    const float rimW = 3 * s;                    // light ring round the portrait
    const float cx = g_w - 150 * s, cy = g_h - 160 * s;
    const float rFace = 72 * s;
    const float rPortrait = rFace + rimW + e;    // portrait with its outline
    const float r0 = rPortrait, r1 = r0 + 20 * s;   // HP ring starts at the portrait's outline

    float hpMax = st.hpMax > 0 ? static_cast<float>(st.hpMax) : 1;
    float hpFill = st.hp <= 0 ? 0 : (st.hp >= st.hpMax ? 1 : st.hp / hpMax);
    float hpBar = hpMax * 2.2f;                  // longer bar for more max HP
    hpBar = (hpBar < 80 ? 80 : (hpBar > 560 ? 560 : hpBar)) * s;

    const float line[4]    = {0.03f, 0.04f, 0.09f, 1.0f};
    const float well[4]    = {0.13f, 0.14f, 0.18f, 1.0f};
    const float hpIn[4]    = {0.62f, 0.98f, 0.30f, 1.0f};
    const float hpOut[4]   = {0.13f, 0.60f, 0.10f, 1.0f};
    const float lowIn[4]   = {1.00f, 0.60f, 0.22f, 1.0f};
    const float lowOut[4]  = {0.78f, 0.13f, 0.10f, 1.0f};
    const float mpTop[4]   = {0.50f, 0.80f, 1.00f, 1.0f};
    const float mpBot[4]   = {0.10f, 0.30f, 0.90f, 1.0f};
    const float chTop[4]   = {1.00f, 0.62f, 0.86f, 1.0f};
    const float chBot[4]   = {0.78f, 0.14f, 0.54f, 1.0f};
    const float rim[4]     = {0.88f, 0.91f, 0.97f, 1.0f};
    const float faceBg[4]  = {0.05f, 0.06f, 0.10f, 1.0f};
    const float white[4]   = {1, 1, 1, 1};
    const float pink[4]    = {1.00f, 0.72f, 0.90f, 1.0f};
    const float shadow[4]  = {0, 0, 0, 0.6f};

    // MP sits directly on top of the HP bar: its bottom edge is the HP bar's
    // top outline. It runs right until it disappears behind the portrait.
    const float mh = 12 * s;
    const float my1 = cy + r0 - e, my0 = my1 - mh;
    const float mxL = cx - r0 - 170 * s;
    float dy = (my0 + my1) / 2 - cy;
    float reach = rPortrait * rPortrait - dy * dy;
    const float mxR = cx - (reach > 0 ? sqrtf(reach) : 0);   // where the portrait covers it
    float mpFill = st.mp < 0 ? 0 : (st.mp > 1 ? 1 : st.mp);

    // 1. outlines
    gauge(cx, cy, r0, r1, hpBar, 1, e, line, line);
    rect(mxL - e, my0 - e, cx, my1 + e, line, line);

    // 2. empty wells
    gauge(cx, cy, r0, r1, hpBar, 1, 0, well, well);
    rect(mxL, my0, cx, my1, well, well);

    // 3. fills
    bool low = hpFill <= 0.25f;
    gauge(cx, cy, r0, r1, hpBar, hpFill, 0, low ? lowIn : hpIn, low ? lowOut : hpOut);
    if (mpFill > 0)
        rect(mxR - (mxR - mxL) * mpFill, my0, cx, my1, st.mpCharging ? chTop : mpTop, st.mpCharging ? chBot : mpBot);

    // 4. portrait over the top, hiding where the MP bar runs in behind it
    disc(cx, cy, rPortrait, line);
    disc(cx, cy, rFace + rimW, rim);
    disc(cx, cy, rFace, faceBg);
    if (g_face) {
        g_out = &g_faceVerts;
        quad(cx - rFace, cy - rFace, 2 * rFace, 2 * rFace, white, white, 0, 0, 0, 1, 1);
        g_out = &g_verts;
    }

    if (st.mpCharging) {
        text(mxL + 2 * s, my0 - e - 28 * s + 2 * s, 24 * s, "MP CHARGE", shadow);
        text(mxL, my0 - e - 28 * s, 24 * s, "MP CHARGE", pink);
    }
}

// ---- KH2's own gauge art ----
//
// Everything below works in KH2's HUD units with the portrait's centre as the
// origin, the same way the game's layout data does, and the numbers are taken
// from that data (field2d gaug.sqd). The HP gauge is one ring quarter mirrored
// into three quadrants, open at the bottom left, and the bar is the ring's
// cross-section pulled out to the left from the bottom of the ring.

struct Hud {
    float cx, cy, unit;   // origin in pixels, pixels per KH2 unit
};

const float kRingR = 45;             // ring quarter is 45x45
const float kRingBandIn = 30;        // nothing but transparency inside this
const float kRingMidR = 39.5f;       // middle of the coloured band, for arc length
const float kUvCentreV = 105;        // v at the ring's centre
const float kUvGreenU = 95;          // u at the centre, green quarter
const float kUvDarkU = 46;           // u at the centre, dark quarter

void hud_vert(const Hud& h, float x, float y, float u, float v, const float c[4]) {
    vert(h.cx + x * h.unit, h.cy + y * h.unit, u * kKh2Texel / kKh2SheetW, v * kKh2Texel / kKh2SheetH, c);
}

// Sprite from the sheet, stretched over a rectangle given in HUD units. A zero
// width or height source is a one texel strip, sampled through its middle.
void hud_sprite(const Hud& h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                const float c[4]) {
    const float half = 0.5f / kKh2Texel;
    if (u0 == u1) u0 = u1 = u0 + half;
    if (v0 == v1) v0 = v1 = v0 + half;
    hud_vert(h, x0, y0, u0, v0, c);
    hud_vert(h, x1, y0, u1, v0, c);
    hud_vert(h, x0, y1, u0, v1, c);
    hud_vert(h, x1, y0, u1, v0, c);
    hud_vert(h, x1, y1, u1, v1, c);
    hud_vert(h, x0, y1, u0, v1, c);
}

// Part of the ring between two angles (degrees, 0 = right, clockwise on
// screen). Each point takes its texel from the mirrored quarter it falls in.
void hud_ring(const Hud& h, float centreU, float a0, float a1, const float c[4]) {
    if (a1 <= a0) return;
    const int steps = static_cast<int>((a1 - a0) / 3) + 1;
    for (int i = 0; i < steps; ++i) {
        float t0 = (a0 + (a1 - a0) * i / steps) * kPi / 180;
        float t1 = (a0 + (a1 - a0) * (i + 1) / steps) * kPi / 180;
        float pts[4][2] = {
            {kRingBandIn * cosf(t0), kRingBandIn * sinf(t0)}, {kRingR * 1.05f * cosf(t0), kRingR * 1.05f * sinf(t0)},
            {kRingBandIn * cosf(t1), kRingBandIn * sinf(t1)}, {kRingR * 1.05f * cosf(t1), kRingR * 1.05f * sinf(t1)},
        };
        const int order[6] = {0, 1, 2, 1, 3, 2};
        for (int k : order) {
            float x = pts[k][0], y = pts[k][1];
            float ax = fabsf(x) > kRingR ? kRingR : fabsf(x), ay = fabsf(y) > kRingR ? kRingR : fabsf(y);
            hud_vert(h, x, y, centreU - ax, kUvCentreV - ay, c);
        }
    }
}

void draw_status_kh2() {
    const OverlayState st = g_state;
    Hud h;
    h.unit = g_h / 448.0f;
    h.cx = g_w - 64 * h.unit;
    h.cy = g_h - 68 * h.unit;

    const float white[4] = {1, 1, 1, 1};
    const float black[4] = {0, 0, 0, 1};
    const float shade[4] = {0, 0, 0, 0.5f};
    const float red[4]   = {2.6f, 0.35f, 0.30f, 1};          // low HP backing
    const float mpWell[4]   = {0.031f, 0.086f, 0.656f, 1};   // tints straight from the layout data
    const float mpCharge[4] = {1.31f, 0.156f, 1.05f, 1};
    const float mpTag[4]    = {0.60f, 1.00f, 1.56f, 1};
    const float mpTagCh[4]  = {1.56f, 0.45f, 1.10f, 1};

    g_out = &g_gaugeVerts;

    // --- portrait backing first, so the gauges sit on top of its flourishes ---
    const float q = 29;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            hud_sprite(h, sx * q, sy * q, 0, 0, 127, 113, 156, 142, shade);
            hud_sprite(h, sx * q, sy * q, 0, 0, 159, 103, 188, 132, black);
        }
    hud_sprite(h, 19, -29, 57, -3, 149, 60, 187, 86, black);
    hud_sprite(h, -47, 12, -6, 28, 148, 86, 189, 102, black);

    // --- HP ---
    float hpMax = st.hpMax > 0 ? static_cast<float>(st.hpMax) : 1;
    float fill = st.hp <= 0 ? 0 : (st.hp >= st.hpMax ? 1 : st.hp / hpMax);
    float bar = hpMax * 0.9f;                                 // bar grows with max HP
    bar = bar < 30 ? 30 : (bar > 300 ? 300 : bar);
    const float ringLen = 1.5f * kPi * kRingMidR;             // three quarters of a circle
    const float filled = (ringLen + bar) * fill;
    const bool low = fill <= 0.25f;

    // backing: ring from 9 o'clock round over the top to 6 o'clock, then the bar
    const float* back = low ? red : white;
    hud_ring(h, kUvDarkU, 180, 450, back);
    hud_sprite(h, -bar, kRingR, 0, 0, kUvDarkU, 60, kUvDarkU, kUvCentreV, back);
    hud_sprite(h, -kRingR, 0, 0, 2, 189, 49, 234, 52, white);                 // closes the ring's open end
    hud_sprite(h, -bar - 1, 34, -bar, kRingR, 183, 49, 185, 59, black);       // bar's end cap

    // fill: anchored at the ring's open end, so the bar's far tip goes first
    if (filled > 0) {
        float ringPart = filled < ringLen ? filled : ringLen;
        hud_ring(h, kUvGreenU, 180, 180 + 270 * ringPart / ringLen, white);
        if (filled > ringLen)
            hud_sprite(h, -(filled - ringLen), kRingR, 0, 0, kUvGreenU, 60, kUvGreenU, kUvCentreV, white);
    }

    // --- MP: sits in the ring's open quadrant, just above the HP bar ---
    const float mpY = 31, mpRight = -42, mpLen = 100;
    float mp = st.mp < 0 ? 0 : (st.mp > 1 ? 1 : st.mp);
    hud_sprite(h, mpRight - mpLen - 1, mpY - 5, mpRight + 1, mpY + 5, 183, 49, 185, 59, mpWell);
    if (mp > 0) {
        if (st.mpCharging)
            hud_sprite(h, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 219, 40, 219, 46, mpCharge);
        else
            hud_sprite(h, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 216, 40, 216, 46, white);
    }
    hud_sprite(h, -40, mpY - 5, -16, mpY + 5, 157, 37, 181, 47, st.mpCharging ? mpTagCh : mpTag);

    g_out = &g_faceVerts;
    if (g_face) {
        const float r = 30 * h.unit;
        quad(h.cx - r, h.cy - r, 2 * r, 2 * r, white, white, 0, 0, 0, 1, 1);
    }
    g_out = &g_verts;
}

void draw_status() {
    if (g_kh2) draw_status_kh2();
    else draw_status_plain();
}

// ---- KH2's command menu ----
//
// Animation group numbers are from the Twilight Town sheet's comw.sqd; the
// other worlds' sheets use the same numbering with their own art.
namespace cmd {
constexpr int kHeaderField = 17, kHeaderBattle = 30;   // COMMANDS header, cyan and red
constexpr int kHeaderFrames = 36;            // length of the header's glow, which the game replays
constexpr int kSlot = 11;                    // plain slot
constexpr int kSlotField = 9, kSlotBattle = 22;        // selected slot, glowing
constexpr int kBottom = 6;                   // cap under the last slot
constexpr int kCursor = 20;
constexpr int kIcon[4] = {37, 41, 39, 44};   // keyblade, hat, potion, charm
constexpr int kSubHeader[4] = {-1, 60, 51, 73};      // MAGIC, ITEMS, SUMMONS
constexpr int kSubSlot = 62;                 // wide slot for the lists
constexpr int kSubSlotSel[4] = {-1, 61, 48, 74};
constexpr float kRowPitch = 22;
}

// White text with a dark edge, sized in KH2 units.
void menu_text(float x, float y, float px, const char* s, const float col[4]) {
    const float edge[4] = {0.05f, 0.04f, 0.03f, 0.9f};
    const float o = px * 0.07f;
    text(x - o, y, px, s, edge);
    text(x + o, y, px, s, edge);
    text(x, y - o, px, s, edge);
    text(x, y + o, px, s, edge);
    text(x, y, px, s, col);
}

void draw_menu_kh2() {
    static const char* kItems[4] = {"Attack", "Magic", "Items", "Summon"};
    const OverlayState st = g_state;
    const float u = g_h / 448.0f;
    const float ox = 27 * u;
    const float oy = g_h - (3 * cmd::kRowPitch + 15 + 24) * u;   // bottom slot sits a little off the edge
    const int f = static_cast<int>(g_frameNo);
    const float white[4] = {1, 1, 1, 1};
    const float dim[4] = {0.93f, 0.90f, 0.82f, 1};

    g_out = &g_cmdVerts;
    if (!st.subOpen) {
        // headers are authored one slot above the menu's origin
        seq_draw(g_cmdSeq, st.battle ? cmd::kHeaderBattle : cmd::kHeaderField, f % cmd::kHeaderFrames, ox,
                 oy - cmd::kRowPitch * u, u);
        seq_draw(g_cmdSeq, cmd::kBottom, f, ox, oy, u);
        for (int i = 0; i < 4; ++i) seq_draw(g_cmdSeq, cmd::kSlot, f, ox, oy + i * cmd::kRowPitch * u, u);
        float sy = oy + st.selected * cmd::kRowPitch * u;
        seq_draw(g_cmdSeq, st.battle ? cmd::kSlotBattle : cmd::kSlotField, f, ox, sy, u);
        for (int i = 0; i < 4; ++i) seq_draw(g_cmdSeq, cmd::kIcon[i], f, ox, oy + i * cmd::kRowPitch * u, u);
        seq_draw(g_cmdSeq, cmd::kCursor, f, ox, sy, u);
        g_out = &g_verts;
        for (int i = 0; i < 4; ++i)
            menu_text(ox + 4 * u, oy + (i * cmd::kRowPitch - 7.5f) * u, 14 * u, kItems[i], i == st.selected ? white : dim);
        return;
    }

    // A list: its own header, then up to four wide slots that scroll with the cursor.
    const int kind = st.selected >= 1 && st.selected <= 3 ? st.selected : 1;
    const int kRows = 4;
    int first = st.subSelected >= kRows ? st.subSelected - kRows + 1 : 0;
    int shown = st.subCount - first < kRows ? st.subCount - first : kRows;
    if (shown < 1) shown = 1;
    seq_draw(g_cmdSeq, cmd::kSubHeader[kind], f, ox, oy - cmd::kRowPitch * u, u);
    for (int i = 0; i < shown; ++i) {
        bool sel = first + i == st.subSelected && st.subCount > 0;
        seq_draw(g_cmdSeq, sel ? cmd::kSubSlotSel[kind] : cmd::kSubSlot, f, ox, oy + i * cmd::kRowPitch * u, u);
    }
    if (st.subCount > 0)
        seq_draw(g_cmdSeq, cmd::kCursor, f, ox - 8 * u, oy + (st.subSelected - first) * cmd::kRowPitch * u, u);
    g_out = &g_verts;
    if (st.subCount == 0) {
        menu_text(ox - 4 * u, oy - 7.5f * u, 14 * u, "Nothing", dim);
        return;
    }
    for (int i = 0; i < shown; ++i) {
        const char* label = st.subNames[first + i] ? st.subNames[first + i] : "?";
        menu_text(ox - 4 * u, oy + (i * cmd::kRowPitch - 7.5f) * u, 14 * u, label,
                  first + i == st.subSelected ? white : dim);
    }
}

void draw_menu() {
    if (!g_state.battle) return;   // outside battle only the status gauges show
    if (g_cmd) draw_menu_kh2();
    else draw_menu_plain();
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
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    // same order the vertex buffer was filled in
    UINT at = 0;
    struct Layer { std::vector<Vertex>* verts; ID3D11ShaderResourceView* tex; };
    const Layer layers[4] = {{&g_cmdVerts, g_cmd}, {&g_gaugeVerts, g_kh2}, {&g_faceVerts, g_face}, {&g_verts, g_font}};
    for (const Layer& l : layers) {
        UINT n = static_cast<UINT>(l.verts->size());
        if (n && l.tex) {
            g_ctx->PSSetShaderResources(0, 1, &l.tex);
            g_ctx->Draw(n, at);
        }
        at += n;
    }
}

bool fill_vertices(UINT width, UINT height) {
    g_w = static_cast<float>(width);
    g_h = static_cast<float>(height);
    g_cmdVerts.clear();
    g_gaugeVerts.clear();
    g_faceVerts.clear();
    g_verts.clear();
    g_out = &g_verts;
    ++g_frameNo;
    draw_menu();
    draw_status();
    if (g_cmdVerts.empty() && g_gaugeVerts.empty() && g_faceVerts.empty() && g_verts.empty()) return false;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    Vertex* dst = static_cast<Vertex*>(map.pData);
    for (std::vector<Vertex>* v : {&g_cmdVerts, &g_gaugeVerts, &g_faceVerts, &g_verts}) {
        memcpy(dst, v->data(), v->size() * sizeof(Vertex));
        dst += v->size();
    }
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
