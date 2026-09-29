#include "chaos_monkey.h"

#include "util.h"

#include <std/str/view.h>
#include <std/mem/obj_pool.h>

#ifdef IM_FOR_TESTS
    #include <stdlib.h>
    #include <std/lib/vector.h>
#endif

using namespace stl;

#ifdef IM_FOR_TESTS
// The test binary's monkey. IM_CHAOS lists the faults as FAULT=ARG words,
// read once at start, and each fault is spent by the call it fires on, so
// a scenario states exactly which call goes wrong:
//   memory-types=N    the next N memory-type queries find none
//   vulkan=K          K checked Vulkan calls pass, the one after fails
//   vulkan-at=SITE    the checked Vulkan call named SITE fails (the word
//                     may repeat for several)
//   no-ext=NAME       the Vulkan device does not offer extension NAME (the
//                     word may repeat for several)
//   swapchain=K       K swapchain acquires and presents pass, the one after
//                     reports the swapchain out of date
//   swapchain-suboptimal=K  the same, reporting it suboptimal instead
//   encoder-alloc=K   K encoder allocations pass, the one after fails as
//                     out of memory
//   encoder-output=K  K JPEG XL output steps pass, the one after fails
//   count=NAME:N      the device answers N to the count named NAME
//                     (devices, queue-families, surface-formats)
//   discrete-gpu=1    the physical device says it is a discrete GPU
//   no-wsi=1          the queue cannot present to the surface
//   image-counts=MIN:MAX  the surface wants MIN images at least and
//                     allows MAX at most
namespace {
    struct NamedCount {
        StringView what;
        u32 count;
    };

    struct TestChaosMonkey: public ChaosMonkey {
        int memoryFaults = 0;
        int vulkanSkip = -1;
        Vector<StringView> failingSites;
        Vector<StringView> hiddenExtensions;
        int swapchainSkip = -1;
        VkResult swapchainFault = VK_SUCCESS;
        int encoderAllocSkip = -1;
        int encoderOutputSkip = -1;
        Vector<NamedCount> counts;
        bool discreteGpu = false;
        bool noWsi = false;
        bool imageCountsSet = false;
        u32 minImages = 0;
        u32 maxImages = 0;

        explicit TestChaosMonkey(StringView script);

        void arm(StringView script);
        void armFault(StringView fault, StringView arg);

        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        bool encoderAlloc(bool pending) override;
        bool encoderOutput(bool produced) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };

    // a counted fault: fires while the count lasts, each firing spends one
    static bool spend(int& count) {
        if (count <= 0) {
            return false;
        }

        count--;

        return true;
    }

    // an ordinal fault, -1 while unarmed: K calls pass, the next one fails,
    // and the fault is spent
    static bool failsOnce(int& skip) {
        if (skip < 0) {
            return false;
        }

        return skip-- == 0;
    }
}

TestChaosMonkey::TestChaosMonkey(StringView script) {
    arm(script);
}

