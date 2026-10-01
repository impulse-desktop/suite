#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/vector.h>

#include <plt/window.h>
#include <vulkan/vulkan.h>

namespace stl {
    class ObjPool;
}

struct Renderer;
struct ChaosMonkey;
struct ImDrawCmd;
struct ImDrawList;
struct ImDrawData;

struct Frame {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkCommandPool commandPool;
    VkCommandBuffer commandBuffer;
    VkFence fence;
};

struct Sync {
    VkSemaphore acquired;
    VkSemaphore rendered;
};

struct Presenter {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format = {};
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    stl::Vector<Frame> frames;
    stl::Vector<Sync> syncs;
    u32 frameIndex = 0;
    u32 syncIndex = 0;
    int width = 0;
    int height = 0;
    VkClearValue clear = {};
};

struct GpuOptions {
    ChaosMonkey* chaos = nullptr;
    bool hdr = false;
    bool sharedBuffer = false;
    const u8* deviceUuid = nullptr;
    u32 textures = 1;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSet imageSet = VK_NULL_HANDLE;
};

struct Gpu {
    ChaosMonkey* chaos = nullptr;

    VkAllocationCallbacks* alloc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    u32 queueFamily = (u32)-1;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    Presenter present;
    bool rebuild = false;
    bool linearHdr = false;
    float sdrWhiteNits = 203.f;

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

    static Gpu* create(stl::ObjPool& pool, const GpuOptions& options);
    static Gpu* createFor(stl::ObjPool& pool, plt::Window& window, const GpuOptions& options);

    void vkc(VkResult e);
    void vkcAt(stl::StringView site, VkResult e);

    VkSurfaceKHR createSurface(plt::Window& window);
    void setupWindow(stl::ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr);
    void setupLinearHdr(stl::ObjPool& pool, u32 width, u32 height);
    void setupBackend(stl::ObjPool& pool, bool hdr);

    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props);
    void finishTexture(VkFormat format, Texture& tex);
    void uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex);
    void destroyTexture(Texture& tex);

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

struct ImageDraw {
    Gpu* gpu;
    VkDescriptorSet texture;
    float x0, y0, x1, y1;
    float sdrWhiteNits;
};

void drawImage(const ImDrawList*, const ImDrawCmd* cmd);

Renderer* createVulkanRenderer(stl::ObjPool& pool, Gpu& gpu);
