#include "view.h"

#include "ui.h"
#include "gpu.h"
#include "util.h"
#include "pooled.h"
#include "decoder.h"
#include "imgui_plt.h"
#include "chaos_monkey.h"

#include <std/sys/fs.h>
#include <std/ios/sys.h>
#include <std/thr/pool.h>
#include <std/alg/qsort.h>
#include <std/sys/throw.h>
#include <std/thr/guard.h>
#include <std/thr/mutex.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/ios/fs_utils.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <time.h>
#include <errno.h>
#include <imgui.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <plt/poller.h>
#include <plt/window.h>
#include <plt/platform.h>
#include <plt/loop_wake.h>

using namespace stl;

// im view <file|dir>...: the list on the left, the image in the middle,
// the file's properties on the right. Decoding runs on a thread pool, a
// fresh sandboxed ImageMagick for every file; the results come back to
// the platform thread through its loop's doorbell, where the textures are
// made. Thumbnails are decoded for the rows in view and a few beyond (and
// dropped again when many have scrolled by), the shown image and its two
// neighbours are kept decoded.

namespace {
    // each side panel takes this share of the window's width
    constexpr float sideShare = .2f;
    // the gutter around thumbnails, a design length
    constexpr Design gap = 4_d;
    // a thumbnail fills the list's width; until it is decoded its row is
    // this tall for its width, a photo's proportion
    constexpr float placeholderAspect = .75f;
    // a thumbnail's long side in texels follows the list's width, in steps
    constexpr u32 thumbTexelsStep = 64;
    constexpr u32 thumbTexelsMin = 128;
    constexpr u32 thumbTexelsMax = 512;
    // decoded thumbnails kept at most; the rows in view are re-requested
    constexpr size_t maxThumbs = 256;
    // the rows around the ones in view whose thumbnails are still wanted
    constexpr size_t thumbMargin = 8;
    // the rows beyond the ones in view whose thumbnails are decoded ahead
    constexpr size_t thumbAhead = 2;
    constexpr float zoomMin = 0.02f;
    constexpr float zoomMax = 32.f;
    constexpr float zoomStep = 1.25f;

    enum class Load : u8 {
        None,
        Pending,
        Ready,
        Failed
    };

    // one file of the list, with its thumbnail
    struct Entry {
        Buffer path;
        size_t nameAt = 0;
        Load thumb = Load::None;
        Texture thumbTex;
        u32 thumbW = 0;
        u32 thumbH = 0;
        // the long side the thumbnail was asked at: a list grown much
        // wider since asks again
        u32 thumbSide = 0;
        // the frame the thumbnail was last drawn in, for the eviction
        u64 drawnAt = 0;
        // the full image: decoded and cached, or failed with this reason
        Load full = Load::None;
        Buffer error;

        StringView name() const;
    };

    StringView Entry::name() const {
        StringView whole = sv(path);

        return StringView(whole.begin() + nameAt, whole.end());
    }

    // a decode handed to a worker and back: the file to read, the answer
    struct Job {
        size_t index = 0;
        bool thumbnail = false;
        // a thumbnail's long side in texels
        u32 side = 0;
        Buffer path;
        Buffer name;
        DecodedImage image;
        Buffer error;
        bool ok = false;
        // a worker found the job no longer wanted and left it undone
        bool skipped = false;
    };

    // what the platform thread and the workers share, under the mutex
    struct Shared {
        Mutex* mutex = nullptr;
        plt::LoopWake* bell = nullptr;
        Vector<Job*> done;
        // the rows in view: a thumbnail outside them is not worth decoding
        size_t wantFirst = 0;
        size_t wantLast = 0;
        // the shown entry: a full decode of another than its neighbours is stale
        size_t current = 0;
        bool quit = false;
        // what a Vulkan image may measure on this device
        u32 maxSide = 4096;
    };

    // area-averaged to fit the side: what a thumbnail is, and what an image
    // wider than the device's images becomes
    void shrinkToSide(DecodedImage& img, u32 side) {
        if (img.width <= side && img.height <= side) {
            return;
        }

        u32 dw = img.width >= img.height ? side : (u32)max<u64>(1, (u64)side * img.width / img.height);
        u32 dh = img.height >= img.width ? side : (u32)max<u64>(1, (u64)side * img.height / img.width);
        Buffer out;

        out.zero((size_t)dw * dh * 4);

        const unsigned char* src = (const unsigned char*)img.rgba.data();
        unsigned char* dst = (unsigned char*)out.mutData();

        for (u32 dy = 0; dy < dh; dy++) {
            u32 y0 = (u32)((u64)dy * img.height / dh);
            u32 y1 = (u32)max<u64>(y0 + 1, (u64)(dy + 1) * img.height / dh);

            for (u32 dx = 0; dx < dw; dx++) {
                u32 x0 = (u32)((u64)dx * img.width / dw);
                u32 x1 = (u32)max<u64>(x0 + 1, (u64)(dx + 1) * img.width / dw);
                u64 sum[4] = {};

                for (u32 y = y0; y < y1; y++) {
                    const unsigned char* row = src + ((size_t)y * img.width + x0) * 4;

                    for (u32 x = x0; x < x1; x++) {
                        sum[0] += row[0];
                        sum[1] += row[1];
                        sum[2] += row[2];
                        sum[3] += row[3];
                        row += 4;
                    }
                }

                u64 count = (u64)(x1 - x0) * (y1 - y0);
                unsigned char* px = dst + ((size_t)dy * dw + dx) * 4;

                px[0] = (unsigned char)((sum[0] + count / 2) / count);
                px[1] = (unsigned char)((sum[1] + count / 2) / count);
                px[2] = (unsigned char)((sum[2] + count / 2) / count);
                px[3] = (unsigned char)((sum[3] + count / 2) / count);
            }
        }

        img.width = dw;
        img.height = dh;
        img.rgba.xchg(out);
    }

