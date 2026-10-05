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
ID3D11ShaderResourceView* g_kfont = nullptr;  // KH2's system font, optional
uint8_t g_kfontWidth[512];                    // advance of each glyph, in KH2 units
int g_kfontCount = 0;
IDXGISwapChain* g_target = nullptr;
bool g_ready = false;
bool g_failed = false;

Glyph g_glyphs[kLastChar - kFirstChar + 1];
float g_whiteU = 0, g_whiteV = 0;   // a solid texel in the atlas, for flat quads
// Layers, drawn in this order, one texture each.
std::vector<Vertex> g_cmdVerts;    // KH2 command menu sheet
std::vector<Vertex> g_gaugeVerts;  // KH2 gauge sheet
std::vector<Vertex> g_faceVerts;   // portrait
std::vector<Vertex> g_kfontVerts;  // KH2 font
std::vector<Vertex> g_verts;       // font atlas: text and flat shapes
std::vector<Vertex>* g_out = &g_verts;
float g_mode = 1;                  // blend mode for the vertices being added
unsigned g_frameNo = 0;            // counts presented frames, drives looping animations
// The game draws a 16:9 picture and bars the rest of the back buffer. g_w and
// g_h are that picture, which everything is laid out in; g_ox and g_oy are
// where it starts in the real back buffer of g_bw by g_bh.
float g_w = 1, g_h = 1;
float g_ox = 0, g_oy = 0, g_bw = 1, g_bh = 1;

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

// Reads a small file from the assets folder. Returns how many bytes it got.
size_t load_bytes(const char* name, uint8_t* out, size_t capacity) {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&load_bytes), &self);
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') --n;
    if (n == 0 || n + 40 >= MAX_PATH) return 0;
    sprintf_s(path + n, MAX_PATH - n, "assets\\%s", name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return 0;
    size_t got = fread(out, 1, capacity, f);
    fclose(f);
    return got;
}

// ---- KH2 sequence data ----
//
// A .sqd from a KH2 .2dd, flattened to a binary by tools_dev/make_assets.py.
// Sprites are rectangles on the sheet, sprite groups place a few of them
// around an origin, and an animation group is a list of timed animations that
// each move, scale, tint and fade one sprite group. Format and behaviour follow
// OpenKH's documentation of the file.

// us, vs: how far the texture slides each frame, as a fraction of the sheet
struct SeqSprite { float u0, v0, u1, v1; float us, vs; float col[4][4]; };   // corner colours: left, top, right, bottom
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
    int sheetW = 0, sheetH = 0;   // the remastered sheet, in pixels
    float texel = 1;              // sheet pixels per unit of sprite coordinate
    std::vector<SeqSprite> sprites;
    std::vector<SeqPart> parts;
    std::vector<SeqRange> groups;
    std::vector<SeqAnim> anims;
    std::vector<SeqAnimGroup> animGroups;
};

