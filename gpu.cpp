#include "gpu.h"

#include "ui.h"
#include "util.h"
#include "pooled.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/ios/sys.h>

#include <math.h>
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

namespace {
    // ImGui's drag and double-click thresholds, as a design length: its
    // ScaleAllSizes leaves them alone
    constexpr Design mouseThreshold = 6_d;

    // the style at the scale into the current context, with the input
    // thresholds
    void applyScaledStyle(float scale) {
        ImGui::GetStyle() = scaledStyle(scale);

        ImGuiIO& io = ImGui::GetIO();

        io.MouseDragThreshold = scaledPx(mouseThreshold, scale);
        io.MouseDoubleClickMaxDist = scaledPx(mouseThreshold, scale);
    }

    // three: MoltenVK takes the CAMetalLayer drawable at submit, and with
    // two the submit blocks until the frame before last has left the
    // screen, a display link tick lost each time
    constexpr u32 kMinImageCount = 3;

    // gpu_image.vert's push constants: ImGui's own scale and
    // translate, the quad in ImGui's screen space, the white the fragment
    // stage divides its nits by
    struct ImagePush {
        float scale[2];
        float translate[2];
        float rect[4];
        float sdrWhiteNits;
    };
}

float scaleFromEnv() {
    if (const char* s = getenv("IM_SCALE")) {
        double v = parseFloat(StringView(s));

        if (v > 0.0) {
            return (float)v;
        }
    }

    return 1.f;
}

float scaledPx(Design d, float scale) {
    float v = floorf(d.value * scale + .5f);

    return d.value > 0.f && v < 1.f ? 1.f : v;
}

// from a fresh default every time: ScaleAllSizes compounds
ImGuiStyle scaledStyle(float scale) {
    ImGuiStyle style;

    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;

    return style;
}

int drawErrorPanel(StringView tool, float scale, StringView msg) {
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    int result = 0;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));

    ImGui::Begin("##err", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    float pad = scaledPx(24_d, scale);

    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::BeginGroup();

    StringBuilder heading;

    heading << "im "_sv << tool;
    ImGui::TextDisabled("%s", heading.cStr());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(vp->Size.x - pad);
    ImGui::TextUnformatted((const char*)msg.data(), (const char*)msg.data() + msg.length());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Exit", ImVec2(scaledPx(120_d, scale), 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        result = -1;
    }

    ImGui::EndGroup();

    ImGui::End();
    ImGui::PopStyleColor();

    return result;
}

Gpu* Gpu::create(ObjPool& pool, const GpuOptions& options) {
    Gpu* gpu = pool.make<Gpu>();

    gpu->tool = options.tool;
    gpu->scale = options.scale;
    gpu->chaos = options.chaos;
    gpu->traceFrames = options.traceFrames;
    gpu->setupVulkan(pool, options);

    return gpu;
}

Gpu* Gpu::createFor(ObjPool& pool, plt::Window& window, const GpuOptions& options) {
    Gpu* gpu = create(pool, options);
    VkSurfaceKHR surface = gpu->createSurface(window);
    plt::WindowInfo info = window.info();

    gpu->setupWindow(pool, surface, (int)info.width, (int)info.height, options.hdr);
    gpu->setupImGui(pool, options.hdr);

    return gpu;
}

void Gpu::trace(StringView what) {
    traceTool(tool, what);
}

