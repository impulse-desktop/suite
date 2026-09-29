#pragma once

#include <imgui.h>
#include <imgui_internal.h>

// The WM's contract with the vendored ImGui fork. Everything the tree
// relies on beyond the stock public API is redeclared or asserted here,
// so an upstream merge that loses a piece fails this header instead of
// surfacing at random call sites. Keep the fork and this list in sync —
// this IS the merge checklist.

// fork: per-window drop shadows; the desktop chrome, dock and window
// glow hook ImGuiIO::WindowShadowCallback
static_assert(sizeof(ImGuiWindowShadowCallback) == sizeof(void (*)()), "the shadow callback typedef left the fork");
static_assert(sizeof(ImGuiIO::WindowShadowCallback) == sizeof(ImGuiWindowShadowCallback), "ImGuiIO lost the shadow callback slot");

// upstream-internal api the wm is built on; the fork must keep exporting
// it: the fixed side bars carve the dock and the top bar out of the work
// area, and the docking branch supplies the tiling
namespace ImGui {
    IMGUI_API bool BeginViewportSideBar(const char* name, ImGuiViewport* viewport, ImGuiDir dir, float size, ImGuiWindowFlags window_flags);
}

// fork: the vulkan backend composes in linear light with per-texture
// color metadata — the renderer feeds every client surface's color
// description through these before drawing it. Redeclared without the
// backend header on purpose: a signature drift in the fork collides with
// these declarations in the renderer's translation unit.
void ImGui_ImplVulkan_SetSdrWhite(float nits);
void ImGui_ImplVulkan_SetTextureColor(int source, int primaries, float reference_nits, float min_nits, float max_nits, const float* primaries_to_bt2020, const float* gamma, int alpha_mode, int yuv_coefficients, int yuv_range, int yuv_chroma_location, int yuv_bits);
