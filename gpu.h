#pragma once

#include "ui.h"

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/vector.h>

#include <plt/window.h>
#include <vulkan/vulkan.h>

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;
}

struct ImGuiPlt;
struct ChaosMonkey;
struct ImDrawCmd;
struct ImDrawList;
struct ImDrawData;

// What every tool's window is made of: one Vulkan device, the window's
// presenter (its surface, swapchain and the frames drawn into it), ImGui's
// Vulkan backend over it, the textures a tool shows and the frame driver
// behind plt's inverted loop. The HDR path lives here too: an FP16
// linear-light scene the tool draws into, encoded to the PQ swapchain by
// the output stage. All of it is the Gpu object's, made in a pool.

// the ui scale: IM_SCALE, 1 without it
float scaleFromEnv();
// a design length in whole pixels at the scale, never under one for a
// positive length; positions are sums of these, so they stay whole too
float scaledPx(Design d, float scale);
// ImGui's style at the scale: its defaults through its own scaling
ImGuiStyle scaledStyle(float scale);

// a full-window panel that replaces the tool's ui (not an overlay) when
// something goes wrong: reads like a message from the compositor, a
// heading with the tool's name, the error text, and a single Exit button;
// -1 on exit
int drawErrorPanel(stl::StringView tool, float scale, stl::StringView msg);

// one swapchain image and what records into it
struct Frame {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkFence fence;
};

// the semaphores of one frame in flight: an image acquired, a frame
// rendered into it; one pair more than images, as an acquire hands its
// image out only afterwards
struct Sync {
    VkSemaphore acquired;
    VkSemaphore rendered;
};

// the window's surface, its swapchain and the frames drawn into it. The
// render pass outlives the swapchain: the format never changes
struct Presenter {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format = {};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    stl::Vector<Frame> frames;
    stl::Vector<Sync> syncs;
    // the frame whose image the last acquire handed out, and the sync
    // pair the next acquire uses
    u32 frameIndex = 0;
    u32 syncIndex = 0;
    int width = 0;
    int height = 0;
    VkClearValue clear = {};
};

// who the device is for and what is wanted of it: the tool's name in the
// trace lines, the ui scale, the fault seam, IM_TRACE_FRAMES; an HDR
// swapchain (the colour space extension), a buffer shared by another
// process (imported on the device its UUID names, with the dma-buf
// extensions), and how many textures the tool registers with ImGui at once
struct GpuOptions {
    stl::StringView tool;
    float scale = 1.f;
    ChaosMonkey* chaos = nullptr;
    bool traceFrames = false;
    bool hdr = false;
    bool sharedBuffer = false;
    const u8* deviceUuid = nullptr;
    u32 textures = 1;
};

// a sampled image and its descriptors
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    // ImGui's descriptor, for AddImage; the image pipeline's own, in HDR
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSet imageSet = VK_NULL_HANDLE;
};

struct Gpu {
    stl::StringView tool;
    float scale = 1.f;
    ChaosMonkey* chaos = nullptr;
    bool traceFrames = false;

    VkAllocationCallbacks* alloc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    u32 queueFamily = (u32)-1;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    Presenter present;
    // the swapchain no longer fits the window: the next frame rebuilds it
    bool rebuild = false;
    // the HDR scene is up: ImGui draws into it, the output stage encodes it
    bool linearHdr = false;
    // the HDR scene's white in nits, what the output stage scales it by
    float sdrWhiteNits = 203.f;

    // the frame trace's clocks: when the last frame began, how many there
    // were, and how long each phase of this one took
    u64 frameBegan = 0;
    u64 frameCount = 0;
    u64 acquireNs = 0;
    u64 fenceNs = 0;
    u64 submitNs = 0;
    u64 presentNs = 0;

    // the HDR scene: its pass, its FP16 target, the output stage that
    // encodes it to the swapchain, and the image pipeline drawImage runs
    // from an ImGui draw callback, PQ decoding an image into the scene
    VkRenderPass scenePass = VK_NULL_HANDLE;
    VkImage sceneImage = VK_NULL_HANDLE;
    VkDeviceMemory sceneMemory = VK_NULL_HANDLE;
    VkImageView sceneView = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
    VkSampler sceneSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout outputSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet outputSet = VK_NULL_HANDLE;
    VkPipelineLayout outputPipelineLayout = VK_NULL_HANDLE;
    VkPipeline outputPipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout imageSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout imagePipelineLayout = VK_NULL_HANDLE;
    VkPipeline imagePipeline = VK_NULL_HANDLE;

