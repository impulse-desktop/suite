#include "platform.h"

#if defined(__APPLE__)
    #include "platform_cocoa.h"
#elif defined(__linux__)
    #include "platform_wayland.h"
#else
    #error Unsupported platform
#endif

using namespace plt;

Platform* Platform::create(stl::ObjPool& owner) {
#if defined(__APPLE__)
    return createCocoaPlatform(owner);
#elif defined(__linux__)
    return createWaylandPlatform(owner);
#else
    #error Unsupported platform
#endif
}
