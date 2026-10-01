#include "gpu.h"

#include "ui.h"
#include "util.h"
#include "pooled.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/ios/sys.h>

#include <time.h>
#include <stdlib.h>
#include <string.h>

// the window's surface: a CAMetalLayer on macOS (MoltenVK's metal
// surface), a Wayland surface elsewhere; the platform headers name the
// native objects through pointers only
#if defined(__APPLE__)
    #include <vulkan/vulkan_metal.h>
#else
struct wl_display;
struct wl_surface;

    #include <vulkan/vulkan_wayland.h>
#endif

#include <plt/platform.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <fullscreen_vert.spv.h>
#include <gpu_image_frag.spv.h>
#include <gpu_image_vert.spv.h>
#include <gpu_output_frag.spv.h>
#include <gpu_scene_frag.spv.h>

using namespace stl;

ChaosMonkey* gChaos = nullptr;

VkAllocationCallbacks* gAlloc = nullptr;
VkInstance gInstance = VK_NULL_HANDLE;
VkPhysicalDevice gPhys = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
u32 gQueueFamily = (u32)-1;
VkQueue gQueue = VK_NULL_HANDLE;
VkDescriptorPool gDescPool = VK_NULL_HANDLE;
Presenter gPresent;
float gSdrWhiteNits = 203.f;
bool gRebuild = false;
bool gLinearHdr = false;

namespace {
    // the frame trace's clocks: when the last frame began, how many there
    // were, and how long each phase of this one took
    u64 gFrameBegan = 0;
    u64 gFrameCount = 0;
    u64 gAcquireNs = 0;
    u64 gFenceNs = 0;
    u64 gSubmitNs = 0;
    u64 gPresentNs = 0;

    // three: MoltenVK takes the CAMetalLayer drawable at submit, and with
    // two the submit blocks until the frame before last has left the
    // screen, a display link tick lost each time
    constexpr u32 kMinImageCount = 3;
    VkRenderPass gScenePass = VK_NULL_HANDLE;
    VkImage gSceneImage = VK_NULL_HANDLE;
    VkDeviceMemory gSceneMemory = VK_NULL_HANDLE;
    VkImageView gSceneView = VK_NULL_HANDLE;
    VkFramebuffer gSceneFramebuffer = VK_NULL_HANDLE;
    VkSampler gSceneSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout gOutputSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet gOutputSet = VK_NULL_HANDLE;
    VkPipelineLayout gOutputPipelineLayout = VK_NULL_HANDLE;
    VkPipeline gOutputPipeline = VK_NULL_HANDLE;
    // the image pipeline in the HDR scene: drawImage runs it from an ImGui
    // draw callback, PQ decoding the image into the scene's linear light
    VkDescriptorSetLayout gImageSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout gImagePipelineLayout = VK_NULL_HANDLE;
    VkPipeline gImagePipeline = VK_NULL_HANDLE;

    // gpu_image.vert's push constants: ImGui's own scale and
    // translate, the quad in ImGui's screen space, the white the fragment
    // stage divides its nits by
    struct ImagePush {
        float scale[2];
        float translate[2];
        float rect[4];
        float sdrWhiteNits;
    };

    bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
        u32 count = 0;

        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        Vector<VkExtensionProperties> props;

        props.zero(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.mutData());

        bool offered = false;

        for (const VkExtensionProperties& prop : props) {
            if (StringView(prop.extensionName) == StringView(name)) {
                offered = true;

                break;
            }
        }