// One command menu skin: a world's sheet and its layout data.
struct Skin {
    const char* name;
    ID3D11ShaderResourceView* tex;
    Seq seq;
    bool tried;
};
Skin g_skins[] = {{"zz0"}, {"al0"}, {"he0"}, {"nm0"}, {"lm0"}, {"hb0"}, {"po0"}, {"tt0"}, {"di0"}};
Skin* g_skin = nullptr;        // the one being drawn
unsigned g_menuOpenFrame = 0;  // frame the menu last came up on
int g_testSince = -1;          // test harness: pretend the menu came up this many frames ago

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
    uint32_t head[9] = {};
    bool ok = fread(head, sizeof(uint32_t), 9, f) == 9 && head[0] == 0x33514553 /* "SEQ3" */ &&
              read_vec(f, seq.sprites, head[1]) && read_vec(f, seq.parts, head[2]) &&
              read_vec(f, seq.groups, head[3]) && read_vec(f, seq.anims, head[4]) &&
              read_vec(f, seq.animGroups, head[5]);
    seq.sheetW = static_cast<int>(head[6]);
    seq.sheetH = static_cast<int>(head[7]);
    seq.texel = head[8] / 256.0f;
    ok = ok && seq.sheetW > 0 && seq.sheetH > 0 && seq.texel > 0;
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

            // A sliding sprite wraps inside its own rectangle, so it is drawn
            // as up to two strips each way, split where the wrap falls.
            float fu[3] = {0, 1, 1}, su[2][2] = {{sp.u0, sp.u1}, {0, 0}};
            float fv[3] = {0, 1, 1}, sv[2][2] = {{sp.v0, sp.v1}, {0, 0}};
            int nu = 1, nv = 1;
            const float w = sp.u1 - sp.u0, hgt = sp.v1 - sp.v0;
            if (sp.us != 0 && w > 0) {
                float off = fmodf(sp.us * (seq.sheetW / seq.texel) * g_frameNo, w);
                if (off < 0) off += w;
                fu[1] = (w - off) / w;
                su[0][0] = sp.u0 + off; su[0][1] = sp.u1;
                su[1][0] = sp.u0;       su[1][1] = sp.u0 + off;
                nu = 2;
            }
            if (sp.vs != 0 && hgt > 0) {
                float off = fmodf(sp.vs * (seq.sheetH / seq.texel) * g_frameNo, hgt);
                if (off < 0) off += hgt;
                fv[1] = (hgt - off) / hgt;
                sv[0][0] = sp.v0 + off; sv[0][1] = sp.v1;
                sv[1][0] = sp.v0;       sv[1][1] = sp.v0 + off;
                nv = 2;
            }

            for (int iu = 0; iu < nu; ++iu)
                for (int iv = 0; iv < nv; ++iv) {
                    const float fx[2] = {fu[iu], fu[iu + 1]}, fy[2] = {fv[iv], fv[iv + 1]};
                    if (fx[1] <= fx[0] || fy[1] <= fy[0]) continue;
                    // corners: top-left, top-right, bottom-left, bottom-right
                    float px[4], py[4], pu[4], pvv[4], pc[4][4];
                    for (int k = 0; k < 4; ++k) {
                        const float tx = fx[k & 1], ty = fy[k >> 1];
                        float lx = part.x0 + (part.x1 - part.x0) * tx, ly = part.y0 + (part.y1 - part.y0) * ty;
                        float vx = (lx + pvx) * sx, vy = (ly + pvy) * sy;
                        px[k] = ox + (vx * cs - vy * sn + x) * unit;
                        py[k] = oy + (vx * sn + vy * cs + y) * unit;
                        pu[k] = su[iu][k & 1];
                        pvv[k] = sv[iv][k >> 1];
                        for (int c = 0; c < 4; ++c) {
                            float top = sp.col[0][c] * (1 - tx) + sp.col[1][c] * tx;
                            float bot = sp.col[2][c] * (1 - tx) + sp.col[3][c] * tx;
                            pc[k][c] = (top * (1 - ty) + bot * ty) * col[c];
                        }
                    }
                    const int order[6] = {0, 1, 2, 1, 3, 2};
                    for (int k : order)
                        vert(px[k], py[k], pu[k] * seq.texel / seq.sheetW, pvv[k] * seq.texel / seq.sheetH, pc[k]);
                }
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
    g_face = load_rgba("kh2_face.rgba", 1024, 512);
    g_kh2 = load_rgba("kh2_gauge.rgba", kKh2SheetW, kKh2SheetH);
    g_kfont = load_rgba("kh2_font.rgba", 1024, 512);
    if (g_kfont) {
        g_kfontCount = static_cast<int>(load_bytes("kh2_font.bin", g_kfontWidth, sizeof(g_kfontWidth)));
        if (g_kfontCount < 150) {
            g_kfont->Release();
            g_kfont = nullptr;
        }
    }
    return true;
}

// --- drawing, coordinates in backbuffer pixels ---

void vert(float x, float y, float u, float v, const float c[4]) {
    if (g_verts.size() + g_faceVerts.size() + g_gaugeVerts.size() + g_cmdVerts.size() + g_kfontVerts.size() >= kMaxVerts)
        return;
    g_out->push_back({(x + g_ox) / g_bw * 2 - 1, 1 - (y + g_oy) / g_bh * 2, u, v, c[0], c[1], c[2], c[3], g_mode});
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
// Measured on the sheet: the coloured band runs from 30 to 40.5 units out,
// and the black ring that backs it from 28 to 42.5, which is what gives the
// gauge its border.
const float kRingBandIn = 27;        // nothing but transparency inside this
const float kRingMidR = 35.25f;      // middle of the coloured band, for arc length
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
    // Thin slices, and the outer edge exactly on the sprite's edge: every point
    // then maps to its own texel with no clamping, so the ring's outline keeps
    // an even width all the way round.
    const int steps = static_cast<int>((a1 - a0) / 1.5f) + 1;
    for (int i = 0; i < steps; ++i) {
        float t0 = (a0 + (a1 - a0) * i / steps) * kPi / 180;
        float t1 = (a0 + (a1 - a0) * (i + 1) / steps) * kPi / 180;
        float pts[4][2] = {
            {kRingBandIn * cosf(t0), kRingBandIn * sinf(t0)}, {kRingR * cosf(t0), kRingR * sinf(t0)},
            {kRingBandIn * cosf(t1), kRingBandIn * sinf(t1)}, {kRingR * cosf(t1), kRingR * sinf(t1)},
        };
        const int order[6] = {0, 1, 2, 1, 3, 2};
        for (int k : order) {
            float x = pts[k][0], y = pts[k][1];
            hud_vert(h, x, y, centreU - fabsf(x), kUvCentreV - fabsf(y), c);
        }
    }
}

