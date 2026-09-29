#include "vulkan.h"

#include <std/dbg/insist.h>
#include <std/lib/vector.h>

#include <cairo.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    static void check(VkResult result) {
        if (result != VK_SUCCESS) {
            fprintf(stderr, "Vulkan failure: %d\n", result);
        }
        STD_INSIST(result == VK_SUCCESS);
    }

    struct Presenter final: public VulkanPresenter {
        explicit Presenter(const RenderContext& context);
        ~Presenter();
        bool paint(App& app, const WindowInfo& info) override;
        void resize(u32 width, u32 height);
        void clearSwapchain();

        VkInstance instance = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        u32 family = 0;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* pixels = nullptr;
        VkExtent2D extent{};
        Vector<VkImage> images;
    };
}

Presenter::Presenter(const RenderContext& context) {
    const char* const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "plt-e2e", .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instanceInfo{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application, .enabledExtensionCount = 2, .ppEnabledExtensionNames = extensions};
    check(vkCreateInstance(&instanceInfo, nullptr, &instance));
    const VkWaylandSurfaceCreateInfoKHR surfaceInfo{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = static_cast<wl_display*>(context.connection),
        .surface = static_cast<wl_surface*>(context.window),
    };
    check(vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, nullptr, &surface));
    u32 count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    Vector<VkPhysicalDevice> devices;
    devices.zero(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.mutData()));
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            continue;
        }
        if (strstr(properties.deviceName, "llvmpipe") == nullptr && strstr(properties.deviceName, "lavapipe") == nullptr) {
            continue;
        }
        physical = candidate;
        printf("VULKAN %s\n", properties.deviceName);
        break;
    }
    STD_INSIST(physical != VK_NULL_HANDLE);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    Vector<VkQueueFamilyProperties> families;
    families.zero(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.mutData());
    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        VkBool32 supported;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported));
        if (supported && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            family = i;
            found = true;
            break;
        }
    }
    STD_INSIST(found);
    const float priority = 1;
    const VkDeviceQueueCreateInfo queueInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char* const deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo deviceInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queueInfo, .enabledExtensionCount = 1, .ppEnabledExtensionNames = deviceExtensions};
    check(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    const VkCommandBufferAllocateInfo commandInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    check(vkAllocateCommandBuffers(device, &commandInfo, &command));
    const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired));
    check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rendered));
}

Presenter::~Presenter() {
    vkDeviceWaitIdle(device);
    clearSwapchain();
    vkDestroySemaphore(device, rendered, nullptr);
    vkDestroySemaphore(device, acquired, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

void Presenter::clearSwapchain() {
    if (pixels != nullptr) {
        vkUnmapMemory(device, memory);
        pixels = nullptr;
    }
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, memory, nullptr);
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    staging = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
}

void Presenter::resize(u32 width, u32 height) {
    check(vkDeviceWaitIdle(device));
    clearSwapchain();
    VkSurfaceCapabilitiesKHR caps;
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps));
    STD_INSIST(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    u32 count = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
    Vector<VkSurfaceFormatKHR> formats;
    formats.zero(count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.mutData()));
    bool found = false;
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            found = true;
        }
    }
    STD_INSIST(found);
    extent = {width, height};
    count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && count > caps.maxImageCount) {
        count = caps.maxImageCount;
    }
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = count,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain));
    check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
    images.zero(count);
    check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.mutData()));
    const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)width * height * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(device, &bufferInfo, nullptr, &staging));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    u32 memoryType = UINT32_MAX;
    const u32 wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            memoryType = i;
            break;
        }
    }
    STD_INSIST(memoryType != UINT32_MAX);
    const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = memoryType};
    check(vkAllocateMemory(device, &allocation, nullptr, &memory));
    check(vkBindBufferMemory(device, staging, memory, 0));
    check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &pixels));
}

bool Presenter::paint(App& app, const WindowInfo& info) {
    if (swapchain == VK_NULL_HANDLE || extent.width != info.width || extent.height != info.height) {
        resize(info.width, info.height);
    }
    u32 index;
    const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        resize(info.width, info.height);
        app.window->requestFrame();
        return false;
    }
    STD_INSIST(acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR);
    cairo_surface_t* const image = cairo_image_surface_create_for_data(static_cast<unsigned char*>(pixels), CAIRO_FORMAT_ARGB32, info.width, info.height, info.width * 4);
    cairo_t* const context = cairo_create(image);
    Canvas canvas{context, info.width, info.height};
    app.paint(canvas);
    cairo_destroy(context);
    cairo_surface_flush(image);
    cairo_surface_destroy(image);
    check(vkResetCommandBuffer(command, 0));
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[index],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {info.width, info.height, 1}};
    vkCmdCopyBufferToImage(command, staging, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    check(vkEndCommandBuffer(command));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired, .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &command, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered};
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    const VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index};
    const VkResult result = vkQueuePresentKHR(queue, &present);
    check(vkQueueWaitIdle(queue));
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        resize(info.width, info.height);
        app.window->requestFrame();
        return false;
    }
    check(result);
    return true;
}

VulkanPresenter* VulkanPresenter::create(ObjPool& owner, const RenderContext& context) {
    return owner.make<Presenter>(context);
}
