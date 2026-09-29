#pragma once

#include <std/str/view.h>

#include <vulkan/vulkan.h>

namespace stl {
    class ObjPool;
}

// The fault seam, one per process. A call site hands over the result it
// has just got, or the outcome a call is about to have, and carries on
// with whatever comes back. The production build gives everything back
// untouched; the test build (IM_FOR_TESTS) breaks what IM_CHAOS asks for.
struct ChaosMonkey {
    // the device's memory types, before a heap is picked out of them
    virtual void memoryTypes(VkPhysicalDeviceMemoryProperties& props) = 0;
    // the result of a Vulkan call its caller checks
    virtual VkResult vulkan(VkResult result) = 0;
    // whether the Vulkan device offers the named extension, as the device
    // answered: a device without it takes the fallback of its own
    virtual bool deviceExtension(const char* name, bool offered) = 0;
    // the result of acquiring a swapchain image or presenting one
    virtual VkResult swapchain(VkResult result) = 0;
    // the outcome an encoder allocation is about to have (libpng's write
    // and info structs, libjxl's encoder and frame settings), handed over
    // before the call: false stands for it failing, and nothing is made
    virtual bool encoderAlloc(bool pending) = 0;

    static ChaosMonkey* create(stl::ObjPool& pool);
};
