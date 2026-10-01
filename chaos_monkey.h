#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>

#include <vulkan/vulkan.h>

namespace stl {
    class ObjPool;
}

struct ChaosMonkey {
    virtual void memoryTypes(VkPhysicalDeviceMemoryProperties& props) = 0;
    virtual VkResult vulkan(VkResult result) = 0;
    virtual VkResult vulkanAt(stl::StringView site, VkResult result) = 0;
    virtual bool deviceExtension(const char* name, bool offered) = 0;
    virtual VkResult swapchain(VkResult result) = 0;
    virtual bool encoderAlloc(bool pending) = 0;
    virtual bool encoderOutput(bool produced) = 0;
    virtual u32 count(stl::StringView what, u32 count) = 0;
    virtual VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) = 0;
    virtual VkBool32 surfaceSupport(VkBool32 supported) = 0;
    virtual void imageCounts(VkSurfaceCapabilitiesKHR& caps) = 0;

    static ChaosMonkey* create(stl::ObjPool& pool);
};