        return gChaos->deviceExtension(name, offered);
    }

    // a discrete GPU when there is one, the first device otherwise
    VkPhysicalDevice selectPhysicalDevice() {
        u32 count = 0;

        vkc(vkEnumeratePhysicalDevices(gInstance, &count, nullptr));
        count = gChaos->count("devices"_sv, count);

        if (!count) {
            fail("no vulkan device"_sv);
        }

        Vector<VkPhysicalDevice> devices;

        devices.zero(count);
        vkc(vkEnumeratePhysicalDevices(gInstance, &count, devices.mutData()));

        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties props;

            vkGetPhysicalDeviceProperties(device, &props);

            if (gChaos->deviceType(props.deviceType) == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                return device;
            }
        }

        return devices[0];
    }

    u32 selectQueueFamily(VkPhysicalDevice device) {
        u32 count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

        Vector<VkQueueFamilyProperties> families;

        families.zero(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.mutData());
        // the driver's own query stays whole; the answer is what the seam bends
        count = gChaos->count("queue-families"_sv, count);

        for (u32 i = 0; i < count; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                return i;
            }
        }

        fail("no vulkan graphics queue"_sv);
    }

    // the first of the wanted formats the surface offers in the color space,
    // else whatever it offers first: the HDR caller checks the color space
    VkSurfaceFormatKHR selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace) {
        u32 count = 0;

        vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, surface, &count, nullptr));
        count = gChaos->count("surface-formats"_sv, count);

        if (!count) {
            fail("vulkan WSI offers no surface format"_sv);
        }

        Vector<VkSurfaceFormatKHR> available;

        available.zero(count);
        vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, surface, &count, available.mutData()));

        for (u32 i = 0; i < nwanted; i++) {
            for (const VkSurfaceFormatKHR& format : available) {
                if (format.format == wanted[i] && format.colorSpace == colorSpace) {
                    return format;
                }
            }
        }

        return available[0];
    }

    void createPresentPass() {
        VkAttachmentDescription attachment{};

        attachment.format = gPresent.format.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};

        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;

        // the acquire's semaphore is waited at color output: the pass's
        // write may not start before it
        VkSubpassDependency dependency{};

        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

        rpci.attachmentCount = 1;
        rpci.pAttachments = &attachment;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dependency;
        vkc(vkCreateRenderPass(gDevice, &rpci, gAlloc, &gPresent.renderPass));
    }

    void destroyFrames() {
        for (size_t i = 0; i < gPresent.frames.length(); i++) {
            const Frame& frame = gPresent.frames[i];

            if (frame.framebuffer) {
                vkDestroyFramebuffer(gDevice, frame.framebuffer, gAlloc);
            }
            if (frame.view) {
                vkDestroyImageView(gDevice, frame.view, gAlloc);
            }
            if (frame.fence) {
                vkDestroyFence(gDevice, frame.fence, gAlloc);
            }
            if (frame.commandPool) {
                vkDestroyCommandPool(gDevice, frame.commandPool, gAlloc);
            }
        }

        for (size_t i = 0; i < gPresent.syncs.length(); i++) {
            const Sync& sync = gPresent.syncs[i];

            if (sync.acquired) {
                vkDestroySemaphore(gDevice, sync.acquired, gAlloc);
            }
            if (sync.rendered) {
                vkDestroySemaphore(gDevice, sync.rendered, gAlloc);
            }
        }

        gPresent.frames.clear();
        gPresent.syncs.clear();
    }

    // the swapchain at the window's size; a swapchain already there is
    // retired by the new one, its frames torn down once the device is idle
    void createSwapchain(u32 width, u32 height) {
        VkSurfaceCapabilitiesKHR caps;

        vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhys, gPresent.surface, &caps));
        gChaos->imageCounts(caps);

        // Wayland leaves the extent to the client: the window's size, held
        // to what the surface allows
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

        ci.surface = gPresent.surface;
        ci.minImageCount = images;
        ci.imageFormat = gPresent.format.format;
        ci.imageColorSpace = gPresent.format.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = gPresent.swapchain;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;

        // a driver that cannot present to this compositor's surface fails
        // here, and that is the tool's own report; a refusal the monkey
        // made up leaves the driver's swapchain behind, and it goes
        VkResult made = vkCreateSwapchainKHR(gDevice, &ci, gAlloc, &swapchain);

        if (gChaos->vulkanAt("swapchain"_sv, made) < 0) {
            if (made == VK_SUCCESS) {
                vkDestroySwapchainKHR(gDevice, swapchain, gAlloc);
            }

            fail("vulkan cannot make a swapchain on this surface"_sv);
        }

        if (gPresent.swapchain) {
            vkDeviceWaitIdle(gDevice);
            destroyFrames();
            vkDestroySwapchainKHR(gDevice, gPresent.swapchain, gAlloc);
        }

        gPresent.swapchain = swapchain;
        gPresent.width = (int)extent.width;
        gPresent.height = (int)extent.height;
        gPresent.frameIndex = 0;
        gPresent.syncIndex = 0;

        u32 count = 0;

        vkc(vkGetSwapchainImagesKHR(gDevice, swapchain, &count, nullptr));

        Vector<VkImage> handles;

        handles.zero(count);
        vkc(vkGetSwapchainImagesKHR(gDevice, swapchain, &count, handles.mutData()));
        gPresent.frames.zero(count);
        gPresent.syncs.zero(count + 1);

        for (u32 i = 0; i < count; i++) {
            Frame& frame = gPresent.frames.mut(i);

            frame.image = handles[i];

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

            vci.image = frame.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = gPresent.format.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkc(vkCreateImageView(gDevice, &vci, gAlloc, &frame.view));

            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

            fci.renderPass = gPresent.renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &frame.view;
            fci.width = extent.width;
            fci.height = extent.height;
            fci.layers = 1;
            vkc(vkCreateFramebuffer(gDevice, &fci, gAlloc, &frame.framebuffer));

            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

            pci.queueFamilyIndex = gQueueFamily;
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            vkc(vkCreateCommandPool(gDevice, &pci, gAlloc, &frame.commandPool));

            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

            cai.commandPool = frame.commandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkc(vkAllocateCommandBuffers(gDevice, &cai, &frame.commandBuffer));

            // signaled: the first frame has nothing to wait for
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkc(vkCreateFence(gDevice, &fenceInfo, gAlloc, &frame.fence));
        }

        for (u32 i = 0; i <= count; i++) {
            Sync& sync = gPresent.syncs.mut(i);
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

            vkc(vkCreateSemaphore(gDevice, &semaphoreInfo, gAlloc, &sync.acquired));
            vkc(vkCreateSemaphore(gDevice, &semaphoreInfo, gAlloc, &sync.rendered));
        }
    }

    void destroyPresenter() {
        destroyFrames();

        if (gPresent.swapchain) {
            vkDestroySwapchainKHR(gDevice, gPresent.swapchain, gAlloc);
        }
        if (gPresent.renderPass) {
            vkDestroyRenderPass(gDevice, gPresent.renderPass, gAlloc);
        }
        if (gPresent.surface) {
            vkDestroySurfaceKHR(gInstance, gPresent.surface, gAlloc);
        }

        gPresent.swapchain = VK_NULL_HANDLE;
        gPresent.renderPass = VK_NULL_HANDLE;
        gPresent.surface = VK_NULL_HANDLE;
    }

    VkShaderModule shaderModule(const u32* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

        ci.codeSize = bytes;
        ci.pCode = code;

        VkShaderModule module = VK_NULL_HANDLE;

        vkc(vkCreateShaderModule(gDevice, &ci, gAlloc, &module));

        return module;
    }

    void destroySceneTarget() {
        if (gOutputSet) {
            vkFreeDescriptorSets(gDevice, gDescPool, 1, &gOutputSet);
            gOutputSet = VK_NULL_HANDLE;
        }
        if (gSceneFramebuffer) {
            vkDestroyFramebuffer(gDevice, gSceneFramebuffer, gAlloc);
        }
        if (gSceneView) {
            vkDestroyImageView(gDevice, gSceneView, gAlloc);
        }
        if (gSceneImage) {
            vkDestroyImage(gDevice, gSceneImage, gAlloc);
        }
        if (gSceneMemory) {
            vkFreeMemory(gDevice, gSceneMemory, gAlloc);
        }
        gSceneFramebuffer = VK_NULL_HANDLE;
        gSceneView = VK_NULL_HANDLE;
        gSceneImage = VK_NULL_HANDLE;
        gSceneMemory = VK_NULL_HANDLE;
    }

    void createSceneTarget(u32 width, u32 height) {
        destroySceneTarget();

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkc(vkCreateImage(gDevice, &ici, gAlloc, &gSceneImage));

        VkMemoryRequirements req{};

        vkGetImageMemoryRequirements(gDevice, gSceneImage, &req);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &gSceneMemory));
        vkc(vkBindImageMemory(gDevice, gSceneImage, gSceneMemory, 0));

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

        vci.image = gSceneImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc(vkCreateImageView(gDevice, &vci, gAlloc, &gSceneView));

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

        fci.renderPass = gScenePass;
        fci.attachmentCount = 1;
        fci.pAttachments = &gSceneView;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        vkc(vkCreateFramebuffer(gDevice, &fci, gAlloc, &gSceneFramebuffer));

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

        ai.descriptorPool = gDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &gOutputSetLayout;
        vkc(vkAllocateDescriptorSets(gDevice, &ai, &gOutputSet));

        VkDescriptorImageInfo image{gSceneSampler, gSceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

        write.dstSet = gOutputSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(gDevice, 1, &write, 0, nullptr);
    }

    // a pipeline whose vertices come from gl_VertexIndex alone: no vertex
    // input, no blending, the viewport and scissor set at draw time
    VkPipeline vertexlessPipeline(const u32* vertCode, size_t vertBytes, const u32* fragCode, size_t fragBytes, VkPipelineLayout layout, VkRenderPass pass) {
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

        vkc(vkCreateGraphicsPipelines(gDevice, VK_NULL_HANDLE, 1, &gpci, gAlloc, &pipeline));
        vkDestroyShaderModule(gDevice, frag, gAlloc);
        vkDestroyShaderModule(gDevice, vert, gAlloc);

        return pipeline;
    }

    void destroyLinearHdr() {
        destroySceneTarget();
        if (gImagePipeline) {
            vkDestroyPipeline(gDevice, gImagePipeline, gAlloc);
        }
        if (gImagePipelineLayout) {
            vkDestroyPipelineLayout(gDevice, gImagePipelineLayout, gAlloc);
        }
        if (gImageSetLayout) {
            vkDestroyDescriptorSetLayout(gDevice, gImageSetLayout, gAlloc);
        }
        if (gOutputPipeline) {
            vkDestroyPipeline(gDevice, gOutputPipeline, gAlloc);
        }
        if (gOutputPipelineLayout) {
            vkDestroyPipelineLayout(gDevice, gOutputPipelineLayout, gAlloc);
        }
        if (gOutputSetLayout) {
            vkDestroyDescriptorSetLayout(gDevice, gOutputSetLayout, gAlloc);
        }
        if (gSceneSampler) {
            vkDestroySampler(gDevice, gSceneSampler, gAlloc);
        }
        if (gScenePass) {
            vkDestroyRenderPass(gDevice, gScenePass, gAlloc);
        }
    }

    void frameRender(ImDrawData* draw) {
        Sync& sync = gPresent.syncs.mut(gPresent.syncIndex);
        u64 t0 = nowNs();
        VkResult e = gChaos->swapchain(vkAcquireNextImageKHR(gDevice, gPresent.swapchain, UINT64_MAX, sync.acquired, VK_NULL_HANDLE, &gPresent.frameIndex));

        gAcquireNs = nowNs() - t0;

        if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
            gRebuild = true;
        }

        if (e == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }

        Frame& fd = gPresent.frames.mut(gPresent.frameIndex);

        t0 = nowNs();
        vkWaitForFences(gDevice, 1, &fd.fence, VK_TRUE, UINT64_MAX);
        gFenceNs = nowNs() - t0;
        vkResetFences(gDevice, 1, &fd.fence);
        vkResetCommandPool(gDevice, fd.commandPool, 0);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fd.commandBuffer, &bi);

        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};

        rp.renderArea.extent.width = (u32)gPresent.width;
        rp.renderArea.extent.height = (u32)gPresent.height;
        rp.clearValueCount = 1;
        rp.pClearValues = &gPresent.clear;

        if (gLinearHdr) {
            VkClearValue sceneClear{};

            rp.renderPass = gScenePass;
            rp.framebuffer = gSceneFramebuffer;
            rp.pClearValues = &sceneClear;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
            vkCmdEndRenderPass(fd.commandBuffer);

            float sdrWhiteNits = gSdrWhiteNits;

            rp.renderPass = gPresent.renderPass;
            rp.framebuffer = fd.framebuffer;
            rp.pClearValues = &gPresent.clear;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gOutputPipeline);
            vkCmdBindDescriptorSets(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gOutputPipelineLayout, 0, 1, &gOutputSet, 0, nullptr);
            vkCmdPushConstants(fd.commandBuffer, gOutputPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sdrWhiteNits), &sdrWhiteNits);
            VkViewport viewport{0, 0, (float)gPresent.width, (float)gPresent.height, 0, 1};
            VkRect2D scissor{{0, 0}, {(u32)gPresent.width, (u32)gPresent.height}};

            vkCmdSetViewport(fd.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(fd.commandBuffer, 0, 1, &scissor);
            vkCmdDraw(fd.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(fd.commandBuffer);
        } else {
            rp.renderPass = gPresent.renderPass;
            rp.framebuffer = fd.framebuffer;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
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
        si.pSignalSemaphores = &sync.rendered;
        vkEndCommandBuffer(fd.commandBuffer);
        t0 = nowNs();
        vkQueueSubmit(gQueue, 1, &si, fd.fence);
        gSubmitNs = nowNs() - t0;
    }

    void framePresent() {
        if (gRebuild) {
            return;
        }

        Sync& sync = gPresent.syncs.mut(gPresent.syncIndex);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sync.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &gPresent.swapchain;
        pi.pImageIndices = &gPresent.frameIndex;

        u64 t0 = nowNs();
        VkResult e = gChaos->swapchain(vkQueuePresentKHR(gQueue, &pi));

        gPresentNs = nowNs() - t0;

        if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
            gRebuild = true;
        }

        gPresent.syncIndex = (gPresent.syncIndex + 1) % (u32)gPresent.syncs.length();
    }
}

