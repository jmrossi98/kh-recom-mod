#define _CRT_SECURE_NO_WARNINGS   // getenv is fine for a test tool
// Same idea as overlay_test.cpp, but with a D3D12 swap chain like the game's,
// so the D3D11On12 path can be exercised without starting the game.
//
//   overlay_test12.exe out.bmp [width height] [selected] [sub]
#include <cstring>
#include "../src/overlay.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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

static ID3D12Device* dev;
static ID3D12CommandQueue* queue;
static ID3D12CommandAllocator* alloc;
static ID3D12GraphicsCommandList* list;
static ID3D12Fence* fence;
static UINT64 fenceValue = 0;
static HANDLE fenceEvent;

static void run_and_wait() {
    list->Close();
    ID3D12CommandList* lists[1] = {list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
    alloc->Reset();
    list->Reset(alloc, nullptr);
}

static void wait_queue() {
    queue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, fenceEvent);
    WaitForSingleObject(fenceEvent, INFINITE);
}

static void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "overlay12.bmp";
    UINT w = argc > 3 ? atoi(argv[2]) : 1920;
    UINT h = argc > 3 ? atoi(argv[3]) : 1080;
    int selected = argc > 4 ? atoi(argv[4]) : 0;

    HWND wnd = CreateWindowExA(0, "STATIC", "overlay test", WS_OVERLAPPEDWINDOW, 0, 0, w, h, nullptr, nullptr,
                               nullptr, nullptr);
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&dev)))) { printf("no d3d12 device\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                reinterpret_cast<void**>(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList),
                           reinterpret_cast<void**>(&list));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, wnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { printf("swap chain failed %08lX\n", hr); return 1; }
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc));

    UINT index = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* back = nullptr;
    sc->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back));

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(back, nullptr, rtv);

    const float clear[4] = {0.16f, 0.17f, 0.20f, 1};
    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    run_and_wait();

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
    // OVERLAY_HP and OVERLAY_HPMAX set the gauge, OVERLAY_MP the MP bar (0-100,
    // negative to show it recharging)
    if (const char* v = getenv("OVERLAY_HP")) overlay_state().hp = atoi(v);
    if (const char* v = getenv("OVERLAY_HPMAX")) overlay_state().hpMax = atoi(v);
    if (const char* v = getenv("OVERLAY_MP")) {
        overlay_state().mp = abs(atoi(v)) / 100.0f;
        overlay_state().mpCharging = atoi(v) < 0;
    }
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
    // OVERLAY_HURT: lose HP between the two frames, to see the damage effect
    const bool hurt = getenv("OVERLAY_HURT") != nullptr;
    if (hurt) overlay_state().hp = 72;
    if (!overlay_draw_now(sc, &logf)) { printf("overlay did not initialise\n"); return 1; }
    // a second frame on the same buffer, to catch anything that only breaks on reuse
    if (hurt) overlay_state().hp = 44;
    overlay_draw_now(sc, &logf);
    wait_queue();

    D3D12_RESOURCE_DESC bd = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&bd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                 __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback));

    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    run_and_wait();

    void* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, &data))) { printf("map failed\n"); return 1; }
    std::vector<unsigned char> rows(w * h * 3);
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* s = static_cast<const unsigned char*>(data) + fp.Offset + y * fp.Footprint.RowPitch;
        unsigned char* d = &rows[(h - 1 - y) * w * 3];
        for (UINT x = 0; x < w; ++x) {
            d[x * 3 + 0] = s[x * 4 + 2];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 0];
        }
    }
    readback->Unmap(0, nullptr);

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

    // presenting afterwards must still work
    hr = sc->Present(0, 0);
    printf("present %08lX\n", hr);
    return 0;
}
