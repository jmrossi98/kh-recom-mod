#pragma once

// What the overlay should show this frame. Written by the game-side code,
// read on the render thread.
struct OverlayState {
    static constexpr int kMaxEntries = 16;

    bool visible = false;   // anything at all: in a battle or walking around
    bool battle = false;    // in a battle: the command menu shows and its header goes red
    int selected = 0;       // highlighted command on the main menu, 0..3
    int world = 0;          // Re:CoM world number, picks the menu's skin

    // Second level (Magic / Items / Summon). Replaces the main rows while open.
    bool subOpen = false;
    const char* subTitle = "";
    int subCount = 0;
    int subSelected = 0;
    const char* subNames[kMaxEntries] = {};

    // status gauges, bottom right
    int hp = 0;
    int hpMax = 0;
    float mp = 1.0f;          // 0..1
    bool mpCharging = false;  // empty and refilling, magic unavailable
};

OverlayState& overlay_state();
bool overlay_install(void (*log)(const char* fmt, ...));

// For the test harness: draw as if the menu came up this many frames ago,
// so any point of its opening animation can be looked at.
void overlay_test_since(int frames);

// Draws straight into the swap chain's back buffer. For the test harness.
struct IDXGISwapChain;
bool overlay_draw_now(IDXGISwapChain* sc, void (*log)(const char* fmt, ...));