    // the instance, the device and its queue, the descriptor pool; every
    // object registers its teardown with the pool as it comes up
    static Gpu* create(stl::ObjPool& pool, const GpuOptions& options);
    // all of it for a tool that draws by ImGui alone: the device, the
    // presenter on the window at its size, ImGui and its backend
    static Gpu* createFor(stl::ObjPool& pool, plt::Window& window, const GpuOptions& options);

    // a Vulkan result the caller checks; a failure ends the tool with the
    // error, as vulkan_error, or at the named site (IM_CHAOS vulkan-at=SITE)
    void vkc(VkResult e);
    void vkcAt(stl::StringView site, VkResult e);

    // the test build's account of a step, under the tool's name
    void trace(stl::StringView what);
    void traceSize(stl::StringView what, int w, int h);

    // the window's Vulkan surface
    VkSurfaceKHR createSurface(plt::Window& window);
    // the presenter on the surface: its format (HDR10 PQ when asked, and a
    // failure when the surface has none), render pass and first swapchain
    void setupWindow(stl::ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr);
    // the HDR scene: the render pass ImGui draws into, its FP16 target,
    // the output stage and the image pipeline drawImage runs
    void setupLinearHdr(stl::ObjPool& pool, u32 width, u32 height);
    // ImGui's context, at the scale, and its Vulkan backend on the
    // presenter (in the HDR scene, with the scene's fragment stage); the
    // largest texture the device makes goes into ImGui's platform io
    void setupImGui(stl::ObjPool& pool, bool hdr);

    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props);
    // the view, sampler and descriptors of an image already made and filled
    void finishTexture(VkFormat format, Texture& tex);
    // upload RGBA8 pixels into a device-local sampled texture and register
    // it with ImGui. one-shot: staging buffer, copy on a transient command
    // buffer, block once — refresh is never needed for a still image
    void uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex);
    // whatever of the texture was made goes; safe on one half made
    void destroyTexture(Texture& tex);

    // the frame: the swapchain (and the scene) at the window's size, the
    // draw data recorded and submitted, the image presented
    void createSwapchain(u32 width, u32 height);
    void createSceneTarget(u32 width, u32 height);
    void frameRender(ImDrawData* draw);
    void framePresent();

private:
    void setupVulkan(stl::ObjPool& pool, const GpuOptions& options);
    bool hasDeviceExtension(VkPhysicalDevice candidate, const char* name);
    VkPhysicalDevice selectPhysicalDevice();
    u32 selectQueueFamily(VkPhysicalDevice candidate);
    VkSurfaceFormatKHR selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace);
    void createPresentPass();
    void destroyFrames();
    void destroyPresenter();
    VkShaderModule shaderModule(const u32* code, size_t bytes);
    void destroySceneTarget();
    VkPipeline vertexlessPipeline(const u32* vertCode, size_t vertBytes, const u32* fragCode, size_t fragBytes, VkPipelineLayout layout, VkRenderPass pass);
    void destroyLinearHdr();
};

// what drawImage is handed: ImGui copies it into the draw list
struct ImageDraw {
    Gpu* gpu;
    VkDescriptorSet texture;
    float x0, y0, x1, y1;
    float sdrWhiteNits;
};

// an image's draw in the HDR scene, from ImGui's draw list: the command's
// clip as the backend applies its own, then the image quad by the image
// pipeline, PQ decoded into the scene's linear light;
// ImDrawCallback_ResetRenderState follows it
void drawImage(const ImDrawList*, const ImDrawCmd* cmd);

// clamp to 90% of the output, whose size arrived with the platform's
// registry roundtrips, before any window
void clampWindowSize(const plt::WindowInfo& info, int& w, int& h);

// the tool's frame: its ImGui windows, between NewFrame and Render. A
// nonzero result stops the platform loop and lands in the driver's action
struct UiFrame {
    virtual int frame() = 0;
};

// the vulkan/imgui frame driver behind plt's inverted loop: plt calls
// frame() for every granted frame; it resizes the swapchain on demand,
// draws the tool's ui and re-requests the next frame. A nonzero draw
// result (window close counts as exit) stops the platform loop and lands
// in action.
struct FrameDriver final: plt::FrameCallback, plt::WindowEvents {
    plt::Platform* platform = nullptr;
    plt::Window* window = nullptr;
    ImGuiPlt* imgui = nullptr;
    Gpu* gpu = nullptr;
    UiFrame* ui = nullptr;
    int action = 0;

    bool frame(const plt::WindowInfo& info) override;
    void close() override;
};

// show the window (idempotent) and run the platform loop until a draw
// verdict or the close button stops it
int runUi(FrameDriver& driver);