    // a worker's whole job: read, decode, shrink, hand back. The decoder
    // and its pool live for the one file: a failure spends the decoder,
    // and a module grown to a large image's pixel cache never shrinks
    void runJob(Shared& shared, Job& job) {
        {
            LockGuard lock(shared.mutex);

            if (shared.quit) {
                job.skipped = true;
            } else if (job.thumbnail) {
                job.skipped = job.index + thumbMargin < shared.wantFirst || job.index > shared.wantLast + thumbMargin;
            } else {
                job.skipped = job.index + 1 < shared.current || job.index > shared.current + 1;
            }
        }

        if (!job.skipped) {
            try {
                Buffer file;

                readFileContent(job.path, file);

                ObjPool::Ref pool = ObjPool::fromMemory();

                Decoder::create(*pool)->decode(sv(file), sv(job.name), job.image);
                shrinkToSide(job.image, job.thumbnail ? job.side : shared.maxSide);
                job.ok = true;
            } catch (...) {
                job.error = Buffer(Exception::current());
                job.ok = false;
            }
        }

        LockGuard lock(shared.mutex);

        shared.done.pushBack(&job);
        shared.bell->signal();
    }

    bool imageName(StringView name) {
        static const char* const extensions[] = {
            "png",
            "jpg",
            "jpeg",
            "jpe",
            "webp",
            "tif",
            "tiff",
            "jp2",
            "j2k",
            "jxl",
            "gif",
            "bmp",
            "pnm",
            "ppm",
            "pgm",
            "pbm",
            "pam",
            "tga",
            "pcx",
            "sgi",
            "miff",
        };
        size_t dot = name.length();

        while (dot > 0 && name[dot - 1] != '.') {
            dot--;
        }

        if (dot == 0) {
            return false;
        }

        StringView ext(name.begin() + dot, name.end());

        for (const char* candidate : extensions) {
            StringView want(candidate);

            if (want.length() != ext.length()) {
                continue;
            }

            bool same = true;

            for (size_t i = 0; i < ext.length() && same; i++) {
                same = ((u8)ext[i] | 0x20) == (u8)want[i];
            }

            if (same) {
                return true;
            }
        }

        return false;
    }

    // the entries live in the pool: the list holds them by pointer, as a
    // Vector holds only what needs no destructor
    Entry* makeEntry(ObjPool& pool, StringView path) {
        Entry* entry = pool.make<Entry>();
        size_t slash = path.length();

        while (slash > 0 && path[slash - 1] != '/') {
            slash--;
        }

        entry->path = Buffer(path);
        entry->nameAt = slash;

        return entry;
    }

    void addFile(ObjPool& pool, Vector<Entry*>& entries, StringView path) {
        entries.pushBack(makeEntry(pool, path));
    }

    // the directory's images, by name; throws where it cannot be read
    void addDirectory(ObjPool& pool, Vector<Entry*>& entries, StringView dir) {
        Vector<Entry*> found;

        listDir(dir, [&](const TPathInfo& info) {
            if (!info.isDir && imageName(info.item)) {
                StringBuilder path;

                path << dir;

                if (!dir.endsWith("/"_sv)) {
                    path << "/"_sv;
                }

                path << info.item;
                found.pushBack(makeEntry(pool, sv(path)));
            }
        });
        quickSort(found.mutBegin(), found.mutEnd(), [](const Entry* a, const Entry* b) {
            return a->name() < b->name();
        });
        entries.append(found.begin(), found.end());
    }

    // the viewer: its list, the shown image, the view on it, and the
    // exchange with the workers
    // a thumbnail's long side in texels for a list this wide, in steps,
    // so a list a few px wider asks nothing new
    u32 thumbSideFor(float innerW) {
        u32 side = (u32)ceilf(innerW / (float)thumbTexelsStep) * thumbTexelsStep;

        return side < thumbTexelsMin ? thumbTexelsMin : side > thumbTexelsMax ? thumbTexelsMax : side;
    }

