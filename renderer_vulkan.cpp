#include "frame.h"

#include "gpu.h"
#include "chaos_monkey.h"

#include <std/mem/obj_pool.h>

#include <imgui.h>
#include <plt/window.h>
#include <imgui_impl_vulkan.h>

using namespace stl;

namespace {
    constexpr u32 maxTextureCount = 16384;

    struct VulkanRenderer final: Renderer {
        Gpu* gpu = nullptr;

        void beginFrame(u32 width, u32 height) override;
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
    };
}

void VulkanRenderer::beginFrame(u32 width, u32 height) {
    if (gpu->rebuild || gpu->present.width != (int)width || gpu->present.height != (int)height) {
        gpu->createSwapchain(width, height);

        if (gpu->linearHdr) {
            gpu->createSceneTarget(width, height);
        }

        gpu->rebuild = false;
    }

    ImGui_ImplVulkan_NewFrame();
}

bool VulkanRenderer::endFrame(ImDrawData* draw) {
    gpu->frameRender(draw);
    gpu->framePresent();

    return !gpu->rebuild;
}

u32 VulkanRenderer::maxTextureSide() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    return (u32)(pio.Renderer_TextureMaxWidth < pio.Renderer_TextureMaxHeight ? pio.Renderer_TextureMaxWidth : pio.Renderer_TextureMaxHeight);
}

u32 VulkanRenderer::maxTextures() {
    return maxTextureCount;
}

Renderer* createVulkanRenderer(ObjPool& pool, Gpu& gpu) {
    VulkanRenderer* renderer = pool.make<VulkanRenderer>();

    renderer->gpu = &gpu;
    gpu.present.clear.color.float32[0] = 0.1f;
    gpu.present.clear.color.float32[1] = 0.1f;
    gpu.present.clear.color.float32[2] = 0.1f;
    gpu.present.clear.color.float32[3] = 1.0f;

    return renderer;
}

Renderer* Renderer::create(ObjPool& pool, plt::Window& window) {
    GpuOptions wants;

    wants.chaos = ChaosMonkey::create(pool);
    wants.textures = maxTextureCount;

    return createVulkanRenderer(pool, *Gpu::createFor(pool, window, wants));
}
