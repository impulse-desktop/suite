#include "renderer_vulkan.h"

#include "error.h"
#include "pooled.h"
#include "renderer.h"

#include <std/alg/defer.h>
#include <std/dbg/verify.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>
#include <std/mem/small_obj_allocator.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <imgui_impl_vulkan.h>
#include <gpu_image_frag.spv.h>
#include <gpu_image_vert.spv.h>
#include <gpu_scene_frag.spv.h>
#include <fullscreen_vert.spv.h>
#include <gpu_output_frag.spv.h>
#include <vulkan/vulkan_wayland.h>

using namespace stl;

namespace {
    struct VulkanChaos {
        virtual void memoryTypes(VkPhysicalDeviceMemoryProperties& props) = 0;
        virtual VkResult vulkan(VkResult result) = 0;
        virtual VkResult vulkanAt(stl::StringView site, VkResult result) = 0;
        virtual bool deviceExtension(const char* name, bool offered) = 0;
        virtual VkResult swapchain(VkResult result) = 0;
        virtual u32 count(stl::StringView what, u32 count) = 0;
        virtual VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) = 0;
        virtual VkBool32 surfaceSupport(VkBool32 supported) = 0;
        virtual void imageCounts(VkSurfaceCapabilitiesKHR& caps) = 0;

        static VulkanChaos* create(stl::ObjPool& pool);
    };

    struct Frame {
        VkImage image;
        VkImageView view;
        VkFramebuffer framebuffer;
        VkCommandPool commandPool;
        VkCommandBuffer commandBuffer;
        VkFence fence;
        VkSemaphore rendered;
        u64 serial;
    };

    struct Sync {
        VkSemaphore acquired;
        u64 serial;
    };

    struct Presenter {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSurfaceFormatKHR format = {};
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkRenderPass loadPass = VK_NULL_HANDLE;
        VkImageUsageFlags storage = 0;
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
        VulkanChaos* chaos = nullptr;
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

    struct VulkanImage;
    struct Gpu;

    struct Flight final: public plt::PollCallback {
        Gpu* gpu;
        u64 serial;
        plt::PollWaiter waiter;
        Vector<VulkanImage*> images;

        Flight(Gpu* gpu, u64 serial, int fd);
        void ready(PollFD event) override;
    };

    struct PollGpu final: public plt::TimerCallback {
        Gpu* gpu;
        explicit PollGpu(Gpu* gpu);
        void ready() override;
    };

    struct ImageDraw {
        Gpu* gpu;
        VkPipeline pipeline;
        VkPipelineLayout layout;
        VkDescriptorSet texture;
        float x0, y0, x1, y1;
        float sdrWhiteNits;
    };

    struct KernelDraw {
        VkPipeline pipeline;
        VkBuffer words;
        u32 tile;
        u32 width;
        u32 height;
    };

    struct Gpu {
        VulkanChaos* chaos = nullptr;

        VkAllocationCallbacks* alloc = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        u32 queueFamily = (u32)-1;
        VkQueue queue = VK_NULL_HANDLE;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        Presenter present;
        plt::Platform* platform = nullptr;
        plt::Window* window = nullptr;
        PollGpu* timer = nullptr;
        SmallObjAllocator* smallObjects = nullptr;
        PFN_vkGetFenceFdKHR fenceFd = nullptr;
        Vector<VulkanImage*> drawn;
        Vector<ImageDraw> underlays;
        Vector<KernelDraw> kernelDraws;
        u64 submitted = 0;
        u64 completed = 0;
        bool acquired = false;
        bool retry = false;
        bool rebuild = false;
        PFN_vkGetMemoryHostPointerPropertiesEXT hostProperties = nullptr;
        VkDeviceSize hostAlignment = 0;
        bool linearHdr = false;
        float sdrWhiteNits = 203.f;
        bool kernels = false;
        PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
        VkDescriptorSetLayout kernelSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout kernelPipelineLayout = VK_NULL_HANDLE;

        VkRenderPass scenePass = VK_NULL_HANDLE;
        VkRenderPass sceneLoadPass = VK_NULL_HANDLE;
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
        VkDescriptorSetLayout shadeSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout shadePipelineLayout = VK_NULL_HANDLE;
        VkRenderPass readPass = VK_NULL_HANDLE;

        static Gpu* create(stl::ObjPool& pool, const GpuOptions& options);

        void vkc(VkResult e);
        void vkcAt(stl::StringView site, VkResult e);

        VkSurfaceKHR createSurface(plt::Window& window);
        void setupWindow(stl::ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr);
        void setupLinearHdr(stl::ObjPool& pool, u32 width, u32 height);
        void setupBackend(stl::ObjPool& pool, bool hdr);

        u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props);
        void finishTexture(VkFormat format, Texture& tex);
        void createTexture(u32 w, u32 h, Texture& tex, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageUsageFlags usage = 0);
        void uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex);
        void destroyTexture(Texture& tex);

        void createSwapchain(u32 width, u32 height);
        void createSceneTarget(u32 width, u32 height);
        bool acquireFrame();
        void track(VkFence fence);
        void landed(Flight* flight);
        void recordImages(VkCommandBuffer command);
        void recordUnderlays(VkCommandBuffer command, const ImDrawData& draw);
        bool recordKernels(VkCommandBuffer command, VkImage target, VkImageView view, const VkClearValue& clear);
        void pushKernel(VkCommandBuffer command, VkPipeline pipeline, VkBuffer words, VkImageView view);
        VkShaderModule shaderModule(const u32* code, size_t bytes);
        VkPipeline vertexlessPipeline(const u32* vertCode, size_t vertBytes, const u32* fragCode, size_t fragBytes, VkPipelineLayout layout, VkRenderPass pass);
        void frameRender(ImDrawData* draw);
        void framePresent();

    private:
        void setupVulkan(stl::ObjPool& pool, const GpuOptions& options);
        bool hasDeviceExtension(VkPhysicalDevice candidate, const char* name);
        VkPhysicalDevice selectPhysicalDevice();
        u32 selectQueueFamily(VkPhysicalDevice candidate);
        VkSurfaceFormatKHR selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace);
        VkRenderPass createPresentPass(bool load);
        void destroyFrames();
        void destroyPresenter();
        void destroySceneTarget();
        void destroyLinearHdr();
    };

    static void drawImage(const ImDrawList*, const ImDrawCmd* cmd);
    static void recordDraw(VkCommandBuffer command, const ImageDraw& draw, const ImDrawData& data, VkRect2D scissor);
}
#ifdef IM_FOR_TESTS
namespace {
    struct NamedCount {
        StringView what;
        u32 count;
    };

    struct TestVulkanChaos: public VulkanChaos {
        int memoryFaults = 0;
        int vulkanSkip = -1;
        Vector<StringView> failingSites;
        Vector<StringView> hiddenExtensions;
        int swapchainSkip = -1;
        VkResult swapchainFault = VK_SUCCESS;
        Vector<NamedCount> counts;
        bool discreteGpu = false;
        bool noWsi = false;
        bool imageCountsSet = false;
        u32 minImages = 0;
        u32 maxImages = 0;

        explicit TestVulkanChaos(StringView script);

        void arm(StringView script);
        void armFault(StringView fault, StringView arg);

        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };

    static bool spend(int& count) {
        if (count <= 0) {
            return false;
        }

        count--;

        return true;
    }

    static bool failsOnce(int& skip) {
        if (skip < 0) {
            return false;
        }

        return skip-- == 0;
    }
}

TestVulkanChaos::TestVulkanChaos(StringView script) {
    arm(script);
}

void TestVulkanChaos::arm(StringView script) {
    while (!script.empty()) {
        StringView word, rest, fault, arg;

        if (script.split(' ', word, rest)) {
            script = rest;
        } else {
            word = script;
            script = {};
        }

        if (word.split('=', fault, arg)) {
            armFault(fault, arg);
        }
    }
}

void TestVulkanChaos::armFault(StringView fault, StringView arg) {
    if (fault == StringView(u8"memory-types")) {
        memoryFaults = (int)arg.stou();
    } else if (fault == StringView(u8"vulkan")) {
        vulkanSkip = (int)arg.stou();
    } else if (fault == StringView(u8"vulkan-at")) {
        failingSites.pushBack(arg);
    } else if (fault == StringView(u8"no-ext")) {
        hiddenExtensions.pushBack(arg);
    } else if (fault == StringView(u8"swapchain") || fault == StringView(u8"swapchain-suboptimal")) {
        swapchainSkip = (int)arg.stou();
        swapchainFault = fault == StringView(u8"swapchain") ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUBOPTIMAL_KHR;
    } else if (fault == StringView(u8"count")) {
        StringView what, n;

        if (arg.split(':', what, n)) {
            counts.pushBack({what, (u32)n.stou()});
        }
    } else if (fault == StringView(u8"discrete-gpu")) {
        discreteGpu = true;
    } else if (fault == StringView(u8"no-wsi")) {
        noWsi = true;
    } else if (fault == StringView(u8"image-counts")) {
        StringView lo, hi;

        if (arg.split(':', lo, hi)) {
            imageCountsSet = true;
            minImages = (u32)lo.stou();
            maxImages = (u32)hi.stou();
        }
    }
}

void TestVulkanChaos::memoryTypes(VkPhysicalDeviceMemoryProperties& props) {
    if (spend(memoryFaults)) {
        props.memoryTypeCount = 0;
    }
}

