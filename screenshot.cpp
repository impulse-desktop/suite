#include "screenshot.h"

#include "util.h"
#include "color.h"
#include "pooled.h"
#include "check_true.h"

#include <std/sys/fd.h>
#include <std/ios/sys.h>
#include <std/sys/throw.h>
#include <std/sys/types.h>
#include <std/ios/out_fd.h>
#include <std/lib/vector.h>
#include <std/ios/fs_utils.h>

#include <math.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// vulkan_wayland.h only names these through pointers
struct wl_display;
struct wl_surface;

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include "chaos_monkey.h"

#include <plt/window.h>
#include <plt/platform.h>

#include "imgui_plt.h"
#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <fullscreen_vert.spv.h>
#include <screenshot_image_frag.spv.h>
#include <screenshot_image_vert.spv.h>
#include <screenshot_output_frag.spv.h>
#include <screenshot_scene_frag.spv.h>

#include <png.h>
#include <jxl/color_encoding.h>
#include <jxl/encode.h>

using namespace stl;

// im screenshot <path>: a standalone plt+Vulkan imgui client. On KMS the
// path names an owned scanout DMA-BUF and metadata comes through the
// environment; a readback comes as a self-describing IMW1 memfd. The HDR viewer
// decodes the shared PQ image into an FP16 linear-BT.2020/nits target, blends
// ImGui there, then encodes the result to its PQ swapchain. Save still
// reads only the selected source region. plt owns window/input.

namespace {
    // a screenshot-specific exception carrying a human message; raised by fail()
    // and shown verbatim on the error panel. derives stl::Exception so
    // Exception::current() surfaces it in a generic catch, like the rest of the suite
    struct ShotError: Exception {
        Buffer msg;

        explicit ShotError(Buffer m)
            : msg((Buffer&&)m)
        {
        }

        ExceptionKind kind() const noexcept override {
            return ExceptionKind::Verify;
        }

        StringView description() override {
            return sv(msg);
        }
    };

    [[noreturn]] void fail(StringView m, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        const char* base = strrchr(file, '/');

        throw ShotError(Buffer(sv(StringBuilder() << StringView(base ? base + 1 : file) << ":"_sv << (long)line << ": "_sv << m)));
    }

    // ---- decoded source image ----
    // the whole memfd (IMW1 header + RGBA8 rows) is read into one buffer; px
    // points past the 12-byte header, so no separate pixel allocation
    struct Image {
        Buffer file;
        u32 w = 0, h = 0;
        const u8* px = nullptr;
        int dmaFd = -1;
        u32 format = 0;
        u32 offset = 0;
        u32 stride = 0;
        u64 modifier = 0;
        u64 allocationSize = 0;
        u8 deviceUuid[VK_UUID_SIZE] = {};
        bool dmabuf = false;
        OutputColorState color;
        Buffer rgb16;

        bool shared() const;
    };

    struct Texture;
    void readTexture(const Image& img, const Texture& tex, int x0, int y0, int x1, int y1, Image& out);

    constexpr u32 kMagic = 0x31574d49u; // 'IMW1' little-endian

    bool Image::shared() const {
        return dmabuf;
    }

    // W:H:FORMAT:OFFSET:STRIDE:MODIFIER:SIZE:UUID, seven decimal fields and
    // the exporting GPU's deviceUUID as 32 hex digits
    bool parseShared(StringView spec, Image& img) {
        u64 values[7] = {};
        size_t pos = 0;

        for (int i = 0; i < 7; i++) {
            size_t begin = pos;

            while (pos < spec.length() && spec[pos] >= '0' && spec[pos] <= '9') {
                u64 digit = (u64)(spec[pos] - '0');

                if (values[i] > (UINT64_MAX - digit) / 10) {
                    return false;
                }

                values[i] = values[i] * 10 + digit;
                pos++;
            }

            if (pos == begin || pos >= spec.length() || spec[pos] != ':') {
                return false;
            }

            pos++;
        }

        if (spec.length() - pos != 2 * VK_UUID_SIZE) {
            return false;
        }

        for (size_t i = 0; i < 2 * VK_UUID_SIZE; i++) {
            char c = spec[pos + i];
            int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

            if (nibble < 0) {
                return false;
            }

            img.deviceUuid[i / 2] = (u8)(img.deviceUuid[i / 2] << 4 | nibble);
        }

        img.w = (u32)values[0];
        img.h = (u32)values[1];
        img.format = (u32)values[2];
        img.offset = (u32)values[3];
        img.stride = (u32)values[4];
        img.modifier = values[5];
        img.allocationSize = values[6];

        return img.w && img.h && img.stride && img.allocationSize;
    }

    // throws (ShotError, or the Errno readFileContent raises) on any failure.
    // "fd:3" names the buffer handed over the spawn socket: dma-bufs cannot
    // be reopened through /proc at all (ENXIO), so the fd itself travels
    void loadImage(StringView path, Image& img) {
        bool inherited = path == "fd:3"_sv;
        Buffer p(inherited ? "/proc/self/fd/3"_sv : path);

        if (const char* color = getenv("IMWAY_SHOT_COLOR")) {
            StringView value(color), hs, rest;

            if (value.split(':', hs, rest)) {
                // the white level leads; a display volume after it counts
                // only when all three of its fields are there
                StringView whiteString = rest;
                StringView minString, peakString, fallString, head, tail;
                bool volume = false;

                if (rest.split(':', head, tail)) {
                    whiteString = head;
                    volume = tail.split(':', minString, tail) && tail.split(':', peakString, fallString);
                }

                double white = parseFloat(whiteString);

                img.color = hs == "1"_sv ? OutputColorState::hdr10(white) : OutputColorState::sdr();

                if (volume) {
                    img.color.displayMinNits = parseFloat(minString);
                    img.color.displayPeakNits = parseFloat(peakString);
                    img.color.displayMaxFallNits = parseFloat(fallString);
                    img.color.encoding.targetMinNits = img.color.displayMinNits;
                    img.color.encoding.targetMaxNits = img.color.displayPeakNits;
                }
            }
        }

        if (const char* spec = getenv("IMWAY_SHOT_DMABUF")) {
            if (!parseShared(StringView(spec), img)) {
                fail("bad shared screenshot metadata"_sv);
            }

            img.dmaFd = inherited ? fcntl(3, F_DUPFD_CLOEXEC, 0) : open(p.cStr(), O_RDONLY | O_CLOEXEC);

            if (img.dmaFd < 0) {
                fail(sv(StringBuilder() << "cannot take the shared screenshot fd: "_sv << StringView(strerror(errno))));
            }

            img.dmabuf = true;

            return;
        }

        readFileContent(p, img.file);

        if (img.file.used() < 12) {
            fail("not an imway screenshot (too small)"_sv);
        }

        const u32* h = (const u32*)img.file.data();

        if (h[0] != kMagic || !h[1] || !h[2]) {
            fail("not an imway screenshot (bad header)"_sv);
        }

        img.w = h[1];
        img.h = h[2];

        if (img.file.used() < 12 + (size_t)img.w * img.h * 4) {
            fail("truncated screenshot"_sv);
        }

        img.px = (const u8*)img.file.data() + 12;
    }

    // ---- encoded output ----
    // mkdir -p: create each '/'-separated prefix of the path in turn
    void mkdirs(StringView path) {
        Buffer b(path);
        char* s = b.cStr();

        for (char* p = s + 1; *p; p++) {
            if (*p == '/') {
                *p = 0;
                mkdir(s, 0755);
                *p = '/';
            }
        }

        mkdir(s, 0755);
    }

    // libpng writes go through this into a growable buffer, so the same encode
    // path feeds both the file save and the clipboard data source
    void pngWrite(png_structp png, png_bytep data, png_size_t len) {
        ((Buffer*)png_get_io_ptr(png))->append(data, (size_t)len);
    }

    double pqToNits(double value) {
        constexpr double m1 = 2610.0 / 16384.0;
        constexpr double m2 = 2523.0 / 32.0;
        constexpr double c1 = 3424.0 / 4096.0;
        constexpr double c2 = 2413.0 / 128.0;
        constexpr double c3 = 2392.0 / 128.0;
        double p = pow(fmax(value, 0.0), 1.0 / m2);

        return pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * 10000.0;
    }

    u8 linearToSrgb8(double value) {
        value = fmax(0.0, fmin(1.0, value));
        double encoded = value <= .0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - .055;

        return (u8)lround(encoded * 255.0);
    }

    // the viewer's own fault seam, a monkey of the same kind as the
    // compositor's, configured from the viewer's environment
    ChaosMonkey* gChaos = nullptr;

    // encode the [x0,y0,x1,y1) region of img (image px, already clamped) as an
    // RGBA png into out; throws on failure
    void encodePng(const Image& img, int x0, int y0, int x1, int y1, Buffer& out) {
        png_structp png = gChaos->encoderAlloc(true) ? png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr) : nullptr;
        png_infop info = png && gChaos->encoderAlloc(true) ? png_create_info_struct(png) : nullptr;

        if (!png || !info || setjmp(png_jmpbuf(png))) {
            if (png) {
                png_destroy_write_struct(&png, info ? &info : nullptr);
            }

            fail("png encode failed"_sv);
        }