void vkc(VkResult e) {
    e = gChaos->vulkan(e);

    if (e < 0) {
        fail(sv(StringBuilder() << "vulkan error "_sv << (i64)e));
    }
}

// a checked call a scenario can name (IM_CHAOS vulkan-at=SITE)
void vkcAt(StringView site, VkResult e) {
    e = gChaos->vulkanAt(site, e);

    if (e < 0) {
        fail(sv(StringBuilder() << "vulkan error "_sv << (i64)e << " at "_sv << site));
    }
}

void setupVulkan(ObjPool& pool, const VulkanWants& wants) {
    VkApplicationInfo app = {};

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "im";
    app.apiVersion = VK_API_VERSION_1_2;

    // the window's surface, and the HDR colour space on it when asked
    Vector<const char*> instanceExts;

    instanceExts.pushBack(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(__APPLE__)
    instanceExts.pushBack(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#else
    instanceExts.pushBack(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif

    if (wants.hdr) {
        instanceExts.pushBack(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ci = {};

    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (u32)instanceExts.length();
    ci.ppEnabledExtensionNames = instanceExts.data();
    vkc(vkCreateInstance(&ci, gAlloc, &gInstance));
    pooledGuard(pool, [] {
        vkDestroyInstance(gInstance, gAlloc);
    });

    if (wants.sharedBuffer) {
        u32 count = 0;

        vkEnumeratePhysicalDevices(gInstance, &count, nullptr);
        Vector<VkPhysicalDevice> devices;

        devices.zero(count);
        vkEnumeratePhysicalDevices(gInstance, &count, devices.mutData());

        // the buffer is only known to import on the GPU that exported
        // it; its deviceUUID names that GPU in any process, a software
        // device without a drm node included
        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

            props.pNext = &ids;
            vkGetPhysicalDeviceProperties2(device, &props);

            if (memcmp(ids.deviceUUID, wants.deviceUuid, VK_UUID_SIZE) == 0) {
                gPhys = device;

                break;
            }
        }

        if (!gPhys) {
            fail("shared screenshot gpu is unavailable"_sv);
        }
    } else {
        gPhys = selectPhysicalDevice();
    }

    gQueueFamily = selectQueueFamily(gPhys);

    const char* wantedExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    u32 wantedCount = wants.sharedBuffer ? 4 : 1;
    Vector<const char*> devExts;

    for (u32 i = 0; i < wantedCount; i++) {
        if (!hasDeviceExtension(gPhys, wantedExts[i])) {
            fail(sv(StringBuilder() << "vulkan lacks "_sv << StringView(wantedExts[i])));
        }

        devExts.pushBack(wantedExts[i]);
    }

#if defined(__APPLE__)
    // a device that is a portability subset (MoltenVK over Metal) wants to
    // be told that it is used as one
    const char* portability = "VK_KHR_portability_subset";

    if (hasDeviceExtension(gPhys, portability)) {
        devExts.pushBack(portability);
    }
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};

    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = gQueueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {};

    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (u32)devExts.length();
    dci.ppEnabledExtensionNames = devExts.data();
    vkc(vkCreateDevice(gPhys, &dci, gAlloc, &gDevice));
    pooledGuard(pool, [] {
        vkDestroyDevice(gDevice, gAlloc);
    });
    vkGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);

    // the backend's own sets, and one for every texture the tool registers
    VkDescriptorPoolSize sz = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + wants.textures};
    VkDescriptorPoolCreateInfo pi = {};

    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = sz.descriptorCount;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &sz;
    vkc(vkCreateDescriptorPool(gDevice, &pi, gAlloc, &gDescPool));
    pooledGuard(pool, [] {
        vkDestroyDescriptorPool(gDevice, gDescPool, gAlloc);
    });
}

VkSurfaceKHR createSurface(plt::Window& window) {
    plt::RenderContext render = window.renderContext();
    VkSurfaceKHR surface = VK_NULL_HANDLE;

#if defined(__APPLE__)
    // plt's Cocoa window draws through a CAMetalLayer, its connection
    VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};

    sci.pLayer = (const CAMetalLayer*)render.connection;
    vkc(vkCreateMetalSurfaceEXT(gInstance, &sci, gAlloc, &surface));
#else
    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};

    sci.display = (wl_display*)render.connection;
    sci.surface = (wl_surface*)render.window;
    vkc(vkCreateWaylandSurfaceKHR(gInstance, &sci, gAlloc, &surface));