VkResult TestVulkanChaos::vulkan(VkResult result) {
    if (!failsOnce(vulkanSkip)) {
        return result;
    }

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult TestVulkanChaos::vulkanAt(StringView site, VkResult result) {
    for (StringView failing : failingSites) {
        if (failing == site) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    return result;
}

bool TestVulkanChaos::deviceExtension(const char* name, bool offered) {
    for (StringView hidden : hiddenExtensions) {
        if (hidden == StringView(name)) {
            return false;
        }
    }

    return offered;
}

VkResult TestVulkanChaos::swapchain(VkResult result) {
    if (!failsOnce(swapchainSkip)) {
        return result;
    }

    return swapchainFault;
}

u32 TestVulkanChaos::count(StringView what, u32 count) {
    for (const NamedCount& named : counts) {
        if (named.what == what) {
            return named.count;
        }
    }

    return count;
}

VkPhysicalDeviceType TestVulkanChaos::deviceType(VkPhysicalDeviceType type) {
    return discreteGpu ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : type;
}

VkBool32 TestVulkanChaos::surfaceSupport(VkBool32 supported) {
    return noWsi ? VK_FALSE : supported;
}

void TestVulkanChaos::imageCounts(VkSurfaceCapabilitiesKHR& caps) {
    if (imageCountsSet) {
        caps.minImageCount = minImages;
        caps.maxImageCount = maxImages;
    }
}

VulkanChaos* VulkanChaos::create(ObjPool& pool) {
    const char* script = getenv("IM_CHAOS");

    return pool.make<TestVulkanChaos>(StringView(script ? script : ""));
}
#else
namespace {
    struct IdleVulkanChaos: public VulkanChaos {
        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };
}

void IdleVulkanChaos::memoryTypes(VkPhysicalDeviceMemoryProperties&) {
}

VkResult IdleVulkanChaos::vulkan(VkResult result) {
    return result;
}

VkResult IdleVulkanChaos::vulkanAt(StringView, VkResult result) {
    return result;
}

bool IdleVulkanChaos::deviceExtension(const char*, bool offered) {
    return offered;
}

VkResult IdleVulkanChaos::swapchain(VkResult result) {
    return result;
}

u32 IdleVulkanChaos::count(StringView, u32 count) {
    return count;
}

VkPhysicalDeviceType IdleVulkanChaos::deviceType(VkPhysicalDeviceType type) {
    return type;
}

VkBool32 IdleVulkanChaos::surfaceSupport(VkBool32 supported) {
    return supported;
}

void IdleVulkanChaos::imageCounts(VkSurfaceCapabilitiesKHR&) {
}

VulkanChaos* VulkanChaos::create(ObjPool& pool) {
    return pool.make<IdleVulkanChaos>();
}
#endif

namespace {
    constexpr u32 kMinImageCount = 3;

    struct ImagePush {
        float scale[2];
        float translate[2];
        float rect[4];
        float sdrWhiteNits;
    };
}

Gpu* Gpu::create(ObjPool& pool, const GpuOptions& options) {
    Gpu* gpu = pool.make<Gpu>();

    gpu->chaos = options.chaos;
    gpu->setupVulkan(pool, options);

    return gpu;
}

bool Gpu::hasDeviceExtension(VkPhysicalDevice candidate, const char* name) {
    u32 count = 0;

    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
    Vector<VkExtensionProperties> props;

    props.zero(count);
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, props.mutData());

    bool offered = false;

    for (const VkExtensionProperties& prop : props) {
        if (StringView(prop.extensionName) == StringView(name)) {
            offered = true;

            break;
        }
    }

    return chaos->deviceExtension(name, offered);
}

VkPhysicalDevice Gpu::selectPhysicalDevice() {
    u32 count = 0;

    vkc(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    count = chaos->count(StringView(u8"devices"), count);

    if (!count) {
        fail(StringView(u8"no vulkan device"));
    }

    Vector<VkPhysicalDevice> devices;

    devices.zero(count);
    vkc(vkEnumeratePhysicalDevices(instance, &count, devices.mutData()));

    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props;

        vkGetPhysicalDeviceProperties(candidate, &props);

        if (chaos->deviceType(props.deviceType) == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return candidate;
        }
    }

    return devices[0];
}

u32 Gpu::selectQueueFamily(VkPhysicalDevice candidate) {
    u32 count = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);

    Vector<VkQueueFamilyProperties> families;

    families.zero(count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.mutData());
    count = chaos->count(StringView(u8"queue-families"), count);

    for (u32 i = 0; i < count; i++) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            return i;
        }
    }

    fail(StringView(u8"no vulkan graphics queue"));
}

VkSurfaceFormatKHR Gpu::selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace) {
    u32 count = 0;

    vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr));
    count = chaos->count(StringView(u8"surface-formats"), count);

    if (!count) {
        fail(StringView(u8"vulkan WSI offers no surface format"));
    }

    Vector<VkSurfaceFormatKHR> available;

    available.zero(count);
    vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, available.mutData()));

    for (u32 i = 0; i < nwanted; i++) {
        for (const VkSurfaceFormatKHR& format : available) {
            if (format.format == wanted[i] && format.colorSpace == colorSpace) {
                return format;
            }
        }
    }

    return available[0];
}

VkRenderPass Gpu::createPresentPass(bool load) {
    VkAttachmentDescription attachment{};

    attachment.format = present.format.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;

    VkSubpassDependency dependency{};

    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkRenderPass pass = VK_NULL_HANDLE;

    rpci.attachmentCount = 1;
    rpci.pAttachments = &attachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dependency;
    vkc(vkCreateRenderPass(device, &rpci, alloc, &pass));

    return pass;
}

void Gpu::destroyFrames() {
    for (size_t i = 0; i < present.frames.length(); i++) {
        const Frame& frame = present.frames[i];

        if (frame.framebuffer) {
            vkDestroyFramebuffer(device, frame.framebuffer, alloc);
        }
        if (frame.view) {
            vkDestroyImageView(device, frame.view, alloc);
        }
        if (frame.fence) {
            vkDestroyFence(device, frame.fence, alloc);
        }
        if (frame.rendered) {
            vkDestroySemaphore(device, frame.rendered, alloc);
        }
        if (frame.commandPool) {
            vkDestroyCommandPool(device, frame.commandPool, alloc);
        }
    }

    for (size_t i = 0; i < present.syncs.length(); i++) {
        const Sync& sync = present.syncs[i];

        if (sync.acquired) {
            vkDestroySemaphore(device, sync.acquired, alloc);
        }
    }

    present.frames.clear();
    present.syncs.clear();
}

void Gpu::createSwapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;

    vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, present.surface, &caps));
    chaos->imageCounts(caps);

    VkExtent2D extent = caps.currentExtent;

    if (extent.width == 0xffffffffu) {
        extent.width = width < caps.minImageExtent.width ? caps.minImageExtent.width : width > caps.maxImageExtent.width ? caps.maxImageExtent.width : width;
        extent.height = height < caps.minImageExtent.height ? caps.minImageExtent.height : height > caps.maxImageExtent.height ? caps.maxImageExtent.height : height;
    }

    u32 images = caps.minImageCount > kMinImageCount ? caps.minImageCount : kMinImageCount;

    if (caps.maxImageCount && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};

    ci.surface = present.surface;
    ci.minImageCount = images;
    ci.imageFormat = present.format.format;
    ci.imageColorSpace = present.format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | present.storage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = present.swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    VkResult made = vkCreateSwapchainKHR(device, &ci, alloc, &swapchain);

    if (chaos->vulkanAt(StringView(u8"swapchain"), made) < 0) {
        if (made == VK_SUCCESS) {
            vkDestroySwapchainKHR(device, swapchain, alloc);
        }

        fail(StringView(u8"vulkan cannot make a swapchain on this surface"));
    }

    if (present.swapchain) {
        vkDeviceWaitIdle(device);
        completed = submitted;
        acquired = false;
        destroyFrames();
        vkDestroySwapchainKHR(device, present.swapchain, alloc);
    }

    present.swapchain = swapchain;
    present.width = (int)extent.width;
    present.height = (int)extent.height;
    present.frameIndex = 0;
    present.syncIndex = 0;

    u32 count = 0;

    vkc(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));

    Vector<VkImage> handles;

    handles.zero(count);
    vkc(vkGetSwapchainImagesKHR(device, swapchain, &count, handles.mutData()));
    present.frames.zero(count);
    present.syncs.zero(count + 1);

    for (u32 i = 0; i < count; i++) {
        Frame& frame = present.frames.mut(i);

        frame.image = handles[i];

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

        vci.image = frame.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = present.format.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc(vkCreateImageView(device, &vci, alloc, &frame.view));

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

        fci.renderPass = present.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &frame.view;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        vkc(vkCreateFramebuffer(device, &fci, alloc, &frame.framebuffer));

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

        pci.queueFamilyIndex = queueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkc(vkCreateCommandPool(device, &pci, alloc, &frame.commandPool));

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

        cai.commandPool = frame.commandPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkc(vkAllocateCommandBuffers(device, &cai, &frame.commandBuffer));

        VkExportFenceCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

        exportInfo.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
        fenceInfo.pNext = &exportInfo;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkc(vkCreateFence(device, &fenceInfo, alloc, &frame.fence));
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &frame.rendered));
    }

    for (u32 i = 0; i <= count; i++) {
        Sync& sync = present.syncs.mut(i);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &sync.acquired));
    }
}

void Gpu::destroyPresenter() {
    destroyFrames();

    if (present.swapchain) {
        vkDestroySwapchainKHR(device, present.swapchain, alloc);
    }
    if (present.renderPass) {
        vkDestroyRenderPass(device, present.renderPass, alloc);
    }
    if (present.loadPass) {
        vkDestroyRenderPass(device, present.loadPass, alloc);
    }
    if (present.surface) {
        vkDestroySurfaceKHR(instance, present.surface, alloc);
    }

    present.swapchain = VK_NULL_HANDLE;
    present.renderPass = VK_NULL_HANDLE;
    present.loadPass = VK_NULL_HANDLE;
    present.surface = VK_NULL_HANDLE;
}

VkShaderModule Gpu::shaderModule(const u32* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

    ci.codeSize = bytes;
    ci.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;

    vkc(vkCreateShaderModule(device, &ci, alloc, &module));

    return module;
}