// KH2 lays its HUD out on a 416 line screen and pins it to the corners.
float kh2_unit() { return g_h / 416.0f; }

// Sprite from the portrait sheet (256x128 in the layout data, four times that
// remastered), placed in HUD units like the gauge sprites.
void face_sprite(const Hud& h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float white[4] = {1, 1, 1, 1};
    const float ku = 4.0f / 1024, kv = 4.0f / 512;
    const float px0 = h.cx + x0 * h.unit, px1 = h.cx + x1 * h.unit;
    const float py0 = h.cy + y0 * h.unit, py1 = h.cy + y1 * h.unit;
    vert(px0, py0, u0 * ku, v0 * kv, white);
    vert(px1, py0, u1 * ku, v0 * kv, white);
    vert(px0, py1, u0 * ku, v1 * kv, white);
    vert(px1, py0, u1 * ku, v0 * kv, white);
    vert(px1, py1, u1 * ku, v1 * kv, white);
    vert(px0, py1, u0 * ku, v1 * kv, white);
}

void draw_status_kh2() {
    const OverlayState st = g_state;
    // Everything hangs off the centre of the portrait's frame, which KH2's
    // layout data pins at (455, 355) on its 512x416 screen: the frame itself,
    // the portrait and the MP gauge. The HP ring is the one piece the data does
    // not place; measured on KH2 it sits 4.5 units lower than the frame.
    Hud o;
    o.unit = kh2_unit();
    o.cx = g_w - 60 * o.unit;
    o.cy = g_h - (416 - 355) * o.unit;
    Hud h = o;
    h.cy += 4.5f * o.unit;

    const float white[4] = {1, 1, 1, 1};
    const float black[4] = {0, 0, 0, 1};
    const float mpWell[4]   = {0.031f, 0.086f, 0.656f, 1};   // tints straight from the layout data
    const float mpCharge[4] = {1.31f, 0.156f, 1.05f, 1};
    const float mpTag[4]    = {0.60f, 1.00f, 1.56f, 1};
    const float mpTagCh[4]  = {1.56f, 0.45f, 1.10f, 1};

    g_out = &g_gaugeVerts;

    // Behind the portrait: a see-through dark disc with a thin outline, both
    // straight from KH2's gauge sheet. The scene shows through it, as in KH2.
    // Damage, the way KH2 shows it: the HP just lost stays on the gauge in red
    // for a moment and then fades out, the portrait winces and the disc
    // behind it flushes red.
    static float lastHp = -1, trailHp = 0;
    static unsigned hurtFrame = 0;
    static bool hurtSeen = false;
    {
        float now = static_cast<float>(st.hp);
        if (lastHp >= 0 && now < lastHp) {
            if (!hurtSeen || g_frameNo - hurtFrame > 60 || trailHp < lastHp) trailHp = lastHp;
            hurtFrame = g_frameNo;
            hurtSeen = true;
        }
        lastHp = now;
    }
    const unsigned sinceHurt = hurtSeen ? g_frameNo - hurtFrame : 100000;
    const float trail = sinceHurt < 30 ? 1.0f : (sinceHurt < 60 ? 1.0f - (sinceHurt - 30) / 30.0f : 0.0f);
    const bool wince = sinceHurt < 45;

    // The frame: a half-clear disc and a thin circle, 37 units out, in the
    // sprites and arrangement KH2 uses for Sora's own (sprite group 117). KH2
    // tints it: black at rest, red fading to black when hit, pulsing red when
    // HP is low.
    float flush = wince ? 1.0f - sinceHurt / 45.0f : 0.0f;
    if (st.hp > 0 && st.hp <= st.hpMax / 4) {
        float t = (g_frameNo % 40) / 20.0f;          // 20 frames each way, as in the data
        float pulse = t < 1 ? 1 - t : t - 1;
        if (pulse > flush) flush = pulse;
    }
    const float tint[4] = {1.56f * flush, 0, 0, 1};
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const float disc[4] = {tint[0] * 144 / 255.0f * 2, 0, 0, 0.5f};
            hud_sprite(o, sx * 37.0f, sy * 37.0f, 0, 0, 119, 146, 156, 183, disc);
            hud_sprite(o, sx * 37.0f, sy * 37.0f, 0, 0, 217, 0, 254, 37, tint);
        }

    // --- HP ---
    // The gauge starts at 9 o'clock and runs clockwise. How far it reaches
    // depends on max HP: a short arc early on, the full three quarters later,
    // and past that a bar running left from the bottom of the ring. Current HP
    // fills it from the same start, so damage comes off the far end.
    const float kDegPerHp = 2.0f;   // about what KH2 shows: 60 HP is a third of the ring
    const float kUnitsPerDeg = kPi / 180 * kRingMidR;
    float hpMax = st.hpMax > 0 ? static_cast<float>(st.hpMax) : 1;
    float hp = st.hp < 0 ? 0 : (st.hp > st.hpMax ? hpMax : static_cast<float>(st.hp));
    float maxDeg = hpMax * kDegPerHp, curDeg = hp * kDegPerHp;
    float bar = maxDeg > 270 ? (maxDeg - 270) * kUnitsPerDeg : 0;
    if (bar > 330) bar = 330;
    const bool low = hp <= hpMax * 0.25f;

    // The black ring behind the gauge, as far round as max HP reaches. It is
    // wider than the coloured band, so it is both the gauge's border and what
    // shows where HP has been lost. It runs a little past each end to close them.
    const float kCapDeg = 2.0f / kUnitsPerDeg;
    {
        float end = maxDeg < 270 ? maxDeg + kCapDeg : 270;
        hud_ring(h, kUvDarkU, 180 - kCapDeg, 180 + end, black);
        if (bar > 0) hud_sprite(h, -bar - 2, kRingR, 0, 0, kUvDarkU, 60, kUvDarkU, kUvCentreV, black);
    }

    // HP just lost: red, from where HP is now back to where it was
    if (trail > 0 && trailHp > hp) {
        const float redCol[4] = {2.6f, 0.30f, 0.25f, trail};
        float lostDeg = (trailHp > hpMax ? hpMax : trailHp) * kDegPerHp;
        hud_ring(h, kUvDarkU, 180 + (curDeg < 270 ? curDeg : 270), 180 + (lostDeg < 270 ? lostDeg : 270), redCol);
        if (lostDeg > 270 && bar > 0) {
            float from = curDeg > 270 ? (curDeg - 270) * kUnitsPerDeg : 0;
            float to = (lostDeg - 270) * kUnitsPerDeg;
            if (to > bar) to = bar;
            if (to > from) hud_sprite(h, -to, kRingR, -from, 0, kUvDarkU, 60, kUvDarkU, kUvCentreV, redCol);
        }
    }

    if (curDeg > 0) {
        hud_ring(h, kUvGreenU, 180, 180 + (curDeg < 270 ? curDeg : 270), white);
        if (curDeg > 270 && bar > 0) {
            float len = (curDeg - 270) * kUnitsPerDeg;
            if (len > bar) len = bar;
            hud_sprite(h, -len, kRingR, 0, 0, kUvGreenU, 60, kUvGreenU, kUvCentreV, white);
        }
    }

    // --- MP: its tag is placed by the data at (-40, 31) from the frame's
    // centre, and the bar runs left from the tag with no gap ---
    const float mpY = 31, mpRight = -40, mpLen = 100;
    float mp = st.mp < 0 ? 0 : (st.mp > 1 ? 1 : st.mp);
    hud_sprite(o, mpRight - mpLen - 1, mpY - 5, mpRight, mpY + 5, 183, 49, 185, 59, mpWell);
    if (mp > 0) {
        if (st.mpCharging)
            hud_sprite(o, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 219, 40, 219, 46, mpCharge);
        else
            hud_sprite(o, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 216, 40, 216, 46, white);
    }
    hud_sprite(o, -40, mpY - 5, -16, mpY + 5, 157, 37, 181, 47, st.mpCharging ? mpTagCh : mpTag);

    // --- portrait: KH2's own, placed by its own layout data, over the gauge ---
    g_out = &g_faceVerts;
    if (g_face) {
        if (hp <= 0) face_sprite(o, -35, -51, 36, 44, 143, 0, 214, 95);        // down
        else if (low || wince) face_sprite(o, -34, -50, 35, 45, 72, 0, 141, 95);   // hurt
        else face_sprite(o, -33, -50, 31, 45, 3, 0, 67, 95);
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
// Every skin numbers its animation groups the same way. Most come in a field
// (blue) and a battle (red) version; index 0 is field, 1 battle.
constexpr int kOpen = 7;                     // the whole menu coming up
constexpr int kHeader[2] = {17, 30};         // COMMANDS
constexpr int kSlotSel[2] = {10, 23};        // selected slot, glowing
constexpr int kSlotMid[2] = {16, 29};        // unselected slot
constexpr int kSlotLast[2] = {11, 24};       // unselected slot closing the menu
constexpr int kIconRing[2] = {45, 46};       // glow behind the selected slot's icon
constexpr int kBottom = 6;                   // trim under the last slot, on skins that have it
constexpr int kIcon[4] = {37, 41, 39, 44};   // keyblade, hat, potion, charm
// lists, by command: Magic, Items, Summon
constexpr int kSubHeader[4] = {-1, 60, 51, 73};
constexpr int kSubSel[4] = {-1, 61, 48, 74};
constexpr int kSubMid[4] = {-1, 62, 49, 75};
constexpr int kSubLast[4] = {-1, 63, 50, 76};
constexpr float kRowPitch = 22;
constexpr float kSelectedShift = 10;         // the selected slot sits this far right
constexpr float kLabelPad = 13;              // from a slot's left edge to its label
}