#endif

    return surface;
}

void setupVulkanWindow(ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr) {
    // the surface is the presenter's from here: registered before
    // anything can throw, the guard tears down whatever was made
    gPresent.surface = surface;
    pooledGuard(pool, [] {
        destroyPresenter();
    });

    VkBool32 supported = VK_FALSE;

    vkc(vkGetPhysicalDeviceSurfaceSupportKHR(gPhys, gQueueFamily, surface, &supported));

    if (!gChaos->surfaceSupport(supported)) {
        fail("no vulkan WSI support"_sv);
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

    gPresent.format = selectSurfaceFormat(surface, fmts, 4, colorSpace);

    if (hdr && gPresent.format.colorSpace != colorSpace) {
        fail("vulkan WSI has no BT.2020/PQ surface"_sv);
    }

    traceText(hdr ? "surface HDR10 PQ"_sv : "surface sRGB"_sv);
    createPresentPass();
    createSwapchain((u32)w, (u32)h);
    // the first present's size, as every later rebuild's: a scenario waits
    // for the size the tool draws at, whichever way it got there
    traceSize("presenting"_sv, gPresent.width, gPresent.height);
}

u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(gPhys, &mp);
    gChaos->memoryTypes(mp);

    for (u32 i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }

    fail("no vulkan memory type fits"_sv);
}

