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
#include <initializer_list>
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
ID3D11ShaderResourceView* g_kh1 = nullptr;    // KH1's command menu sheets, optional
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
// KH2's 2D screen is 512x416 stretched over a 4:3 picture, so one unit across
// is 13/12 of one unit down.
constexpr float kUnitAcross = (4.0f / 3) / (512.0f / 416);

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
                        px[k] = ox + (vx * cs - vy * sn + x) * unit * kUnitAcross;
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
    g_kh1 = load_rgba("kh1_command.rgba", 1024, 2560);   // the 256x640 layout, enlarged four times
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
    float cx, cy;     // origin in pixels
    float ux, unit;   // pixels per KH2 unit, across and down
};

const float kRingR = 45;             // ring quarter is 45x45
// Measured on the sheet: the coloured band runs from 30 to 40.5 units out,
// and the black ring that backs it from 28 to 42.5, which is what gives the
// gauge its border.
const float kRingBandIn = 27;        // nothing but transparency inside this
const float kUvCentreV = 105;        // v at the ring's centre
const float kUvGreenU = 95;          // u at the centre, green quarter
const float kUvDarkU = 46;           // u at the centre, dark quarter
const float kUvRedU = 144;           // u at the centre, the quarter shown for lost HP

// KH2's own gauge rules, from its code: the ring takes the first 60 HP at
// 4.5 degrees each, which is its full three quarters, and every HP past that
// adds 3 units of bar. Sora starts KH2 with 20 HP and Re:CoM with 80, so
// Re:CoM's HP is shown at a quarter to give the same gauge at the same point
// in the game.
const float kRingHp = 60;
const float kDegPerHp = 4.5f;
const float kBarPerHp = 3;
const float kHpScale = 0.25f;