        png_set_write_fn(png, &out, pngWrite, nullptr);
        png_set_IHDR(png, info, (u32)(x1 - x0), (u32)(y1 - y0), 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
        png_write_info(png, info);

        if (!img.color.hdr()) {
            for (int y = y0; y < y1; y++) {
                png_write_row(png, (png_bytep)(img.px + ((size_t)y * img.w + x0) * 4));
            }
        } else {
            Buffer row;
            row.zero((size_t)(x1 - x0) * 4);
            OutputMapping mapping = outputMapping(OutputColorState::sdr());

            for (int y = y0; y < y1; y++) {
                u8* dst = (u8*)row.mutData();

                for (int x = x0; x < x1; x++) {
                    size_t source = ((size_t)y * img.w + x);
                    ColorRgb pq;

                    if (img.rgb16.length()) {
                        const u16* src = (const u16*)img.rgb16.data() + source * 3;

                        pq = {(double)src[0] / 65535.0, (double)src[1] / 65535.0, (double)src[2] / 65535.0};
                    } else {
                        const u8* src = img.px + source * 4;

                        pq = {(double)src[0] / 255.0, (double)src[1] / 255.0, (double)src[2] / 255.0};
                    }

                    ColorRgb nits{pqToNits(pq.r), pqToNits(pq.g), pqToNits(pq.b)};
                    ColorRgb mapped = mapping.toTarget.apply(mapOutputNits(mapping, nits));
                    size_t at = (size_t)(x - x0) * 4;

                    dst[at + 0] = linearToSrgb8(mapped.r / 203.0);
                    dst[at + 1] = linearToSrgb8(mapped.g / 203.0);
                    dst[at + 2] = linearToSrgb8(mapped.b / 203.0);
                    dst[at + 3] = 255;
                }

                png_write_row(png, (png_bytep)dst);
            }
        }

        png_write_end(png, nullptr);
        png_destroy_write_struct(&png, &info);
    }

    // stream the encoded png to a file; throws on open/write failure
    void saveFile(const Buffer& data, StringView file) {
        ScopedFD fd(open(Buffer(file).cStr(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

        if (fd.get() < 0) {
            fail(sv(StringBuilder() << "cannot write "_sv << file));
        }

        FDRegular out(fd);

        out.write(data.data(), data.length());
        out.flush();
    }

    void encodeJxlPixels(const Image& img, const u16* pixels, u32 w, u32 h, Buffer& out) {
        JxlEncoder* enc = gChaos->encoderAlloc(true) ? JxlEncoderCreate(nullptr) : nullptr;

        if (!enc) {
            fail("jxl encoder allocation failed"_sv);
        }

        JxlBasicInfo info;

        JxlEncoderInitBasicInfo(&info);
        info.xsize = w;
        info.ysize = h;
        info.bits_per_sample = 16;
        info.num_color_channels = 3;
        info.uses_original_profile = JXL_TRUE;

        if (img.color.hdr()) {
            info.intensity_target = (float)img.color.displayPeakNits;
            info.min_nits = (float)img.color.displayMinNits;
            info.relative_to_max_display = JXL_FALSE;
            info.linear_below = (float)img.color.displayMinNits;
        }

        JxlColorEncoding color{};

        if (img.color.hdr()) {
            color.color_space = JXL_COLOR_SPACE_RGB;
            color.white_point = JXL_WHITE_POINT_D65;
            color.primaries = JXL_PRIMARIES_2100;
            color.transfer_function = JXL_TRANSFER_FUNCTION_PQ;
            color.rendering_intent = JXL_RENDERING_INTENT_RELATIVE;
        } else {
            JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
        }

        JxlEncoderFrameSettings* frame = gChaos->encoderAlloc(true) ? JxlEncoderFrameSettingsCreate(enc, nullptr) : nullptr;
        JxlPixelFormat format{3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
        size_t bytes = (size_t)w * h * 3 * sizeof(u16);
        bool lossless = !getenv("IMWAY_SHOT_LOSSLESS") || StringView(getenv("IMWAY_SHOT_LOSSLESS")) != "0"_sv;
        bool frameConfigured = false;

        if (frame) {
            if (lossless) {
                frameConfigured = JxlEncoderSetFrameLossless(frame, JXL_TRUE) == JXL_ENC_SUCCESS;
            } else {
                double quality = 90.;

                if (const char* value = getenv("IMWAY_SHOT_QUALITY")) {
                    quality = strtod(value, nullptr);
                }

                quality = quality < 1. ? 1. : quality > 100. ? 100. : quality;
                float distance = quality >= 100. ? 0.f : (float)(.1 + (100. - quality) * .15);

                frameConfigured = JxlEncoderSetFrameDistance(frame, distance) == JXL_ENC_SUCCESS;
            }
        }

        bool ok = JxlEncoderSetBasicInfo(enc, &info) == JXL_ENC_SUCCESS && JxlEncoderSetColorEncoding(enc, &color) == JXL_ENC_SUCCESS && frameConfigured && JxlEncoderAddImageFrame(frame, &format, pixels, bytes) == JXL_ENC_SUCCESS;

        if (!ok) {
            JxlEncoderDestroy(enc);
            fail("jxl encode setup failed"_sv);
        }

        JxlEncoderCloseInput(enc);
        out.reset();

        for (;;) {
            unsigned char chunk[64 * 1024];
            unsigned char* next = chunk;
            size_t available = sizeof(chunk);
            JxlEncoderStatus status = JxlEncoderProcessOutput(enc, &next, &available);

            out.append(chunk, sizeof(chunk) - available);

            if (status == JXL_ENC_SUCCESS) {
                break;
            }
            if (status != JXL_ENC_NEED_MORE_OUTPUT) {
                JxlEncoderDestroy(enc);
                fail("jxl encode failed"_sv);
            }
        }

        JxlEncoderDestroy(enc);
    }

    void encodeJxlSelection(const Image& img, const Texture& tex, int x0, int y0, int x1, int y1, Buffer& out) {
        Image selected;

        if (img.shared()) {
            readTexture(img, tex, x0, y0, x1, y1, selected);
        } else {
            selected.w = (u32)(x1 - x0);
            selected.h = (u32)(y1 - y0);
            selected.rgb16.zero((size_t)selected.w * selected.h * 3 * sizeof(u16));
            u16* dst = (u16*)selected.rgb16.mutData();

            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    const u8* src = img.px + ((size_t)y * img.w + x) * 4;
                    size_t at = ((size_t)(y - y0) * selected.w + (x - x0)) * 3;

                    dst[at + 0] = (u16)(src[0] * 257);
                    dst[at + 1] = (u16)(src[1] * 257);
                    dst[at + 2] = (u16)(src[2] * 257);
                }
            }
        }

        selected.color = img.color;
        encodeJxlPixels(selected, (const u16*)selected.rgb16.data(), selected.w, selected.h, out);
    }

    // User directory/template, falling back to
    // $XDG_PICTURES_DIR/screenshots/imway-YYYYMMDD-HHMMSS.<format>.
    Buffer destPath() {
        Buffer dir;
        StringBuilder builder((Buffer&&)dir);
        const char* configured = getenv("IMWAY_SHOT_DIR");
        const char* base = getenv("XDG_PICTURES_DIR");

        if (configured && *configured) {
            builder << StringView(configured);
        } else if (base && *base) {
            builder << StringView(base);
        } else {
            const char* home = getenv("HOME");

            builder << StringView(home ? home : ".") << "/Pictures"_sv;
        }

        if (!configured || !*configured) {
            builder << "/screenshots"_sv;
        }

        builder.xchg(dir);
        mkdirs(sv(dir));

        time_t t = time(nullptr);
        struct tm tm;

        localtime_r(&t, &tm);

        char stamp[256];
        const char* name = getenv("IMWAY_SHOT_NAME");

        if (!name || !*name) {
            name = "imway-%Y%m%d-%H%M%S";
        }

        if (!strftime(stamp, sizeof(stamp), name, &tm)) {
            fail("screenshot filename is too long"_sv);
        }

        StringView extension = getenv("IMWAY_SHOT_FORMAT") && StringView(getenv("IMWAY_SHOT_FORMAT")) == "png"_sv ? ".png"_sv : ".jxl"_sv;

        return Buffer(sv(StringBuilder() << sv(dir) << "/"_sv << StringView(stamp) << extension));
    }

    // ---- vulkan plumbing ----
    VkAllocationCallbacks* gAlloc = nullptr;
    VkInstance gInstance = VK_NULL_HANDLE;
    VkPhysicalDevice gPhys = VK_NULL_HANDLE;
    VkDevice gDevice = VK_NULL_HANDLE;
    u32 gQueueFamily = (u32)-1;
    VkQueue gQueue = VK_NULL_HANDLE;
    VkDescriptorPool gDescPool = VK_NULL_HANDLE;

    // one swapchain image and what records into it
    struct Frame {
        VkImage image;
        VkImageView view;
        VkFramebuffer framebuffer;
        VkCommandPool commandPool;
        VkCommandBuffer commandBuffer;
        VkFence fence;
    };

    // the semaphores of one frame in flight: an image acquired, a frame
    // rendered into it; one pair more than images, as an acquire hands its
    // image out only afterwards
    struct Sync {
        VkSemaphore acquired;
        VkSemaphore rendered;
    };

    // the window's surface, its swapchain and the frames drawn into it. The
    // render pass outlives the swapchain: the format never changes
    struct Presenter {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSurfaceFormatKHR format = {};
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        Vector<Frame> frames;
        Vector<Sync> syncs;
        // the frame whose image the last acquire handed out, and the sync
        // pair the next acquire uses
        u32 frameIndex = 0;
        u32 syncIndex = 0;
        int width = 0;
        int height = 0;
        VkClearValue clear = {};
    };

    Presenter gPresent;
    constexpr u32 kMinImageCount = 2;
    // the HDR scene's white in nits, what the output stage scales it by
    float gSdrWhiteNits = 203.f;
    bool gRebuild = false;
    bool gLinearHdr = false;
    VkRenderPass gScenePass = VK_NULL_HANDLE;
    VkImage gSceneImage = VK_NULL_HANDLE;
    VkDeviceMemory gSceneMemory = VK_NULL_HANDLE;
    VkImageView gSceneView = VK_NULL_HANDLE;
    VkFramebuffer gSceneFramebuffer = VK_NULL_HANDLE;
    VkSampler gSceneSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout gOutputSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet gOutputSet = VK_NULL_HANDLE;
    VkPipelineLayout gOutputPipelineLayout = VK_NULL_HANDLE;
    VkPipeline gOutputPipeline = VK_NULL_HANDLE;
    // the screenshot's own pipeline in the HDR scene: it draws the image
    // from an ImGui draw callback, PQ decoded into the scene's linear light
    VkDescriptorSetLayout gImageSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout gImagePipelineLayout = VK_NULL_HANDLE;
    VkPipeline gImagePipeline = VK_NULL_HANDLE;

    // screenshot_image.vert's push constants: ImGui's own scale and
    // translate, the quad in ImGui's screen space, the white the fragment
    // stage divides its nits by
    struct ImagePush {
        float scale[2];
        float translate[2];
        float rect[4];
        float sdrWhiteNits;
    };

    // what the draw callback is handed: ImGui copies it into the draw list
    struct ImageDraw {
        VkDescriptorSet texture;
        float x0, y0, x1, y1;
        float sdrWhiteNits;
    };

    // ui scale handed down from the compositor via IMGUI_SCALE (its clients
    // otherwise render at scale 1, so the panel/text would be tiny on hidpi)
    float gUiScale = 1.f;

    void vkc(VkResult e) {
        e = gChaos->vulkan(e);

        if (e < 0) {
            fail(sv(StringBuilder() << "vulkan error "_sv << (i64)e));
        }
    }

    bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
        u32 count = 0;

        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        Vector<VkExtensionProperties> props;

        props.zero(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.mutData());

        bool offered = false;

        for (const VkExtensionProperties& prop : props) {
            if (StringView(prop.extensionName) == StringView(name)) {
                offered = true;

                break;
            }
        }

        return gChaos->deviceExtension(name, offered);
    }

    // a discrete GPU when there is one, the first device otherwise
    VkPhysicalDevice selectPhysicalDevice() {
        u32 count = 0;

        vkc(vkEnumeratePhysicalDevices(gInstance, &count, nullptr));

        if (!count) {
            fail("no vulkan device"_sv);
        }

        Vector<VkPhysicalDevice> devices;

        devices.zero(count);
        vkc(vkEnumeratePhysicalDevices(gInstance, &count, devices.mutData()));

        for (VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties props;

            vkGetPhysicalDeviceProperties(device, &props);

            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                return device;
            }
        }

        return devices[0];
    }