void setupLinearHdr(ObjPool& pool, u32 width, u32 height) {
    // registered before the first object: a throw part way through
    // tears down exactly the objects already made (the rest are null)
    pooledGuard(pool, [] {
        destroyLinearHdr();
        gLinearHdr = false;
    });

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
    vkcAt("scene-pass"_sv, vkCreateRenderPass(gDevice, &rpci, gAlloc, &gScenePass));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkc(vkCreateSampler(gDevice, &sci, gAlloc, &gSceneSampler));

    VkDescriptorSetLayoutBinding binding{};

    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    dlci.bindingCount = 1;
    dlci.pBindings = &binding;
    vkc(vkCreateDescriptorSetLayout(gDevice, &dlci, gAlloc, &gOutputSetLayout));

    // the scene holds SDR white at 1.0; the output stage scales it to
    // nits by this constant
    VkPushConstantRange outputRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    plci.setLayoutCount = 1;
    plci.pSetLayouts = &gOutputSetLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &outputRange;
    vkc(vkCreatePipelineLayout(gDevice, &plci, gAlloc, &gOutputPipelineLayout));

    gOutputPipeline = vertexlessPipeline(fullscreen_vert_spv, sizeof(fullscreen_vert_spv), gpu_output_frag_spv, sizeof(gpu_output_frag_spv), gOutputPipelineLayout, gPresent.renderPass);

    VkDescriptorSetLayoutBinding imageBinding{};

    imageBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    imageBinding.descriptorCount = 1;
    imageBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ilci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

    ilci.bindingCount = 1;
    ilci.pBindings = &imageBinding;
    vkc(vkCreateDescriptorSetLayout(gDevice, &ilci, gAlloc, &gImageSetLayout));

    // one range for both stages, and the one push names both
    VkPushConstantRange imageRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ImagePush)};
    VkPipelineLayoutCreateInfo iplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    iplci.setLayoutCount = 1;
    iplci.pSetLayouts = &gImageSetLayout;
    iplci.pushConstantRangeCount = 1;
    iplci.pPushConstantRanges = &imageRange;
    vkc(vkCreatePipelineLayout(gDevice, &iplci, gAlloc, &gImagePipelineLayout));
    gImagePipeline = vertexlessPipeline(gpu_image_vert_spv, sizeof(gpu_image_vert_spv), gpu_image_frag_spv, sizeof(gpu_image_frag_spv), gImagePipelineLayout, gScenePass);

    createSceneTarget(width, height);
    gLinearHdr = true;
}