void hud_vert(const Hud& h, float x, float y, float u, float v, const float c[4]) {
    vert(h.cx + x * h.ux, h.cy + y * h.unit, u * kKh2Texel / kKh2SheetW, v * kKh2Texel / kKh2SheetH, c);
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

// Sprite turned clockwise about the HUD origin and then moved, for the gauge's
// end cap. v0 goes with y0 and v1 with y1.
void hud_sprite_turned(const Hud& h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                       float deg, float tx, float ty, const float c[4]) {
    const float a = deg * kPi / 180, cs = cosf(a), sn = sinf(a);
    const float px[4] = {x0, x1, x0, x1}, py[4] = {y0, y0, y1, y1};
    const float pu[4] = {u0, u1, u0, u1}, pv[4] = {v0, v0, v1, v1};
    const int order[6] = {0, 1, 2, 1, 3, 2};
    for (int k : order)
        hud_vert(h, px[k] * cs - py[k] * sn + tx, px[k] * sn + py[k] * cs + ty, pu[k], pv[k], c);
}

// KH2 lays its HUD out on a 512x416 screen made for a 4:3 picture, so a unit
// is a little wider than it is tall, and the HD version keeps that: 416 units
// down the picture, and across it as many as fit at that shape (682 at 16:9).
// Everything is pinned to the corners.
float kh2_unit() { return g_h / 416.0f; }
float kh2_unit_x() { return kh2_unit() * kUnitAcross; }

// Sprite from the portrait sheet (256x128 in the layout data, four times that
// remastered), placed in HUD units like the gauge sprites.
void face_sprite(const Hud& h, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float white[4] = {1, 1, 1, 1};
    const float ku = 4.0f / 1024, kv = 4.0f / 512;
    const float px0 = h.cx + x0 * h.ux, px1 = h.cx + x1 * h.ux;
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
    // Everything hangs off the centre of the portrait's frame: the frame, the
    // portrait, the HP ring around it and the MP gauge. KH2's layout data puts
    // it at (455, 355) on a 512x416 screen and the game keeps that distance
    // from the right edge on a wide one, so 57 units in and 61 up.
    Hud o;
    o.unit = kh2_unit();
    o.ux = kh2_unit_x();
    o.cx = g_w - (512 - 455) * o.ux;
    o.cy = g_h - (416 - 355) * o.unit;
    const Hud& h = o;

    const bool full = st.gauge;   // frame, portrait and HP; otherwise MP alone
    const float white[4] = {1, 1, 1, 1};
    const float mpWell[4]   = {0.031f, 0.086f, 0.656f, 1};   // tints straight from the layout data
    const float mpCharge[4] = {1.31f, 0.156f, 1.05f, 1};
    const float mpTag[4]    = {0.60f, 1.00f, 1.56f, 1};
    const float mpTagCh[4]  = {1.56f, 0.59f, 0.76f, 1};

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
            if (!hurtSeen || g_frameNo - hurtFrame > 90 || trailHp < lastHp) trailHp = lastHp;
            hurtFrame = g_frameNo;
            hurtSeen = true;
        }
        lastHp = now;
    }
    const unsigned sinceHurt = hurtSeen ? g_frameNo - hurtFrame : 100000;
    // lost HP: a white flash for 10 frames, held until 60, faded out by 90
    const float trail = sinceHurt <= 60 ? 1.0f : (sinceHurt < 90 ? 1.0f - (sinceHurt - 60) / 30.0f : 0.0f);
    const float trailGlow = sinceHurt < 10 ? 2.0f - sinceHurt / 10.0f : 1.0f;
    const bool wince = sinceHurt < 45;

    // The frame: a half-clear disc and a thin circle, 37 units out, in the
    // sprites and arrangement KH2 uses for Sora's own (sprite group 117). KH2
    // tints it: black at rest, red fading to black when hit, pulsing red when
    // HP is low.
    float hpMax = (st.hpMax > 0 ? static_cast<float>(st.hpMax) : 1) * kHpScale;
    float hp = (st.hp < 0 ? 0 : (st.hp > st.hpMax ? st.hpMax : st.hp)) * kHpScale;
    const bool low = hp > 0 && hp <= hpMax * 0.25f;

    float flush = wince ? 1.0f - sinceHurt / 45.0f : 0.0f;
    float pulse = 0;
    const float beat = (g_frameNo % 40) / 40.0f;   // low HP runs on a 40 frame loop
    if (low && full) {
        pulse = beat < 0.5f ? 1 - beat * 2 : beat * 2 - 1;
        if (pulse > flush) flush = pulse;
        // a red ring that spins out from the frame and fades as it goes
        const float grow = 1.7f + (4.0f - 1.7f) * beat;
        const float wave[4] = {1.56f, 0, 0, 1 - beat};
        for (int q = 0; q < 4; ++q) {
            const float qx = (q == 0 || q == 1) ? 17.0f : -17.0f, qy = (q == 1 || q == 2) ? 17.0f : -17.0f;
            hud_sprite_turned(o, qx * grow, qy * grow, 0, 0, 238, 99, 255, 116, beat * 360, 0, 0, wave);
        }
    }
    const float tint[4] = {1.56f * flush, 0, 0, 1};
    for (int sx = -1; full && sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const float disc[4] = {tint[0] * 144 / 255.0f * 2, 0, 0, 0.5f};
            hud_sprite(o, sx * 37.0f, sy * 37.0f, 0, 0, 119, 146, 156, 183, disc);
            hud_sprite(o, sx * 37.0f, sy * 37.0f, 0, 0, 217, 0, 254, 37, tint);
        }

    // --- HP ---
    // The gauge starts at 9 o'clock and runs clockwise. How far it reaches
    // depends on max HP: a short arc early on, the full three quarters at 60,
    // and past that a bar running left from the bottom of the ring. Current HP
    // fills it from the same start, so damage comes off the far end.
    auto ring_deg = [](float v) { return (v < kRingHp ? v : kRingHp) * kDegPerHp; };
    auto bar_len = [](float v) { return v > kRingHp ? (v - kRingHp) * kBarPerHp : 0.0f; };
    const float maxDeg = ring_deg(hpMax), bar = bar_len(hpMax);

    // The dark ring behind the gauge, as far round as max HP reaches. It is
    // wider than the coloured band, so it is both the gauge's border and what
    // shows where HP has been lost. At low HP it pulses red with the frame.
    const float back[4] = {1 + 0.56f * pulse, 1 - pulse, 1 - pulse, 1};
    if (full) {
    hud_sprite(h, -45, 0, 0, 2, 189, 49, 234, 52, back);   // closes the start of the ring
    hud_ring(h, kUvDarkU, 180, 180 + maxDeg, back);
    if (bar > 0) hud_sprite(h, -bar, kRingR, 0, 0, kUvDarkU, 60, kUvDarkU, kUvCentreV, back);

    // HP just lost: red up to where HP was, under what is left
    if (trail > 0 && trailHp * kHpScale > hp) {
        const float was = trailHp * kHpScale > hpMax ? hpMax : trailHp * kHpScale;
        const float red[4] = {1.0f * trailGlow, 0.0625f * trailGlow, 0.19f * trailGlow, trail};
        hud_ring(h, kUvRedU, 180, 180 + ring_deg(was), red);
        if (bar_len(was) > 0) hud_sprite(h, -bar_len(was), kRingR, 0, 0, kUvRedU, 60, kUvRedU, kUvCentreV, red);
    }

    if (hp > 0) {
        hud_ring(h, kUvGreenU, 180, 180 + ring_deg(hp), white);
        if (bar_len(hp) > 0) hud_sprite(h, -bar_len(hp), kRingR, 0, 0, kUvGreenU, 60, kUvGreenU, kUvCentreV, white);
    }

    // the cap on the far end: across the ring where max HP stops, or standing
    // at the end of the bar once there is one
    if (bar > 0)
        hud_sprite_turned(h, -45, 1, 0, -3, 189, 49, 234, 53, 270, -bar, 0, white);
    else
        hud_sprite_turned(h, -45, 1, 0, -3, 189, 49, 234, 53, maxDeg, 0, 0, white);
    }

    // --- MP: the layout data hangs it at (-40, 31) from the frame's centre.
    // The tag sits there and the bar runs left from it, one unit to an MP,
    // closed by a cap. While it recharges the bar and tag blink. ---
    const float mpY = 31, mpRight = -40, mpLen = 100;
    float mp = st.mp < 0 ? 0 : (st.mp > 1 ? 1 : st.mp);
    if (st.mpBar) {
    const float blink = st.mpCharging && (g_frameNo % 80) < 40 ? 2.0f : 1.0f;
    const float charge[4] = {mpCharge[0] * blink, mpCharge[1] * blink, mpCharge[2] * blink, 1};
    const float tagCh[4] = {mpTagCh[0] * blink, mpTagCh[1] * blink, mpTagCh[2] * blink, 1};
    const float wellCh[4] = {0.656f, 0.031f, 0.031f, 1};
    hud_sprite(o, mpRight - mpLen, mpY - 5, mpRight, mpY + 5, 183, 49, 185, 59, st.mpCharging ? wellCh : mpWell);
    hud_sprite(o, mpRight - mpLen - 3, mpY - 5, mpRight - mpLen, mpY + 5, 154, 37, 157, 47, white);
    if (mp > 0) {
        if (st.mpCharging)
            hud_sprite(o, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 219, 40, 219, 46, charge);
        else
            hud_sprite(o, mpRight - mpLen * mp, mpY - 3, mpRight, mpY + 3, 216, 40, 216, 46, white);
    }
    hud_sprite(o, -40, mpY - 5, -16, mpY + 5, 157, 37, 181, 47, st.mpCharging ? tagCh : mpTag);
    }

    // --- portrait: KH2's own, placed by its own layout data, over the gauge ---
    g_out = &g_faceVerts;
    if (g_face && full) {
        if (hp <= 0) face_sprite(o, -35, -51, 36, 44, 143, 0, 214, 95);        // down
        else if (low || wince) face_sprite(o, -34, -50, 35, 45, 72, 0, 141, 95);   // hurt
        else face_sprite(o, -33, -50, 31, 45, 3, 0, 67, 95);
    }
    g_out = &g_verts;
}

