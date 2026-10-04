#pragma once

// What the overlay should show this frame. Written by the game-side code,
// read on the render thread.
struct OverlayState {
    bool visible = false;   // only while a battle is running
    int selected = 0;       // highlighted command, 0..3
};

OverlayState& overlay_state();
bool overlay_install(void (*log)(const char* fmt, ...));

// Draws straight into the swap chain's back buffer. For the test harness.
struct IDXGISwapChain;
bool overlay_draw_now(IDXGISwapChain* sc, void (*log)(const char* fmt, ...));
