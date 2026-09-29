#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createWaylandPlatform(stl::ObjPool& owner);
}