void Gpu::destroySceneTarget() {
    if (outputSet) {
        vkFreeDescriptorSets(device, descPool, 1, &outputSet);
        outputSet = VK_NULL_HANDLE;
    }
    if (sceneFramebuffer) {
        vkDestroyFramebuffer(device, sceneFramebuffer, alloc);
    }
    if (sceneView) {
        vkDestroyImageView(device, sceneView, alloc);
    }
    if (sceneImage) {
        vkDestroyImage(device, sceneImage, alloc);
    }
    if (sceneMemory) {
        vkFreeMemory(device, sceneMemory, alloc);
    }
    sceneFramebuffer = VK_NULL_HANDLE;
    sceneView = VK_NULL_HANDLE;
    sceneImage = VK_NULL_HANDLE;
    sceneMemory = VK_NULL_HANDLE;
}

void Gpu::createSceneTarget(u32 width, u32 height) {
    destroySceneTarget();

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (kernels ? VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0);
    vkc(vkCreateImage(device, &ici, alloc, &sceneImage));

    VkMemoryRequirements req{};

    vkGetImageMemoryRequirements(device, sceneImage, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &sceneMemory));
    vkc(vkBindImageMemory(device, sceneImage, sceneMemory, 0));

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

    vci.image = sceneImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc(vkCreateImageView(device, &vci, alloc, &sceneView));

    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

    fci.renderPass = scenePass;
    fci.attachmentCount = 1;
    fci.pAttachments = &sceneView;
    fci.width = width;
    fci.height = height;
    fci.layers = 1;
    vkc(vkCreateFramebuffer(device, &fci, alloc, &sceneFramebuffer));

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &outputSetLayout;
    vkc(vkAllocateDescriptorSets(device, &ai, &outputSet));

    VkDescriptorImageInfo image{sceneSampler, sceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

    write.dstSet = outputSet;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

VkPipeline Gpu::vertexlessPipeline(const u32* vertCode, size_t vertBytes, const u32* fragCode, size_t fragBytes, VkPipelineLayout layout, VkRenderPass pass) {
    VkShaderModule vert = shaderModule(vertCode, vertBytes);
    VkShaderModule frag = shaderModule(fragCode, fragBytes);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};

    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vertex;
    gpci.pInputAssemblyState = &assembly;
    gpci.pViewportState = &viewport;
    gpci.pRasterizationState = &raster;
    gpci.pMultisampleState = &multisample;
    gpci.pColorBlendState = &blend;
    gpci.pDynamicState = &dynamic;
    gpci.layout = layout;
    gpci.renderPass = pass;

    VkPipeline pipeline = VK_NULL_HANDLE;

    vkc(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, alloc, &pipeline));
    vkDestroyShaderModule(device, frag, alloc);
    vkDestroyShaderModule(device, vert, alloc);

    return pipeline;
}

void Gpu::destroyLinearHdr() {
    destroySceneTarget();
    if (imagePipeline) {
        vkDestroyPipeline(device, imagePipeline, alloc);
    }
    if (imagePipelineLayout) {
        vkDestroyPipelineLayout(device, imagePipelineLayout, alloc);
    }
    if (imageSetLayout) {
        vkDestroyDescriptorSetLayout(device, imageSetLayout, alloc);
    }
    if (outputPipeline) {
        vkDestroyPipeline(device, outputPipeline, alloc);
    }
    if (outputPipelineLayout) {
        vkDestroyPipelineLayout(device, outputPipelineLayout, alloc);
    }
    if (outputSetLayout) {
        vkDestroyDescriptorSetLayout(device, outputSetLayout, alloc);
    }
    if (sceneSampler) {
        vkDestroySampler(device, sceneSampler, alloc);
    }
    if (scenePass) {
        vkDestroyRenderPass(device, scenePass, alloc);
    }
    if (sceneLoadPass) {
        vkDestroyRenderPass(device, sceneLoadPass, alloc);
    }
}

bool Gpu::acquireFrame() {
    Sync& sync = present.syncs.mut(present.syncIndex);
    if (sync.serial > completed) {
        retry = true;
        return false;
    }
    if (!acquired) {
        VkResult e = chaos->swapchain(vkAcquireNextImageKHR(device, present.swapchain, 0, sync.acquired, VK_NULL_HANDLE, &present.frameIndex));
        if (e == VK_NOT_READY || e == VK_TIMEOUT || e == VK_ERROR_OUT_OF_DATE_KHR) {
            rebuild = e == VK_ERROR_OUT_OF_DATE_KHR;
            retry = true;
            platform->poller()->timeout(1000, *timer);
            return false;
        }
        vkc(e);
        rebuild = e == VK_SUBOPTIMAL_KHR;
        acquired = true;
    }
    Frame& frame = present.frames.mut(present.frameIndex);
    if (frame.serial > completed) {
        retry = true;
        return false;
    }
    retry = false;
    return true;
}

void Gpu::recordUnderlays(VkCommandBuffer command, const ImDrawData& draw) {
    VkViewport viewport{0, 0, (float)present.width, (float)present.height, 0, 1};
    VkRect2D scissor{{0, 0}, {(u32)present.width, (u32)present.height}};

    if (!underlays.empty()) {
        vkCmdSetViewport(command, 0, 1, &viewport);
    }
    for (const ImageDraw& image : underlays) {
        recordDraw(command, image, draw, scissor);
    }
    underlays.clear();
}