void TestChaosMonkey::arm(StringView script) {
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

void TestChaosMonkey::armFault(StringView fault, StringView arg) {
    if (fault == "memory-types"_sv) {
        memoryFaults = (int)arg.stou();
    } else if (fault == "vulkan"_sv) {
        vulkanSkip = (int)arg.stou();
    } else if (fault == "vulkan-at"_sv) {
        failingSites.pushBack(arg);
    } else if (fault == "no-ext"_sv) {
        hiddenExtensions.pushBack(arg);
    } else if (fault == "swapchain"_sv || fault == "swapchain-suboptimal"_sv) {
        swapchainSkip = (int)arg.stou();
        swapchainFault = fault == "swapchain"_sv ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUBOPTIMAL_KHR;
    } else if (fault == "encoder-alloc"_sv) {
        encoderAllocSkip = (int)arg.stou();
    } else if (fault == "encoder-output"_sv) {
        encoderOutputSkip = (int)arg.stou();
    } else if (fault == "count"_sv) {
        StringView what, n;

        if (arg.split(':', what, n)) {
            counts.pushBack({what, (u32)n.stou()});
        }
    } else if (fault == "discrete-gpu"_sv) {
        discreteGpu = true;
    } else if (fault == "no-wsi"_sv) {
        noWsi = true;
    } else if (fault == "image-counts"_sv) {
        StringView lo, hi;

        if (arg.split(':', lo, hi)) {
            imageCountsSet = true;
            minImages = (u32)lo.stou();
            maxImages = (u32)hi.stou();
        }
    }
}

void TestChaosMonkey::memoryTypes(VkPhysicalDeviceMemoryProperties& props) {
    if (spend(memoryFaults)) {
        props.memoryTypeCount = 0;
    }
}

VkResult TestChaosMonkey::vulkan(VkResult result) {
    if (!failsOnce(vulkanSkip)) {
        return result;
    }

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VkResult TestChaosMonkey::vulkanAt(StringView site, VkResult result) {
    for (StringView failing : failingSites) {
        if (failing == site) {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    return result;
}

bool TestChaosMonkey::deviceExtension(const char* name, bool offered) {
    for (StringView hidden : hiddenExtensions) {
        if (hidden == StringView(name)) {
            return false;
        }
    }

    return offered;
}

VkResult TestChaosMonkey::swapchain(VkResult result) {
    if (!failsOnce(swapchainSkip)) {
        return result;
    }

    return swapchainFault;
}

bool TestChaosMonkey::encoderAlloc(bool pending) {
    if (!failsOnce(encoderAllocSkip)) {
        return pending;
    }

    return false;
}

bool TestChaosMonkey::encoderOutput(bool produced) {
    if (!failsOnce(encoderOutputSkip)) {
        return produced;
    }

    return false;
}

u32 TestChaosMonkey::count(StringView what, u32 count) {
    for (const NamedCount& named : counts) {
        if (named.what == what) {
            return named.count;
        }
    }

    return count;
}

VkPhysicalDeviceType TestChaosMonkey::deviceType(VkPhysicalDeviceType type) {
    return discreteGpu ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : type;
}

VkBool32 TestChaosMonkey::surfaceSupport(VkBool32 supported) {
    return noWsi ? VK_FALSE : supported;
}

void TestChaosMonkey::imageCounts(VkSurfaceCapabilitiesKHR& caps) {
    if (imageCountsSet) {
        caps.minImageCount = minImages;
        caps.maxImageCount = maxImages;
    }
}

ChaosMonkey* ChaosMonkey::create(ObjPool& pool) {
    const char* script = getenv("IM_CHAOS");

    return pool.make<TestChaosMonkey>(StringView(script ? script : ""));
}
#else
// The production monkey: every call gives its argument back.
namespace {
    struct IdleChaosMonkey: public ChaosMonkey {
        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        VkResult vulkanAt(StringView site, VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        bool encoderAlloc(bool pending) override;
        bool encoderOutput(bool produced) override;
        u32 count(StringView what, u32 count) override;
        VkPhysicalDeviceType deviceType(VkPhysicalDeviceType type) override;
        VkBool32 surfaceSupport(VkBool32 supported) override;
        void imageCounts(VkSurfaceCapabilitiesKHR& caps) override;
    };
}

void IdleChaosMonkey::memoryTypes(VkPhysicalDeviceMemoryProperties&) {
}

VkResult IdleChaosMonkey::vulkan(VkResult result) {
    return result;
}

VkResult IdleChaosMonkey::vulkanAt(StringView, VkResult result) {
    return result;
}

bool IdleChaosMonkey::deviceExtension(const char*, bool offered) {
    return offered;
}

VkResult IdleChaosMonkey::swapchain(VkResult result) {
    return result;
}

bool IdleChaosMonkey::encoderAlloc(bool pending) {
    return pending;
}

bool IdleChaosMonkey::encoderOutput(bool produced) {
    return produced;
}

u32 IdleChaosMonkey::count(StringView, u32 count) {
    return count;
}

VkPhysicalDeviceType IdleChaosMonkey::deviceType(VkPhysicalDeviceType type) {
    return type;
}

VkBool32 IdleChaosMonkey::surfaceSupport(VkBool32 supported) {
    return supported;
}

void IdleChaosMonkey::imageCounts(VkSurfaceCapabilitiesKHR&) {
}

ChaosMonkey* ChaosMonkey::create(ObjPool& pool) {
    return pool.make<IdleChaosMonkey>();
}
#endif
