#pragma once

namespace stl {
    class ObjPool;
}

namespace plt {
    struct InputSink;
    struct Window;
}

// ImGui platform bindings over plt, replacing imgui_impl_glfw: the sink
// feeds plt input events into ImGui IO, newFrame syncs the per-frame
// display metrics and pushes the ImGui-chosen pointer icon back into the
// window. Rendering stays with imgui_impl_vulkan. The ImGui context must
// exist before the platform loop delivers the first event.
struct ImGuiPlt {
    // hand this to plt::WindowOptions::input when creating the window
    virtual plt::InputSink* sink() = 0;

    // call inside the plt frame callback, before ImGui::NewFrame
    virtual void newFrame(plt::Window& window) = 0;

    static ImGuiPlt* create(stl::ObjPool& pool);
};