void Gpu::pushKernel(VkCommandBuffer command, VkPipeline pipeline, VkBuffer words, VkImageView view) {
    VkDescriptorBufferInfo buffer{words, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo image{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};

    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &buffer;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &image;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    pushDescriptorSet(command, VK_PIPELINE_BIND_POINT_COMPUTE, kernelPipelineLayout, 0, 2, writes);
}

bool Gpu::recordKernels(VkCommandBuffer command, VkImage target, VkImageView view, const VkClearValue& clear) {
    if (kernelDraws.empty()) {
        return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_GENERAL, &clear.color, 1, &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    for (const KernelDraw& kernel : kernelDraws) {
        pushKernel(command, kernel.pipeline, kernel.words, view);
        vkCmdDispatch(command, (kernel.width + kernel.tile - 1) / kernel.tile, (kernel.height + kernel.tile - 1) / kernel.tile, 1);
    }

    kernelDraws.clear();
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    return true;
}

void Gpu::frameRender(ImDrawData* draw) {
    Sync& sync = present.syncs.mut(present.syncIndex);
    Frame& fd = present.frames.mut(present.frameIndex);
    vkc(vkResetFences(device, 1, &fd.fence));
    vkc(vkResetCommandPool(device, fd.commandPool, 0));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkc(vkBeginCommandBuffer(fd.commandBuffer, &bi));
    recordImages(fd.commandBuffer);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};

    rp.renderArea.extent.width = (u32)present.width;
    rp.renderArea.extent.height = (u32)present.height;
    rp.clearValueCount = 1;
    rp.pClearValues = &present.clear;

    if (linearHdr) {
        VkClearValue sceneClear{};

        rp.renderPass = recordKernels(fd.commandBuffer, sceneImage, sceneView, sceneClear) ? sceneLoadPass : scenePass;
        rp.framebuffer = sceneFramebuffer;
        rp.pClearValues = &sceneClear;
        vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        recordUnderlays(fd.commandBuffer, *draw);
        ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
        vkCmdEndRenderPass(fd.commandBuffer);

        bool deep = present.format.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || present.format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        float output[3] = {sdrWhiteNits, deep ? 1023.f : 255.f, (float)((submitted * 2654435761u) % 4096)};

        rp.renderPass = present.renderPass;
        rp.framebuffer = fd.framebuffer;
        rp.pClearValues = &present.clear;
        vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline);
        vkCmdBindDescriptorSets(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipelineLayout, 0, 1, &outputSet, 0, nullptr);
        vkCmdPushConstants(fd.commandBuffer, outputPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(output), output);
        VkViewport viewport{0, 0, (float)present.width, (float)present.height, 0, 1};
        VkRect2D scissor{{0, 0}, {(u32)present.width, (u32)present.height}};

        vkCmdSetViewport(fd.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(fd.commandBuffer, 0, 1, &scissor);
        vkCmdDraw(fd.commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(fd.commandBuffer);
    } else {
        rp.renderPass = recordKernels(fd.commandBuffer, fd.image, fd.view, present.clear) ? present.loadPass : present.renderPass;
        rp.framebuffer = fd.framebuffer;
        vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        recordUnderlays(fd.commandBuffer, *draw);
        ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
        vkCmdEndRenderPass(fd.commandBuffer);
    }

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sync.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fd.commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &fd.rendered;
    vkc(vkEndCommandBuffer(fd.commandBuffer));
    vkc(vkQueueSubmit(queue, 1, &si, fd.fence));
    fd.serial = ++submitted;
    sync.serial = submitted;
    track(fd.fence);
}

void Gpu::framePresent() {
    Frame& frame = present.frames.mut(present.frameIndex);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &frame.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &present.swapchain;
    pi.pImageIndices = &present.frameIndex;

    VkResult e = chaos->swapchain(vkQueuePresentKHR(queue, &pi));

    if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
        rebuild = true;
    }

    acquired = false;
    present.syncIndex = (present.syncIndex + 1) % (u32)present.syncs.length();
}

void Gpu::vkc(VkResult e) {
    e = chaos->vulkan(e);

    if (e < 0) {
        fail(StringView(StringBuilder() << StringView(u8"vulkan error ") << (i64)e));
    }
}

void Gpu::vkcAt(StringView site, VkResult e) {
    e = chaos->vulkanAt(site, e);

    if (e < 0) {
        fail(StringView(StringBuilder() << StringView(u8"vulkan error ") << (i64)e << StringView(u8" at ") << site));
    }
}

void Gpu::setupVulkan(ObjPool& pool, const GpuOptions& wants) {
    VkApplicationInfo app = {};

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "im";
    app.apiVersion = VK_API_VERSION_1_2;

    Vector<const char*> instanceExts;

    instanceExts.pushBack(VK_KHR_SURFACE_EXTENSION_NAME);
    instanceExts.pushBack(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

    if (wants.hdr) {
        instanceExts.pushBack(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ci = {};

    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (u32)instanceExts.length();
    ci.ppEnabledExtensionNames = instanceExts.data();
    vkc(vkCreateInstance(&ci, alloc, &instance));
    pooledGuard(pool, [this] {
        vkDestroyInstance(instance, alloc);
    });

    if (wants.sharedBuffer) {
        u32 count = 0;

        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        Vector<VkPhysicalDevice> devices;

        devices.zero(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.mutData());

        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

            props.pNext = &ids;
            vkGetPhysicalDeviceProperties2(candidate, &props);

            if (memcmp(ids.deviceUUID, wants.deviceUuid, VK_UUID_SIZE) == 0) {
                phys = candidate;

                break;
            }
        }

        if (!phys) {
            fail(StringView(u8"shared screenshot gpu is unavailable"));
        }
    } else {
        phys = selectPhysicalDevice();
    }

    queueFamily = selectQueueFamily(phys);

    const char* wantedExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    u32 wantedCount = wants.sharedBuffer ? 5 : 2;
    Vector<const char*> devExts;

    for (u32 i = 0; i < wantedCount; i++) {
        if (!hasDeviceExtension(phys, wantedExts[i])) {
            fail(StringView(StringBuilder() << StringView(u8"vulkan lacks ") << StringView(wantedExts[i])));
        }

        devExts.pushBack(wantedExts[i]);
    }

    VkPhysicalDeviceExternalFenceInfo fenceQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
    VkExternalFenceProperties fenceSupport{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};

    fenceQuery.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    vkGetPhysicalDeviceExternalFenceProperties(phys, &fenceQuery, &fenceSupport);

    if (!(fenceSupport.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) {
        fail(StringView(u8"vulkan cannot export fences as sync files"));
    }

    if (hasDeviceExtension(phys, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &host;
        vkGetPhysicalDeviceProperties2(phys, &properties);
        hostAlignment = host.minImportedHostPointerAlignment;
        devExts.pushBack(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }

    VkPhysicalDeviceFeatures offered;
    VkPhysicalDeviceFeatures features{};

    vkGetPhysicalDeviceFeatures(phys, &offered);

    if (offered.shaderStorageImageWriteWithoutFormat && hasDeviceExtension(phys, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        devExts.pushBack(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};

    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {};

    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (u32)devExts.length();
    dci.ppEnabledExtensionNames = devExts.data();
    dci.pEnabledFeatures = &features;
    vkc(vkCreateDevice(phys, &dci, alloc, &device));
    pooledGuard(pool, [this] {
        vkDestroyDevice(device, alloc);
    });
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    fenceFd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(device, "vkGetFenceFdKHR");
    if (!fenceFd) {
        fail(StringView(u8"vulkan lacks vkGetFenceFdKHR"));
    }
    if (hostAlignment) {
        hostProperties = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    }

    VkDescriptorPoolSize sz = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + wants.textures};
    VkDescriptorPoolCreateInfo pi = {};

    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = sz.descriptorCount;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &sz;
    vkc(vkCreateDescriptorPool(device, &pi, alloc, &descPool));
    pooledGuard(pool, [this] {
        vkDestroyDescriptorPool(device, descPool, alloc);
    });

    VkDescriptorSetLayoutBinding shadeBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo shadeLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    shadeLayout.bindingCount = 1;
    shadeLayout.pBindings = &shadeBinding;
    vkc(vkCreateDescriptorSetLayout(device, &shadeLayout, alloc, &shadeSetLayout));
    pooledGuard(pool, [this] {
        vkDestroyDescriptorSetLayout(device, shadeSetLayout, alloc);
    });

    VkPushConstantRange shadeRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ImagePush)};
    VkPipelineLayoutCreateInfo shadePipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    shadePipeline.setLayoutCount = 1;
    shadePipeline.pSetLayouts = &shadeSetLayout;
    shadePipeline.pushConstantRangeCount = 1;
    shadePipeline.pPushConstantRanges = &shadeRange;
    vkc(vkCreatePipelineLayout(device, &shadePipeline, alloc, &shadePipelineLayout));
    pooledGuard(pool, [this] {
        vkDestroyPipelineLayout(device, shadePipelineLayout, alloc);
    });

    if (features.shaderStorageImageWriteWithoutFormat) {
        VkDescriptorSetLayoutBinding kernelBindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo kernelLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

        kernelLayout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        kernelLayout.bindingCount = 2;
        kernelLayout.pBindings = kernelBindings;
        vkc(vkCreateDescriptorSetLayout(device, &kernelLayout, alloc, &kernelSetLayout));
        pooledGuard(pool, [this] {
            vkDestroyDescriptorSetLayout(device, kernelSetLayout, alloc);
        });

        VkPipelineLayoutCreateInfo kernelPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

        kernelPipeline.setLayoutCount = 1;
        kernelPipeline.pSetLayouts = &kernelSetLayout;
        vkc(vkCreatePipelineLayout(device, &kernelPipeline, alloc, &kernelPipelineLayout));
        pooledGuard(pool, [this] {
            vkDestroyPipelineLayout(device, kernelPipelineLayout, alloc);
        });
        pushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    }

    VkAttachmentDescription target{};

    target.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    target.samples = VK_SAMPLE_COUNT_1_BIT;
    target.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    target.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    target.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    target.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;

    VkSubpassDependency written{0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo readInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

    readInfo.attachmentCount = 1;
    readInfo.pAttachments = &target;
    readInfo.subpassCount = 1;
    readInfo.pSubpasses = &subpass;
    readInfo.dependencyCount = 1;
    readInfo.pDependencies = &written;
    vkc(vkCreateRenderPass(device, &readInfo, alloc, &readPass));
    pooledGuard(pool, [this] {
        vkDestroyRenderPass(device, readPass, alloc);
    });
}

VkSurfaceKHR Gpu::createSurface(plt::Window& window) {
    plt::RenderContext render = window.renderContext();
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};

    sci.display = (wl_display*)render.connection;
    sci.surface = (wl_surface*)render.window;
    vkc(vkCreateWaylandSurfaceKHR(instance, &sci, alloc, &surface));

    return surface;
}

void Gpu::setupWindow(ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr) {
    present.surface = surface;
    pooledGuard(pool, [this] {
        destroyPresenter();
    });

    VkBool32 supported = VK_FALSE;

    vkc(vkGetPhysicalDeviceSurfaceSupportKHR(phys, queueFamily, surface, &supported));

    if (!chaos->surfaceSupport(supported)) {
        fail(StringView(u8"no vulkan WSI support"));
    }

    const VkFormat hdrFmts[] = {
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    };
    const VkFormat sdrFmts[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };
    const VkFormat* fmts = hdr ? hdrFmts : sdrFmts;
    VkColorSpaceKHR colorSpace = hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLORSPACE_SRGB_NONLINEAR_KHR;

    present.format = selectSurfaceFormat(surface, fmts, 4, colorSpace);

    if (hdr && present.format.colorSpace != colorSpace) {
        fail(StringView(u8"vulkan WSI has no BT.2020/PQ surface"));
    }

    present.renderPass = createPresentPass(false);
    present.loadPass = createPresentPass(true);

    if (pushDescriptorSet && !hdr) {
        VkSurfaceCapabilitiesKHR caps;
        VkFormatProperties properties;
        VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
        vkGetPhysicalDeviceFormatProperties(phys, present.format.format, &properties);
        kernels = (caps.supportedUsageFlags & storage) == storage && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        present.storage = kernels ? storage : 0;
    }

    createSwapchain((u32)w, (u32)h);
}

u32 Gpu::findMemoryType(u32 typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    chaos->memoryTypes(mp);

    for (u32 i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }

    fail(StringView(u8"no vulkan memory type fits"));
}

void Gpu::setupLinearHdr(ObjPool& pool, u32 width, u32 height) {
    pooledGuard(pool, [this] {
        destroyLinearHdr();
        linearHdr = false;
    });

    VkFormatProperties properties;

    vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R16G16B16A16_SFLOAT, &properties);
    kernels = pushDescriptorSet && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

    VkAttachmentDescription attachment{};

    attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};

    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;

    VkSubpassDependency dependencies[2]{};

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

    rpci.attachmentCount = 1;
    rpci.pAttachments = &attachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dependencies;
    vkcAt(StringView(u8"scene-pass"), vkCreateRenderPass(device, &rpci, alloc, &scenePass));
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkc(vkCreateRenderPass(device, &rpci, alloc, &sceneLoadPass));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkc(vkCreateSampler(device, &sci, alloc, &sceneSampler));

    VkDescriptorSetLayoutBinding binding{};

    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    dlci.bindingCount = 1;
    dlci.pBindings = &binding;
    vkc(vkCreateDescriptorSetLayout(device, &dlci, alloc, &outputSetLayout));

    VkPushConstantRange outputRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 3 * sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    plci.setLayoutCount = 1;
    plci.pSetLayouts = &outputSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &outputRange;
    vkc(vkCreatePipelineLayout(device, &plci, alloc, &outputPipelineLayout));

    outputPipeline = vertexlessPipeline(fullscreen_vert_spv, sizeof(fullscreen_vert_spv), gpu_output_frag_spv, sizeof(gpu_output_frag_spv), outputPipelineLayout, present.renderPass);

    VkDescriptorSetLayoutBinding imageBinding{};

    imageBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    imageBinding.descriptorCount = 1;
    imageBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ilci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    ilci.bindingCount = 1;
    ilci.pBindings = &imageBinding;
    vkc(vkCreateDescriptorSetLayout(device, &ilci, alloc, &imageSetLayout));

    VkPushConstantRange imageRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ImagePush)};
    VkPipelineLayoutCreateInfo iplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    iplci.setLayoutCount = 1;
    iplci.pSetLayouts = &imageSetLayout;
    iplci.pushConstantRangeCount = 1;
    iplci.pPushConstantRanges = &imageRange;
    vkc(vkCreatePipelineLayout(device, &iplci, alloc, &imagePipelineLayout));
    imagePipeline = vertexlessPipeline(gpu_image_vert_spv, sizeof(gpu_image_vert_spv), gpu_image_frag_spv, sizeof(gpu_image_frag_spv), imagePipelineLayout, scenePass);

    createSceneTarget(width, height);
    linearHdr = true;
}

void Gpu::setupBackend(ObjPool& pool, bool hdr) {
    ImGui_ImplVulkan_InitInfo ii = {};

    ii.Instance = instance;
    ii.PhysicalDevice = phys;
    ii.Device = device;
    ii.QueueFamily = queueFamily;
    ii.Queue = queue;
    ii.DescriptorPool = descPool;
    ii.MinImageCount = kMinImageCount;
    ii.ImageCount = (u32)present.frames.length();
    ii.PipelineInfoMain.RenderPass = linearHdr ? scenePass : present.renderPass;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (hdr) {
        ii.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ii.CustomShaderFragCreateInfo.codeSize = sizeof(gpu_scene_frag_spv);
        ii.CustomShaderFragCreateInfo.pCode = gpu_scene_frag_spv;
    }

    ImGui_ImplVulkan_Init(&ii);
    pooledGuard(pool, [] {
        ImGui_ImplVulkan_Shutdown();
    });

    VkPhysicalDeviceProperties props;

    vkGetPhysicalDeviceProperties(phys, &props);

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    pio.Renderer_TextureMaxWidth = (int)props.limits.maxImageDimension2D;
    pio.Renderer_TextureMaxHeight = (int)props.limits.maxImageDimension2D;

    pooledGuard(pool, [this] {
        vkDeviceWaitIdle(device);
    });
}

void Gpu::finishTexture(VkFormat format, Texture& tex) {
    VkImageViewCreateInfo vci = {};

    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc(vkCreateImageView(device, &vci, alloc, &tex.view));

    VkSamplerCreateInfo sci = {};

    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.minLod = -1000;
    sci.maxLod = 1000;
    vkc(vkCreateSampler(device, &sci, alloc, &tex.sampler));

    tex.ds = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (linearHdr) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

        ai.descriptorPool = descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &imageSetLayout;
        vkc(vkAllocateDescriptorSets(device, &ai, &tex.imageSet));

        VkDescriptorImageInfo image{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

        write.dstSet = tex.imageSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void Gpu::createTexture(u32 w, u32 h, Texture& tex, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ici = {};

    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkc(vkCreateImage(device, &ici, alloc, &tex.image));

    VkMemoryRequirements req;

    vkGetImageMemoryRequirements(device, tex.image, &req);

    VkMemoryAllocateInfo mai = {};

    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &tex.memory));
    vkc(vkBindImageMemory(device, tex.image, tex.memory, 0));
}

void Gpu::uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex) {
    createTexture(w, h, tex);
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    ScopedGuard stagingDone = [&] {
        if (staging) {
            vkDestroyBuffer(device, staging, alloc);
        }
        if (stagingMem) {
            vkFreeMemory(device, stagingMem, alloc);
        }
    };
    VkBufferCreateInfo bci = {};

    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkc(vkCreateBuffer(device, &bci, alloc, &staging));
    vkGetBufferMemoryRequirements(device, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &stagingMem));
    vkc(vkBindBufferMemory(device, staging, stagingMem, 0));

    {
        void* map = nullptr;
        STD_DEFER {
            if (map) {
                vkUnmapMemory(device, stagingMem);
            }
        };
        vkc(vkMapMemory(device, stagingMem, 0, bytes, 0, &map));
        memcpy(map, rgba, bytes);
    }

    VkCommandPool pool = VK_NULL_HANDLE;
    ScopedGuard commandsDone = [&] {
        if (pool) {
            vkQueueWaitIdle(queue);
            vkDestroyCommandPool(device, pool, alloc);
        }
    };
    VkCommandPoolCreateInfo pci = {};

    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkc(vkCreateCommandPool(device, &pci, alloc, &pool));

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbi = {};

    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkc(vkAllocateCommandBuffers(device, &cbi, &cmd));

    VkCommandBufferBeginInfo begin = {};

    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkc(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier bar = {};

    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = tex.image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy copy = {};

    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    vkc(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit = {};

    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkc(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    vkc(vkQueueWaitIdle(queue));

    finishTexture(VK_FORMAT_R8G8B8A8_UNORM, tex);
}

void Gpu::destroyTexture(Texture& tex) {
    if (tex.ds) {
        ImGui_ImplVulkan_RemoveTexture(tex.ds);
    }
    if (tex.imageSet) {
        vkFreeDescriptorSets(device, descPool, 1, &tex.imageSet);
    }
    if (tex.sampler) {
        vkDestroySampler(device, tex.sampler, alloc);
    }
    if (tex.view) {
        vkDestroyImageView(device, tex.view, alloc);
    }
    if (tex.image) {
        vkDestroyImage(device, tex.image, alloc);
    }
    if (tex.memory) {
        vkFreeMemory(device, tex.memory, alloc);
    }

    tex = Texture();
}

namespace {
    static void recordDraw(VkCommandBuffer command, const ImageDraw& draw, const ImDrawData& data, VkRect2D scissor) {
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, 1, &draw.texture, 0, nullptr);

        ImagePush push;

        push.scale[0] = 2.f / data.DisplaySize.x;
        push.scale[1] = 2.f / data.DisplaySize.y;
        push.translate[0] = -1.f - data.DisplayPos.x * push.scale[0];
        push.translate[1] = -1.f - data.DisplayPos.y * push.scale[1];
        push.rect[0] = draw.x0;
        push.rect[1] = draw.y0;
        push.rect[2] = draw.x1;
        push.rect[3] = draw.y1;
        push.sdrWhiteNits = draw.sdrWhiteNits;
        vkCmdPushConstants(command, draw.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(command, 6, 1, 0, 0);
    }

    static void drawImage(const ImDrawList*, const ImDrawCmd* cmd) {
        const ImageDraw& draw = *(const ImageDraw*)cmd->UserCallbackData;
        auto* state = (ImGui_ImplVulkan_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
        ImDrawData* dd = ImGui::GetDrawData();
        float clipX0 = cmd->ClipRect.x - dd->DisplayPos.x;
        float clipY0 = cmd->ClipRect.y - dd->DisplayPos.y;
        float clipX1 = cmd->ClipRect.z - dd->DisplayPos.x;
        float clipY1 = cmd->ClipRect.w - dd->DisplayPos.y;

        clipX0 = clipX0 < 0.f ? 0.f : clipX0;
        clipY0 = clipY0 < 0.f ? 0.f : clipY0;

        if (clipX1 <= clipX0 || clipY1 <= clipY0) {
            return;
        }

        VkRect2D scissor{{(i32)clipX0, (i32)clipY0}, {(u32)(clipX1 - clipX0), (u32)(clipY1 - clipY0)}};

        recordDraw(state->CommandBuffer, draw, *dd, scissor);
    }
}

namespace {
    constexpr u32 maxTextureCount = 16384;

    struct VulkanRenderer final: Renderer {
        plt::Window* window = nullptr;
        Gpu* gpu = nullptr;
        RenderImage* upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) override;
        RenderImage* import(ObjPool& pool, SharedImage& source, bool hdr) override;

        RenderImage* bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) override;
        RenderShader* compileShader(ObjPool& pool, const void* code, size_t size) override;
        RenderShader* compileKernel(ObjPool& pool, const void* code, size_t size, u32 tile) override;
        RenderImage* shade(ObjPool& pool, RenderShader& shader, u32 width, u32 height, const void* data, size_t size, Runable& retired) override;
        bool beginFrame(u32 width, u32 height) override;
        bool endFrame(ImDrawData* draw) override;
        u32 maxTextureSide() override;
        u32 maxTextures() override;
        bool software() override;
        bool kernels() override;
    };
}

bool VulkanRenderer::beginFrame(u32 width, u32 height) {
    if (gpu->rebuild || gpu->present.width != (int)width || gpu->present.height != (int)height) {
        gpu->createSwapchain(width, height);

        if (gpu->linearHdr) {
            gpu->createSceneTarget(width, height);
        }

        gpu->rebuild = false;
    }

    if (!gpu->acquireFrame()) {
        return false;
    }
    ImGui_ImplVulkan_NewFrame();
    return true;
}

bool VulkanRenderer::endFrame(ImDrawData* draw) {
    gpu->frameRender(draw);
    gpu->framePresent();

    if (gpu->rebuild) {
        window->requestFrame();
    }

    return !gpu->rebuild;
}

u32 VulkanRenderer::maxTextureSide() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    return (u32)(pio.Renderer_TextureMaxWidth < pio.Renderer_TextureMaxHeight ? pio.Renderer_TextureMaxWidth : pio.Renderer_TextureMaxHeight);
}