// ---- KH2's system font ----
//
// The sheet is a grid, 28 glyphs to a row, each cell 18x24 KH2 units (twice
// that in the remastered sheet). Glyphs are stored grey and doubled when
// drawn, like every colour in KH2's 2D data.
constexpr int kFontCols = 28;
constexpr float kFontCellW = 18, kFontCellH = 24;

// Where a character sits in the grid, or -1 if the font has none.
int kfont_glyph(char c) {
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'N') return 14 + (c - 'A');
    if (c >= 'O' && c <= 'Z') return 28 + (c - 'O');
    if (c >= 'a' && c <= 'r') return 4 * kFontCols + 10 + (c - 'a');
    if (c >= 's' && c <= 'z') return 5 * kFontCols + (c - 's');
    switch (c) {
        case '+': return 11;
        case '-': return 12;
        case '!': return 40;
        case '?': return 41;
        case '%': return 42;
        case '/': return 43;
        case '.': return 47;
        case ',': return 48;
        case ':': return 50;
        case '\'': return 55;
        default: return -1;
    }
}

// Text in KH2's font. x, y are the top left in pixels, unit the size of one
// KH2 unit and scale how much smaller than the font's natural size to draw.
float kfont_text(float x, float y, float unit, float scale, const char* s, const float col[4]) {
    const float k = unit * scale;
    const float c2[4] = {col[0] * 2, col[1] * 2, col[2] * 2, col[3]};
    for (; *s; ++s) {
        int g = kfont_glyph(*s);
        if (g < 0 || g >= g_kfontCount) {
            x += 6 * k;   // space, and anything the font lacks
            continue;
        }
        float u0 = (g % kFontCols) * kFontCellW * 2 / 1024.0f, v0 = (g / kFontCols) * kFontCellH * 2 / 512.0f;
        float u1 = u0 + kFontCellW * 2 / 1024.0f, v1 = v0 + kFontCellH * 2 / 512.0f;
        float x1 = x + kFontCellW * k, y1 = y + kFontCellH * k;
        vert(x, y, u0, v0, c2);
        vert(x1, y, u1, v0, c2);
        vert(x, y1, u0, v1, c2);
        vert(x1, y, u1, v0, c2);
        vert(x1, y1, u1, v1, c2);
        vert(x, y1, u0, v1, c2);
        x += g_kfontWidth[g] * k;
    }
    return x;
}