    u32 selectQueueFamily(VkPhysicalDevice device) {
        u32 count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

        Vector<VkQueueFamilyProperties> families;

        families.zero(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.mutData());

        for (u32 i = 0; i < count; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                return i;
            }
        }

        fail("no vulkan graphics queue"_sv);
    }

    void setupVulkan(ObjPool& shot, const char** exts, u32 nexts, const Image& img) {
        VkApplicationInfo app = {};

        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "im screenshot";
        app.apiVersion = VK_API_VERSION_1_2;

        Vector<const char*> instanceExts;

        for (u32 i = 0; i < nexts; i++) {
            instanceExts.pushBack(exts[i]);
        }

        if (img.color.hdr()) {
            instanceExts.pushBack(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }

        VkInstanceCreateInfo ci = {};

        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (u32)instanceExts.length();
        ci.ppEnabledExtensionNames = instanceExts.data();
        vkc(vkCreateInstance(&ci, gAlloc, &gInstance));
        pooledGuard(shot, [] {
            vkDestroyInstance(gInstance, gAlloc);
        });

        if (img.shared()) {
            u32 count = 0;

            vkEnumeratePhysicalDevices(gInstance, &count, nullptr);
            Vector<VkPhysicalDevice> devices;

            devices.zero(count);
            vkEnumeratePhysicalDevices(gInstance, &count, devices.mutData());

            // the buffer is only known to import on the GPU that exported
            // it; its deviceUUID names that GPU in any process, a software
            // device without a drm node included
            for (VkPhysicalDevice device : devices) {
                VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

                props.pNext = &ids;
                vkGetPhysicalDeviceProperties2(device, &props);

                if (memcmp(ids.deviceUUID, img.deviceUuid, VK_UUID_SIZE) == 0) {
                    gPhys = device;

                    break;
                }
            }

            if (!gPhys) {
                fail("shared screenshot gpu is unavailable"_sv);
            }
        } else {
            gPhys = selectPhysicalDevice();
        }

        gQueueFamily = selectQueueFamily(gPhys);

        const char* devExts[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        };
        u32 devExtCount = img.shared() ? 4 : 1;

        for (u32 i = 0; i < devExtCount; i++) {
            if (!hasDeviceExtension(gPhys, devExts[i])) {
                fail(sv(StringBuilder() << "vulkan lacks "_sv << StringView(devExts[i])));
            }
        }
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi = {};

        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = gQueueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;

        VkDeviceCreateInfo dci = {};

        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = devExtCount;
        dci.ppEnabledExtensionNames = devExts;
        vkc(vkCreateDevice(gPhys, &dci, gAlloc, &gDevice));
        pooledGuard(shot, [] {
            vkDestroyDevice(gDevice, gAlloc);
        });
        vkGetDeviceQueue(gDevice, gQueueFamily, 0, &gQueue);

        VkDescriptorPoolSize sz = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE};
        VkDescriptorPoolCreateInfo pi = {};

        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = sz.descriptorCount;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &sz;
        vkc(vkCreateDescriptorPool(gDevice, &pi, gAlloc, &gDescPool));
        pooledGuard(shot, [] {
            vkDestroyDescriptorPool(gDevice, gDescPool, gAlloc);
        });
    }

    // the first of the wanted formats the surface offers in the color space,
    // else whatever it offers first: the HDR caller checks the color space
    VkSurfaceFormatKHR selectSurfaceFormat(VkSurfaceKHR surface, const VkFormat* wanted, u32 nwanted, VkColorSpaceKHR colorSpace) {
        u32 count = 0;

        vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, surface, &count, nullptr));

        if (!count) {
            fail("vulkan WSI offers no surface format"_sv);
        }

        Vector<VkSurfaceFormatKHR> available;

        available.zero(count);
        vkc(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhys, surface, &count, available.mutData()));

        for (u32 i = 0; i < nwanted; i++) {
            for (const VkSurfaceFormatKHR& format : available) {
                if (format.format == wanted[i] && format.colorSpace == colorSpace) {
                    return format;
                }
            }
        }

        return available[0];
    }

    void createPresentPass() {
        VkAttachmentDescription attachment{};

        attachment.format = gPresent.format.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};

        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;

        // the acquire's semaphore is waited at color output: the pass's
        // write may not start before it
        VkSubpassDependency dependency{};

        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

        rpci.attachmentCount = 1;
        rpci.pAttachments = &attachment;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        rpci.dependencyCount = 1;
        rpci.pDependencies = &dependency;
        vkc(vkCreateRenderPass(gDevice, &rpci, gAlloc, &gPresent.renderPass));
    }

    void destroyFrames() {
        for (size_t i = 0; i < gPresent.frames.length(); i++) {
            const Frame& frame = gPresent.frames[i];

            if (frame.framebuffer) {
                vkDestroyFramebuffer(gDevice, frame.framebuffer, gAlloc);
            }
            if (frame.view) {
                vkDestroyImageView(gDevice, frame.view, gAlloc);
            }
            if (frame.fence) {
                vkDestroyFence(gDevice, frame.fence, gAlloc);
            }
            if (frame.commandPool) {
                vkDestroyCommandPool(gDevice, frame.commandPool, gAlloc);
            }
        }

        for (size_t i = 0; i < gPresent.syncs.length(); i++) {
            const Sync& sync = gPresent.syncs[i];

            if (sync.acquired) {
                vkDestroySemaphore(gDevice, sync.acquired, gAlloc);
            }
            if (sync.rendered) {
                vkDestroySemaphore(gDevice, sync.rendered, gAlloc);
            }
        }

        gPresent.frames.clear();
        gPresent.syncs.clear();
    }

    // the swapchain at the window's size; a swapchain already there is
    // retired by the new one, its frames torn down once the device is idle
    void createSwapchain(u32 width, u32 height) {
        VkSurfaceCapabilitiesKHR caps;

        vkc(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhys, gPresent.surface, &caps));

        // Wayland leaves the extent to the client: the window's size, held
        // to what the surface allows
        VkExtent2D extent = caps.currentExtent;

        if (extent.width == 0xffffffffu) {
            extent.width = width < caps.minImageExtent.width ? caps.minImageExtent.width : width > caps.maxImageExtent.width ? caps.maxImageExtent.width : width;
            extent.height = height < caps.minImageExtent.height ? caps.minImageExtent.height : height > caps.maxImageExtent.height ? caps.maxImageExtent.height : height;
        }

        u32 images = caps.minImageCount > kMinImageCount ? caps.minImageCount : kMinImageCount;

        if (caps.maxImageCount && images > caps.maxImageCount) {
            images = caps.maxImageCount;
        }

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};

        ci.surface = gPresent.surface;
        ci.minImageCount = images;
        ci.imageFormat = gPresent.format.format;
        ci.imageColorSpace = gPresent.format.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = gPresent.swapchain;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;

        // a driver that cannot present to this compositor's surface fails
        // here, and that is the tool's own report
        if (gChaos->vulkan(vkCreateSwapchainKHR(gDevice, &ci, gAlloc, &swapchain)) < 0) {
            fail("vulkan cannot make a swapchain on this surface"_sv);
        }

        if (gPresent.swapchain) {
            vkDeviceWaitIdle(gDevice);
            destroyFrames();
            vkDestroySwapchainKHR(gDevice, gPresent.swapchain, gAlloc);
        }

        gPresent.swapchain = swapchain;
        gPresent.width = (int)extent.width;
        gPresent.height = (int)extent.height;
        gPresent.frameIndex = 0;
        gPresent.syncIndex = 0;

        u32 count = 0;

        vkc(vkGetSwapchainImagesKHR(gDevice, swapchain, &count, nullptr));

        Vector<VkImage> handles;

        handles.zero(count);
        vkc(vkGetSwapchainImagesKHR(gDevice, swapchain, &count, handles.mutData()));
        gPresent.frames.zero(count);
        gPresent.syncs.zero(count + 1);

        for (u32 i = 0; i < count; i++) {
            Frame& frame = gPresent.frames.mut(i);

            frame.image = handles[i];

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

            vci.image = frame.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = gPresent.format.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkc(vkCreateImageView(gDevice, &vci, gAlloc, &frame.view));

            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

            fci.renderPass = gPresent.renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &frame.view;
            fci.width = extent.width;
            fci.height = extent.height;
            fci.layers = 1;
            vkc(vkCreateFramebuffer(gDevice, &fci, gAlloc, &frame.framebuffer));

            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

            pci.queueFamilyIndex = gQueueFamily;
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            vkc(vkCreateCommandPool(gDevice, &pci, gAlloc, &frame.commandPool));

            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

            cai.commandPool = frame.commandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkc(vkAllocateCommandBuffers(gDevice, &cai, &frame.commandBuffer));

            // signaled: the first frame has nothing to wait for
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkc(vkCreateFence(gDevice, &fenceInfo, gAlloc, &frame.fence));
        }

        for (u32 i = 0; i <= count; i++) {
            Sync& sync = gPresent.syncs.mut(i);
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

            vkc(vkCreateSemaphore(gDevice, &semaphoreInfo, gAlloc, &sync.acquired));
            vkc(vkCreateSemaphore(gDevice, &semaphoreInfo, gAlloc, &sync.rendered));
        }
    }

    void destroyPresenter() {
        destroyFrames();

        if (gPresent.swapchain) {
            vkDestroySwapchainKHR(gDevice, gPresent.swapchain, gAlloc);
        }
        if (gPresent.renderPass) {
            vkDestroyRenderPass(gDevice, gPresent.renderPass, gAlloc);
        }
        if (gPresent.surface) {
            vkDestroySurfaceKHR(gInstance, gPresent.surface, gAlloc);
        }

        gPresent.swapchain = VK_NULL_HANDLE;
        gPresent.renderPass = VK_NULL_HANDLE;
        gPresent.surface = VK_NULL_HANDLE;
    }

    void setupVulkanWindow(ObjPool& shot, VkSurfaceKHR surface, int w, int h, bool hdr) {
        // the surface is the presenter's from here: registered before
        // anything can throw, the guard tears down whatever was made
        gPresent.surface = surface;
        pooledGuard(shot, [] {
            destroyPresenter();
        });

        VkBool32 supported = VK_FALSE;

        vkc(vkGetPhysicalDeviceSurfaceSupportKHR(gPhys, gQueueFamily, surface, &supported));

        if (!supported) {
            fail("no vulkan WSI support"_sv);
        }

        const VkFormat hdrFmts[] = {
            VK_FORMAT_A2R10G10B10_UNORM_PACK32,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
        };
        const VkFormat sdrFmts[] = {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8_UNORM,
            VK_FORMAT_R8G8B8_UNORM,
        };
        const VkFormat* fmts = hdr ? hdrFmts : sdrFmts;
        VkColorSpaceKHR colorSpace = hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLORSPACE_SRGB_NONLINEAR_KHR;

        gPresent.format = selectSurfaceFormat(surface, fmts, 4, colorSpace);

        if (hdr && gPresent.format.colorSpace != colorSpace) {
            fail("vulkan WSI has no BT.2020/PQ surface"_sv);
        }

        createPresentPass();
        createSwapchain((u32)w, (u32)h);
    }

    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;

        vkGetPhysicalDeviceMemoryProperties(gPhys, &mp);
        gChaos->memoryTypes(mp);

        for (u32 i = 0; i < mp.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }

        fail("no vulkan memory type fits"_sv);
    }

    VkShaderModule shaderModule(const u32* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

        ci.codeSize = bytes;
        ci.pCode = code;

        VkShaderModule module = VK_NULL_HANDLE;

        vkc(vkCreateShaderModule(gDevice, &ci, gAlloc, &module));

        return module;
    }

    void destroySceneTarget() {
        if (gOutputSet) {
            vkFreeDescriptorSets(gDevice, gDescPool, 1, &gOutputSet);
            gOutputSet = VK_NULL_HANDLE;
        }
        if (gSceneFramebuffer) {
            vkDestroyFramebuffer(gDevice, gSceneFramebuffer, gAlloc);
        }
        if (gSceneView) {
            vkDestroyImageView(gDevice, gSceneView, gAlloc);
        }
        if (gSceneImage) {
            vkDestroyImage(gDevice, gSceneImage, gAlloc);
        }
        if (gSceneMemory) {
            vkFreeMemory(gDevice, gSceneMemory, gAlloc);
        }
        gSceneFramebuffer = VK_NULL_HANDLE;
        gSceneView = VK_NULL_HANDLE;
        gSceneImage = VK_NULL_HANDLE;
        gSceneMemory = VK_NULL_HANDLE;
    }

    void createSceneTarget(u32 width, u32 height) {
        destroySceneTarget();

        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = {width, height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkc(vkCreateImage(gDevice, &ici, gAlloc, &gSceneImage));

        VkMemoryRequirements req{};

        vkGetImageMemoryRequirements(gDevice, gSceneImage, &req);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &gSceneMemory));
        vkc(vkBindImageMemory(gDevice, gSceneImage, gSceneMemory, 0));

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

        vci.image = gSceneImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc(vkCreateImageView(gDevice, &vci, gAlloc, &gSceneView));

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};

        fci.renderPass = gScenePass;
        fci.attachmentCount = 1;
        fci.pAttachments = &gSceneView;
        fci.width = width;
        fci.height = height;
        fci.layers = 1;
        vkc(vkCreateFramebuffer(gDevice, &fci, gAlloc, &gSceneFramebuffer));

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

        ai.descriptorPool = gDescPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &gOutputSetLayout;
        vkc(vkAllocateDescriptorSets(gDevice, &ai, &gOutputSet));

        VkDescriptorImageInfo image{gSceneSampler, gSceneView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

        write.dstSet = gOutputSet;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(gDevice, 1, &write, 0, nullptr);
    }

    // a pipeline whose vertices come from gl_VertexIndex alone: no vertex
    // input, no blending, the viewport and scissor set at draw time
    VkPipeline vertexlessPipeline(const u32* vertCode, size_t vertBytes, const u32* fragCode, size_t fragBytes, VkPipelineLayout layout, VkRenderPass pass) {
        VkShaderModule vert = shaderModule(vertCode, vertBytes);
        VkShaderModule frag = shaderModule(fragCode, fragBytes);
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
        };
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState blendAttachment{};
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};

        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        viewport.viewportCount = viewport.scissorCount = 1;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.f;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};

        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vertex;
        gpci.pInputAssemblyState = &assembly;
        gpci.pViewportState = &viewport;
        gpci.pRasterizationState = &raster;
        gpci.pMultisampleState = &multisample;
        gpci.pColorBlendState = &blend;
        gpci.pDynamicState = &dynamic;
        gpci.layout = layout;
        gpci.renderPass = pass;

        VkPipeline pipeline = VK_NULL_HANDLE;

        vkc(vkCreateGraphicsPipelines(gDevice, VK_NULL_HANDLE, 1, &gpci, gAlloc, &pipeline));
        vkDestroyShaderModule(gDevice, frag, gAlloc);
        vkDestroyShaderModule(gDevice, vert, gAlloc);

        return pipeline;
    }

    void destroyLinearHdr();

    void setupLinearHdr(ObjPool& shot, u32 width, u32 height) {
        // registered before the first object: a throw part way through
        // tears down exactly the objects already made (the rest are null)
        pooledGuard(shot, [] {
            destroyLinearHdr();
            gLinearHdr = false;
        });

        VkAttachmentDescription attachment{};

        attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};

        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;

        VkSubpassDependency dependencies[2]{};

        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};

        rpci.attachmentCount = 1;
        rpci.pAttachments = &attachment;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        rpci.dependencyCount = 2;
        rpci.pDependencies = dependencies;
        vkc(vkCreateRenderPass(gDevice, &rpci, gAlloc, &gScenePass));

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkc(vkCreateSampler(gDevice, &sci, gAlloc, &gSceneSampler));

        VkDescriptorSetLayoutBinding binding{};

        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

        dlci.bindingCount = 1;
        dlci.pBindings = &binding;
        vkc(vkCreateDescriptorSetLayout(gDevice, &dlci, gAlloc, &gOutputSetLayout));

        // the scene holds SDR white at 1.0; the output stage scales it to
        // nits by this constant
        VkPushConstantRange outputRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

        plci.setLayoutCount = 1;
        plci.pSetLayouts = &gOutputSetLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &outputRange;
        vkc(vkCreatePipelineLayout(gDevice, &plci, gAlloc, &gOutputPipelineLayout));

        gOutputPipeline = vertexlessPipeline(fullscreen_vert_spv, sizeof(fullscreen_vert_spv), screenshot_output_frag_spv, sizeof(screenshot_output_frag_spv), gOutputPipelineLayout, gPresent.renderPass);

        VkDescriptorSetLayoutBinding imageBinding{};

        imageBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imageBinding.descriptorCount = 1;
        imageBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ilci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};

        ilci.bindingCount = 1;
        ilci.pBindings = &imageBinding;
        vkc(vkCreateDescriptorSetLayout(gDevice, &ilci, gAlloc, &gImageSetLayout));

        // one range for both stages, and the one push names both
        VkPushConstantRange imageRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ImagePush)};
        VkPipelineLayoutCreateInfo iplci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

        iplci.setLayoutCount = 1;
        iplci.pSetLayouts = &gImageSetLayout;
        iplci.pushConstantRangeCount = 1;
        iplci.pPushConstantRanges = &imageRange;
        vkc(vkCreatePipelineLayout(gDevice, &iplci, gAlloc, &gImagePipelineLayout));
        gImagePipeline = vertexlessPipeline(screenshot_image_vert_spv, sizeof(screenshot_image_vert_spv), screenshot_image_frag_spv, sizeof(screenshot_image_frag_spv), gImagePipelineLayout, gScenePass);

        createSceneTarget(width, height);
        gLinearHdr = true;
    }

    void destroyLinearHdr() {
        destroySceneTarget();
        if (gImagePipeline) {
            vkDestroyPipeline(gDevice, gImagePipeline, gAlloc);
        }
        if (gImagePipelineLayout) {
            vkDestroyPipelineLayout(gDevice, gImagePipelineLayout, gAlloc);
        }
        if (gImageSetLayout) {
            vkDestroyDescriptorSetLayout(gDevice, gImageSetLayout, gAlloc);
        }
        if (gOutputPipeline) {
            vkDestroyPipeline(gDevice, gOutputPipeline, gAlloc);
        }
        if (gOutputPipelineLayout) {
            vkDestroyPipelineLayout(gDevice, gOutputPipelineLayout, gAlloc);
        }
        if (gOutputSetLayout) {
            vkDestroyDescriptorSetLayout(gDevice, gOutputSetLayout, gAlloc);
        }
        if (gSceneSampler) {
            vkDestroySampler(gDevice, gSceneSampler, gAlloc);
        }
        if (gScenePass) {
            vkDestroyRenderPass(gDevice, gScenePass, gAlloc);
        }
    }

    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        // ImGui's descriptor, for AddImage; the image pipeline's own, in HDR
        VkDescriptorSet ds = VK_NULL_HANDLE;
        VkDescriptorSet imageSet = VK_NULL_HANDLE;
    };

    void finishTexture(const Image& img, Texture& tex) {
        VkImageViewCreateInfo vci = {};

        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = tex.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = img.shared() ? (VkFormat)img.format : VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkc(vkCreateImageView(gDevice, &vci, gAlloc, &tex.view));

        VkSamplerCreateInfo sci = {};

        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = -1000;
        sci.maxLod = 1000;
        vkc(vkCreateSampler(gDevice, &sci, gAlloc, &tex.sampler));

        tex.ds = ImGui_ImplVulkan_AddTexture(tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        if (gLinearHdr) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};

            ai.descriptorPool = gDescPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &gImageSetLayout;
            vkc(vkAllocateDescriptorSets(gDevice, &ai, &tex.imageSet));

            VkDescriptorImageInfo image{tex.sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};

            write.dstSet = tex.imageSet;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image;
            vkUpdateDescriptorSets(gDevice, 1, &write, 0, nullptr);
        }
    }

    void importTexture(Image& img, Texture& tex) {
        VkSubresourceLayout plane = {};

        plane.offset = img.offset;
        plane.rowPitch = img.stride;

        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {};

        modifier.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modifier.drmFormatModifier = img.modifier;
        modifier.drmFormatModifierPlaneCount = 1;
        modifier.pPlaneLayouts = &plane;

        VkExternalMemoryImageCreateInfo external = {};

        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.pNext = &modifier;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkImageCreateInfo ici = {};

        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &external;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = (VkFormat)img.format;
        ici.extent = {img.w, img.h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkc(vkCreateImage(gDevice, &ici, gAlloc, &tex.image));

        VkMemoryRequirements req = {};

        vkGetImageMemoryRequirements(gDevice, tex.image, &req);

        auto getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(gDevice, "vkGetMemoryFdPropertiesKHR");
        VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};

        if (!getFdProps || getFdProps(gDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, img.dmaFd, &fdProps) != VK_SUCCESS) {
            fail("cannot query shared screenshot memory"_sv);
        }

        u32 memoryTypes = req.memoryTypeBits & fdProps.memoryTypeBits;

        if (!memoryTypes) {
            fail("shared screenshot memory is incompatible"_sv);
        }

        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};

        dedicated.image = tex.image;

        VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};

        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import.fd = img.dmaFd;

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

        mai.pNext = &import;
        mai.allocationSize = img.allocationSize;
        mai.memoryTypeIndex = findMemoryType(memoryTypes, 0);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &tex.memory));
        img.dmaFd = -1;
        vkc(vkBindImageMemory(gDevice, tex.image, tex.memory, 0));

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

        pci.queueFamilyIndex = gQueueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkc(vkCreateCommandPool(gDevice, &pci, gAlloc, &pool));

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkc(vkAllocateCommandBuffers(gDevice, &cai, &cmd));

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkc(vkBeginCommandBuffer(cmd, &begin));

        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        barrier.dstQueueFamilyIndex = gQueueFamily;
        barrier.image = tex.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkc(vkEndCommandBuffer(cmd));

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkc(vkQueueSubmit(gQueue, 1, &submit, VK_NULL_HANDLE));
        vkc(vkQueueWaitIdle(gQueue));
        vkDestroyCommandPool(gDevice, pool, gAlloc);
        finishTexture(img, tex);
    }

    // upload the RGBA source image into a device-local sampled texture and
    // register it with imgui. one-shot: staging buffer, copy on a transient
    // command buffer, block once — refresh is never needed for a still frame
    void uploadTexture(const Image& img, Texture& tex) {
        VkDeviceSize bytes = (VkDeviceSize)img.w * img.h * 4;

        VkImageCreateInfo ici = {};

        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {img.w, img.h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkc(vkCreateImage(gDevice, &ici, gAlloc, &tex.image));

        VkMemoryRequirements req;

        vkGetImageMemoryRequirements(gDevice, tex.image, &req);

        VkMemoryAllocateInfo mai = {};

        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &tex.memory));
        vkBindImageMemory(gDevice, tex.image, tex.memory, 0);

        VkBuffer staging;
        VkDeviceMemory stagingMem;
        VkBufferCreateInfo bci = {};

        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkc(vkCreateBuffer(gDevice, &bci, gAlloc, &staging));
        vkGetBufferMemoryRequirements(gDevice, staging, &req);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &stagingMem));
        vkBindBufferMemory(gDevice, staging, stagingMem, 0);

        void* map = nullptr;

        vkMapMemory(gDevice, stagingMem, 0, bytes, 0, &map);
        memcpy(map, img.px, bytes);
        vkUnmapMemory(gDevice, stagingMem);

        VkCommandPool pool;
        VkCommandPoolCreateInfo pci = {};

        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = gQueueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(gDevice, &pci, gAlloc, &pool);

        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cbi = {};

        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        vkAllocateCommandBuffers(gDevice, &cbi, &cmd);

        VkCommandBufferBeginInfo begin = {};

        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkImageMemoryBarrier bar = {};

        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = tex.image;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy copy = {};

        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {img.w, img.h, 1};
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit = {};

        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(gQueue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(gQueue);

        vkDestroyCommandPool(gDevice, pool, gAlloc);
        vkDestroyBuffer(gDevice, staging, gAlloc);
        vkFreeMemory(gDevice, stagingMem, gAlloc);

        finishTexture(img, tex);
    }

    void readTexture(const Image& img, const Texture& tex, int x0, int y0, int x1, int y1, Image& out) {
        out.w = (u32)(x1 - x0);
        out.h = (u32)(y1 - y0);

        VkDeviceSize bytes = (VkDeviceSize)out.w * out.h * sizeof(u32);
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkc(vkCreateBuffer(gDevice, &bci, gAlloc, &buffer));

        VkMemoryRequirements req = {};

        vkGetBufferMemoryRequirements(gDevice, buffer, &req);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};

        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkc(vkAllocateMemory(gDevice, &mai, gAlloc, &memory));
        vkc(vkBindBufferMemory(gDevice, buffer, memory, 0));

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

        pci.queueFamilyIndex = gQueueFamily;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkc(vkCreateCommandPool(gDevice, &pci, gAlloc, &pool));

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};

        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkc(vkAllocateCommandBuffers(gDevice, &cai, &cmd));

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkc(vkBeginCommandBuffer(cmd, &begin));

        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy = {};

        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {x0, y0, 0};
        copy.imageExtent = {out.w, out.h, 1};
        vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkc(vkEndCommandBuffer(cmd));

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};

        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkc(vkQueueSubmit(gQueue, 1, &submit, VK_NULL_HANDLE));
        vkc(vkQueueWaitIdle(gQueue));

        void* map = nullptr;

        vkc(vkMapMemory(gDevice, memory, 0, bytes, 0, &map));
        out.file.zero((size_t)bytes);
        out.rgb16.zero((size_t)out.w * out.h * 3 * sizeof(u16));

        const u32* source = (const u32*)map;
        u32* dest = (u32*)out.file.mutData();
        u16* rgb16 = (u16*)out.rgb16.mutData();

        for (size_t i = 0; i < (size_t)out.w * out.h; i++) {
            u32 pixel = source[i];

            if ((VkFormat)img.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
                u32 r = (pixel >> 20) & 1023;
                u32 g = (pixel >> 10) & 1023;
                u32 b = pixel & 1023;

                dest[i] = unorm10To8(r) | (unorm10To8(g) << 8) | (unorm10To8(b) << 16) | 0xff000000u;
                rgb16[i * 3 + 0] = (u16)((r * 65535 + 511) / 1023);
                rgb16[i * 3 + 1] = (u16)((g * 65535 + 511) / 1023);
                rgb16[i * 3 + 2] = (u16)((b * 65535 + 511) / 1023);
            } else {
                u32 r = (pixel >> 16) & 0xff;
                u32 g = (pixel >> 8) & 0xff;
                u32 b = pixel & 0xff;

                dest[i] = r | (g << 8) | (b << 16) | 0xff000000u;
                rgb16[i * 3 + 0] = (u16)(r * 257);
                rgb16[i * 3 + 1] = (u16)(g * 257);
                rgb16[i * 3 + 2] = (u16)(b * 257);
            }
        }

        out.px = (const u8*)out.file.data();
        vkUnmapMemory(gDevice, memory);
        vkDestroyCommandPool(gDevice, pool, gAlloc);
        vkDestroyBuffer(gDevice, buffer, gAlloc);
        vkFreeMemory(gDevice, memory, gAlloc);
    }

    void encodeSelection(const Image& img, const Texture& tex, int x0, int y0, int x1, int y1, Buffer& png) {
        if (!img.shared()) {
            encodePng(img, x0, y0, x1, y1, png);

            return;
        }

        Image pixels;

        readTexture(img, tex, x0, y0, x1, y1, pixels);
        pixels.color = img.color;
        encodePng(pixels, 0, 0, (int)pixels.w, (int)pixels.h, png);
    }

    void frameRender(ImDrawData* draw) {
        Sync& sync = gPresent.syncs.mut(gPresent.syncIndex);
        VkResult e = gChaos->swapchain(vkAcquireNextImageKHR(gDevice, gPresent.swapchain, UINT64_MAX, sync.acquired, VK_NULL_HANDLE, &gPresent.frameIndex));

        if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
            gRebuild = true;
        }

        if (e == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }

        Frame& fd = gPresent.frames.mut(gPresent.frameIndex);

        vkWaitForFences(gDevice, 1, &fd.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(gDevice, 1, &fd.fence);
        vkResetCommandPool(gDevice, fd.commandPool, 0);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fd.commandBuffer, &bi);

        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};

        rp.renderArea.extent.width = (u32)gPresent.width;
        rp.renderArea.extent.height = (u32)gPresent.height;
        rp.clearValueCount = 1;
        rp.pClearValues = &gPresent.clear;

        if (gLinearHdr) {
            VkClearValue sceneClear{};

            rp.renderPass = gScenePass;
            rp.framebuffer = gSceneFramebuffer;
            rp.pClearValues = &sceneClear;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
            vkCmdEndRenderPass(fd.commandBuffer);

            float sdrWhiteNits = gSdrWhiteNits;

            rp.renderPass = gPresent.renderPass;
            rp.framebuffer = fd.framebuffer;
            rp.pClearValues = &gPresent.clear;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gOutputPipeline);
            vkCmdBindDescriptorSets(fd.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gOutputPipelineLayout, 0, 1, &gOutputSet, 0, nullptr);
            vkCmdPushConstants(fd.commandBuffer, gOutputPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sdrWhiteNits), &sdrWhiteNits);
            VkViewport viewport{0, 0, (float)gPresent.width, (float)gPresent.height, 0, 1};
            VkRect2D scissor{{0, 0}, {(u32)gPresent.width, (u32)gPresent.height}};

            vkCmdSetViewport(fd.commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(fd.commandBuffer, 0, 1, &scissor);
            vkCmdDraw(fd.commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(fd.commandBuffer);
        } else {
            rp.renderPass = gPresent.renderPass;
            rp.framebuffer = fd.framebuffer;
            vkCmdBeginRenderPass(fd.commandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(draw, fd.commandBuffer);
            vkCmdEndRenderPass(fd.commandBuffer);
        }

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};

        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sync.acquired;
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &fd.commandBuffer;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sync.rendered;
        vkEndCommandBuffer(fd.commandBuffer);
        vkQueueSubmit(gQueue, 1, &si, fd.fence);
    }

    void framePresent() {
        if (gRebuild) {
            return;
        }

        Sync& sync = gPresent.syncs.mut(gPresent.syncIndex);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};

        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sync.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &gPresent.swapchain;
        pi.pImageIndices = &gPresent.frameIndex;

        VkResult e = gChaos->swapchain(vkQueuePresentKHR(gQueue, &pi));

        if (e == VK_ERROR_OUT_OF_DATE_KHR || e == VK_SUBOPTIMAL_KHR) {
            gRebuild = true;
        }

        gPresent.syncIndex = (gPresent.syncIndex + 1) % (u32)gPresent.syncs.length();
    }

    // the screenshot's draw in the HDR scene, from ImGui's draw list: the
    // command's clip as the backend applies its own, then the image quad
    // by the image pipeline; ImDrawCallback_ResetRenderState follows it
    void drawImage(const ImDrawList*, const ImDrawCmd* cmd) {
        const ImageDraw& draw = *(const ImageDraw*)cmd->UserCallbackData;
        auto* state = (ImGui_ImplVulkan_RenderState*)ImGui::GetPlatformIO().Renderer_RenderState;
        ImDrawData* dd = ImGui::GetDrawData();
        float clipX0 = cmd->ClipRect.x - dd->DisplayPos.x;
        float clipY0 = cmd->ClipRect.y - dd->DisplayPos.y;
        float clipX1 = cmd->ClipRect.z - dd->DisplayPos.x;
        float clipY1 = cmd->ClipRect.w - dd->DisplayPos.y;

        clipX0 = clipX0 < 0.f ? 0.f : clipX0;
        clipY0 = clipY0 < 0.f ? 0.f : clipY0;

        if (clipX1 <= clipX0 || clipY1 <= clipY0) {
            return;
        }

        VkRect2D scissor{{(i32)clipX0, (i32)clipY0}, {(u32)(clipX1 - clipX0), (u32)(clipY1 - clipY0)}};

        vkCmdSetScissor(state->CommandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gImagePipeline);
        vkCmdBindDescriptorSets(state->CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gImagePipelineLayout, 0, 1, &draw.texture, 0, nullptr);

        ImagePush push;

        push.scale[0] = 2.f / dd->DisplaySize.x;
        push.scale[1] = 2.f / dd->DisplaySize.y;
        push.translate[0] = -1.f - dd->DisplayPos.x * push.scale[0];
        push.translate[1] = -1.f - dd->DisplayPos.y * push.scale[1];
        push.rect[0] = draw.x0;
        push.rect[1] = draw.y0;
        push.rect[2] = draw.x1;
        push.rect[3] = draw.y1;
        push.sdrWhiteNits = draw.sdrWhiteNits;
        vkCmdPushConstants(state->CommandBuffer, gImagePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(state->CommandBuffer, 6, 1, 0, 0);
    }

    // ---- crop interaction ----
    // selection in image px; empty (zero-area) means "no selection", which the
    // save/copy path treats as the whole frame. drawn by a left-drag, wiped by
    // a pan or a zoom change
    struct Crop {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool dragging = false;
        float dragOx = 0, dragOy = 0;

        bool empty() const {
            return x1 - x0 < 1 || y1 - y0 < 1;
        }

        void clear() {
            x0 = y0 = x1 = y1 = 0;
            dragging = false;
        }
    };

    // view state: the on-screen zoom (percent, view-only — save/copy always use
    // full-res image px) plus the current selection
    constexpr int kInitialZoom = 50;

    struct Viewer {
        int zoom = kInitialZoom;
        Crop crop;
    };

    float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    constexpr int kZoomMin = 10, kZoomMax = 400, kZoomStep = 10;

    // clamp to 90% of the output; zero screen dimensions mean the output
    // announcement has not arrived and the size stays as computed
    void clampWindowSize(const plt::WindowInfo& info, int& w, int& h) {
        if (info.screenPixelWidth == 0 || info.screenPixelHeight == 0) {
            return;
        }

        int maxW = (int)info.screenPixelWidth * 9 / 10;
        int maxH = (int)info.screenPixelHeight * 9 / 10;

        if (w > maxW) {
            w = maxW;
        }

        if (h > maxH) {
            h = maxH;
        }
    }

    void initialWindowSize(const Image& img, const ImGuiStyle& style, int& w, int& h) {
        float zoom = (float)kInitialZoom / 100.f;

        w = (int)ceilf(200.f * gUiScale + style.ItemSpacing.x + img.w * zoom);
        h = (int)ceilf(img.h * zoom);

        int minH = (int)(220.f * gUiScale);

        if (h < minH) {
            h = minH;
        }
    }

    // the test build says what the editor did with its input, so scenarios
    // wait for the editor itself instead of for pixels
    void traceView(StringView what, const Viewer& v) {
#ifdef IM_FOR_TESTS
        sysO << "im screenshot: "_sv << what << " zoom "_sv << v.zoom << endL;
#else
        (void)what;
        (void)v;
#endif
    }

    void traceSize(StringView what, int w, int h) {
#ifdef IM_FOR_TESTS
        sysO << "im screenshot: "_sv << what << " "_sv << w << "x"_sv << h << endL;
#else
        (void)what;
        (void)w;
        (void)h;
#endif
    }

    // nudge the zoom by delta% (clamped); any change drops the selection
    void applyZoom(Viewer& v, int delta) {
        int z = (int)clampf((float)(v.zoom + delta), (float)kZoomMin, (float)kZoomMax);

        if (z != v.zoom) {
            v.zoom = z;
            v.crop.clear();
            traceView("zoomed"_sv, v);
        }
    }

    void resetView(Viewer& v) {
        v.zoom = kInitialZoom;
        v.crop.clear();
        traceView("reset"_sv, v);
    }

    // left control panel: zoom on top, then Save/Reset in one row, then
    // the selection readout. writes the chosen action into result.
    void drawPanel(Viewer& v, int& result, bool& reset) {
        Crop& crop = v.crop;

        ImGui::TextUnformatted("Zoom (view only)");
        ImGui::SetNextItemWidth(-FLT_MIN);

        int z = v.zoom;

        if (ImGui::SliderInt("##zoom", &z, kZoomMin, kZoomMax, "%d%%")) {
            applyZoom(v, z - v.zoom); // clamps + drops the selection
        }

        ImGui::Spacing();

        // two equal buttons across the panel width
        float avail = ImGui::GetContentRegionAvail().x;
        float bw = (avail - ImGui::GetStyle().ItemSpacing.x) / 2.f;

        if (ImGui::Button("Save", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            result = 1;
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset", ImVec2(bw, 0))) {
            resetView(v);
            reset = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            result = -1;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (crop.empty()) {
            ImGui::TextDisabled("whole frame");
        } else {
            auto& text = sb();

            text << "selection "_sv << (i64)(crop.x1 - crop.x0 + 0.5f) << " x "_sv << (i64)(crop.y1 - crop.y0 + 0.5f);
            ImGui::TextUnformatted(text.cStr());
        }

        ImGui::TextDisabled("drag: select");
        ImGui::TextDisabled("middle-drag: pan");
        ImGui::TextDisabled("scroll / +-: zoom, 0: reset");
    }

    // right canvas: the image at v.zoom in a scrollable viewport. left-drag
    // draws the crop selection; middle-drag pans and wipes the selection.
    void drawCanvas(const Image& img, Texture& tex, Viewer& v, bool reset) {
        Crop& crop = v.crop;

        if (reset) {
            ImGui::SetScrollX(0.f);
            ImGui::SetScrollY(0.f);
        }

        float scale = (float)v.zoom / 100.f;
        ImVec2 content((float)img.w * scale, (float)img.h * scale);
        ImVec2 origin = ImGui::GetCursorScreenPos();

        // an invisible button both sizes the scroll region and captures the
        // left-drag for selection
        ImGui::InvisibleButton("img", content, ImGuiButtonFlags_MouseButtonLeft);

        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (img.color.hdr()) {
            // the screenshot draws itself, PQ decoded into the scene's
            // linear light by its own pipeline; ImGui's state comes back after
            ImageDraw draw{tex.imageSet, origin.x, origin.y, origin.x + content.x, origin.y + content.y, (float)img.color.sdrWhiteNits};

            dl->AddCallback(drawImage, &draw, sizeof(draw));
            dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        } else {
            dl->AddImage((ImTextureID)tex.ds, origin, ImVec2(origin.x + content.x, origin.y + content.y));
        }

        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto toScreen = [&](float px, float py) {
            return ImVec2(origin.x + px * scale, origin.y + py * scale);
        };
        auto toImg = [&](ImVec2 s) {
            return ImVec2(clampf((s.x - origin.x) / scale, 0.f, (float)img.w), clampf((s.y - origin.y) / scale, 0.f, (float)img.h));
        };

        // middle-drag pans the viewport and clears the selection
        if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;

            ImGui::SetScrollX(ImGui::GetScrollX() - d.x);
            ImGui::SetScrollY(ImGui::GetScrollY() - d.y);
            crop.clear();
            traceView("panned"_sv, v);
        }

        // wheel zooms (the child has NoScrollWithMouse, so the wheel is ours)
        float wheel = ImGui::GetIO().MouseWheel;

        if (wheel != 0.f && ImGui::IsWindowHovered()) {
            applyZoom(v, wheel > 0.f ? kZoomStep : -kZoomStep);
        }

        // left-drag on the image draws a fresh selection
        if (ImGui::IsItemActivated()) {
            ImVec2 p = toImg(mouse);

            crop.dragOx = p.x;
            crop.dragOy = p.y;
            crop.dragging = true;
        }

        if (crop.dragging) {
            ImVec2 p = toImg(mouse);

            crop.x0 = crop.dragOx < p.x ? crop.dragOx : p.x;
            crop.y0 = crop.dragOy < p.y ? crop.dragOy : p.y;
            crop.x1 = crop.dragOx > p.x ? crop.dragOx : p.x;
            crop.y1 = crop.dragOy > p.y ? crop.dragOy : p.y;

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                crop.dragging = false;
            }
        }

        // dim outside the selection, outline it (clipped to the child)
        if (!crop.empty()) {
            ImVec2 s0 = toScreen(crop.x0, crop.y0);
            ImVec2 s1 = toScreen(crop.x1, crop.y1);
            ImVec2 hi(origin.x + content.x, origin.y + content.y);
            ImU32 dim = IM_COL32(0, 0, 0, 140);

            dl->AddRectFilled(origin, ImVec2(hi.x, s0.y), dim);             // above
            dl->AddRectFilled(ImVec2(origin.x, s1.y), hi, dim);             // below
            dl->AddRectFilled(ImVec2(origin.x, s0.y), s0, dim);             // left
            dl->AddRectFilled(ImVec2(s1.x, s0.y), ImVec2(hi.x, s1.y), dim); // right
            dl->AddRect(s0, s1, IM_COL32(255, 255, 255, 230), 0, 0, 1.5f);
        }
    }

    // draw the whole cropper; returns 1 = save, -1 = cancel, 0 = keep going
    int drawUi(plt::Window& window, const Image& img, Texture& tex, Viewer& v) {
        ImGuiViewport* vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);

        int result = 0;
        bool reset = false;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        checkTrue(ImGui::Begin("##shot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings));
        const float panelW = 200.f * gUiScale;

        // +/- zoom, handled before the panel so the slider reflects it
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
            applyZoom(v, kZoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
            applyZoom(v, -kZoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            resetView(v);
            reset = true;
        }

        checkTrue(ImGui::BeginChild("panel", ImVec2(panelW, 0), ImGuiChildFlags_Borders));
        drawPanel(v, result, reset);

        ImGui::EndChild();
        ImGui::SameLine();

        checkTrue(ImGui::BeginChild("canvas", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
        drawCanvas(img, tex, v, reset);

        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleVar();

        if (reset) {
            int w, h;

            initialWindowSize(img, ImGui::GetStyle(), w, h);
            clampWindowSize(window.info(), w, h);
            // requestResize speaks pixels and converts to logical itself
            window.requestResize((u32)w, (u32)h);
        }

        return result;
    }

    // a full-window panel that replaces the cropper (not an overlay) when
    // something goes wrong — reads like a message from the compositor: a
    // heading, the error text, and a single Exit button. returns -1 on exit.
    int drawError(StringView msg) {
        ImGuiViewport* vp = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);

        int result = 0;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 28, 32, 255));

        checkTrue(ImGui::Begin("##err", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings));
        float pad = 24.f * gUiScale;

        ImGui::SetCursorPos(ImVec2(pad, pad));
        ImGui::BeginGroup();
        ImGui::TextDisabled("im screenshot");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(vp->Size.x - pad);
        ImGui::TextUnformatted((const char*)msg.data(), (const char*)msg.data() + msg.length());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Spacing();

        if (ImGui::Button("Exit", ImVec2(120.f * gUiScale, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            result = -1;
        }

        ImGui::EndGroup();

        ImGui::End();
        ImGui::PopStyleColor();

        return result;
    }

    // the vulkan/imgui frame driver behind plt's inverted loop: plt calls
    // frame() for every granted frame; it resizes the swapchain on demand,
    // draws the cropper or the error panel and re-requests the next frame.
    // A nonzero draw result (window close counts as exit) stops the
    // platform loop and lands in action.
    struct FrameDriver final: plt::FrameCallback, plt::WindowEvents {
        plt::Platform* platform = nullptr;
        plt::Window* window = nullptr;
        ImGuiPlt* imgui = nullptr;
        const Image* img = nullptr;
        Texture* tex = nullptr;
        Viewer* view = nullptr;
        // non-empty switches the ui to the error panel
        const Buffer* error = nullptr;
        int action = 0;

        bool frame(const plt::WindowInfo& info) override;
        void close() override;
    };

    bool FrameDriver::frame(const plt::WindowInfo& info) {
        int nw = (int)info.width;
        int nh = (int)info.height;

        if (nw <= 0 || nh <= 0) {
            window->requestFrame();

            return false;
        }

        if (gRebuild || gPresent.width != nw || gPresent.height != nh) {
            createSwapchain((u32)nw, (u32)nh);
            traceSize("presenting"_sv, gPresent.width, gPresent.height);

            if (gLinearHdr) {
                createSceneTarget((u32)nw, (u32)nh);
            }

            gRebuild = false;
        }

        ImGui_ImplVulkan_NewFrame();
        imgui->newFrame(*window);
        ImGui::NewFrame();

        int result = error->empty() ? drawUi(*window, *img, *tex, *view) : drawError(sv(*error));

        ImGui::Render();

        // the draw data is the window's size, checked positive above
        ImDrawData* dd = ImGui::GetDrawData();

        gPresent.clear.color.float32[0] = 0.1f;
        gPresent.clear.color.float32[1] = 0.1f;
        gPresent.clear.color.float32[2] = 0.1f;
        gPresent.clear.color.float32[3] = 1.0f;

        frameRender(dd);
        framePresent();

        if (result != 0) {
            action = result;
            platform->stop();
        } else {
            // imgui animates every frame while interactive; plt paces this
            // through the compositor's frame callbacks
            window->requestFrame();
        }

        // a swapchain-rebuild frame presented nothing: returning false makes
        // plt retry instead of waiting on a frame callback that never comes
        return !gRebuild;
    }

    void FrameDriver::close() {
        action = -1;
        platform->stop();
    }

    // show the window (idempotent) and run the platform loop until a draw
    // verdict or the close button stops it
    int runUi(FrameDriver& driver) {
        driver.action = 0;
        driver.window->requestShow();
        driver.window->requestFrame();
        driver.platform->run();

        return driver.action ? driver.action : -1;
    }

    // the crop rect in image px, with the empty-selection-is-whole-frame rule
    void cropRegion(const Image& img, const Crop& c, int& x0, int& y0, int& x1, int& y1) {
        x0 = (int)(clampf(c.x0, 0, (float)img.w) + 0.5f);
        y0 = (int)(clampf(c.y0, 0, (float)img.h) + 0.5f);
        x1 = (int)(clampf(c.x1, 0, (float)img.w) + 0.5f);
        y1 = (int)(clampf(c.y1, 0, (float)img.h) + 0.5f);

        if (x1 - x0 < 1 || y1 - y0 < 1) {
            x0 = 0;
            y0 = 0;
            x1 = (int)img.w;
            y1 = (int)img.h;
        }
    }
}

int mainScreenshot(StringView path) {
    if (const char* s = getenv("IMGUI_SCALE")) {
        double v = parseFloat(StringView(s));

        if (v > 0.0) {
            gUiScale = (float)v;
        }
    }

    // load first; any failure becomes an on-screen error panel, not a console
    // line, so it reads like a message from the compositor
    Image img;
    Buffer errText;
    bool loaded = false;

    try {
        loadImage(path, img);
        loaded = true;
    } catch (ShotError& e) {
        errText = Buffer(e.description());
    } catch (...) {
        errText = Buffer(Exception::current());
    }

    // Prepare the exact style that drawUi will use before sizing the native
    // window. drawUi holds WindowPadding at zero while creating both children,
    // so only SameLine's ItemSpacing separates the panel and image viewport.
    ImGuiStyle uiStyle;

    if (gUiScale != 1.f) {
        uiStyle.FontScaleMain = gUiScale;
        uiStyle.ScaleAllSizes(gUiScale);
    }

    int rc = 0;

    try {
        // pooled unwind: every stage registers its teardown right after it
        // succeeds, so the arena's LIFO death replays the epilogue in order
        // and an exception mid-setup unwinds exactly the completed stages.
        // tex outlives the pool: its guard reads it at pool-death time;
        // driver outlives it too: the dying window still points at it
        Texture tex;
        FrameDriver driver;
        ObjPool::Ref shot = ObjPool::fromMemory();

        gChaos = ChaosMonkey::create(*shot);

        // the platform, the input bridge and the window live in the same
        // arena: LIFO death tears the window down after every vulkan guard
        // below and before the platform it belongs to
        plt::Platform& platform = *plt::Platform::create(*shot);
        ImGuiPlt& imgui = *ImGuiPlt::create(*shot);

        // Open at the image's on-screen size (50% zoom) plus the actual
        // ImGui chrome. A bare error panel gets a small fixed size. Clamp
        // to 90% of the output.
        int winW, winH;

        if (loaded) {
            initialWindowSize(img, uiStyle, winW, winH);
        } else {
            winW = (int)(480.f * gUiScale);
            winH = (int)(180.f * gUiScale);
        }

        plt::WindowOptions options;

        options.appId = "im-screenshot"_sv;
        options.title = "im screenshot"_sv;
        options.width = (u32)winW;
        options.height = (u32)winH;
        options.input = imgui.sink();
        options.events = &driver;
        options.frame = &driver;

        plt::Window& window = *platform.createWindow(*shot, options);

        // the output size arrived with the platform's registry roundtrips;
        // only a window can report it, so the clamp lands as a resize
        int clampedW = winW, clampedH = winH;

        clampWindowSize(window.info(), clampedW, clampedH);

        if (clampedW != winW || clampedH != winH) {
            window.requestResize((u32)clampedW, (u32)clampedH);
        }

        const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};

        setupVulkan(*shot, exts, 2, img);

        plt::RenderContext render = window.renderContext();
        VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};

        sci.display = (wl_display*)render.connection;
        sci.surface = (wl_surface*)render.window;

        VkSurfaceKHR surface;

        vkc(vkCreateWaylandSurfaceKHR(gInstance, &sci, gAlloc, &surface));

        plt::WindowInfo bootInfo = window.info();
        int fbw = (int)bootInfo.width;
        int fbh = (int)bootInfo.height;

        setupVulkanWindow(*shot, surface, fbw, fbh, loaded && img.color.hdr());

        if (loaded && img.color.hdr()) {
            setupLinearHdr(*shot, (u32)fbw, (u32)fbh);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        pooledGuard(*shot, [] {
            ImGui::DestroyContext();
        });
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetStyle() = uiStyle;

        ImGui_ImplVulkan_InitInfo ii = {};

        ii.Instance = gInstance;
        ii.PhysicalDevice = gPhys;
        ii.Device = gDevice;
        ii.QueueFamily = gQueueFamily;
        ii.Queue = gQueue;
        ii.DescriptorPool = gDescPool;
        ii.MinImageCount = kMinImageCount;
        ii.ImageCount = (u32)gPresent.frames.length();
        ii.PipelineInfoMain.RenderPass = gLinearHdr ? gScenePass : gPresent.renderPass;
        ii.PipelineInfoMain.Subpass = 0;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

        if (loaded && img.color.hdr()) {
            ii.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            ii.CustomShaderFragCreateInfo.codeSize = sizeof(screenshot_scene_frag_spv);
            ii.CustomShaderFragCreateInfo.pCode = screenshot_scene_frag_spv;
        }

        ImGui_ImplVulkan_Init(&ii);
        pooledGuard(*shot, [] {
            ImGui_ImplVulkan_Shutdown();
        });
        gSdrWhiteNits = (float)img.color.sdrWhiteNits;

        if (loaded) {
            // registered before the import so a mid-import throw still
            // releases the partially built handles
            pooledGuard(*shot, [t = &tex] {
                if (t->ds) {
                    ImGui_ImplVulkan_RemoveTexture(t->ds);
                }
                if (t->imageSet) {
                    vkFreeDescriptorSets(gDevice, gDescPool, 1, &t->imageSet);
                }
                if (t->sampler) {
                    vkDestroySampler(gDevice, t->sampler, gAlloc);
                }
                if (t->view) {
                    vkDestroyImageView(gDevice, t->view, gAlloc);
                }
                if (t->image) {
                    vkDestroyImage(gDevice, t->image, gAlloc);
                }
                if (t->memory) {
                    vkFreeMemory(gDevice, t->memory, gAlloc);
                }
            });

            if (img.shared()) {
                importTexture(img, tex);
            } else {
                uploadTexture(img, tex);
            }
        }

        // last in, first out: the queue drains before anything above dies
        pooledGuard(*shot, [] {
            vkDeviceWaitIdle(gDevice);
        });

        Viewer view; // zoom 50%, no selection (whole frame) until the user drags

        driver.platform = &platform;
        driver.window = &window;
        driver.imgui = &imgui;
        driver.img = &img;
        driver.tex = &tex;
        driver.view = &view;
        driver.error = &errText;

        // interactive phase: the cropper, or the error panel if the load failed
        int action = 0;
        StringView configuredAction(getenv("IMWAY_SHOT_ACTION") ? getenv("IMWAY_SHOT_ACTION") : "editor");

        // errText is still empty here whenever the load succeeded: only the
        // load's own failure has written it yet
        if (loaded && configuredAction == "save"_sv) {
            // non-interactive: encode straight from the texture, the window
            // never maps. "copy" lands in the editor below until the
            // clipboard path returns.
            action = 1;
        } else {
            action = runUi(driver);
        }

        // action phase: encode + save; a failure switches to the error panel
        if (loaded && action == 1) {
            try {
                int x0, y0, x1, y1;

                cropRegion(img, view.crop, x0, y0, x1, y1);

                Buffer encoded;
                bool png = getenv("IMWAY_SHOT_FORMAT") && StringView(getenv("IMWAY_SHOT_FORMAT")) == "png"_sv;

                if (png) {
                    encodeSelection(img, tex, x0, y0, x1, y1, encoded);
                } else {
                    encodeJxlSelection(img, tex, x0, y0, x1, y1, encoded);
                }

                Buffer dest = destPath();

                saveFile(encoded, sv(dest));
                sysO << "im screenshot: saved "_sv << sv(dest) << endL;
            } catch (ShotError& e) {
                errText = Buffer(e.description());
            } catch (...) {
                errText = Buffer(Exception::current());
            }

            if (!errText.empty()) {
                // a save-mode window was never shown; runUi maps it now
                runUi(driver);
            }
        }

        // teardown happens here: the pool ref dies at the end of the block
        // and its guards unwind the whole stack in reverse creation order
    } catch (...) {
        // vulkan/imgui setup blew up — nothing to show it on; the pool has
        // already unwound the stages that did come up; log and leave
        sysE << "im screenshot: "_sv << Exception::current() << endL;
        rc = 1;
    }

    if (img.dmaFd >= 0) {
        close(img.dmaFd);
    }

    return rc;
}
