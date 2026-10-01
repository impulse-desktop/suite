#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>

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
struct ImGuiStyle;

// What every tool's window is made of: one Vulkan device, the window's
// presenter (its surface, swapchain and the frames drawn into it), ImGui's
// Vulkan backend over it, the textures a tool shows and the frame driver
// behind plt's inverted loop. The HDR path lives here too: an FP16
// linear-light scene the tool draws into, encoded to the PQ swapchain by
// the output stage. One window per process, so the state is the process's.

// the fault seam, configured from the tool's environment in the test build
extern ChaosMonkey* gChaos;
// IM_TRACE_FRAMES: a line per frame on stderr with the gap since the last
// one and the time of each phase, and one per decode; to see where a
// jerk comes from
extern bool gTraceFrames;
u64 nowNs();
// "12.3", milliseconds to a tenth
void appendMs(stl::StringBuilder& text, u64 ns);

extern VkAllocationCallbacks* gAlloc;
extern VkInstance gInstance;
extern VkPhysicalDevice gPhys;
extern VkDevice gDevice;
extern u32 gQueueFamily;
extern VkQueue gQueue;
extern VkDescriptorPool gDescPool;

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

extern Presenter gPresent;
// the swapchain no longer fits the window: the next frame rebuilds it
extern bool gRebuild;
// the HDR scene is up: ImGui draws into it, the output stage encodes it
extern bool gLinearHdr;
// the HDR scene's white in nits, what the output stage scales it by
extern float gSdrWhiteNits;

// a Vulkan result the caller checks; a failure ends the tool with the
// error, as vulkan_error, or at the named site (IM_CHAOS vulkan-at=SITE)
void vkc(VkResult e);
void vkcAt(stl::StringView site, VkResult e);

// what the tool wants of the device: an HDR swapchain (the colour space
// extension), a buffer shared by another process (imported on the device
// its UUID names, with the dma-buf extensions), and how many textures it
// will register with ImGui over the window's life
struct VulkanWants {
    bool hdr = false;
    bool sharedBuffer = false;
    const u8* deviceUuid = nullptr;
    u32 textures = 1;
};

// the instance, the device and its queue, the descriptor pool; every
// object registers its teardown with the pool as it comes up
void setupVulkan(stl::ObjPool& pool, const VulkanWants& wants);
// the window's Vulkan surface
VkSurfaceKHR createSurface(plt::Window& window);
// the presenter on the surface: its format (HDR10 PQ when asked, and a
// failure when the surface has none), render pass and first swapchain
void setupVulkanWindow(stl::ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr);
// the HDR scene: the render pass ImGui draws into, its FP16 target, the
// output stage and the image pipeline drawImage runs
void setupLinearHdr(stl::ObjPool& pool, u32 width, u32 height);
// ImGui's context, at the ui scale, and its Vulkan backend on the
// presenter (in the HDR scene, with the scene's fragment stage); the
// largest texture the device makes goes into ImGui's platform io, for a
// tool sizing its own textures by ImGui alone
void setupImGui(stl::ObjPool& pool, bool hdr);
// all of the above for a tool that draws by ImGui alone: the device,
// the presenter on the window at its size, ImGui and its backend
void setupGpu(stl::ObjPool& pool, plt::Window& window, const VulkanWants& wants);

u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props);

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

// the view, sampler and descriptors of an image already made and filled
void finishTexture(VkFormat format, Texture& tex);
// upload RGBA8 pixels into a device-local sampled texture and register it
// with ImGui. one-shot: staging buffer, copy on a transient command buffer,
// block once — refresh is never needed for a still image
void uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex);
// whatever of the texture was made goes; safe on one half made
void destroyTexture(Texture& tex);

// what drawImage is handed: ImGui copies it into the draw list
struct ImageDraw {
    VkDescriptorSet texture;
    float x0, y0, x1, y1;
    float sdrWhiteNits;
};

// an image's draw in the HDR scene, from ImGui's draw list: the command's
// clip as the backend applies its own, then the image quad by the image
// pipeline, PQ decoded into the scene's linear light;
// ImDrawCallback_ResetRenderState follows it
void drawImage(const ImDrawList*, const ImDrawCmd* cmd);

float clampf(float v, float lo, float hi);

// clamp to 90% of the output, whose size arrived with the platform's
// registry roundtrips, before any window
void clampWindowSize(const plt::WindowInfo& info, int& w, int& h);

// a full-window panel that replaces the tool's ui (not an overlay) when
// something goes wrong — reads like a message from the compositor: a
// heading, the error text, and a single Exit button. returns -1 on exit.
int drawErrorPanel(stl::StringView msg);

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
    UiFrame* ui = nullptr;
    int action = 0;

    bool frame(const plt::WindowInfo& info) override;
    void close() override;
};

// show the window (idempotent) and run the platform loop until a draw
// verdict or the close button stops it
int runUi(FrameDriver& driver);