void setupImGui(ObjPool& pool, bool hdr) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    pooledGuard(pool, [] {
        ImGui::DestroyContext();
    });
    ImGui::GetIO().IniFilename = nullptr;
    applyUiStyle();

    ImGui_ImplVulkan_InitInfo ii = {};

    ii.Instance = gInstance;
    ii.PhysicalDevice = gPhys;
    ii.Device = gDevice;
    ii.QueueFamily = gQueueFamily;
    ii.Queue = gQueue;
    ii.DescriptorPool = gDescPool;
    ii.MinImageCount = kMinImageCount;
    ii.ImageCount = (u32)gPresent.frames.length();
    ii.PipelineInfoMain.RenderPass = gLinearHdr ? gScenePass : gPresent.renderPass;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (hdr) {
        // ImGui's own draws land in the scene's linear light through the
        // scene's fragment stage
        ii.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ii.CustomShaderFragCreateInfo.codeSize = sizeof(gpu_scene_frag_spv);
        ii.CustomShaderFragCreateInfo.pCode = gpu_scene_frag_spv;
    }

    ImGui_ImplVulkan_Init(&ii);
    pooledGuard(pool, [] {
        ImGui_ImplVulkan_Shutdown();
    });

    // the backend leaves the texture limit unsaid; the device's
    VkPhysicalDeviceProperties props;

    vkGetPhysicalDeviceProperties(gPhys, &props);

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    pio.Renderer_TextureMaxWidth = (int)props.limits.maxImageDimension2D;
    pio.Renderer_TextureMaxHeight = (int)props.limits.maxImageDimension2D;

    // the last guard in, the first out: the device finishes the frames
    // in flight before the backend's textures, the frames and their
    // fences go under them
    pooledGuard(pool, [] {
        vkDeviceWaitIdle(gDevice);
    });
}

