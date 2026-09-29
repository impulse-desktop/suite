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
//   no-ext=NAME       the Vulkan device does not offer extension NAME (the
//                     word may repeat for several)
//   swapchain=K       K swapchain acquires and presents pass, the one after
//                     reports the swapchain out of date
//   swapchain-suboptimal=K  the same, reporting it suboptimal instead
//   encoder-alloc=K   K encoder allocations pass, the one after fails as
//                     out of memory
namespace {
    struct TestChaosMonkey: public ChaosMonkey {
        int memoryFaults = 0;
        int vulkanSkip = -1;
        Vector<StringView> hiddenExtensions;
        int swapchainSkip = -1;
        VkResult swapchainFault = VK_SUCCESS;
        int encoderAllocSkip = -1;

        explicit TestChaosMonkey(StringView script);

        void arm(StringView script);
        void armFault(StringView fault, StringView arg);

        void memoryTypes(VkPhysicalDeviceMemoryProperties& props) override;
        VkResult vulkan(VkResult result) override;
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        bool encoderAlloc(bool pending) override;
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
    } else if (fault == "no-ext"_sv) {
        hiddenExtensions.pushBack(arg);
    } else if (fault == "swapchain"_sv || fault == "swapchain-suboptimal"_sv) {
        swapchainSkip = (int)arg.stou();
        swapchainFault = fault == "swapchain"_sv ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUBOPTIMAL_KHR;
    } else if (fault == "encoder-alloc"_sv) {
        encoderAllocSkip = (int)arg.stou();
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
        bool deviceExtension(const char* name, bool offered) override;
        VkResult swapchain(VkResult result) override;
        bool encoderAlloc(bool pending) override;
    };
}

void IdleChaosMonkey::memoryTypes(VkPhysicalDeviceMemoryProperties&) {
}

VkResult IdleChaosMonkey::vulkan(VkResult result) {
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

ChaosMonkey* ChaosMonkey::create(ObjPool& pool) {
    return pool.make<IdleChaosMonkey>();
}
#endif