u32 VulkanRenderer::maxTextures() {
    return maxTextureCount;
}

bool VulkanRenderer::software() {
    VkPhysicalDeviceProperties props;

    vkGetPhysicalDeviceProperties(gpu->phys, &props);

    return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

bool VulkanRenderer::kernels() {
    return gpu->kernels;
}

namespace {
    struct DmaImage final: SharedImage {
        int fd = -1;
        u32 format = 0;
        u32 offset = 0;
        u32 stride = 0;
        u64 modifier = 0;
        u64 allocationSize = 0;
        u8 deviceUuid[VK_UUID_SIZE] = {};
    };

    struct VulkanShader final: RenderShader {
        Gpu* gpu = nullptr;
        Buffer code;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline readPipeline = VK_NULL_HANDLE;
        u32 tile = 0;
        u64 lastUse = 0;

        ~VulkanShader() noexcept;
    };

    struct VulkanImage final: RenderImage {
        Gpu* gpu = nullptr;
        Texture texture;
        u32 width = 0;
        u32 height = 0;
        PixelLayout layout = PixelLayout::Rgba8;
        bool hdr = false;

        const void* source = nullptr;
        size_t stride = 0;
        size_t bytes = 0;
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VulkanShader* shader = nullptr;
        VkDescriptorPool shadePool = VK_NULL_HANDLE;
        VkDescriptorSet shadeSet = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        void* map = nullptr;
        bool hostImported = false;
        bool coherent = true;
        bool dirty = false;
        bool initialized = true;
        u64 lastUse = 0;
        Runable* retired = nullptr;

        ~VulkanImage() noexcept;
        void prepare() override;
        void allocateBuffer(size_t size);
        bool importHost(size_t size);
        void setupShade();
        void record(VkCommandBuffer command);
        void readShaded(int x0, int y0, int x1, int y1, ImagePixels& out);
        void recordRead(VkCommandBuffer cmd, VkFramebuffer framebuffer, int x0, int y0, u32 w, u32 h);
        void draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) override;
        void underlay(ImVec2 lo, ImVec2 hi) override;
        void read(int x0, int y0, int x1, int y1, ImagePixels& out) override;
        void shadeWith(RenderShader& with) override;
    };
}