// White text with a dark edge, sized in KH2 units.
void menu_text(float x, float y, float px, const char* s, const float col[4]) {
    if (g_kfont) {
        // px is the height asked for; the font's cell is 24 units tall
        const float unit = kh2_unit();
        const float scale = px / unit / 15.5f;
        const float shadow[4] = {0.02f, 0.02f, 0.03f, 0.85f};
        std::vector<Vertex>* was = g_out;
        g_out = &g_kfontVerts;
        const float top = y - 4 * unit * scale;   // glyphs sit a little down in their cell
        kfont_text(x + unit, top + unit, unit, scale, s, shadow);
        kfont_text(x, top, unit, scale, s, col);
        g_out = was;
        return;
    }
    const float edge[4] = {0.05f, 0.04f, 0.03f, 0.9f};
    const float o = px * 0.07f;
    text(x - o, y, px, s, edge);
    text(x + o, y, px, s, edge);
    text(x, y - o, px, s, edge);
    text(x, y + o, px, s, edge);
    text(x, y, px, s, col);
}

// Which KH2 skin goes with which Re:CoM world. KH2 has no Traverse Town,
// Wonderland, Monstro, Neverland or Castle Oblivion, so those get its standard
// menu; the rest get their own.
Skin* skin_for_world(int world) {
    const char* name = "zz0";
    switch (world) {
        case 2: name = "al0"; break;    // Agrabah
        case 3: name = "he0"; break;    // Olympus Coliseum
        case 6: name = "nm0"; break;    // Halloween Town
        case 7: name = "lm0"; break;    // Atlantica
        case 9: name = "hb0"; break;    // Hollow Bastion
        case 10: name = "po0"; break;   // 100 Acre Wood
        case 11: name = "tt0"; break;   // Twilight Town
        case 12: name = "di0"; break;   // Destiny Islands
    }
    Skin* fallback = nullptr;
    Skin* found = nullptr;
    for (Skin& k : g_skins) {
        if (!strcmp(k.name, name)) found = &k;
        if (!strcmp(k.name, "zz0")) fallback = &k;
    }
    for (Skin* k : {found, fallback}) {
        if (!k) continue;
        if (!k->tried) {
            k->tried = true;
            char file[40];
            sprintf_s(file, "cmd_%s.seq", k->name);
            if (load_seq(file, k->seq)) {
                sprintf_s(file, "cmd_%s.rgba", k->name);
                k->tex = load_rgba(file, k->seq.sheetW, k->seq.sheetH);
            }
        }
        if (k->tex) return k;
    }
    return nullptr;
}