void Gpu::traceSize(StringView what, int w, int h) {
    traceTool(tool, sv(StringBuilder() << what << " "_sv << w << "x"_sv << h));
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

// a discrete GPU when there is one, the first device otherwise
VkPhysicalDevice Gpu::selectPhysicalDevice() {
    u32 count = 0;

    vkc(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    count = chaos->count("devices"_sv, count);

    if (!count) {
        fail("no vulkan device"_sv);
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
    // the driver's own query stays whole; the answer is what the seam bends
    count = chaos->count("queue-families"_sv, count);

    for (u32 i = 0; i < count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }

    fail("no vulkan graphics queue"_sv);
}

// the first of the wanted formats the surface offers in the color space,
// else whatever it offers first: the HDR caller checks the color space
VkSurfaceFormatKHR Gpu::selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace) {
    u32 count = 0;

    vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &count, nullptr));
    count = chaos->count("surface-formats"_sv, count);

    if (!count) {
        fail("vulkan WSI offers no surface format"_sv);
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

void Gpu::createPresentPass() {
    VkAttachmentDescription attachment{};

    attachment.format = present.format.format;
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
    vkc(vkCreateRenderPass(device, &rpci, alloc, &present.renderPass));
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
        if (frame.commandPool) {
            vkDestroyCommandPool(device, frame.commandPool, alloc);
        }
    }

    for (size_t i = 0; i < present.syncs.length(); i++) {
        const Sync& sync = present.syncs[i];

        if (sync.acquired) {
            vkDestroySemaphore(device, sync.acquired, alloc);
        }
        if (sync.rendered) {
            vkDestroySemaphore(device, sync.rendered, alloc);
        }
    }

    present.frames.clear();
    present.syncs.clear();
}

// the swapchain at the window's size; a swapchain already there is
// retired by the new one, its frames torn down once the device is idle
void Gpu::createSwapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;

    vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, present.surface, &caps));
    chaos->imageCounts(caps);

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

    ci.surface = present.surface;
    ci.minImageCount = images;
    ci.imageFormat = present.format.format;
    ci.imageColorSpace = present.format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = present.swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;

    // a driver that cannot present to this compositor's surface fails
    // here, and that is the tool's own report; a refusal the monkey
    // made up leaves the driver's swapchain behind, and it goes
    VkResult made = vkCreateSwapchainKHR(device, &ci, alloc, &swapchain);

    if (chaos->vulkanAt("swapchain"_sv, made) < 0) {
        if (made == VK_SUCCESS) {
            vkDestroySwapchainKHR(device, swapchain, alloc);
        }

        fail("vulkan cannot make a swapchain on this surface"_sv);
    }

    if (present.swapchain) {
        vkDeviceWaitIdle(device);
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

        // signaled: the first frame has nothing to wait for
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkc(vkCreateFence(device, &fenceInfo, alloc, &frame.fence));
    }

    for (u32 i = 0; i <= count; i++) {
        Sync& sync = present.syncs.mut(i);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &sync.acquired));
        vkc(vkCreateSemaphore(device, &semaphoreInfo, alloc, &sync.rendered));
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
    if (present.surface) {
        vkDestroySurfaceKHR(instance, present.surface, alloc);
    }

    present.swapchain = VK_NULL_HANDLE;
    present.renderPass = VK_NULL_HANDLE;
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
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

// a pipeline whose vertices come from gl_VertexIndex alone: no vertex
// input, no blending, the viewport and scissor set at draw time
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
}

void Gpu::frameRender(ImDrawData* draw) {
    Sync& sync = present.syncs.mut(present.syncIndex);
    u64 t0 = nowNs();
    VkResult e = chaos->swapchain(vkAcquireNextImageKHR(device, present.swapchain, UINT64_MAX, sync.acquired, VK_NULL_HANDLE, &present.frameIndex));

    acquireNs = nowNs() - t0;

    if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
        rebuild = true;
    }

    if (e == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }

    Frame& fd = present.frames.mut(present.frameIndex);

    t0 = nowNs();
    vkWaitForFences(device, 1, &fd.fence, VK_TRUE, UINT64_MAX);
    fenceNs = nowNs() - t0;
    vkResetFences(device, 1, &fd.fence);
    vkResetCommandPool(device, fd.commandPool, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd.commandBuffer, &bi);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};

    rp.renderArea.extent.width = (u32)present.width;
    rp.renderArea.extent.height = (u32)present.height;
    rp.clearValueCount = 1;
    rp.pClearValues = &present.clear;

    if (linearHdr) {
        VkClearValue sceneClear{};

        rp.renderPass = scenePass;
        rp.framebuffer = sceneFramebuffer;
        rp.pClearValues = &sceneClear;
        vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
        vkCmdEndRenderPass(fd.commandBuffer);

        float white = sdrWhiteNits;

        rp.renderPass = present.renderPass;
        rp.framebuffer = fd.framebuffer;
        rp.pClearValues = &present.clear;
        vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipeline);
        vkCmdBindDescriptorSets(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, outputPipelineLayout, 0, 1, &outputSet, 0, nullptr);
        vkCmdPushConstants(fd.commandBuffer, outputPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(white), &white);
        VkViewport viewport{0, 0, (float)present.width, (float)present.height, 0, 1};
        VkRect2D scissor{{0, 0}, {(u32)present.width, (u32)present.height}};

        vkCmdSetViewport(fd.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(fd.commandBuffer, 0, 1, &scissor);
        vkCmdDraw(fd.commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(fd.commandBuffer);
    } else {
        rp.renderPass = present.renderPass;
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
    vkQueueSubmit(queue, 1, &si, fd.fence);
    submitNs = nowNs() - t0;
}

void Gpu::framePresent() {
    if (rebuild) {
        return;
    }

    Sync& sync = present.syncs.mut(present.syncIndex);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sync.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &present.swapchain;
    pi.pImageIndices = &present.frameIndex;

    u64 t0 = nowNs();
    VkResult e = chaos->swapchain(vkQueuePresentKHR(queue, &pi));

    presentNs = nowNs() - t0;

    if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
        rebuild = true;
    }

    present.syncIndex = (present.syncIndex + 1) % (u32)present.syncs.length();
}