PollGpu::PollGpu(Gpu* value)
    : gpu(value)
{
}

void PollGpu::ready() {
    if (gpu->retry) {
        gpu->window->requestFrame();
    }
}

Flight::Flight(Gpu* gpu_, u64 serial_, int fd)
    : gpu(gpu_)
    , serial(serial_)
{
    waiter.fd.fd = fd;
    waiter.fd.flags = PollFlag::In;
    waiter.callback = this;

    for (VulkanImage* image : gpu->drawn) {
        if (image->retired) {
            images.pushBack(image);
        }
    }
}

void Flight::ready(PollFD) {
    close(waiter.fd.fd);
    gpu->landed(this);
}

void Gpu::track(VkFence fence) {
    VkFenceGetFdInfoKHR info{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
    int fd = -1;

    info.fence = fence;
    info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
    vkc(fenceFd(device, &info, &fd));

    Flight* flight = smallObjects->make<Flight>(this, submitted, fd);

    drawn.clear();

    if (fd < 0) {
        landed(flight);
    } else {
        platform->poller()->arm(flight->waiter);
    }
}

void Gpu::landed(Flight* flight) {
    Vector<VulkanImage*> done;

    if (flight->serial > completed) {
        completed = flight->serial;
    }
    done.xchg(flight->images);
    smallObjects->release(flight);
    for (VulkanImage* image : done) {
        image->retired->run();
    }
    if (retry) {
        window->requestFrame();
    }
}

void Gpu::recordImages(VkCommandBuffer command) {
    for (VulkanImage* image : drawn) {
        image->record(command);
        image->lastUse = submitted + 1;
        if (image->shader) {
            image->shader->lastUse = submitted + 1;
        }
    }
}

bool VulkanImage::importHost(size_t size) {
    if (!gpu->hostAlignment || (uintptr_t)source % gpu->hostAlignment || size % gpu->hostAlignment) {
        return false;
    }
    VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (gpu->hostProperties(gpu->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, source, &host) != VK_SUCCESS) {
        return false;
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = &external;
    info.size = bytes;
    info.usage = usage;
    if (vkCreateBuffer(gpu->device, &info, gpu->alloc, &buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(gpu->device, buffer, &requirements);
    size_t allocation = (requirements.size + gpu->hostAlignment - 1) / gpu->hostAlignment * gpu->hostAlignment;
    if (allocation > size) {
        return false;
    }
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu->phys, &properties);
    u32 type = UINT32_MAX;
    for (u32 i = 0; i < properties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & host.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        return false;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = const_cast<void*>(source);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.pNext = &import;
    memory.allocationSize = allocation;
    memory.memoryTypeIndex = type;
    if (vkAllocateMemory(gpu->device, &memory, gpu->alloc, &bufferMemory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindBufferMemory(gpu->device, buffer, bufferMemory, 0) != VK_SUCCESS) {
        return false;
    }
    coherent = (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!coherent && vkMapMemory(gpu->device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        return false;
    }
    return true;
}

void VulkanImage::allocateBuffer(size_t size) {
    if (importHost(size)) {
        hostImported = true;
        return;
    }
    if (map) {
        vkUnmapMemory(gpu->device, bufferMemory);
        map = nullptr;
    }
    if (buffer) {
        vkDestroyBuffer(gpu->device, buffer, gpu->alloc);
        buffer = VK_NULL_HANDLE;
    }
    if (bufferMemory) {
        vkFreeMemory(gpu->device, bufferMemory, gpu->alloc);
        bufferMemory = VK_NULL_HANDLE;
    }
    coherent = true;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    gpu->vkc(vkCreateBuffer(gpu->device, &info, gpu->alloc, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(gpu->device, buffer, &requirements);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = gpu->findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gpu->vkc(vkAllocateMemory(gpu->device, &memory, gpu->alloc, &bufferMemory));
    gpu->vkc(vkBindBufferMemory(gpu->device, buffer, bufferMemory, 0));
    gpu->vkc(vkMapMemory(gpu->device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &map));
}

void VulkanImage::prepare() {
    if (source) {
        if (!hostImported) {
            memcpy(map, source, bytes);
        }
        dirty = true;
    }
}

void VulkanImage::record(VkCommandBuffer command) {
    if (!dirty) {
        return;
    }
    if (!coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = bufferMemory;
        range.size = VK_WHOLE_SIZE;
        gpu->vkc(vkFlushMappedMemoryRanges(gpu->device, 1, &range));
    }
    if (shader) {
        dirty = false;
        return;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.bufferRowLength = (u32)(stride / 4);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized = true;
    dirty = false;
}

RenderImage* VulkanRenderer::bind(ObjPool& pool, u32 width, u32 height, const void* data, size_t size, size_t stride, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || stride < (size_t)width * 4 || stride % 4 || stride / 4 > UINT32_MAX || stride > size / height) {
        fail(StringView(u8"invalid bound image buffer"));
    }
    VulkanImage* image = pool.make<VulkanImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->source = data;
    image->stride = stride;
    image->bytes = stride * height;
    image->retired = &retired;
    image->initialized = false;
    gpu->createTexture(width, height, image->texture);
    gpu->finishTexture(VK_FORMAT_R8G8B8A8_UNORM, image->texture);
    image->allocateBuffer(size);
    return image;
}

RenderShader* VulkanRenderer::compileShader(ObjPool& pool, const void* code, size_t size) {
    if (!code || !size || size % 4) {
        fail(StringView(u8"invalid shader code"));
    }
    VulkanShader* shader = pool.make<VulkanShader>();
    shader->gpu = gpu;
    shader->code.append(code, size);
    shader->pipeline = gpu->vertexlessPipeline(gpu_image_vert_spv, sizeof(gpu_image_vert_spv), (const u32*)shader->code.data(), size, gpu->shadePipelineLayout, gpu->linearHdr ? gpu->scenePass : gpu->present.renderPass);
    return shader;
}

RenderShader* VulkanRenderer::compileKernel(ObjPool& pool, const void* code, size_t size, u32 tile) {
    if (!code || !size || size % 4 || !tile) {
        fail(StringView(u8"invalid kernel code"));
    }
    if (!gpu->pushDescriptorSet) {
        fail(StringView(u8"this device runs no kernels"));
    }
    VulkanShader* shader = pool.make<VulkanShader>();
    shader->gpu = gpu;
    shader->tile = tile;

    VkShaderModule module = gpu->shaderModule((const u32*)code, size);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};

    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = gpu->kernelPipelineLayout;

    VkResult made = vkCreateComputePipelines(gpu->device, VK_NULL_HANDLE, 1, &ci, gpu->alloc, &shader->pipeline);

    vkDestroyShaderModule(gpu->device, module, gpu->alloc);
    gpu->vkc(made);
    return shader;
}

RenderImage* VulkanRenderer::shade(ObjPool& pool, RenderShader& shader, u32 width, u32 height, const void* data, size_t size, Runable& retired) {
    checkImageSize(width, height, maxTextureSide());
    if (!data || !size || size % 4) {
        fail(StringView(u8"invalid shaded image source"));
    }
    VulkanImage* image = pool.make<VulkanImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->layout = PixelLayout::Rgba16f;
    image->source = data;
    image->bytes = size;
    image->usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    image->shader = static_cast<VulkanShader*>(&shader);
    image->retired = &retired;
    image->allocateBuffer(size);
    image->setupShade();
    return image;
}

VulkanImage::~VulkanImage() noexcept {
    for (const VulkanImage* image : gpu->drawn) {
        STD_INSIST(image != this);
    }
    if (lastUse > gpu->completed) {
        vkDeviceWaitIdle(gpu->device);
        gpu->completed = gpu->submitted;
    }
    if (map) {
        vkUnmapMemory(gpu->device, bufferMemory);
    }
    if (buffer) {
        vkDestroyBuffer(gpu->device, buffer, gpu->alloc);
    }
    if (bufferMemory) {
        vkFreeMemory(gpu->device, bufferMemory, gpu->alloc);
    }
    if (shadePool) {
        vkDestroyDescriptorPool(gpu->device, shadePool, gpu->alloc);
    }
    gpu->destroyTexture(texture);
}

VulkanShader::~VulkanShader() noexcept {
    if (lastUse > gpu->completed) {
        vkDeviceWaitIdle(gpu->device);
        gpu->completed = gpu->submitted;
    }
    if (pipeline) {
        vkDestroyPipeline(gpu->device, pipeline, gpu->alloc);
    }
    if (readPipeline) {
        vkDestroyPipeline(gpu->device, readPipeline, gpu->alloc);
    }
}

void VulkanImage::setupShade() {
    VkDescriptorPoolSize sizes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};

    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &sizes;
    gpu->vkc(vkCreateDescriptorPool(gpu->device, &pool, gpu->alloc, &shadePool));

    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

    allocate.descriptorPool = shadePool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &gpu->shadeSetLayout;
    gpu->vkc(vkAllocateDescriptorSets(gpu->device, &allocate, &shadeSet));

    VkDescriptorBufferInfo source{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

    write.dstSet = shadeSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &source;
    vkUpdateDescriptorSets(gpu->device, 1, &write, 0, nullptr);
}

void VulkanImage::shadeWith(RenderShader& with) {
    if (!shader) {
        fail(StringView(u8"only a shaded image takes another shader"));
    }
    shader = static_cast<VulkanShader*>(&with);
}

void VulkanImage::underlay(ImVec2 lo, ImVec2 hi) {
    if (!shader) {
        fail(StringView(u8"only a shaded image goes under the interface"));
    }
    if (shader->tile) {
        if (!gpu->kernels) {
            fail(StringView(u8"this display takes no kernels"));
        }
        gpu->drawn.pushBack(this);
        gpu->kernelDraws.pushBack(KernelDraw{shader->pipeline, buffer, shader->tile, (u32)(hi.x - lo.x), (u32)(hi.y - lo.y)});
        return;
    }
    gpu->drawn.pushBack(this);
    gpu->underlays.pushBack(ImageDraw{gpu, shader->pipeline, gpu->shadePipelineLayout, shadeSet, lo.x, lo.y, hi.x, hi.y, gpu->sdrWhiteNits});
}

void VulkanImage::draw(ImDrawList& list, ImVec2 lo, ImVec2 hi) {
    if (shader && shader->tile) {
        fail(StringView(u8"a kernel image only goes under the interface"));
    }
    gpu->drawn.pushBack(this);
    if (shader) {
        ImageDraw draw{gpu, shader->pipeline, gpu->shadePipelineLayout, shadeSet, lo.x, lo.y, hi.x, hi.y, gpu->sdrWhiteNits};
        list.AddCallback(drawImage, &draw, sizeof(draw));
        list.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else if (hdr) {
        ImageDraw draw{gpu, gpu->imagePipeline, gpu->imagePipelineLayout, texture.imageSet, lo.x, lo.y, hi.x, hi.y, gpu->sdrWhiteNits};
        list.AddCallback(drawImage, &draw, sizeof(draw));
        list.AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    } else {
        list.AddImage((ImTextureID)texture.ds, lo, hi);
    }
}

namespace {
    static PixelLayout pixelLayout(VkFormat format) {
        switch (format) {
            case VK_FORMAT_R8G8B8A8_UNORM: {
                return PixelLayout::Rgba8;
            }
            case VK_FORMAT_B8G8R8A8_UNORM: {
                return PixelLayout::Bgra8;
            }
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                return PixelLayout::Rgb10A2;
            }
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                return PixelLayout::Bgr10A2;
            }
            default: {
                fail(StringView(u8"unsupported shared image format"));
            }
        }
    }

    static bool parseShared(StringView spec, DmaImage& img) {
        u64 values[7] = {};
        size_t pos = 0;

        for (int i = 0; i < 7; i++) {
            size_t begin = pos;

            while (pos < spec.length() && spec[pos] >= '0' && spec[pos] <= '9') {
                u64 digit = (u64)(spec[pos] - '0');

                if (values[i] > (UINT64_MAX - digit) / 10) {
                    return false;
                }

                values[i] = values[i] * 10 + digit;
                pos++;
            }

            if (pos == begin || pos >= spec.length() || spec[pos] != ':') {
                return false;
            }

            pos++;
        }

        if (spec.length() - pos != 2 * VK_UUID_SIZE) {
            return false;
        }

        for (size_t i = 0; i < 2 * VK_UUID_SIZE; i++) {
            char c = spec[pos + i];
            int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

            if (nibble < 0) {
                return false;
            }

            img.deviceUuid[i / 2] = (u8)(img.deviceUuid[i / 2] << 4 | nibble);
        }

        for (int i = 0; i < 5; i++) {
            if (values[i] > 0xffffffffu) {
                return false;
            }
        }
        img.width = (u32)values[0];
        img.height = (u32)values[1];
        img.format = (u32)values[2];
        img.offset = (u32)values[3];
        img.stride = (u32)values[4];
        img.modifier = values[5];
        img.allocationSize = values[6];

        return img.width && img.height && img.stride && img.allocationSize;
    }
}

SharedImage* createVulkanSharedImage(ObjPool& pool, StringView description, intptr_t handle) {
    DmaImage* image = pool.make<DmaImage>();
    if (!parseShared(description, *image)) {
        fail(StringView(u8"bad shared screenshot metadata"));
    }
    pixelLayout((VkFormat)image->format);
    checkImageSize(image->width, image->height, 65535);
    if (handle < 0 || handle > 0x7fffffff || image->stride < (u64)image->width * 4 || image->offset > image->allocationSize || (u64)(image->height - 1) * image->stride + (u64)image->width * 4 > image->allocationSize - image->offset) {
        fail(StringView(u8"bad shared screenshot metadata"));
    }
    image->fd = fcntl((int)handle, F_DUPFD_CLOEXEC, 0);
    if (image->fd < 0) {
        fail(StringView(u8"cannot take the shared screenshot fd"));
    }
    pooledGuard(pool, [image] {
        close(image->fd);
    });
    return image;
}

RenderImage* VulkanRenderer::import(ObjPool& pool, SharedImage& source, bool hdr) {
    DmaImage& img = static_cast<DmaImage&>(source);
    checkImageSize(img.width, img.height, maxTextureSide());
    if (hdr && !gpu->linearHdr) {
        fail(StringView(u8"HDR image needs an HDR renderer"));
    }
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    VkPhysicalDeviceExternalImageFormatInfo externalQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    VkPhysicalDeviceImageFormatInfo2 formatQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    VkExternalImageFormatProperties externalSupport{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 formatSupport{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};

    modifierQuery.drmFormatModifier = img.modifier;
    modifierQuery.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    externalQuery.pNext = &modifierQuery;
    externalQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    formatQuery.pNext = &externalQuery;
    formatQuery.format = (VkFormat)img.format;
    formatQuery.type = VK_IMAGE_TYPE_2D;
    formatQuery.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    formatQuery.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    formatSupport.pNext = &externalSupport;

    VkDrmFormatModifierPropertiesListEXT modifiers{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 formatFeatures{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    Vector<VkDrmFormatModifierPropertiesEXT> modifierFeatures;
    VkFormatFeatureFlags features = 0;

    formatFeatures.pNext = &modifiers;
    vkGetPhysicalDeviceFormatProperties2(gpu->phys, (VkFormat)img.format, &formatFeatures);
    modifierFeatures.zero(modifiers.drmFormatModifierCount);
    modifiers.pDrmFormatModifierProperties = modifierFeatures.mutData();
    vkGetPhysicalDeviceFormatProperties2(gpu->phys, (VkFormat)img.format, &formatFeatures);

    for (u32 i = 0; i < modifiers.drmFormatModifierCount; i++) {
        if (modifierFeatures[i].drmFormatModifier == img.modifier) {
            features = modifierFeatures[i].drmFormatModifierTilingFeatures;
        }
    }

    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;

    if ((features & needed) != needed || vkGetPhysicalDeviceImageFormatProperties2(gpu->phys, &formatQuery, &formatSupport) != VK_SUCCESS || !(externalSupport.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        fail(StringView(u8"vulkan cannot import the shared screenshot's format"));
    }

    VulkanImage* image = pool.make<VulkanImage>();
    image->gpu = gpu;
    image->width = img.width;
    image->height = img.height;
    image->layout = pixelLayout((VkFormat)img.format);
    image->hdr = hdr;
    Texture& tex = image->texture;

    VkSubresourceLayout plane = {};

    plane.offset = img.offset;
    plane.rowPitch = img.stride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {};

    modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier.drmFormatModifier = img.modifier;
    modifier.drmFormatModifierPlaneCount = 1;
    modifier.pPlaneLayouts = &plane;

    VkExternalMemoryImageCreateInfo external = {};

    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &modifier;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ici = {};

    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)img.format;
    ici.extent = {img.width, img.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    gpu->vkc(vkCreateImage(gpu->device, &ici, gpu->alloc, &tex.image));

    VkMemoryRequirements req = {};

    vkGetImageMemoryRequirements(gpu->device, tex.image, &req);

    auto getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(gpu->device, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};

    if (!getFdProps || getFdProps(gpu->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, img.fd, &fdProps) != VK_SUCCESS) {
        fail(StringView(u8"cannot query shared screenshot memory"));
    }

    u32 memoryTypes = req.memoryTypeBits & fdProps.memoryTypeBits;

    if (!memoryTypes) {
        fail(StringView(u8"shared screenshot memory is incompatible"));
    }

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};

    dedicated.image = tex.image;

    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};

    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int ownedFd = fcntl(img.fd, F_DUPFD_CLOEXEC, 0);
    if (ownedFd < 0) {
        fail(StringView(u8"cannot duplicate shared image fd"));
    }
    ScopedGuard descriptor = [&] mutable -> void {
        if (ownedFd >= 0) {
            close(ownedFd);
        }
    };
    import.fd = ownedFd;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.pNext = &import;
    mai.allocationSize = img.allocationSize;
    mai.memoryTypeIndex = gpu->findMemoryType(memoryTypes, 0);
    VkResult allocated = vkAllocateMemory(gpu->device, &mai, gpu->alloc, &tex.memory);
    if (allocated == VK_SUCCESS) {
        ownedFd = -1;
    }
    gpu->vkc(allocated);
    gpu->vkc(vkBindImageMemory(gpu->device, tex.image, tex.memory, 0));

    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

    pci.queueFamilyIndex = gpu->queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ScopedGuard commandsDone = [&] mutable -> void {
        if (commands) {
            vkQueueWaitIdle(gpu->queue);
            vkDestroyCommandPool(gpu->device, commands, gpu->alloc);
        }
    };
    gpu->vkc(vkCreateCommandPool(gpu->device, &pci, gpu->alloc, &commands));
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    cai.commandPool = commands;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    gpu->vkc(vkAllocateCommandBuffers(gpu->device, &cai, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    gpu->vkc(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = gpu->queueFamily;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    gpu->vkc(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    gpu->vkc(vkQueueSubmit(gpu->queue, 1, &submit, VK_NULL_HANDLE));
    gpu->vkc(vkQueueWaitIdle(gpu->queue));

    gpu->finishTexture((VkFormat)img.format, tex);
    return image;
}

void VulkanImage::read(int x0, int y0, int x1, int y1, ImagePixels& out) {
    checkImageRegion(width, height, x0, y0, x1, y1);
    if (shader) {
        readShaded(x0, y0, x1, y1, out);
        return;
    }
    const Texture& tex = texture;
    out.width = (u32)(x1 - x0);
    out.height = (u32)(y1 - y0);

    size_t pixel = layout == PixelLayout::Rgba16f ? 8 : 4;
    VkDeviceSize bytes = (VkDeviceSize)out.width * out.height * pixel;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ScopedGuard readback = [&] mutable -> void {
        if (buffer) {
            vkDestroyBuffer(gpu->device, buffer, gpu->alloc);
        }
        if (memory) {
            vkFreeMemory(gpu->device, memory, gpu->alloc);
        }
    };
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    gpu->vkc(vkCreateBuffer(gpu->device, &bci, gpu->alloc, &buffer));

    VkMemoryRequirements req = {};

    vkGetBufferMemoryRequirements(gpu->device, buffer, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gpu->vkc(vkAllocateMemory(gpu->device, &mai, gpu->alloc, &memory));
    gpu->vkc(vkBindBufferMemory(gpu->device, buffer, memory, 0));

    VkCommandPool pool = VK_NULL_HANDLE;
    ScopedGuard commandsDone = [&] mutable -> void {
        if (pool) {
            vkQueueWaitIdle(gpu->queue);
            vkDestroyCommandPool(gpu->device, pool, gpu->alloc);
        }
    };
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

    pci.queueFamilyIndex = gpu->queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    gpu->vkc(vkCreateCommandPool(gpu->device, &pci, gpu->alloc, &pool));

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    gpu->vkc(vkAllocateCommandBuffers(gpu->device, &cai, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    gpu->vkc(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy = {};

    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {x0, y0, 0};
    copy.imageExtent = {out.width, out.height, 1};
    vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    gpu->vkc(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    gpu->vkc(vkQueueSubmit(gpu->queue, 1, &submit, VK_NULL_HANDLE));
    gpu->vkc(vkQueueWaitIdle(gpu->queue));

    void* map = nullptr;

    ScopedGuard mapped = [&] mutable -> void {
        if (map) {
            vkUnmapMemory(gpu->device, memory);
        }
    };
    gpu->vkc(vkMapMemory(gpu->device, memory, 0, bytes, 0, &map));
    unpackPixels(map, out.width, out.height, (size_t)out.width * pixel, layout, out);
}

void VulkanImage::readShaded(int x0, int y0, int x1, int y1, ImagePixels& out) {
    u32 w = (u32)(x1 - x0);
    u32 h = (u32)(y1 - y0);
    VkDevice device = gpu->device;
    bool kernel = shader->tile != 0;

    if (!kernel && !shader->readPipeline) {
        shader->readPipeline = gpu->vertexlessPipeline(gpu_image_vert_spv, sizeof(gpu_image_vert_spv), (const u32*)shader->code.data(), shader->code.length(), gpu->shadePipelineLayout, gpu->readPass);
    }

    if (!coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = bufferMemory;
        range.size = VK_WHOLE_SIZE;
        gpu->vkc(vkFlushMappedMemoryRanges(device, 1, &range));
    }

    Texture target;

    gpu->createTexture(kernel ? width : w, kernel ? height : h, target, VK_FORMAT_R16G16B16A16_SFLOAT, kernel ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    STD_DEFER {
        gpu->destroyTexture(target);
    };

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageView view = VK_NULL_HANDLE;

    vci.image = target.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gpu->vkc(vkCreateImageView(device, &vci, gpu->alloc, &view));
    STD_DEFER {
        vkDestroyImageView(device, view, gpu->alloc);
    };

    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    fci.renderPass = gpu->readPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &view;
    fci.width = w;
    fci.height = h;
    fci.layers = 1;
    if (!kernel) {
        gpu->vkc(vkCreateFramebuffer(device, &fci, gpu->alloc, &framebuffer));
    }
    STD_DEFER {
        vkDestroyFramebuffer(device, framebuffer, gpu->alloc);
    };

    VkDeviceSize bytes = (VkDeviceSize)w * h * 8;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkBuffer readback = VK_NULL_HANDLE;

    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    gpu->vkc(vkCreateBuffer(device, &bci, gpu->alloc, &readback));
    STD_DEFER {
        vkDestroyBuffer(device, readback, gpu->alloc);
    };

    VkMemoryRequirements req;

    vkGetBufferMemoryRequirements(device, readback, &req);

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;

    mai.allocationSize = req.size;
    mai.memoryTypeIndex = gpu->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    gpu->vkc(vkAllocateMemory(device, &mai, gpu->alloc, &readbackMemory));
    STD_DEFER {
        vkFreeMemory(device, readbackMemory, gpu->alloc);
    };
    gpu->vkc(vkBindBufferMemory(device, readback, readbackMemory, 0));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool = VK_NULL_HANDLE;

    pci.queueFamilyIndex = gpu->queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    gpu->vkc(vkCreateCommandPool(device, &pci, gpu->alloc, &pool));
    STD_DEFER {
        vkQueueWaitIdle(gpu->queue);
        vkDestroyCommandPool(device, pool, gpu->alloc);
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    gpu->vkc(vkAllocateCommandBuffers(device, &cai, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    gpu->vkc(vkBeginCommandBuffer(cmd, &begin));

    VkBufferImageCopy copy = {};

    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};

    if (kernel) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        gpu->pushKernel(cmd, shader->pipeline, buffer, view);
        vkCmdDispatch(cmd, (width + shader->tile - 1) / shader->tile, (height + shader->tile - 1) / shader->tile, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        copy.imageOffset = {x0, y0, 0};
    } else {
        recordRead(cmd, framebuffer, x0, y0, w, h);
    }

    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    gpu->vkc(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    gpu->vkc(vkQueueSubmit(gpu->queue, 1, &submit, VK_NULL_HANDLE));
    gpu->vkc(vkQueueWaitIdle(gpu->queue));

    void* map = nullptr;

    gpu->vkc(vkMapMemory(device, readbackMemory, 0, bytes, 0, &map));
    STD_DEFER {
        vkUnmapMemory(device, readbackMemory);
    };
    unpackPixels(map, w, h, (size_t)w * 8, PixelLayout::Rgba16f, out);
}

void VulkanImage::recordRead(VkCommandBuffer cmd, VkFramebuffer framebuffer, int x0, int y0, u32 w, u32 h) {
    VkClearValue clear{};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};

    rp.renderPass = gpu->readPass;
    rp.framebuffer = framebuffer;
    rp.renderArea.extent = {w, h};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D scissor{{0, 0}, {w, h}};
    ImagePush push;

    push.scale[0] = 2.f / (float)w;
    push.scale[1] = 2.f / (float)h;
    push.translate[0] = -1.f - (float)x0 * push.scale[0];
    push.translate[1] = -1.f - (float)y0 * push.scale[1];
    push.rect[0] = 0.f;
    push.rect[1] = 0.f;
    push.rect[2] = (float)width;
    push.rect[3] = (float)height;
    push.sdrWhiteNits = gpu->sdrWhiteNits;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shader->readPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu->shadePipelineLayout, 0, 1, &shadeSet, 0, nullptr);
    vkCmdPushConstants(cmd, gpu->shadePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

RenderImage* VulkanRenderer::upload(ObjPool& pool, u32 width, u32 height, const void* rgba, bool hdr) {
    checkImageSize(width, height, maxTextureSide());
    if (!rgba || (hdr && !gpu->linearHdr)) {
        fail(StringView(u8"invalid renderer image source"));
    }
    VulkanImage* image = pool.make<VulkanImage>();
    image->gpu = gpu;
    image->width = width;
    image->height = height;
    image->hdr = hdr;
    gpu->uploadTexture(width, height, (const u8*)rgba, image->texture);
    return image;
}

Renderer* createVulkanRenderer(ObjPool& pool, plt::Platform& platform, plt::Window& window, const RendererOptions& options) {
    GpuOptions wants;
    wants.chaos = VulkanChaos::create(pool);
    wants.textures = maxTextureCount;
    wants.hdr = options.hdr;
    wants.sharedBuffer = options.shared != nullptr;
    if (options.shared) {
        wants.deviceUuid = static_cast<DmaImage*>(options.shared)->deviceUuid;
    }
    Gpu& gpu = *Gpu::create(pool, wants);
    gpu.smallObjects = SmallObjAllocator::create(&pool);
    gpu.platform = &platform;
    gpu.window = &window;
    gpu.timer = pool.make<PollGpu>(&gpu);
    pooledGuard(pool, [&gpu] {
        gpu.platform->poller()->cancel(*gpu.timer);
    });
    VkSurfaceKHR surface = gpu.createSurface(window);
    plt::WindowInfo info = window.info();
    gpu.setupWindow(pool, surface, (int)info.width, (int)info.height, wants.hdr);
    if (wants.hdr) {
        gpu.setupLinearHdr(pool, info.width, info.height);
    }
    gpu.setupBackend(pool, wants.hdr);
    gpu.sdrWhiteNits = options.sdrWhiteNits;
    gpu.present.clear.color.float32[0] = 0.1f;
    gpu.present.clear.color.float32[1] = 0.1f;
    gpu.present.clear.color.float32[2] = 0.1f;
    gpu.present.clear.color.float32[3] = 1.f;
    VulkanRenderer* renderer = pool.make<VulkanRenderer>();
    renderer->gpu = &gpu;
    renderer->window = &window;
    return renderer;
}