// Left edge of a slot's art, in units from the origin it is drawn at. Skins
// differ in where their slots start, and the labels follow the slot.
float group_left(const Seq& seq, int group) {
    if (group < 0 || group >= static_cast<int>(seq.animGroups.size())) return 0;
    const SeqAnimGroup& ag = seq.animGroups[group];
    if (ag.count == 0) return 0;
    const SeqAnim& a = seq.anims[ag.first];
    const SeqRange& sg = seq.groups[a.group];
    float left = 1e9f;
    for (int p = 0; p < sg.count; ++p) {
        const SeqPart& part = seq.parts[sg.first + p];
        float x = part.x0 < part.x1 ? part.x0 : part.x1;
        if (x < left) left = x;
    }
    return a.tx1 + (left < 1e8f ? left : 0);
}

int group_last(const Seq& seq, int group) {
    return group >= 0 && group < static_cast<int>(seq.animGroups.size()) ? seq.animGroups[group].last : 0;
}

void draw_menu_kh2() {
    static const char* kItems[4] = {"Attack", "Magic", "Items", "Summon"};
    const OverlayState st = g_state;
    const Seq& seq = g_skin->seq;
    const float u = kh2_unit();
    const float ox = 37 * u;                                     // measured off KH2 screenshots
    const float oy = g_h - (3 * cmd::kRowPitch + 15 + 18) * u;
    const int f = static_cast<int>(g_frameNo);
    const float white[4] = {1, 1, 1, 1};
    const float dim[4] = {0.72f, 0.72f, 0.74f, 1};
    const int mode = st.battle ? 1 : 0;
    const float rowText = ox + (group_left(seq, cmd::kSlotMid[mode]) + cmd::kLabelPad) * u;

    g_out = &g_cmdVerts;
    if (!st.subOpen) {
        // Coming up: KH2's own opening animation, with the labels fading in
        // over its last third of a second.
        const int since = g_testSince >= 0 ? g_testSince : static_cast<int>(g_frameNo - g_menuOpenFrame);
        const int openLast = group_last(seq, cmd::kOpen);
        if (since < openLast) {
            seq_draw(seq, cmd::kOpen, since, ox, oy, u);
            float fade = (since - (openLast - 20)) / 20.0f;
            if (fade > 0) {
                const float c[4] = {dim[0], dim[1], dim[2], fade};
                g_out = &g_verts;
                for (int i = 0; i < 4; ++i)
                    menu_text(rowText, oy + (i * cmd::kRowPitch - 6.5f) * u, 12 * u, kItems[i], c);
            }
            g_out = &g_verts;
            return;
        }

        // Headers are authored one slot above the menu's origin. Their glow is a
        // one-shot that plays as the menu comes up, so here it rests on its end.
        seq_draw(seq, cmd::kHeader[mode], 1 << 20, ox, oy - cmd::kRowPitch * u, u);
        seq_draw(seq, cmd::kBottom, f, ox, oy, u);
        for (int i = 0; i < 4; ++i)
            if (i != st.selected)
                seq_draw(seq, i == 3 ? cmd::kSlotLast[mode] : cmd::kSlotMid[mode], f, ox, oy + i * cmd::kRowPitch * u, u);
        // the selected slot is pushed right and is the only one showing its icon
        const float sx = ox + cmd::kSelectedShift * u;
        const float sy = oy + st.selected * cmd::kRowPitch * u;
        seq_draw(seq, cmd::kSlotSel[mode], f, sx, sy, u);
        seq_draw(seq, cmd::kIconRing[mode], f, sx, sy, u);
        seq_draw(seq, cmd::kIcon[st.selected], f, sx, sy, u);
        g_out = &g_verts;
        for (int i = 0; i < 4; ++i) {
            bool sel = i == st.selected;
            menu_text(rowText + (sel ? cmd::kSelectedShift * u : 0), oy + (i * cmd::kRowPitch - 6.5f) * u, 12 * u,
                      kItems[i], sel ? white : dim);
        }
        return;
    }

    // A list: its own header, then up to four wide slots that scroll with the selection.
    const int kind = st.selected >= 1 && st.selected <= 3 ? st.selected : 1;
    const int kRows = 4;
    int first = st.subSelected >= kRows ? st.subSelected - kRows + 1 : 0;
    int shown = st.subCount - first < kRows ? st.subCount - first : kRows;
    if (shown < 1) shown = 1;
    seq_draw(seq, cmd::kSubHeader[kind], f, ox, oy - cmd::kRowPitch * u, u);
    for (int i = 0; i < shown; ++i) {
        bool sel = first + i == st.subSelected && st.subCount > 0;
        const int slot = sel ? cmd::kSubSel[kind] : (i == shown - 1 ? cmd::kSubLast[kind] : cmd::kSubMid[kind]);
        seq_draw(seq, slot, f, ox, oy + i * cmd::kRowPitch * u, u);
    }
    g_out = &g_verts;
    const float listText = ox + (group_left(seq, cmd::kSubMid[kind]) + cmd::kLabelPad) * u;
    if (st.subCount == 0) {
        menu_text(listText, oy - 6.5f * u, 12 * u, "Nothing", dim);
        return;
    }
    for (int i = 0; i < shown; ++i) {
        const char* label = st.subNames[first + i] ? st.subNames[first + i] : "?";
        menu_text(listText, oy + (i * cmd::kRowPitch - 6.5f) * u, 12 * u, label,
                  first + i == st.subSelected ? white : dim);
    }
}

