#include "app.h"

#include "fiber.h"
#include "vulkan.h"

#include <std/str/view.h>
#include <std/dbg/insist.h>
#include <std/lib/buffer.h>

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>

using namespace plt;
using namespace plt::e2e;
using namespace stl;

namespace {
    struct FontCleanup {
        ~FontCleanup();
    };
}

FontCleanup::~FontCleanup() {
    cairo_debug_reset_static_data();
    FcFini();
}

namespace plt::e2e {
    class Surface {
    public:
        Surface(ObjPool& owner, const RenderContext& context);
        ~Surface();
        bool paint(App& app, const WindowInfo& info);

    private:
        struct BufferSlot {
            wl_buffer* buffer = nullptr;
            void* pixels = nullptr;
            size_t size = 0;
            u32 width = 0;
            u32 height = 0;
            bool busy = false;
        };

        static void global(void* data, wl_registry* registry, u32 name, const char* interface, u32 version);
        static void removed(void* data, wl_registry* registry, u32 name);
        static void released(void* data, wl_buffer* buffer);
        void destroy(BufferSlot& slot);

        wl_display* display_;
        wl_surface* surface_;
        wl_registry* registry_;
        wl_shm* shm_ = nullptr;
        VulkanPresenter* vulkan_ = nullptr;
        BufferSlot slots_[3];
    };
}

Surface::Surface(ObjPool& owner, const RenderContext& context)
    : display_(static_cast<wl_display*>(context.connection))
    , surface_(static_cast<wl_surface*>(context.window))
    , registry_(wl_display_get_registry(display_))
{
    STD_INSIST(context.backend == RenderBackend::Wayland);
    static const wl_registry_listener listener = {global, removed};
    wl_registry_add_listener(registry_, &listener, this);
    STD_INSIST(wl_display_roundtrip(display_) >= 0);
    STD_INSIST(shm_ != nullptr);
    if (strcmp(setting("PLT_E2E_RENDERER", "shm"), "lavapipe") == 0) {
        vulkan_ = VulkanPresenter::create(owner, context);
    }
}

Surface::~Surface()
{
    for (auto& slot : slots_) {
        destroy(slot);
    }
    wl_shm_destroy(shm_);
    wl_registry_destroy(registry_);
}

void Surface::global(void* data, wl_registry* registry, u32 name, const char* interface, u32) {
    if (strcmp(interface, "wl_shm") == 0) {
        static_cast<Surface*>(data)->shm_ = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
}

void Surface::removed(void*, wl_registry*, u32) {
}

void Surface::released(void* data, wl_buffer*) {
    static_cast<BufferSlot*>(data)->busy = false;
}

void Surface::destroy(BufferSlot& slot) {
    if (slot.buffer != nullptr) {
        wl_buffer_destroy(slot.buffer);
        munmap(slot.pixels, slot.size);
    }
    slot = {};
}

bool Surface::paint(App& app, const WindowInfo& info) {
    if (vulkan_ != nullptr) {
        return vulkan_->paint(app, info);
    }
    for (auto& slot : slots_) {
        if (slot.busy) {
            continue;
        }
        if (slot.width != info.width || slot.height != info.height) {
            destroy(slot);
            STD_INSIST(info.width > 0 && info.width <= 4096 && info.height > 0 && info.height <= 4096);
            slot.width = info.width;
            slot.height = info.height;
            slot.size = (size_t)info.width * info.height * 4;
            const int fd = memfd_create("plt-e2e-frame", MFD_CLOEXEC);
            STD_INSIST(fd >= 0);
            STD_INSIST(ftruncate(fd, slot.size) == 0);
            slot.pixels = mmap(nullptr, slot.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            STD_INSIST(slot.pixels != MAP_FAILED);
            wl_shm_pool* const pool = wl_shm_create_pool(shm_, fd, slot.size);
            slot.buffer = wl_shm_pool_create_buffer(pool, 0, info.width, info.height, info.width * 4, WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
            ::close(fd);
            static const wl_buffer_listener listener = {released};
            wl_buffer_add_listener(slot.buffer, &listener, &slot);
        }
        cairo_surface_t* const image = cairo_image_surface_create_for_data(static_cast<unsigned char*>(slot.pixels), CAIRO_FORMAT_ARGB32, info.width, info.height, info.width * 4);
        cairo_t* const context = cairo_create(image);
        Canvas canvas{context, info.width, info.height};
        app.paint(canvas);
        cairo_destroy(context);
        cairo_surface_flush(image);
        cairo_surface_destroy(image);
        slot.busy = true;
        wl_surface_attach(surface_, slot.buffer, 0, 0);
        wl_surface_damage_buffer(surface_, 0, 0, info.width, info.height);
        wl_surface_commit(surface_);
        return true;
    }
    app.window->requestFrame();
    return false;
}

void Canvas::rectangle(double x, double y, double w, double h, u32 color) {
    cairo_set_source_rgb(context, ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0);
    cairo_rectangle(context, x, y, w, h);
    cairo_fill(context);
}

void Canvas::clear(u32 color) {
    rectangle(0, 0, width, height, color);
}

void Canvas::text(double x, double y, StringView text, u32 color) {
    Buffer bytes(text);
    cairo_set_source_rgb(context, ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0);
    cairo_select_font_face(context, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 18);
    cairo_move_to(context, x, y);
    cairo_show_text(context, bytes.cStr());
}

App::App(Platform* shared)
    : owner(ObjPool::fromMemory())
    , platform(shared == nullptr ? Platform::create(*owner) : shared)
{
    // Runs at process exit, after every application and rendering context.
    static FontCleanup fonts;
    setvbuf(stdout, nullptr, _IOLBF, 0);
}

App::~App()
{
}

void App::open(const char* title, u32 width, u32 height) {
    id = title;
    window = platform->createWindow(*owner, {
        .appId = StringView(title),
        .title = StringView(title),
        .width = width,
        .height = height,
        .minimumWidth = 160,
        .minimumHeight = 120,
        .decorations = false,
        .input = createFiberInputSink(*owner, *platform->scheduler(), *this),
        .events = this,
        .frame = this,
    });
    surface = owner->make<Surface>(*owner, window->renderContext());
    window->requestShow();
}

void App::run() {
    platform->run();
    printf("CLOSED %s\n", id);
}

bool App::frame(const WindowInfo& info) {
    if (!surface->paint(*this, info)) {
        return false;
    }
    ++frames;
    printf("FRAME %s %u %u %.3f %d %d\n", id, info.width, info.height, info.contentScale, info.focused, info.fullscreen);
    return true;
}

void App::close() {
    platform->stop();
}

void App::key(const KeyInput& input) {
    if (input.key == InputKey::Escape && input.action == InputAction::Press) {
        window->requestClose();
    }
}

void App::text(const TextInput&) {
}

void App::preedit(StringView, i32, i32) {
}

void App::pointerMotion(const PointerMotionInput&) {
}

void App::pointerButton(const PointerButtonInput&) {
}

void App::scroll(const ScrollInput&) {
}

void App::focus(bool focused) {
    printf("FOCUS %s %d\n", id, focused);
    window->requestFrame();
}

void App::pointerPresence(bool) {
}

void App::flush() {
}

const char* plt::e2e::setting(const char* name, const char* fallback) {
    const char* const value = getenv(name);
    return value == nullptr ? fallback : value;
}