void draw_status() {
    if (!g_state.gauge) return;
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
constexpr int kSlotSel[2] = {9, 22};         // selected slot: settles in from the right, then rests
constexpr int kSlotMid[2] = {16, 29};        // unselected slot
constexpr int kSlotLast[2] = {11, 24};       // unselected slot closing the menu
constexpr int kIconRing[2] = {45, 46};       // glow behind the selected slot's icon
constexpr int kBottom = 6;                   // trim under the last slot, on skins that have it
constexpr int kIcon[4] = {37, 41, 39, 44};   // keyblade, hat, potion, charm
// lists, by command: Magic, Items, Summon
constexpr int kSubHeader[4] = {-1, 60, 51, 73};
constexpr int kSubSel[4] = {-1, 57, 48, 70};
constexpr int kSubMid[4] = {-1, 59, 50, 72};
constexpr int kSubLast[4] = {-1, 58, 49, 71};

// Where KH2's code puts things, on its 512x416 screen. The last slot always
// sits at y 388 and the menu grows upward from it, 22 to a row, with the
// header one row above the first slot.
constexpr float kLeft = 28;                  // menu x; a wide screen adds nothing to it
constexpr float kLastSlotY = 388;
constexpr float kRowPitch = 22;
constexpr float kSelectedShift = 8;          // the selected slot sits this far right
constexpr float kLevelShift = 16;            // a list sits this far right of the menu under it
constexpr float kTextX = 10, kTextY = -8;    // label, from the slot's art
constexpr float kTextSize = 14.0f / 18;      // size 14 in a font drawn at 18
constexpr float kTextShadow = 2;             // black at half strength, down and right
constexpr float kGlyphDrop = 1.5f;           // the sheet's glyphs start this far down their cell (measured)
constexpr int kListRows = 5;                 // rows of a list on screen at once
}

// ---- KH2's system font ----
//
// The sheet is a grid, 28 glyphs to a row, each cell 18x24 KH2 units (twice
// that in the remastered sheet). Glyphs are stored grey and doubled when
// drawn, like every colour in KH2's 2D data.
constexpr int kFontCols = 28;
float g_textAcross = kUnitAcross;   // how much wider than tall text is drawn
bool g_textShadow = true;           // KH2 drops a shadow under its labels, KH1 does not
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
            x += 6 * k * g_textAcross;   // space, and anything the font lacks
            continue;
        }
        float u0 = (g % kFontCols) * kFontCellW * 2 / 1024.0f, v0 = (g / kFontCols) * kFontCellH * 2 / 512.0f;
        float u1 = u0 + kFontCellW * 2 / 1024.0f, v1 = v0 + kFontCellH * 2 / 512.0f;
        float x1 = x + kFontCellW * k * g_textAcross, y1 = y + kFontCellH * k;
        vert(x, y, u0, v0, c2);
        vert(x1, y, u1, v0, c2);
        vert(x, y1, u0, v1, c2);
        vert(x1, y, u1, v0, c2);
        vert(x1, y1, u1, v1, c2);
        vert(x, y1, u0, v1, c2);
        x += g_kfontWidth[g] * k * g_textAcross;
    }
    return x;
}