void draw_menu() {
    g_skin = skin_for_world(g_state.world);
    g_cmd = g_skin ? g_skin->tex : nullptr;
    if (g_skin) draw_menu_kh2();
    else draw_menu_plain();
}

void bind_and_draw(ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp{0, 0, g_bw, g_bh, 0, 1};
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
    const Layer layers[5] = {{&g_cmdVerts, g_cmd}, {&g_gaugeVerts, g_kh2}, {&g_faceVerts, g_face},
                             {&g_kfontVerts, g_kfont}, {&g_verts, g_font}};
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
    g_bw = static_cast<float>(width);
    g_bh = static_cast<float>(height);
    const float aspect = 16.0f / 9.0f;
    if (g_bw / g_bh < aspect) {          // taller than 16:9: bars top and bottom
        g_w = g_bw;
        g_h = g_bw / aspect;
    } else {                             // wider: bars left and right
        g_h = g_bh;
        g_w = g_bh * aspect;
    }
    g_ox = (g_bw - g_w) / 2;
    g_oy = (g_bh - g_h) / 2;
    g_cmdVerts.clear();
    g_kfontVerts.clear();
    g_gaugeVerts.clear();
    g_faceVerts.clear();
    g_verts.clear();
    g_out = &g_verts;
    ++g_frameNo;
    static ULONGLONG lastDraw = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastDraw > 300) g_menuOpenFrame = g_frameNo;
    lastDraw = now;
    draw_menu();
    draw_status();
    if (g_cmdVerts.empty() && g_gaugeVerts.empty() && g_faceVerts.empty() && g_verts.empty() && g_kfontVerts.empty())
        return false;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    Vertex* dst = static_cast<Vertex*>(map.pData);
    for (std::vector<Vertex>* v : {&g_cmdVerts, &g_gaugeVerts, &g_faceVerts, &g_kfontVerts, &g_verts}) {
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
// draw, and hand it back in the present state. The wrappers are kept per back
// buffer, because making them every frame is slow enough to feel. They hold
// references to the buffers, so they have to go before the game resizes the
// swap chain: see the ResizeBuffers hooks.
struct WrappedBuffer {
    ID3D12Resource* buffer;
    ID3D11Resource* wrapped;
    ID3D11RenderTargetView* rtv;
    UINT width, height;
};
constexpr int kMaxBuffers = 8;
WrappedBuffer g_buffers[kMaxBuffers];
int g_bufferCount = 0;

void release_buffers() {
    if (!g_bufferCount) return;
    for (int i = 0; i < g_bufferCount; ++i) {
        g_buffers[i].rtv->Release();
        g_buffers[i].wrapped->Release();
        g_buffers[i].buffer->Release();
    }
    g_bufferCount = 0;
    // 11on12 only lets go of the D3D12 resources once its context is flushed
    ID3D11RenderTargetView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &none, nullptr);
    g_ctx->Flush();
}