void Gpu::vkc(VkResult e) {
    e = chaos->vulkan(e);

    if (e < 0) {
        fail(sv(StringBuilder() << "vulkan error "_sv << (i64)e));
    }
}

// a checked call a scenario can name (IM_CHAOS vulkan-at=SITE)
void Gpu::vkcAt(StringView site, VkResult e) {
    e = chaos->vulkanAt(site, e);

    if (e < 0) {
        fail(sv(StringBuilder() << "vulkan error "_sv << (i64)e << " at "_sv << site));
    }
}

void Gpu::setupVulkan(ObjPool& pool, const GpuOptions& wants) {
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

        // the buffer is only known to import on the GPU that exported
        // it; its deviceUUID names that GPU in any process, a software
        // device without a drm node included
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
            fail("shared screenshot gpu is unavailable"_sv);
        }
    } else {
        phys = selectPhysicalDevice();
    }

    queueFamily = selectQueueFamily(phys);

    const char* wantedExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    u32 wantedCount = wants.sharedBuffer ? 4 : 1;
    Vector<const char*> devExts;

    for (u32 i = 0; i < wantedCount; i++) {
        if (!hasDeviceExtension(phys, wantedExts[i])) {
            fail(sv(StringBuilder() << "vulkan lacks "_sv << StringView(wantedExts[i])));
        }

        devExts.pushBack(wantedExts[i]);
    }

#if defined(__APPLE__)
    // a device that is a portability subset (MoltenVK over Metal) wants to
    // be told that it is used as one
    const char* portability = "VK_KHR_portability_subset";

    if (hasDeviceExtension(phys, portability)) {
        devExts.pushBack(portability);
    }
#endif
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
    vkc(vkCreateDevice(phys, &dci, alloc, &device));
    pooledGuard(pool, [this] {
        vkDestroyDevice(device, alloc);
    });
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // the backend's own sets, and one for every texture the tool registers
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
}

VkSurfaceKHR Gpu::createSurface(plt::Window& window) {
    plt::RenderContext render = window.renderContext();
    VkSurfaceKHR surface = VK_NULL_HANDLE;

#if defined(__APPLE__)
    // plt's Cocoa window draws through a CAMetalLayer, its connection
    VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};

    sci.pLayer = (const CAMetalLayer*)render.connection;
    vkc(vkCreateMetalSurfaceEXT(instance, &sci, alloc, &surface));
#else
    VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};

    sci.display = (wl_display*)render.connection;
    sci.surface = (wl_surface*)render.window;
    vkc(vkCreateWaylandSurfaceKHR(instance, &sci, alloc, &surface));
#endif

    return surface;
}