    // a row's height for the list's width: the thumbnail's proportion, or a
    // photo's until it is decoded; whole px, so rows do not blur
    float rowHeightFor(const Entry& entry, float innerW) {
        float aspect = entry.thumb == Load::Ready && entry.thumbW ? (float)entry.thumbH / (float)entry.thumbW : placeholderAspect;

        return max(1.f, floorf(innerW * aspect + .5f));
    }

    // "1.5 MB", or the bytes themselves under a KB
    void appendBytes(StringBuilder& text, i64 bytes) {
        static const StringView units[] = {"KB"_sv, "MB"_sv, "GB"_sv, "TB"_sv};

        if (bytes < 1024) {
            text << bytes << " bytes"_sv;

            return;
        }

        i64 tenths = bytes * 10 / 1024;
        size_t unit = 0;

        while (tenths >= 10240 && unit + 1 < sizeof(units) / sizeof(units[0])) {
            tenths /= 1024;
            unit++;
        }

        text << tenths / 10 << "."_sv << tenths % 10 << " "_sv << units[unit];
    }

    struct ViewApp final: Ui, plt::TimerCallback {
        ObjPool* pool = nullptr;
        plt::Window* window = nullptr;
        ThreadPool* threads = nullptr;
        Shared shared;
        Vector<Entry*> entries;
        // jobs done with, for the next request
        Vector<Job*> spare;
        // decoded full images: the shown one and its neighbours
        Vector<Job*> cache;
        size_t current = 0;
        // the shown image's texture, or why there is none
        Load shown = Load::None;
        size_t shownIndex = (size_t)-1;
        Texture tex;
        u32 texW = 0;
        u32 texH = 0;
        Buffer shownError;
        // the view on it
        float zoom = 1.f;
        bool fit = true;
        float panX = 0.f;
        float panY = 0.f;
        int rotation = 0;
        bool fullscreen = false;
        bool panel = true;
        bool info = true;
        bool scrollToCurrent = true;
        u64 frames = 0;
        size_t readyThumbs = 0;
        int result = 0;
        // the shown file's own facts, for the properties panel: its size
        // (-1 when unknown) and modification time
        i64 fileBytes = -1;
        Buffer fileModified;

        int frame() override;
        void ready() override;

        Job* takeJob();
        void recycle(Job* job);
        void submit(Job* job);
        void requestThumb(size_t index, u32 side);
        void requestFull(size_t index);
        void apply(Job& job);
        void adopt(Job& job);
        void show(size_t index);
        void step(long delta);
        void retire(Texture& texture);
        void stopWorkers();
        void evictThumbs();
        void evictCache();
        void setZoom(float value);
        void fitView();
        void statFile();
        void keys();
        void drawGallery();
        void drawCanvas();
        void drawInfo();
    };

    Job* ViewApp::takeJob() {
        if (!spare.empty()) {
            return spare.popBack();
        }

        return pool->make<Job>();
    }

    void ViewApp::recycle(Job* job) {
        Job fresh;

        // the buffers go back to nothing; the job's memory stays for the next
        job->image.rgba.xchg(fresh.image.rgba);
        job->image.width = 0;
        job->image.height = 0;
        job->error.xchg(fresh.error);
        job->path.xchg(fresh.path);
        job->name.xchg(fresh.name);
        job->ok = false;
        job->skipped = false;
        spare.pushBack(job);
    }

    void ViewApp::submit(Job* job) {
        const Entry& entry = *entries[job->index];

        job->path = Buffer(sv(entry.path));
        job->name = Buffer(entry.name());

        Shared* exchange = &shared;

        threads->submit([job, exchange] {
            runJob(*exchange, *job);
        });
    }

    void ViewApp::requestThumb(size_t index, u32 side) {
        Entry& entry = *entries[index];

        entry.thumb = Load::Pending;
        entry.thumbSide = side;

        Job* job = takeJob();

        job->index = index;
        job->thumbnail = true;
        job->side = side;
        submit(job);
    }

    void ViewApp::requestFull(size_t index) {
        Entry& entry = *entries[index];

        if (entry.full != Load::None) {
            return;
        }

        entry.full = Load::Pending;

        Job* job = takeJob();

        job->index = index;
        job->thumbnail = false;
        submit(job);
    }

    // the workers end before the jobs they write into: a job still running
    // when the pool dies would write into a dead one. Once: the guard that
    // covers a setup that threw finds nothing to do after the normal end
    void ViewApp::stopWorkers() {
        if (!threads) {
            return;
        }

        {
            LockGuard lock(shared.mutex);

            shared.quit = true;
        }

        threads->join();
        threads = nullptr;
    }

    // a texture the frames may still read goes only once the device is done
    void ViewApp::retire(Texture& texture) {
        if (texture.image) {
            vkDeviceWaitIdle(gDevice);
            destroyTexture(texture);
        }
    }