WrappedBuffer* wrap_buffer(ID3D12Resource* back12) {
    for (int i = 0; i < g_bufferCount; ++i)
        if (g_buffers[i].buffer == back12) return &g_buffers[i];
    if (g_bufferCount == kMaxBuffers) release_buffers();

    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    WrappedBuffer w{};
    HRESULT hr = g_on12->CreateWrappedResource(back12, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                               D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource),
                                               reinterpret_cast<void**>(&w.wrapped));
    if (FAILED(hr)) {
        static bool once = false;
        if (!once && g_log) g_log("overlay: CreateWrappedResource failed (%08X)", hr);
        once = true;
        return nullptr;
    }
    if (FAILED(g_dev->CreateRenderTargetView(w.wrapped, nullptr, &w.rtv))) {
        w.wrapped->Release();
        return nullptr;
    }
    D3D12_RESOURCE_DESC desc = back12->GetDesc();
    w.width = static_cast<UINT>(desc.Width);
    w.height = desc.Height;
    w.buffer = back12;
    back12->AddRef();
    g_buffers[g_bufferCount] = w;
    return &g_buffers[g_bufferCount++];
}

void render_d3d12(IDXGISwapChain* sc) {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3)))) return;
    UINT index = sc3->GetCurrentBackBufferIndex();
    sc3->Release();

    ID3D12Resource* back12 = nullptr;
    if (FAILED(sc->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back12)))) return;
    WrappedBuffer* w = wrap_buffer(back12);
    back12->Release();
    if (!w) return;

    g_on12->AcquireWrappedResources(&w->wrapped, 1);
    if (fill_vertices(w->width, w->height)) bind_and_draw(w->rtv);
    ID3D11RenderTargetView* none = nullptr;
    g_ctx->OMSetRenderTargets(1, &none, nullptr);
    g_on12->ReleaseWrappedResources(&w->wrapped, 1);
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

using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                              IUnknown* const*);
ResizeFn g_realResize = nullptr;
Resize1Fn g_realResize1 = nullptr;

// The game cannot resize its swap chain while anything still references the
// back buffers, so drop ours first.
void before_resize(IDXGISwapChain* sc) {
    if (sc != g_target || !g_on12) return;
    __try {
        release_buffers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_failed = true;
    }
}

HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    before_resize(sc);
    return g_realResize(sc, count, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE resize1_hook(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                       const UINT* masks, IUnknown* const* queues) {
    before_resize(sc);
    return g_realResize1(sc, count, w, h, fmt, flags, masks, queues);
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

void overlay_test_since(int frames) { g_testSince = frames; }

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

    // ResizeBuffers is slot 13, ResizeBuffers1 slot 39 of IDXGISwapChain3.
    g_realResize = reinterpret_cast<ResizeFn>(vtbl[13]);
    patch_slot(&vtbl[13], reinterpret_cast<void*>(&resize_hook));
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3)))) {
        void** vtbl3 = *reinterpret_cast<void***>(sc3);
        g_realResize1 = reinterpret_cast<Resize1Fn>(vtbl3[39]);
        patch_slot(&vtbl3[39], reinterpret_cast<void*>(&resize1_hook));
        sc3->Release();
    }

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