void setupGpu(ObjPool& pool, plt::Window& window, const VulkanWants& wants) {
    setupVulkan(pool, wants);

    VkSurfaceKHR surface = createSurface(window);
    plt::WindowInfo info = window.info();

    setupVulkanWindow(pool, surface, (int)info.width, (int)info.height, wants.hdr);
    setupImGui(pool, wants.hdr);
}

void finishTexture(VkFormat format, Texture& tex) {
    VkImageViewCreateInfo vci = {};

    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkc(vkCreateImageView(gDevice, &vci, gAlloc, &tex.view));

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
    vkc(vkCreateSampler(gDevice, &sci, gAlloc, &tex.sampler));

    tex.ds = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (gLinearHdr) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

        ai.descriptorPool = gDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &gImageSetLayout;
        vkc(vkAllocateDescriptorSets(gDevice, &ai, &tex.imageSet));

        VkDescriptorImageInfo image{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

        write.dstSet = tex.imageSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(gDevice, 1, &write, 0, nullptr);
    }
}

void uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex) {
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

    VkImageCreateInfo ici = {};

    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkc(vkCreateImage(gDevice, &ici, gAlloc, &tex.image));

    VkMemoryRequirements req;

    vkGetImageMemoryRequirements(gDevice, tex.image, &req);

    VkMemoryAllocateInfo mai = {};

    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &tex.memory));
    vkBindImageMemory(gDevice, tex.image, tex.memory, 0);

    VkBuffer staging;
    VkDeviceMemory stagingMem;
    VkBufferCreateInfo bci = {};

    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkc(vkCreateBuffer(gDevice, &bci, gAlloc, &staging));
    vkGetBufferMemoryRequirements(gDevice, staging, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &stagingMem));
    vkBindBufferMemory(gDevice, staging, stagingMem, 0);

    void* map = nullptr;

    vkMapMemory(gDevice, stagingMem, 0, bytes, 0, &map);
    memcpy(map, rgba, bytes);
    vkUnmapMemory(gDevice, stagingMem);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {};

    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = gQueueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(gDevice, &pci, gAlloc, &pool);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbi = {};

    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(gDevice, &cbi, &cmd);

    VkCommandBufferBeginInfo begin = {};

    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

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

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};

    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(gQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(gQueue);

    vkDestroyCommandPool(gDevice, pool, gAlloc);
    vkDestroyBuffer(gDevice, staging, gAlloc);
    vkFreeMemory(gDevice, stagingMem, gAlloc);

    finishTexture(VK_FORMAT_R8G8B8A8_UNORM, tex);
}

void destroyTexture(Texture& tex) {
    if (tex.ds) {
        ImGui_ImplVulkan_RemoveTexture(tex.ds);
    }
    if (tex.imageSet) {
        vkFreeDescriptorSets(gDevice, gDescPool, 1, &tex.imageSet);
    }
    if (tex.sampler) {
        vkDestroySampler(gDevice, tex.sampler, gAlloc);
    }
    if (tex.view) {
        vkDestroyImageView(gDevice, tex.view, gAlloc);
    }
    if (tex.image) {
        vkDestroyImage(gDevice, tex.image, gAlloc);
    }
    if (tex.memory) {
        vkFreeMemory(gDevice, tex.memory, gAlloc);
    }

    tex = Texture();
}

