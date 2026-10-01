#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct InputSink;
    struct Window;
}

struct ImGuiPlt {
    virtual plt::InputSink* sink() = 0;

    virtual void newFrame(plt::Window& window) = 0;

    static ImGuiPlt* create(stl::ObjPool& pool, float scale, bool traceFrames);
};