void Gpu::setupWindow(ObjPool& pool, VkSurfaceKHR surface, int w, int h, bool hdr) {
    // the surface is the presenter's from here: registered before
    // anything can throw, the guard tears down whatever was made
    present.surface = surface;
    pooledGuard(pool, [this] {
        destroyPresenter();
    });

    VkBool32 supported = VK_FALSE;

    vkc(vkGetPhysicalDeviceSurfaceSupportKHR(phys, queueFamily, surface, &supported));

    if (!chaos->surfaceSupport(supported)) {
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

    present.format = selectSurfaceFormat(surface, fmts, 4, colorSpace);

    if (hdr && present.format.colorSpace != colorSpace) {
        fail("vulkan WSI has no BT.2020/PQ surface"_sv);
    }

    trace(hdr ? "surface HDR10 PQ"_sv : "surface sRGB"_sv);
    createPresentPass();
    createSwapchain((u32)w, (u32)h);
    // the first present's size, as every later rebuild's: a scenario waits
    // for the size the tool draws at, whichever way it got there
    traceSize("presenting"_sv, present.width, present.height);
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

    fail("no vulkan memory type fits"_sv);
}

void Gpu::setupLinearHdr(ObjPool& pool, u32 width, u32 height) {
    // registered before the first object: a throw part way through
    // tears down exactly the objects already made (the rest are null)
    pooledGuard(pool, [this] {
        destroyLinearHdr();
        linearHdr = false;
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
    vkcAt("scene-pass"_sv, vkCreateRenderPass(device, &rpci, alloc, &scenePass));

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

    // the scene holds SDR white at 1.0; the output stage scales it to
    // nits by this constant
    VkPushConstantRange outputRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
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

    // one range for both stages, and the one push names both
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

void Gpu::setupImGui(ObjPool& pool, bool hdr) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    pooledGuard(pool, [] {
        ImGui::DestroyContext();
    });
    ImGui::GetIO().IniFilename = nullptr;
    applyScaledStyle(scale);

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

    vkGetPhysicalDeviceProperties(phys, &props);

    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();

    pio.Renderer_TextureMaxWidth = (int)props.limits.maxImageDimension2D;
    pio.Renderer_TextureMaxHeight = (int)props.limits.maxImageDimension2D;

    // the last guard in, the first out: the device finishes the frames
    // in flight before the backend's textures, the frames and their
    // fences go under them
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

void Gpu::uploadTexture(u32 w, u32 h, const u8* rgba, Texture& tex) {
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
    vkc(vkCreateImage(device, &ici, alloc, &tex.image));

    VkMemoryRequirements req;

    vkGetImageMemoryRequirements(device, tex.image, &req);

    VkMemoryAllocateInfo mai = {};

    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkc(vkAllocateMemory(device, &mai, alloc, &tex.memory));
    vkBindImageMemory(device, tex.image, tex.memory, 0);

    VkBuffer staging;
    VkDeviceMemory stagingMem;
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
    vkBindBufferMemory(device, staging, stagingMem, 0);

    void* map = nullptr;

    vkMapMemory(device, stagingMem, 0, bytes, 0, &map);
    memcpy(map, rgba, bytes);
    vkUnmapMemory(device, stagingMem);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {};

    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = queueFamily;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(device, &pci, alloc, &pool);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbi = {};

    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cbi, &cmd);

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
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkDestroyCommandPool(device, pool, alloc);
    vkDestroyBuffer(device, staging, alloc);
    vkFreeMemory(device, stagingMem, alloc);

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
    vkCmdBindPipeline(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.gpu->imagePipeline);
    vkCmdBindDescriptorSets(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.gpu->imagePipelineLayout, 0, 1, &draw.texture, 0, nullptr);

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
    vkCmdPushConstants(state->CommandBuffer, draw.gpu->imagePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
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
    u64 gap = gpu->frameBegan ? began - gpu->frameBegan : 0;

    gpu->frameBegan = began;
    gpu->acquireNs = gpu->fenceNs = gpu->submitNs = gpu->presentNs = 0;

    if (gpu->rebuild || gpu->present.width != nw || gpu->present.height != nh) {
        gpu->createSwapchain((u32)nw, (u32)nh);
        gpu->traceSize("presenting"_sv, gpu->present.width, gpu->present.height);

        if (gpu->linearHdr) {
            gpu->createSceneTarget((u32)nw, (u32)nh);
        }

        gpu->rebuild = false;
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

    gpu->present.clear.color.float32[0] = 0.1f;
    gpu->present.clear.color.float32[1] = 0.1f;
    gpu->present.clear.color.float32[2] = 0.1f;
    gpu->present.clear.color.float32[3] = 1.0f;

    u64 drawn = nowNs();

    gpu->frameRender(dd);
    gpu->framePresent();

    if (gpu->traceFrames) {
        // a line per frame: the gap since the last one began, then this
        // one's phases, ImGui and the tool's own drawing first
        StringBuilder text;

        text << "im frame "_sv << (i64)gpu->frameCount++ << ": gap "_sv;
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
        appendMs(text, gpu->acquireNs);
        text << " fence "_sv;
        appendMs(text, gpu->fenceNs);
        text << " submit "_sv;
        appendMs(text, gpu->submitNs);
        text << " present "_sv;
        appendMs(text, gpu->presentNs);
        text << " total "_sv;
        appendMs(text, nowNs() - began);

        if (gpu->rebuild) {
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
    return !gpu->rebuild;
}

void FrameDriver::close() {
    action = -1;
    platform->stop();
}

int runUi(FrameDriver& driver) {
    driver.action = 0;
    driver.window->requestShow();
    driver.window->requestFrame();
    driver.platform->run();

    return driver.action ? driver.action : -1;
}