// White text with a dark edge, sized in KH2 units.
// A menu label. x, y are the top left of the text in pixels.
void menu_text(float x, float y, const char* s, const float col[4]) {
    const float unit = kh2_unit();
    if (g_kfont) {
        const float shadow[4] = {0, 0, 0, 0.5f * col[3]};
        const float off = cmd::kTextShadow * cmd::kTextSize;
        std::vector<Vertex>* was = g_out;
        g_out = &g_kfontVerts;
        if (g_textShadow) kfont_text(x + off * unit * g_textAcross, y + off * unit, unit, cmd::kTextSize, s, shadow);
        kfont_text(x, y, unit, cmd::kTextSize, s, col);
        g_out = was;
        return;
    }
    const float px = 12 * unit;
    const float edge[4] = {0.05f, 0.04f, 0.03f, 0.9f};
    const float o = px * 0.07f;
    y += 2 * unit;
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

// How far right of its origin a slot's art is drawn on a given frame. Skins
// differ in this, and a newly selected slot slides the last few units in. The
// label rides along.
float group_shift(const Seq& seq, int group, int frame) {
    if (group < 0 || group >= static_cast<int>(seq.animGroups.size())) return 0;
    const SeqAnimGroup& ag = seq.animGroups[group];
    if (ag.count == 0) return 0;
    const SeqAnim& a = seq.anims[ag.first];
    if (frame >= a.frame1 || a.frame1 <= a.frame0) return a.tx1;
    if (frame <= a.frame0) return a.tx0;
    return a.tx0 + (a.tx1 - a.tx0) * (frame - a.frame0) / (a.frame1 - a.frame0);
}

int group_last(const Seq& seq, int group) {
    return group >= 0 && group < static_cast<int>(seq.animGroups.size()) ? seq.animGroups[group].last : 0;
}

void draw_menu_kh2() {
    static const char* kItems[4] = {"Attack", "Magic", "Items", "Summon"};
    const OverlayState st = g_state;
    const Seq& seq = g_skin->seq;
    const float u = kh2_unit(), ux = kh2_unit_x();
    const int f = static_cast<int>(g_frameNo);
    const float bright[4] = {0.90f, 0.90f, 0.90f, 1};   // E6E6E6 and B4B4B4 in KH2's tables
    const float dim[4] = {0.71f, 0.71f, 0.71f, 1};
    const int mode = st.battle ? 1 : 0;

    // screen position of a slot: level 0 is the menu, 1 a list over it
    auto slot_x = [&](int level) { return (cmd::kLeft + cmd::kLevelShift * level) * ux; };
    auto slot_y = [&](int rows, int i) {
        return g_h - (416 - (cmd::kLastSlotY - cmd::kRowPitch * (rows - 1 - i))) * u;
    };
    auto label = [&](float x, float y, float shift, const char* s, const float col[4]) {
        menu_text(x + (shift + cmd::kTextX) * ux, y + (cmd::kTextY - cmd::kGlyphDrop) * u, s, col);
    };

    // a slot settles in over its first frames each time the selection lands on it
    static int lastMain = -1, lastSub = -1;
    static unsigned mainFrame = 0, subFrame = 0;
    if (st.selected != lastMain) lastMain = st.selected, mainFrame = g_frameNo;
    const int subKey = st.subOpen ? st.subSelected : -1;
    if (subKey != lastSub) lastSub = subKey, subFrame = g_frameNo;

    const float ox = slot_x(0), oy = slot_y(4, 0);
    g_out = &g_cmdVerts;

    // Coming up: KH2's own opening animation, with the labels fading in over
    // its last third of a second.
    const int since = g_testSince >= 0 ? g_testSince : static_cast<int>(g_frameNo - g_menuOpenFrame);
    const int openLast = group_last(seq, cmd::kOpen);
    if (!st.subOpen && since < openLast) {
        seq_draw(seq, cmd::kOpen, since, ox, oy, u);
        float fade = (since - (openLast - 20)) / 20.0f;
        if (fade > 0) {
            const float c[4] = {dim[0], dim[1], dim[2], fade};
            const float shift = group_shift(seq, cmd::kSlotMid[mode], 1 << 20);
            for (int i = 0; i < 4; ++i) label(ox, slot_y(4, i), shift, kItems[i], c);
        }
        g_out = &g_verts;
        return;
    }

    // A list covers the rows of the menu behind it from its header down.
    const int kind = st.selected >= 1 && st.selected <= 3 ? st.selected : 1;
    int first = 0, shown = 0;
    if (st.subOpen) {
        first = st.subSelected >= cmd::kListRows ? st.subSelected - cmd::kListRows + 1 : 0;
        shown = st.subCount - first < cmd::kListRows ? st.subCount - first : cmd::kListRows;
        if (shown < 1) shown = 1;
    }
    const float coveredFrom = st.subOpen ? slot_y(shown, -1) - cmd::kRowPitch * u / 2 : 1e9f;

    // Headers are authored one slot above the first. Their glow is a one-shot
    // that plays as the menu comes up, so here it rests on its end.
    seq_draw(seq, cmd::kHeader[mode], 1 << 20, ox, slot_y(4, -1), u);
    seq_draw(seq, cmd::kBottom, f, ox, oy, u);
    for (int i = 0; i < 4; ++i)
        if (i != st.selected) seq_draw(seq, i == 3 ? cmd::kSlotLast[mode] : cmd::kSlotMid[mode], f, ox, slot_y(4, i), u);
    // the selected slot is pushed right and is the only one showing its icon
    const int selAge = static_cast<int>(g_frameNo - mainFrame);
    const float sx = ox + cmd::kSelectedShift * ux, sy = slot_y(4, st.selected);
    const float selShift = group_shift(seq, cmd::kSlotSel[mode], selAge);
    seq_draw(seq, cmd::kSlotSel[mode], selAge, sx, sy, u);
    seq_draw(seq, cmd::kIconRing[mode], f, sx + selShift * ux, sy, u);
    seq_draw(seq, cmd::kIcon[st.selected], f, sx + selShift * ux, sy, u);
    {
        const float shift = group_shift(seq, cmd::kSlotMid[mode], 1 << 20);
        for (int i = 0; i < 4; ++i) {
            if (slot_y(4, i) >= coveredFrom) continue;
            if (i == st.selected) label(sx, sy, selShift, kItems[i], bright);
            else label(ox, slot_y(4, i), shift, kItems[i], dim);
        }
    }
    if (!st.subOpen) {
        g_out = &g_verts;
        return;
    }

    // The list: its own header, then its slots, bottom-aligned with the menu
    // and one step to the right of it.
    const float lx = slot_x(1);
    const int subAge = static_cast<int>(g_frameNo - subFrame);
    seq_draw(seq, cmd::kSubHeader[kind], f, lx, slot_y(shown, -1), u);
    for (int i = 0; i < shown; ++i) {
        const bool sel = first + i == st.subSelected && st.subCount > 0;
        const float y = slot_y(shown, i);
        const int group = sel ? cmd::kSubSel[kind] : (i == shown - 1 ? cmd::kSubLast[kind] : cmd::kSubMid[kind]);
        const float x = lx + (sel ? cmd::kSelectedShift * ux : 0);
        seq_draw(seq, group, sel ? subAge : f, x, y, u);
        const char* name = st.subCount == 0 ? "Nothing" : (st.subNames[first + i] ? st.subNames[first + i] : "?");
        label(x, y, group_shift(seq, group, sel ? subAge : 1 << 20), name, sel ? bright : dim);
    }
    g_out = &g_verts;
}

// ---- KH1's command menu ----
//
// What the combat mod shows on its own. The pieces are grey shapes on one
// sheet (command2/uitex.bin) that the game tints and stacks. Where they go,
// their colours and how they move are KH1's own numbers, from its menu code
// and the sprite files it loads (exchange/*_uibin_*.bin): positions on KH1's
// 512 wide screen, colours as bytes, key frames at 30 to the second with
// straight lines between them.
namespace kh1 {
struct Rect { int x0, y0, x1, y1; };             // on the sheet, two texels to a screen unit
constexpr float kSheetW = 256, kSheetH = 640;   // the pieces, with the words and icons sheet under them
constexpr int kWords = 384;                      // where that second sheet starts
constexpr Rect kHeader = {4, 4, 252, 44};
constexpr Rect kSlot = {4, 48, 252, 92};
constexpr Rect kSlotLast = {4, 96, 252, 140};
constexpr Rect kGlowTop = {4, 152, 234, 164};    // the lit slot's inside, upper and lower half
constexpr Rect kGlowBottom = {4, 170, 234, 194};
constexpr Rect kSelected = {4, 200, 250, 248};   // the lit slot's outline
constexpr Rect kContextTop = {4, 253, 228, 265};   // the same three for the context command's bar
constexpr Rect kContextBottom = {4, 270, 234, 296};
constexpr Rect kContext = {6, 300, 248, 348};
constexpr Rect kWordCommand = {4, kWords + 4, 156, kWords + 22};
constexpr Rect kWordItem = {158, kWords + 2, 238, kWords + 25};
constexpr Rect kWordMagic = {3, kWords + 26, 105, kWords + 48};
constexpr Rect kWordSummon = {106, kWords + 26, 237, kWords + 48};
// icons are 16 unit squares, the bottom row of the sheet 18 apart
constexpr Rect kIconKey = {4, kWords + 220, 36, kWords + 252};
constexpr Rect kIconHat = {40, kWords + 220, 72, kWords + 252};
constexpr Rect kIconPotion = {76, kWords + 220, 108, kWords + 252};
constexpr Rect kIconStar = {208, kWords + 132, 240, kWords + 164};
constexpr Rect kIconTriangle = {184, kWords + 220, 216, kWords + 252};

// One menu's colours, as the game's bytes: its frame, the word in its header,
// and the lit slot's outline and the colour its inside runs down to.
struct Look { unsigned char frame[3], word[3], edge[3], glow[3]; };
constexpr Look kField  = {{20, 56, 119}, {86, 126, 223}, {86, 125, 231}, {20, 56, 119}};
constexpr Look kBattle = {{109, 30, 32}, {200, 80, 80}, {216, 109, 100}, {109, 30, 32}};
constexpr Look kMagic  = {{65, 55, 118}, {145, 129, 234}, {117, 98, 218}, {48, 40, 93}};
constexpr Look kItems  = {{58, 86, 66}, {77, 216, 96}, {68, 170, 89}, {30, 69, 57}};
constexpr Look kSummon = {{123, 69, 31}, {213, 135, 38}, {213, 135, 38}, {97, 57, 31}};
constexpr unsigned char kContextEdge[3] = {110, 210, 100}, kContextMid[3] = {3, 11, 3}, kContextGlow[3] = {30, 110, 32};
constexpr unsigned char kIconTints[4][3] = {{238, 100, 100}, {145, 129, 234}, {77, 216, 96}, {213, 135, 38}};
constexpr float kLabel = 230 / 255.0f;       // every usable label, lit or not
constexpr float kLabelDim = 115 / 255.0f;    // a menu with a list open over it

// The 2D screen is 512 wide; I take it as 384 tall, which puts the menu the
// same 16 units from the bottom as from the left.
constexpr float kScreenH = 384;
constexpr float kMenuX = 16, kMenuY = 260;   // top left of the header
constexpr float kHeaderH = 20, kRow = 22;
constexpr float kMainW = 113, kListW = 112, kItemListW = 140;
constexpr float kFrameLeft = 62;             // a frame's left part keeps its size, the rest stretches
constexpr float kCursorX = 11, kCursorY = -2;   // the lit slot, from its row
constexpr float kCursorOver = 13;            // it overshoots to here on its way out
constexpr float kIconX = 13 + 75, kIconY = 4, kIconSize = 16;   // icon, from the lit slot; lists have their own x
constexpr float kListIconX = 87, kItemListIconX = 115;
constexpr float kLitInset = 8;               // the lit slot's inside is this much narrower than its outline
constexpr float kLabelX = 16, kLabelY = 2;
constexpr float kListX = 11;                 // a list, from the menu under it
constexpr float kListBottom = 368;
constexpr int kMagicRows = 8, kSummonRows = 7;
constexpr float kContextW = 147, kContextH = 24;   // the bar's outline; the game stretches the art too
}

float kh1_unit() { return g_h / kh1::kScreenH; }

// A piece stretched over a rectangle (pixels), shaded from one colour at its
// top to another at its bottom. Colours are bytes.
void kh1_quad(float x0, float y0, float x1, float y1, const kh1::Rect& r, const unsigned char top[3],
              const unsigned char bottom[3], float alpha = 1, float bright = 1) {
    // the pieces are mid grey, so twice the tint gives the colour asked for
    const float k = 2 * bright / 255.0f;
    const float ct[4] = {top[0] * k, top[1] * k, top[2] * k, alpha}, cb[4] = {bottom[0] * k, bottom[1] * k, bottom[2] * k, alpha};
    const float u0 = r.x0 / kh1::kSheetW, u1 = r.x1 / kh1::kSheetW;
    const float v0 = r.y0 / kh1::kSheetH, v1 = r.y1 / kh1::kSheetH;
    vert(x0, y0, u0, v0, ct);
    vert(x1, y0, u1, v0, ct);
    vert(x0, y1, u0, v1, cb);
    vert(x1, y0, u1, v0, ct);
    vert(x1, y1, u1, v1, cb);
    vert(x0, y1, u0, v1, cb);
}

// A piece at its own size with its top left at x, y (pixels), unless a width
// in units is given.
void kh1_piece(float x, float y, const kh1::Rect& r, const unsigned char tint[3], float alpha = 1, float bright = 1,
               float width = 0) {
    const float u = kh1_unit();
    const float w = width > 0 ? width : (r.x1 - r.x0) * 0.5f;
    kh1_quad(x, y, x + w * u, y + (r.y1 - r.y0) * 0.5f * u, r, tint, tint, alpha, bright);
}

// A frame piece drawn to a width: the slanted left end as it is, the rest
// stretched or squeezed to fit.
void kh1_frame(float x, float y, float width, const kh1::Rect& r, const unsigned char tint[3], float alpha, float bright) {
    const float u = kh1_unit();
    const int split = r.x0 + static_cast<int>(kh1::kFrameLeft * 2);
    const float y1 = y + (r.y1 - r.y0) * 0.5f * u, xm = x + kh1::kFrameLeft * u;
    kh1_quad(x, y, xm, y1, {r.x0, r.y0, split, r.y1}, tint, tint, alpha, bright);
    kh1_quad(xm, y, x + width * u, y1, {split, r.y0, r.x1, r.y1}, tint, tint, alpha, bright);
}

// A lit shape: shadow, outline, and an inside that is black at the top and
// runs down to a colour. x, y is the outline's top left, width in units. The
// inside sits 6 in, its upper part from 2 down and its lower from 9, and is
// narrower than the outline by `inset`.
void kh1_lit(float x, float y, float width, const kh1::Rect& edge, const kh1::Rect& top, const kh1::Rect& bottom,
             const unsigned char edgeTint[3], const unsigned char mid[3], const unsigned char glow[3], float alpha,
             float inset = kh1::kLitInset, float insetTop = kh1::kLitInset) {
    const float u = kh1_unit();
    const unsigned char black[3] = {0, 0, 0};
    kh1_piece(x + 3 * u, y + 3 * u, edge, black, alpha * 0.5f, 1, width);
    kh1_piece(x, y, edge, edgeTint, alpha, 1, width);
    // The two halves have soft edges where they meet, and laid end to end the
    // outline shows through as a line. Let each run a unit and a half into
    // the other, so each one's soft edge lies on the other's solid part.
    const float lap = 1.5f * u;
    kh1_quad(x + 6 * u, y + 9 * u - lap, x + (6 + width - inset) * u, y + 22 * u, bottom, mid, glow, alpha);
    kh1_quad(x + 6 * u, y + 2 * u, x + (6 + width - insetTop) * u, y + 9 * u + lap, top, black, mid, alpha);
}

// Position of something that moves in a straight line between key frames,
// KH1's way. Keys are at 30 to the second; the overlay counts frames at 60.
float kh1_keys(unsigned since, const float* at, const float* value, int count) {
    const float t = since / 2.0f;
    if (t <= at[0]) return value[0];
    for (int i = 1; i < count; ++i)
        if (t < at[i]) return value[i - 1] + (value[i] - value[i - 1]) * (t - at[i - 1]) / (at[i] - at[i - 1]);
    return value[count - 1];
}

struct Kh1Menu {
    float x, y, width;        // top left of the header, units
    int rows, selected;       // selected -1: no lit slot
    const kh1::Look* look;
    const kh1::Rect* word;
    const char* const* names;
    const kh1::Rect* icon;
    const unsigned char* iconTint;
    unsigned cursorAge;       // frames since the cursor landed on its row
    float headerAlpha;        // the header's own, on top of the menu's: it gives way to the context command
    float iconX;              // icon, from the lit slot's left
    int gapRow;               // a row left empty with no lit slot: where the cursor was when a list opened
    float alpha, bright;      // the whole menu's
    float coverTop, coverBottom;   // labels of rows between these (pixels) are under a list and left out
};

void kh1_menu(const Kh1Menu& m) {
    const float u = kh1_unit();
    const float x = m.x * u, top = g_h - (kh1::kScreenH - m.y) * u;
    const float labelShade = m.bright < 1 ? kh1::kLabelDim : kh1::kLabel;
    const float label[4] = {labelShade, labelShade, labelShade, m.alpha};
    auto covered = [&](float y) { return y + kh1::kRow * u > m.coverTop + 4 * u && y < m.coverBottom - 4 * u; };
    g_out = &g_cmdVerts;
    if (m.headerAlpha > 0) {
        kh1_frame(x, top, m.width, kh1::kHeader, m.look->frame, m.alpha * m.headerAlpha, m.bright);
        kh1_piece(x + 4 * u, top + 6 * u, *m.word, m.look->word, m.alpha * m.headerAlpha, m.bright);
    }
    for (int i = 0; i < m.rows; ++i) {
        // the cursor's row has no slot of its own: the lit one stands in the gap
        if (i == m.selected || i == m.gapRow) continue;
        const float y = top + (kh1::kHeaderH + i * kh1::kRow) * u;
        kh1_frame(x, y, m.width, i == m.rows - 1 ? kh1::kSlotLast : kh1::kSlot, m.look->frame, m.alpha, m.bright);
        if (!covered(y)) menu_text(x + kh1::kLabelX * u, y + (kh1::kLabelY - cmd::kGlyphDrop) * u, m.names[i], label);
    }
    if (m.selected < 0 || m.selected >= m.rows) return;

    // The lit slot starts on its row, is past its place a frame later and
    // settles the frame after, showing up over the first of those. Its icon
    // comes in from the left during that first frame.
    static const float kAt[3] = {0, 1, 2}, kOut[3] = {0, kh1::kCursorOver, kh1::kCursorX};
    static const float kAppear[2] = {0, 1}, kShow[2] = {0, 1}, kFly[2] = {-139, 0};
    const unsigned char black[3] = {0, 0, 0};
    const float out = kh1_keys(m.cursorAge, kAt, kOut, 3);
    const float show = kh1_keys(m.cursorAge, kAppear, kShow, 2) * m.alpha;
    const float y = top + (kh1::kHeaderH + m.selected * kh1::kRow) * u;
    const float sx = x + out * u, sy = y + kh1::kCursorY * u;
    g_out = &g_cmdVerts;
    // as wide as the menu it belongs to
    kh1_lit(sx, sy, m.width, kh1::kSelected, kh1::kGlowTop, kh1::kGlowBottom, m.look->edge, black, m.look->glow, show);
    const float ix = sx + (m.iconX + kh1_keys(m.cursorAge, kAppear, kFly, 2)) * u, iy = sy + kh1::kIconY * u;
    kh1_quad(ix, iy, ix + kh1::kIconSize * u, iy + kh1::kIconSize * u, *m.icon, m.iconTint, m.iconTint, show);
    if (!covered(y)) {
        const float lit[4] = {kh1::kLabel, kh1::kLabel, kh1::kLabel, m.alpha};
        menu_text(sx + kh1::kLabelX * u, y + (kh1::kLabelY - cmd::kGlyphDrop) * u, m.names[m.selected], lit);
    }
}

void draw_menu_kh1() {
    static const char* kItems[4] = {"Attack", "Magic", "Items", "Summon"};
    static const kh1::Rect kIcons[4] = {kh1::kIconKey, kh1::kIconHat, kh1::kIconPotion, kh1::kIconStar};
    const OverlayState st = g_state;
    const float u = kh1_unit();
    const float was = g_textAcross;
    g_textAcross = 1;   // KH1's screen is square, unlike KH2's
    g_textShadow = false;

    const int sel = st.selected >= 0 && st.selected < 4 ? st.selected : 0;
    const bool still = g_testSince >= 0;   // the test programs want the settled picture

    // when the cursor last landed somewhere, and when the list last opened
    static int lastMain = -1, lastSub = -1;
    static bool lastOpen = false;
    static unsigned mainFrame = 0, subFrame = 0, openFrame = 0;
    if (sel != lastMain || (lastOpen && !st.subOpen)) lastMain = sel, mainFrame = g_frameNo;
    if (st.subOpen && !lastOpen) openFrame = g_frameNo;
    const int subKey = st.subOpen ? st.subSelected : -1;
    if (subKey != lastSub) lastSub = subKey, subFrame = g_frameNo;
    lastOpen = st.subOpen;
    const unsigned settled = 1000;

    // The list: bottom on the menu's bottom line, except that a short Magic
    // or Items list hangs from the row it came from, and Summon opens beside
    // its own row when it fits.
    const int cap = sel == 3 ? kh1::kSummonRows : kh1::kMagicRows;
    int first = 0, shown = 0;
    float listY = 0;
    if (st.subOpen) {
        first = st.subSelected >= cap ? st.subSelected - cap + 1 : 0;
        shown = st.subCount - first < cap ? st.subCount - first : cap;
        if (shown < 1) shown = 1;
        listY = kh1::kListBottom - kh1::kHeaderH - kh1::kRow * shown;
        if (sel == 1 && shown < 3) listY = kh1::kMenuY + kh1::kRow;
        if (sel == 2 && shown < 2) listY = kh1::kMenuY + 2 * kh1::kRow;
        const float beside = kh1::kMenuY + kh1::kRow * sel;
        if (sel == 3 && beside + kh1::kHeaderH + kh1::kRow * shown <= kh1::kListBottom + 1) listY = beside;
    }

    // The context command lies over the header, wider than the menu and
    // pointing right. It takes 7 frames to come in, sliding the last 10
    // units, and 7 to fade out where it is. A new label comes in afresh.
    static const char* shownContext = nullptr;
    static unsigned contextFrame = 0;
    static bool contextUp = false;
    const bool want = st.context && !st.subOpen;
    if (want != contextUp || (want && st.context != shownContext)) contextUp = want, contextFrame = g_frameNo;
    if (want) shownContext = st.context;
    static const float kIn[2] = {0, 7}, kFade[2] = {0, 1}, kContextSlide[2] = {-10, 0};
    const unsigned contextAge = still ? settled : g_frameNo - contextFrame;
    const float shade = contextUp ? kh1_keys(contextAge, kIn, kFade, 2) : 1 - kh1_keys(contextAge, kIn, kFade, 2);

    Kh1Menu main{};
    main.headerAlpha = 1 - shade;   // the header makes way for it
    main.x = kh1::kMenuX;
    main.y = kh1::kMenuY;
    main.width = kh1::kMainW;
    main.rows = 4;
    main.selected = st.subOpen ? -1 : sel;   // a menu with a list over it has no cursor
    main.look = st.battle ? &kh1::kBattle : &kh1::kField;
    main.word = &kh1::kWordCommand;
    main.names = kItems;
    main.icon = &kIcons[sel];
    main.iconTint = kh1::kIconTints[sel];
    main.cursorAge = still ? settled : g_frameNo - mainFrame;
    main.iconX = kh1::kIconX;
    main.gapRow = st.subOpen ? sel : -1;   // its row stays empty
    main.alpha = 1;
    // and goes to half brightness, except the red battle frames
    main.bright = st.subOpen && !st.battle ? 0.5f : 1;
    main.coverTop = main.coverBottom = -1;
    if (st.subOpen) {
        main.coverTop = g_h - (kh1::kScreenH - listY) * u;
        main.coverBottom = main.coverTop + (kh1::kHeaderH + kh1::kRow * shown) * u;
    }
    kh1_menu(main);

    if (st.subOpen) {
        // every piece of a list comes in together: from the menu's edge, past
        // its place and back over two frames, showing up over the first
        static const float kAt[3] = {0, 1, 2}, kSlide[3] = {-kh1::kListX, 2, 0};
        static const float kAppear[2] = {0, 1}, kShow[2] = {0, 1};
        const unsigned age = still ? settled : g_frameNo - openFrame;
        const char* names[kh1::kMagicRows];
        for (int i = 0; i < shown; ++i)
            names[i] = st.subCount == 0 ? "Nothing" : (st.subNames[first + i] ? st.subNames[first + i] : "?");
        Kh1Menu list{};
        list.headerAlpha = 1;
        list.x = kh1::kMenuX + kh1::kListX + kh1_keys(age, kAt, kSlide, 3);
        list.y = listY;
        list.width = sel == 2 ? kh1::kItemListW : kh1::kListW;
        list.rows = shown;
        list.selected = st.subCount > 0 ? st.subSelected - first : -1;
        list.look = sel == 2 ? &kh1::kItems : (sel == 3 ? &kh1::kSummon : &kh1::kMagic);
        list.word = sel == 2 ? &kh1::kWordItem : (sel == 3 ? &kh1::kWordSummon : &kh1::kWordMagic);
        list.names = names;
        list.icon = &kIcons[sel];
        list.iconTint = kh1::kIconTints[sel];
        list.cursorAge = still ? settled : g_frameNo - subFrame;
        list.iconX = sel == 2 ? kh1::kItemListIconX : kh1::kListIconX;
        list.gapRow = -1;
        list.alpha = kh1_keys(age, kAppear, kShow, 2);
        list.bright = 1;
        list.coverTop = list.coverBottom = -1;
        kh1_menu(list);
    }

    if (shownContext && shade > 0) {
        const float slide = contextUp ? kh1_keys(contextAge, kIn, kContextSlide, 2) : 0;
        const float bx = (kh1::kMenuX - 8 + slide) * u, by = g_h - (kh1::kScreenH - (kh1::kMenuY - 8)) * u;
        g_out = &g_cmdVerts;
        kh1_lit(bx, by, kh1::kContextW, kh1::kContext, kh1::kContextTop, kh1::kContextBottom, kh1::kContextEdge,
                kh1::kContextMid, kh1::kContextGlow, shade, kh1::kContextW - 136, kh1::kContextW - 134);
        const float gx = bx + 14 * u, gy = by + 5 * u;
        kh1_quad(gx, gy, gx + 16 * u, gy + 16 * u, kh1::kIconTriangle, kh1::kContextEdge, kh1::kContextEdge, shade, 0.6f);
        const float white[4] = {kh1::kLabel, kh1::kLabel, kh1::kLabel, shade};
        menu_text(bx + 35 * u, by + (5 - cmd::kGlyphDrop) * u, shownContext, white);
    }
    g_textAcross = was;
    g_textShadow = true;
    g_out = &g_verts;
}

void draw_menu() {
    if (!g_state.menu) return;
    // On its own the combat mod shows KH1's menu; the KH2 HUD mod brings KH2's.
    if (!g_state.kh2Hud && g_kh1) {
        g_cmd = g_kh1;
        draw_menu_kh1();
        return;
    }
    // KH2's standard menu on its own; with the KH2 HUD mod, the world's
    g_skin = skin_for_world(g_state.kh2Hud ? g_state.world : -1);
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

// More than one mod built on this file can be loaded at once. Only the first
// hooks the swap chain; the rest sign up here and are called from its hooks.
// The list lives in shared memory named after the process, so separate dlls
// find the same one.
struct OverlayHooks {
    static constexpr int kMax = 8;
    void (*present[kMax])(IDXGISwapChain*, UINT flags);
    void (*resize[kMax])(IDXGISwapChain*);
};
OverlayHooks* g_hooks = nullptr;

// Opens the list, making it if this is the first overlay in the process.
bool open_hooks(bool* first) {
    char name[64];
    sprintf_s(name, "Local\\recom_overlay_%lu", GetCurrentProcessId());
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(OverlayHooks), name);
    if (!h) return false;
    *first = GetLastError() != ERROR_ALREADY_EXISTS;
    g_hooks = static_cast<OverlayHooks*>(MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(OverlayHooks)));
    return g_hooks != nullptr;
}

void all_present(IDXGISwapChain* sc, UINT flags) {
    for (auto fn : g_hooks->present)
        if (fn) fn(sc, flags);
}

HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    all_present(sc, flags);
    return g_realPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* params) {
    all_present(sc, flags);
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

void all_resize(IDXGISwapChain* sc) {
    for (auto fn : g_hooks->resize)
        if (fn) fn(sc);
}

HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    all_resize(sc);
    return g_realResize(sc, count, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE resize1_hook(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                       const UINT* masks, IUnknown* const* queues) {
    all_resize(sc);
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
    bool first = false;
    if (!open_hooks(&first)) {
        if (log) log("overlay: could not share the hook list");
        return false;
    }
    for (int i = 0; i < OverlayHooks::kMax; ++i) {
        if (g_hooks->present[i]) continue;
        g_hooks->resize[i] = &before_resize;
        g_hooks->present[i] = &on_present;
        break;
    }
    if (!first) {
        if (log) log("overlay: another mod has the swap chain hooked, drawing from its hooks");
        return true;
    }
    if (log) log("overlay: making the probe swap chain");
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
    if (log) log("overlay: probe made, patching");
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