void drawImage(const ImDrawList*, const ImDrawCmd* cmd) {
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

    vkCmdSetScissor(state->CommandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gImagePipeline);
    vkCmdBindDescriptorSets(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gImagePipelineLayout, 0, 1, &draw.texture, 0, nullptr);

    ImagePush push;

    push.scale[0] = 2.f / dd->DisplaySize.x;
    push.scale[1] = 2.f / dd->DisplaySize.y;
    push.translate[0] = -1.f - dd->DisplayPos.x * push.scale[0];
    push.translate[1] = -1.f - dd->DisplayPos.y * push.scale[1];
    push.rect[0] = draw.x0;
    push.rect[1] = draw.y0;
    push.rect[2] = draw.x1;
    push.rect[3] = draw.y1;
    push.sdrWhiteNits = draw.sdrWhiteNits;
    vkCmdPushConstants(state->CommandBuffer, gImagePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(state->CommandBuffer, 6, 1, 0, 0);
}

void clampWindowSize(const plt::WindowInfo& info, int& w, int& h) {
    int maxW = (int)info.screenPixelWidth * 9 / 10;
    int maxH = (int)info.screenPixelHeight * 9 / 10;

    if (w > maxW) {
        w = maxW;
    }

    if (h > maxH) {
        h = maxH;
    }
}

bool FrameDriver::frame(const plt::WindowInfo& info) {
    int nw = (int)info.width;
    int nh = (int)info.height;
    u64 began = nowNs();
    u64 gap = gFrameBegan ? began - gFrameBegan : 0;

    gFrameBegan = began;
    gAcquireNs = gFenceNs = gSubmitNs = gPresentNs = 0;

    if (gRebuild || gPresent.width != nw || gPresent.height != nh) {
        createSwapchain((u32)nw, (u32)nh);
        traceSize("presenting"_sv, gPresent.width, gPresent.height);

        if (gLinearHdr) {
            createSceneTarget((u32)nw, (u32)nh);
        }

        gRebuild = false;
    }

    ImGui_ImplVulkan_NewFrame();

    u64 backend = nowNs();

    imgui->newFrame(*window);

    u64 informed = nowNs();

    ImGui::NewFrame();

    u64 begun = nowNs();
    int result = ui->frame();
    u64 drew = nowNs();

    ImGui::Render();

    // the draw data is the window's size, checked positive above
    ImDrawData* dd = ImGui::GetDrawData();

    gPresent.clear.color.float32[0] = 0.1f;
    gPresent.clear.color.float32[1] = 0.1f;
    gPresent.clear.color.float32[2] = 0.1f;
    gPresent.clear.color.float32[3] = 1.0f;

    u64 drawn = nowNs();

    frameRender(dd);
    framePresent();

    if (gTraceFrames) {
        // a line per frame: the gap since the last one began, then this
        // one's phases, ImGui and the tool's own drawing first
        auto& text = sb();

        text << "im frame "_sv << (i64)gFrameCount++ << ": gap "_sv;
        appendMs(text, gap);
        text << " ui "_sv;
        appendMs(text, drawn - began);
        text << " (backend "_sv;
        appendMs(text, backend - began);
        text << " info "_sv;
        appendMs(text, informed - backend);
        text << " new "_sv;
        appendMs(text, begun - informed);
        text << " tool "_sv;
        appendMs(text, drew - begun);
        text << " render "_sv;
        appendMs(text, drawn - drew);
        text << ")"_sv;
        text << " acquire "_sv;
        appendMs(text, gAcquireNs);
        text << " fence "_sv;
        appendMs(text, gFenceNs);
        text << " submit "_sv;
        appendMs(text, gSubmitNs);
        text << " present "_sv;
        appendMs(text, gPresentNs);
        text << " total "_sv;
        appendMs(text, nowNs() - began);

        if (gRebuild) {
            text << " rebuild"_sv;
        }

        sysE << sv(text) << endL;
    }

    if (result != 0) {
        action = result;
        platform->stop();
    } else {
        // imgui animates every frame while interactive; plt paces this
        // through the compositor's frame callbacks
        window->requestFrame();
    }

    // a swapchain-rebuild frame presented nothing: returning false makes
    // plt retry instead of waiting on a frame callback that never comes
    return !gRebuild;
}

void FrameDriver::close() {
    action = -1;
    platform->stop();
}

int runUi(FrameDriver& driver) {
    gTraceFrames = getenv("IM_TRACE_FRAMES") != nullptr;
    driver.action = 0;
    driver.window->requestShow();
    driver.window->requestFrame();
    driver.platform->run();

    return driver.action ? driver.action : -1;
}