    // the shown image from a decoded job
    void ViewApp::adopt(Job& job) {
        retire(tex);
        uploadTexture(job.image.width, job.image.height, (const u8*)job.image.rgba.data(), tex);
        texW = job.image.width;
        texH = job.image.height;
        shown = Load::Ready;
        shownIndex = job.index;
        traceText(sv(StringBuilder() << "showing "_sv << entries[job.index]->name() << " "_sv << (i64)texW << "x"_sv << (i64)texH));
    }

    void ViewApp::apply(Job& job) {
        Entry& entry = *entries[job.index];

        if (job.skipped) {
            // asked again when it matters
            if (job.thumbnail) {
                entry.thumb = Load::None;
            } else {
                entry.full = Load::None;
            }

            recycle(&job);

            return;
        }

        if (job.thumbnail) {
            if (job.ok) {
                retire(entry.thumbTex);
                uploadTexture(job.image.width, job.image.height, (const u8*)job.image.rgba.data(), entry.thumbTex);
                entry.thumbW = job.image.width;
                entry.thumbH = job.image.height;
                entry.thumb = Load::Ready;
                readyThumbs++;
                traceText(sv(StringBuilder() << "thumbnail "_sv << entry.name()));
            } else {
                entry.thumb = Load::Failed;
                traceText(sv(StringBuilder() << "no thumbnail "_sv << entry.name() << ": "_sv << sv(job.error)));
            }

            recycle(&job);

            return;
        }

        if (!job.ok) {
            entry.full = Load::Failed;
            entry.error = Buffer(sv(job.error));

            if (job.index == current) {
                shown = Load::Failed;
                shownIndex = job.index;
                shownError = Buffer(sv(job.error));
                traceText(sv(StringBuilder() << "cannot show "_sv << entry.name() << ": "_sv << sv(job.error)));
            }

            recycle(&job);

            return;
        }

        entry.full = Load::Ready;
        cache.pushBack(&job);

        if (job.index == current) {
            adopt(job);
        }
    }

    // the workers' answers, on the platform thread
    void ViewApp::ready() {
        Vector<Job*> arrived;

        {
            LockGuard lock(shared.mutex);

            arrived.xchg(shared.done);
        }

        for (Job* job : arrived) {
            apply(*job);
        }

        evictCache();
        window->requestFrame();
    }

    // the cache keeps the shown image and its neighbours
    void ViewApp::evictCache() {
        for (size_t i = 0; i < cache.length();) {
            Job* job = cache[i];

            if (job->index + 1 < current || job->index > current + 1) {
                entries[job->index]->full = Load::None;
                recycle(job);
                cache.mut(i) = cache.back();
                cache.popBack();
            } else {
                i++;
            }
        }
    }

    // thumbnails drawn a while ago go when there are many
    void ViewApp::evictThumbs() {
        if (readyThumbs <= maxThumbs) {
            return;
        }

        size_t before = readyThumbs;

        for (size_t i = 0; i < entries.length() && readyThumbs > maxThumbs / 2; i++) {
            Entry& entry = *entries[i];

            if (entry.thumb == Load::Ready && entry.drawnAt + 1 < frames) {
                retire(entry.thumbTex);
                entry.thumb = Load::None;
                readyThumbs--;
            }
        }

        traceText(sv(StringBuilder() << "evicted "_sv << (i64)(before - readyThumbs) << " thumbnails"_sv));
    }

    void ViewApp::show(size_t index) {
        current = index;
        scrollToCurrent = true;

        {
            LockGuard lock(shared.mutex);

            shared.current = index;
        }

        Entry& entry = *entries[index];

        traceText(sv(StringBuilder() << "selected "_sv << entry.name()));
        statFile();

        if (entry.full == Load::Ready) {
            for (Job* job : cache) {
                if (job->index == index) {
                    adopt(*job);
                }
            }
        } else if (entry.full == Load::Failed) {
            // decoded ahead, as a neighbour, and failed then
            shown = Load::Failed;
            shownIndex = index;
            shownError = Buffer(sv(entry.error));
            traceText(sv(StringBuilder() << "cannot show "_sv << entry.name() << ": "_sv << sv(entry.error)));
        } else {
            shown = Load::Pending;
            shownIndex = index;
            requestFull(index);
        }

        if (index + 1 < entries.length()) {
            requestFull(index + 1);
        }

        if (index > 0) {
            requestFull(index - 1);
        }

        evictCache();
    }

    void ViewApp::step(long delta) {
        long last = (long)entries.length() - 1;
        long next = (long)current + delta;

        next = next < 0 ? 0 : next > last ? last : next;

        if ((size_t)next != current) {
            show((size_t)next);
        }
    }

    void ViewApp::setZoom(float value) {
        zoom = clampf(value, zoomMin, zoomMax);
        fit = false;
        traceText(sv(StringBuilder() << "zoom "_sv << (i64)(zoom * 100.f + .5f)));
    }

