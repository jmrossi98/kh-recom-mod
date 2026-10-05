#define _CRT_SECURE_NO_WARNINGS   // getenv is fine for a test tool
// Renders the overlay into an offscreen swap chain and saves the result as a
// BMP, so the menu can be worked on without starting the game.
//
//   overlay_test.exe out.bmp [width height] [selected] [sub]
#include <cstring>
#include "../src/overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "overlay.bmp";
    UINT w = argc > 3 ? atoi(argv[2]) : 1920;
    UINT h = argc > 3 ? atoi(argv[3]) : 1080;
    int selected = argc > 4 ? atoi(argv[4]) : 0;

    HWND wnd = CreateWindowExA(0, "STATIC", "overlay test", WS_OVERLAPPEDWINDOW, 0, 0, w, h, nullptr, nullptr,
                               nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
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
    if (FAILED(hr)) { printf("device failed %08lX\n", hr); return 1; }

    ID3D11Texture2D* back = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    const float clear[4] = {0.16f, 0.17f, 0.20f, 1};   // roughly a dim battle scene
    ctx->ClearRenderTargetView(rtv, clear);

    overlay_state().visible = true;
    overlay_state().battle = selected >= 0;   // a negative row shows the walking-around HUD
    if (selected < 0) selected = 0;
    overlay_state().selected = selected;
    overlay_state().mp = 0.62f;
    // OVERLAY_MODS: "combat" or "hud" to draw as if only that mod were installed
    const char* mods = getenv("OVERLAY_MODS");
    overlay_state().menu = !mods || !strcmp(mods, "combat");
    overlay_state().gauge = !mods || !strcmp(mods, "hud");
    overlay_state().kh2Hud = overlay_state().gauge;
    overlay_state().mpBar = !mods;
    overlay_state().context = getenv("OVERLAY_CONTEXT");   // e.g. Examine
    overlay_state().hp = 54;
    overlay_state().hpMax = 80;
    // OVERLAY_WORLD picks the menu skin, OVERLAY_FRAMES how many frames to run
    // before the one that is saved (the menu's opening takes a couple of seconds)
    if (const char* wv = getenv("OVERLAY_WORLD")) overlay_state().world = atoi(wv);
    const char* fv = getenv("OVERLAY_FRAMES");
    overlay_test_since(fv ? atoi(fv) : 100000);
    if (argc > 5) {
        overlay_state().mp = 0.3f;
        overlay_state().mpCharging = true;
        overlay_state().hp = 14;
        // any fifth argument: show a submenu instead, "selected" picks the row
        static const char* names[] = {"Fire", "Blizzard", "Thunder", "Cure", "Gravity", "Stop"};
        OverlayState& o = overlay_state();
        o.subOpen = true;
        o.subTitle = "MAGIC";
        o.subCount = 6;
        o.subSelected = selected;
        for (int i = 0; i < 6; ++i) o.subNames[i] = names[i];
    }
    if (!overlay_draw_now(sc, &logf)) { printf("overlay did not initialise\n"); return 1; }

    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&td, nullptr, &staging);
    ctx->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) { printf("map failed\n"); return 1; }

    std::vector<unsigned char> rows(w * h * 3);
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* src = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch;
        unsigned char* dst = &rows[(h - 1 - y) * w * 3];
        for (UINT x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    ctx->Unmap(staging, 0);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(rows.size());
    FILE* f = nullptr;
    fopen_s(&f, out, "wb");
    if (!f) { printf("cannot write %s\n", out); return 1; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(rows.data(), 1, rows.size(), f);
    fclose(f);
    printf("wrote %s (%ux%u)\n", out, w, h);
    return 0;
}