    void ViewApp::fitView() {
        fit = true;
        panX = 0.f;
        panY = 0.f;
        traceText("fit"_sv);
    }

    void ViewApp::keys() {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Q)) {
            result = -1;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_N)) {
            step(1);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_P)) {
            step(-1);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Home) || (ImGui::IsKeyPressed(ImGuiKey_G) && !io.KeyShift)) {
            step(-(long)entries.length());
        }

        if (ImGui::IsKeyPressed(ImGuiKey_End) || (ImGui::IsKeyPressed(ImGuiKey_G) && io.KeyShift)) {
            step((long)entries.length());
        }

        if (ImGui::IsKeyPressed(ImGuiKey_W) || ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            fitView();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
            panX = 0.f;
            panY = 0.f;
            setZoom(1.f);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
            setZoom(zoom * zoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
            setZoom(zoom / zoomStep);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_F11)) {
            fullscreen = !fullscreen;
            window->requestFullscreen(fullscreen);
            traceText(fullscreen ? "fullscreen on"_sv : "fullscreen off"_sv);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            rotation = (rotation + (io.KeyShift ? 3 : 1)) % 4;
            traceText(sv(StringBuilder() << "rotated "_sv << (i64)(rotation * 90)));
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            panel = !panel;
            traceText(panel ? "panel on"_sv : "panel off"_sv);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_I)) {
            info = !info;
            traceText(info ? "info on"_sv : "info off"_sv);
        }
    }

    // the shown file's facts, read once per selection
    void ViewApp::statFile() {
        struct stat st;

        fileBytes = -1;
        fileModified = Buffer();

        if (stat(entries[current]->path.cStr(), &st) != 0) {
            return;
        }

        fileBytes = (i64)st.st_size;

        struct tm tm;
        char stamp[32];

        localtime_r(&st.st_mtime, &tm);

        if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm)) {
            fileModified = Buffer(StringView(stamp));
        }
    }

    // the list: thumbnails one under another, each as wide as the list
    // and as tall as its proportion asks, the selected one framed. The
    // rows in view and a few beyond get their thumbnails decoded
    void ViewApp::drawGallery() {
        float g = px(gap);
        float innerW = max(1.f, ImGui::GetWindowWidth() - 2.f * g);
        u32 side = thumbSideFor(innerW);
        float viewH = ImGui::GetWindowHeight();
        size_t count = entries.length();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        float total = g;
        float currentTop = g;
        float currentH = 0.f;

        for (size_t i = 0; i < count; i++) {
            float h = rowHeightFor(*entries[i], innerW);

            if (i == current) {
                currentTop = total;
                currentH = h;
            }

            total += h + g;
        }

        // the list's extent, for the scrolling; the rows are placed by hand
        ImGui::Dummy(ImVec2(innerW, total));

        if (scrollToCurrent) {
            ImGui::SetScrollY(currentTop - (viewH - currentH) / 2.f);
            scrollToCurrent = false;
        }

        float scrollY = clampf(ImGui::GetScrollY(), 0.f, max(0.f, total - viewH));
        size_t first = count;
        size_t last = 0;
        float top = g;
        // two layers: the rows' fills under the thumbnails
        ImVec2 hover0;
        ImVec2 hover1;
        bool anyHovered = false;

        dl->ChannelsSplit(2);

        for (size_t i = 0; i < count; i++) {
            Entry& entry = *entries[i];
            float h = rowHeightFor(entry, innerW);
            float bottom = top + h;
            bool inView = bottom > scrollY && top < scrollY + viewH;

            if (inView) {
                first = min(first, i);
                last = max(last, i);
            }

            // a row is worth an item when it is in view or about to be
            bool near = bottom > scrollY - (float)thumbAhead * (innerW + g) && top < scrollY + viewH + (float)thumbAhead * (innerW + g);

            if (near) {
                // decoded at a size the list has since outgrown: again
                if (entry.thumb == Load::Ready && entry.thumbSide * 4 < side * 3) {
                    retire(entry.thumbTex);
                    entry.thumb = Load::None;
                    readyThumbs--;
                }

                if (entry.thumb == Load::None) {
                    requestThumb(i, side);
                }
            }

            if (inView) {
                ImVec2 p0(origin.x + g, origin.y + top);
                ImVec2 p1(p0.x + innerW, p0.y + h);

                ImGui::SetCursorScreenPos(p0);
                ImGui::PushID((int)i);

                if (ImGui::InvisibleButton("##row", ImVec2(innerW, h))) {
                    show(i);
                }

                bool hovered = ImGui::IsItemHovered();

                ImGui::PopID();

                // the row is the thumbnail with its whole gutter, so two rows'
                // fills meet in the gap; the selection's fill goes under,
                // the hovered row's over it, both under the thumbnails
                ImVec2 r0(p0.x - g, p0.y - g);
                ImVec2 r1(p1.x + g, p1.y + g);

                if (i == current) {
                    dl->ChannelsSetCurrent(0);
                    dl->AddRectFilled(r0, r1, ImGui::GetColorU32(ImGuiCol_Header));
                }

                if (hovered) {
                    hover0 = r0;
                    hover1 = r1;
                    anyHovered = true;
                }

                dl->ChannelsSetCurrent(1);

                if (entry.thumb == Load::Ready) {
                    dl->AddImage((ImTextureID)entry.thumbTex.ds, p0, p1);
                    entry.drawnAt = frames;
                } else {
                    const char* mark = entry.thumb == Load::Failed ? "?" : "\xe2\x80\xa6";
                    ImVec2 extent = ImGui::CalcTextSize(mark);

                    dl->AddText(ImVec2(p0.x + (innerW - extent.x) / 2.f, p0.y + (h - extent.y) / 2.f), dimColor, mark);
                }
            }

            top = bottom + g;
        }

        if (anyHovered) {
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(hover0, hover1, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        }

        dl->ChannelsMerge();

        LockGuard lock(shared.mutex);

        shared.wantFirst = first;
        shared.wantLast = last;
    }

    // the properties panel: sections that fold to their title. The image's
    // first (what is shown, at what zoom: a menu picks one), then the
    // file's own facts
    void ViewApp::drawInfo() {
        const Entry& entry = *entries[current];
        bool ready = shown == Load::Ready && shownIndex == current;

        // ImGui's own spacing, frames and colours; every row one frame
        // tall, text sitting where a frame's would
        // a key in the dim column, the value beside it
        auto key = [&](const char* name) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", name);
            ImGui::TableSetColumnIndex(1);
        };
        auto row = [&](const char* name, StringView value) {
            key(name);
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted((const char*)value.begin(), (const char*)value.end());
            ImGui::PopTextWrapPos();
        };
        // the key column as wide as its keys, the value column the rest,
        // its weight given: a weight derived from the contents is nothing
        // on the first pass, and nothing over nothing is not a width
        auto table = [&](const char* id) {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame)) {
                return false;
            }

            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.f);

            return true;
        };

        if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen) && table("image")) {
            if (shown == Load::Failed && shownIndex == current) {
                row("Error", sv(shownError));
            } else if (!ready) {
                row("State", "decoding\xe2\x80\xa6"_sv);
            }

            if (ready) {
                auto& text = sb();
                // megapixels to a tenth
                i64 tenths = ((i64)texW * (i64)texH + 50000) / 100000;

                text << (i64)texW << " \xc3\x97 "_sv << (i64)texH << "   "_sv << tenths / 10 << "."_sv << tenths % 10 << " MP"_sv;
                row("Dimensions", sv(text));
            }

            {
                // the name's extension, upper-cased; the decoder's word on
                // the format comes later
                StringView name = entry.name();
                const u8* dot = name.end();

                for (const u8* p = name.begin(); p != name.end(); ++p) {
                    if (*p == '.') {
                        dot = p;
                    }
                }

                char ext[16];
                size_t n = 0;

                for (const u8* p = dot == name.end() ? dot : dot + 1; p != name.end() && n < sizeof(ext); ++p) {
                    ext[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
                }

                row("Type", n ? StringView((const u8*)ext, (const u8*)ext + n) : "?"_sv);
            }

            if (ready) {
                key("Zoom");

                {
                    auto& text = sb();

                    text << (i64)(zoom * 100.f + .5f) << "%"_sv;

                    if (fit) {
                        text << " (fit)"_sv;
                    }

                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                    if (ImGui::BeginCombo("##zoom", text.cStr())) {
                        if (ImGui::Selectable("Fit", fit)) {
                            fitView();
                        }

                        static const int presets[] = {25, 50, 100, 200, 400, 800};

                        for (int preset : presets) {
                            auto& label = sb();

                            label << (i64)preset << "%"_sv;

                            if (ImGui::Selectable(label.cStr(), !fit && (i64)(zoom * 100.f + .5f) == preset)) {
                                panX = 0.f;
                                panY = 0.f;
                                setZoom((float)preset / 100.f);
                            }
                        }

                        ImGui::EndCombo();
                    }
                }

                {
                    auto& text = sb();

                    text << (i64)(rotation * 90) << "\xc2\xb0"_sv;
                    row("Rotation", sv(text));
                }
            }

            {
                auto& text = sb();

                text << (i64)(current + 1) << " / "_sv << (i64)entries.length();
                row("Position", sv(text));
            }

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen) && table("file")) {
            StringView whole = sv(entry.path);

            row("Name", entry.name());
            // the path up to the name's slash; a bare name is of the working
            // directory, a name under the root of "/"
            row("Folder", entry.nameAt == 0 ? "."_sv : entry.nameAt == 1 ? "/"_sv : StringView(whole.begin(), whole.begin() + entry.nameAt - 1));

            if (fileBytes >= 0) {
                auto& text = sb();

                appendBytes(text, fileBytes);
                row("Size", sv(text));
            }

            if (!fileModified.empty()) {
                row("Modified", sv(fileModified));
            }

            ImGui::EndTable();
        }
    }

    void ViewApp::drawCanvas() {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();

        if (size.x < 1.f || size.y < 1.f) {
            return;
        }

        ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (shown != Load::Ready) {
            const char* text = shown == Load::Failed ? "cannot show this image" : "decoding";
            ImVec2 extent = ImGui::CalcTextSize(text);

            dl->AddText(ImVec2(origin.x + (size.x - extent.x) / 2.f, origin.y + (size.y - extent.y) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);

            return;
        }

        float rw = (float)(rotation & 1 ? texH : texW);
        float rh = (float)(rotation & 1 ? texW : texH);

        if (fit) {
            // fit shrinks: a small image stays at its own size
            zoom = min(1.f, min(size.x / rw, size.y / rh));
        }

        float dw = rw * zoom;
        float dh = rh * zoom;

        // an image within the canvas sits centred; a larger one pans, never
        // past its edges
        panX = dw <= size.x ? 0.f : clampf(panX, (size.x - dw) / 2.f, (dw - size.x) / 2.f);
        panY = dh <= size.y ? 0.f : clampf(panY, (size.y - dh) / 2.f, (dh - size.y) / 2.f);

        ImVec2 centre(origin.x + size.x / 2.f + panX, origin.y + size.y / 2.f + panY);
        ImVec2 p0(centre.x - dw / 2.f, centre.y - dh / 2.f);
        ImVec2 p1(centre.x + dw / 2.f, centre.y + dh / 2.f);
        // the texture's corners, top-left first clockwise; a quarter turn
        // clockwise puts the texture's top-left at the screen's top-right
        const ImVec2 uv[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
        int r = rotation;

        dl->AddImageQuad((ImTextureID)tex.ds, p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), uv[(4 - r) & 3], uv[(5 - r) & 3], uv[(6 - r) & 3], uv[(7 - r) & 3]);

        ImGuiIO& io = ImGui::GetIO();

        // the wheel zooms about the pointer: what is under it stays put
        if (hovered && io.MouseWheel != 0.f) {
            float before = zoom;
            float after = clampf(zoom * powf(zoomStep, io.MouseWheel), zoomMin, zoomMax);
            float tx = (io.MousePos.x - centre.x) / before;
            float ty = (io.MousePos.y - centre.y) / before;

            panX = io.MousePos.x - tx * after - (origin.x + size.x / 2.f);
            panY = io.MousePos.y - ty * after - (origin.y + size.y / 2.f);
            setZoom(after);
        }

        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
            panX += io.MouseDelta.x;
            panY += io.MouseDelta.y;
        }
    }

    int ViewApp::frame() {
        ImGuiViewport* vp = ImGui::GetMainViewport();

        frames++;
        result = 0;
        keys();

        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        // Begin's and BeginChild's results go unread here and below: a
        // window that cannot collapse, and a child of one, is shown every
        // frame, and drawing into a hidden one would only be wasted
        // as an ImGui application lays out: the panels are windows, in the
        // window colour, the canvas between them is the application's own
        // background, the presenter's clear colour
        ImGui::Begin("##view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

        float sideW = floorf(vp->Size.x * sideShare);
        bool left = panel && !fullscreen;
        bool right = info && !fullscreen;

        if (left) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
            ImGui::BeginChild("gallery", ImVec2(sideW, 0.f), 0, ImGuiWindowFlags_NoScrollbar);
            drawGallery();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        ImGui::BeginChild("canvas", ImVec2(right ? -sideW : 0.f, 0.f), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawCanvas();
        ImGui::EndChild();

        if (right) {
            ImGui::SameLine();
            // the zeros placed the children; inside the panel ImGui's own
            // padding and spacing
            ImGui::PopStyleVar(2);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
            ImGui::BeginChild("info", ImVec2(sideW, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
            drawInfo();
            ImGui::EndChild();
            ImGui::PopStyleColor();
        } else {
            ImGui::PopStyleVar(2);
        }

        ImGui::End();
        evictThumbs();

        return result;
    }

    // the error panel in place of the viewer, when there is nothing to show
    struct NothingUi final: Ui {
        const Buffer* error = nullptr;

        int frame() override;
    };

    int NothingUi::frame() {
        return drawErrorPanel(sv(*error));
    }
}

int mainView(int argc, char** argv) {
    gTool = "view"_sv;
    initUiScale();

    if (argc < 2) {
        sysE << "usage: im view <file|dir>..."_sv << endL;

        return 2;
    }

    int rc = 0;

    try {
        ViewApp app;
        NothingUi nothing;
        FrameDriver driver;
        ObjPool::Ref shot = ObjPool::fromMemory();

        app.pool = &*shot;
        gChaos = ChaosMonkey::create(*shot);

        // the list: a directory's images, or the named files; one file
        // selects itself among its directory's
        Buffer errText;

        for (int i = 1; i < argc; i++) {
            StringView arg(argv[i]);
            struct stat st;

            if (stat(argv[i], &st) != 0) {
                sysE << "im view: "_sv << arg << ": "_sv << StringView(strerror(errno)) << endL;

                continue;
            }

            try {
                if (S_ISDIR(st.st_mode)) {
                    addDirectory(*shot, app.entries, arg);
                } else if (argc == 2) {
                    size_t slash = arg.length();

                    while (slash > 0 && arg[slash - 1] != '/') {
                        slash--;
                    }

                    StringView dir = slash == 0 ? "."_sv : slash == 1 ? "/"_sv : StringView(arg.begin(), arg.begin() + slash - 1);
                    StringView name(arg.begin() + slash, arg.end());

                    addDirectory(*shot, app.entries, dir);

                    bool listed = false;

                    for (size_t j = 0; j < app.entries.length() && !listed; j++) {
                        if (app.entries[j]->name() == name) {
                            app.current = j;
                            listed = true;
                        }
                    }

                    if (!listed) {
                        // a name the list did not take: shown all the same, first
                        Vector<Entry*> rest;

                        rest.xchg(app.entries);
                        addFile(*shot, app.entries, arg);
                        app.entries.append(rest.begin(), rest.end());
                        app.current = 0;
                    }
                } else {
                    addFile(*shot, app.entries, arg);
                }
            } catch (...) {
                sysE << "im view: "_sv << arg << ": "_sv << Exception::current() << endL;
            }
        }

        traceText(sv(StringBuilder() << "listed "_sv << (i64)app.entries.length()));

        if (app.entries.empty()) {
            errText = Buffer("no images to show"_sv);
            sysE << "im view: "_sv << sv(errText) << endL;
        }

        // the platform, the input bridge and the window live in the same
        // arena: LIFO death tears the window down after every vulkan guard
        // below and before the platform it belongs to
        plt::Platform& platform = *plt::Platform::create(*shot);
        ImGuiPlt& imgui = *ImGuiPlt::create(*shot);

        int winW = pxi(1000_d);
        int winH = pxi(700_d);

        if (!errText.empty()) {
            winW = pxi(480_d);
            winH = pxi(180_d);
        }

        plt::WindowOptions options;

        options.appId = "im-view"_sv;
        options.title = "im view"_sv;
        options.width = (u32)winW;
        options.height = (u32)winH;
        options.input = imgui.sink();
        options.events = &driver;
        options.frame = &driver;

        plt::Window& window = *platform.createWindow(*shot, options);

        // the window was asked for in the platform's logical units, and it
        // says what pixels it made of them: not the design's pixels on an
        // output that scales logical units by itself, or over the screen;
        // a resize is in pixels
        plt::WindowInfo made = window.info();
        int wantW = winW;
        int wantH = winH;

        clampWindowSize(made, wantW, wantH);

        if (wantW != (int)made.width || wantH != (int)made.height) {
            window.requestResize((u32)wantW, (u32)wantH);
        }

        VulkanWants wants;

        wants.textures = (u32)maxThumbs + 4;
        setupVulkan(*shot, wants);

        VkSurfaceKHR surface = createSurface(window);
        plt::WindowInfo bootInfo = window.info();

        setupVulkanWindow(*shot, surface, (int)bootInfo.width, (int)bootInfo.height, false);
        setupImGui(*shot, false);

        // the workers; their pool is joined before anything they may still
        // write to goes
        VkPhysicalDeviceProperties props;

        vkGetPhysicalDeviceProperties(gPhys, &props);
        app.shared.maxSide = props.limits.maxImageDimension2D;
        app.shared.mutex = Mutex::create(&*shot);
        app.shared.bell = platform.createLoopWake(*shot, app);

        long cpus = sysconf(_SC_NPROCESSORS_ONLN);
        size_t workers = (size_t)(cpus < 1 ? 1 : cpus > 4 ? 4 : cpus);

        app.threads = ThreadPool::simple(&*shot, workers);
        pooledGuard(*shot, [a = &app] {
            a->stopWorkers();
        });
        pooledGuard(*shot, [a = &app] {
            vkDeviceWaitIdle(gDevice);
            destroyTexture(a->tex);

            for (size_t i = 0; i < a->entries.length(); i++) {
                destroyTexture(a->entries[i]->thumbTex);
            }
        });

        app.window = &window;
        driver.platform = &platform;
        driver.window = &window;
        driver.imgui = &imgui;

        if (errText.empty()) {
            driver.ui = &app;
            app.show(app.current);
        } else {
            nothing.error = &errText;
            driver.ui = &nothing;
        }

        runUi(driver);
        // the jobs live in the pool, after every guard: the workers stop
        // here, before the pool's LIFO death reaches them
        app.stopWorkers();
    } catch (...) {
        sysE << "im view: "_sv << Exception::current() << endL;
        rc = 1;
    }

    return rc;
}
