#include "view.h"

#include "ui.h"
#include "error.h"
#include "pooled.h"
#include "timing.h"
#include "decoder.h"

#include <std/sys/fs.h>
#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/alg/defer.h>
#include <std/alg/qsort.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/thr/thread.h>
#include <std/str/builder.h>
#include <std/thr/channel.h>
#include <std/thr/runable.h>
#include <std/ios/fs_utils.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <time.h>
#include <errno.h>
#include <imgui.h>
#include <string.h>
#include <sys/stat.h>

using namespace stl;

namespace {
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    constexpr Design windowWidth = 1000_d;
    constexpr Design windowHeight = 700_d;
    constexpr float sideShare = .2f;
    constexpr Design gap = 4_d;
    constexpr float bulge = .2f;
    constexpr Design bulgeReach = 240_d;
    constexpr float pi = 3.14159265f;
    constexpr float placeholderAspect = .75f;
    constexpr u32 thumbTexelsStep = 64;
    constexpr u32 thumbTexelsMin = 128;
    constexpr u32 thumbTexelsMax = 512;
    constexpr float zoomMin = 0.02f;
    constexpr float zoomMax = 32.f;
    constexpr float zoomStep = 1.25f;
    constexpr size_t workerCount = 4;

    enum class Load : u8 {
        None,
        Ready,
        Failed
    };

    struct Entry {
        Buffer path;
        size_t nameAt = 0;
        u32 arg = 0;
        Buffer error;
        bool loading = false;
        Load thumb = Load::None;
        ImTextureRef thumbTex;
        u32 thumbW = 0;
        u32 thumbH = 0;
        u32 thumbSide = 0;
        StringView name() const;
    };

    struct ScaledImage final: Image {
        Buffer rgba;
        u32 width_;
        u32 height_;

        ScaledImage(const Image& image, u32 side);

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;
    };

    static Image* shrinkToSide(ObjPool& pool, Image* image, u32 side) {
        if (image->width() <= side && image->height() <= side) {
            return image;
        }

        return pool.make<ScaledImage>(*image, side);
    }

    static Image* decodeFile(ObjPool& pool, Buffer& path, size_t nameAt, u32 side, Buffer& timing) {
        u64 began = monotonicNowUs();
        Buffer file;

        readFileContent(path, file);
        StringView whole(path);
        StringView name(whole.begin() + nameAt, whole.end());
        Image* image = decode(pool, StringView(file), name);

        u64 decoded = monotonicNowUs();

        image = shrinkToSide(pool, image, side);

        StringBuilder text;

        text << StringView(u8"im decode ") << name << StringView(u8": read/decode ") << MS{decoded - began};
        text << StringView(u8" shrink ") << MS{monotonicNowUs() - decoded};
        timing = Buffer(StringView(text));
        return image;
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

    StringView nameOf(StringView path) {
        size_t slash = path.length();

        while (slash > 0 && path[slash - 1] != '/') {
            slash--;
        }

        return StringView(path.begin() + slash, path.end());
    }

    u32 thumbSideFor(float innerW) {
        u32 side = (u32)ceilf(innerW / (float)thumbTexelsStep) * thumbTexelsStep;

        return side < thumbTexelsMin ? thumbTexelsMin : side > thumbTexelsMax ? thumbTexelsMax : side;
    }

    float rowHeightFor(const Entry& entry, float innerW) {
        float aspect = entry.thumbW ? (float)entry.thumbH / (float)entry.thumbW : placeholderAspect;

        return max(1.f, floorf(innerW * aspect + .5f));
    }

    float distance(ImVec2 a, ImVec2 b) {
        return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

    void appendBytes(StringBuilder& text, i64 bytes) {
        static const StringView units[] = {StringView(u8"KB"), StringView(u8"MB"), StringView(u8"GB"), StringView(u8"TB")};

        if (bytes < 1024) {
            text << bytes << StringView(u8" bytes");

            return;
        }

        i64 tenths = bytes * 10 / 1024;
        size_t unit = 0;

        while (tenths >= 10240 && unit + 1 < sizeof(units) / sizeof(units[0])) {
            tenths /= 1024;
            unit++;
        }

        text << tenths / 10 << StringView(u8".") << tenths % 10 << StringView(u8" ") << units[unit];
    }

    struct ViewApp;

    struct Job: Runable {
        ObjPool* owner;
        Channel* results;
        Ui* wake;
        bool listing;

        Job(ObjPool* owner, ViewApp& app, bool listing);
        void post();
    };

    struct Decoded final: Job {
        Buffer path;
        size_t nameAt;
        u32 side;
        u32 thumbSide;
        u64 request;
        Image* image = nullptr;
        Image* thumb = nullptr;
        i64 fileBytes = -1;
        Buffer fileModified;
        Buffer error;
        Buffer timing;

        Decoded(ObjPool* owner, ViewApp& app, const Entry& entry, u32 side, u32 thumbSide, u64 request);
        void run() override;
    };

    struct Listed final: Job {
        u32 arg;
        Buffer path;
        bool alone;
        bool directory = false;
        Buffer folder;
        Vector<StringView> names;
        Buffer error;

        Listed(ObjPool* owner, ViewApp& app, u32 arg, StringView path, bool alone);
        void run() override;
    };

    struct ShownImage {
        ObjPool* owner;
        Ui* ui;
        const Entry* entry;
        ImTextureRef texture;
        u32 width = 0;
        u32 height = 0;
        i64 fileBytes;
        Buffer fileModified;
        Buffer error;

        ShownImage(ObjPool* owner, Ui* ui, const Entry* entry, Decoded& result);
        ~ShownImage() noexcept;
    };

    struct ViewApp {
        Ui* ui = nullptr;
        ObjPool* pool = nullptr;
        Channel* jobs = nullptr;
        Channel* results = nullptr;
        Thread* workers[workerCount] = {};
        size_t inFlight = 0;
        u64 showRequest = 0;
        bool showPending = false;
        Vector<size_t> wantedThumbs;
        u32 thumbSide = thumbTexelsMin;
        char** paths = nullptr;
        u32 pathCount = 0;
        u32 listedNext = 0;
        Buffer problems;
        Vector<Entry*> entries;
        size_t current = 0;
        u32 maxSide = 0;
        const ShownImage* shown = nullptr;
        float zoom = 1.f;
        bool fit = true;
        float panX = 0.f;
        float panY = 0.f;
        int rotation = 0;
        bool fullscreen = false;
        bool panel = true;
        bool info = true;
        bool scrollToCurrent = true;
        float tracedScrollY = 0.f;

        void startWorkers(ObjPool& pool);
        void stopWorkers();
        void open(u32 count, char** given);
        void accept();
        void take(Decoded& decoded);
        void adopt(Listed& listed);
        void submit();
        void dispatch(Job* job);
        void setThumb(Entry& entry, u32 side, Image& image);
        void select(size_t index);
        void show(size_t index);
        void replaceShown(const ShownImage* next);
        void step(long delta);
        void setZoom(float value);
        void fitView();
        void keys();
        void draw();
        void drawGallery();
        void drawCanvas();
        void drawInfo();
    };

    struct Worker final: Runable {
        Channel* jobs;

        explicit Worker(Channel* jobs);
        void run() override;
    };
}

ScaledImage::ScaledImage(const Image& image, u32 side) {
    u32 sw = image.width();
    u32 sh = image.height();

    width_ = sw >= sh ? side : (u32)max<u64>(1, (u64)side * sw / sh);
    height_ = sh >= sw ? side : (u32)max<u64>(1, (u64)side * sh / sw);
    rgba.zero((size_t)width_ * height_ * 4);

    const unsigned char* src = (const unsigned char*)image.data();
    unsigned char* dst = (unsigned char*)rgba.mutData();

    for (u32 dy = 0; dy < height_; dy++) {
        u32 y0 = (u32)((u64)dy * sh / height_);
        u32 y1 = (u32)max<u64>(y0 + 1, (u64)(dy + 1) * sh / height_);

        for (u32 dx = 0; dx < width_; dx++) {
            u32 x0 = (u32)((u64)dx * sw / width_);
            u32 x1 = (u32)max<u64>(x0 + 1, (u64)(dx + 1) * sw / width_);
            u64 sum[4] = {};

            for (u32 y = y0; y < y1; y++) {
                const unsigned char* row = src + ((size_t)y * sw + x0) * 4;

                for (u32 x = x0; x < x1; x++) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    sum[3] += row[3];
                    row += 4;
                }
            }

            u64 count = (u64)(x1 - x0) * (y1 - y0);
            unsigned char* px = dst + ((size_t)dy * width_ + dx) * 4;

            px[0] = (unsigned char)((sum[0] + count / 2) / count);
            px[1] = (unsigned char)((sum[1] + count / 2) / count);
            px[2] = (unsigned char)((sum[2] + count / 2) / count);
            px[3] = (unsigned char)((sum[3] + count / 2) / count);
        }
    }
}

const void* ScaledImage::data() const {
    return rgba.data();
}

size_t ScaledImage::length() const {
    return rgba.length();
}

u32 ScaledImage::width() const {
    return width_;
}

u32 ScaledImage::height() const {
    return height_;
}

StringView Entry::name() const {
    StringView whole = StringView(path);

    return StringView(whole.begin() + nameAt, whole.end());
}

ShownImage::ShownImage(ObjPool* owner_, Ui* ui_, const Entry* entry_, Decoded& result)
    : owner(owner_)
    , ui(ui_)
    , entry(entry_)
    , fileBytes(result.fileBytes)
{
    fileModified.xchg(result.fileModified);
    error = Buffer(StringView(result.error));

    if (error.empty()) {
        width = result.image->width();
        height = result.image->height();
        texture = ui->loadTexture(width, height, result.image->data());
    }
}

ShownImage::~ShownImage() noexcept {
    if (error.empty()) {
        ui->releaseTexture(texture);
    }
}

Worker::Worker(Channel* jobs_)
    : jobs(jobs_)
{
}

void Worker::run() {
    void* item;

    while (jobs->dequeue(&item)) {
        ((Runable*)item)->run();
    }
}

Job::Job(ObjPool* owner_, ViewApp& app, bool listing_)
    : owner(owner_)
    , results(app.results)
    , wake(app.ui)
    , listing(listing_)
{
}

void Job::post() {
    Ui* notify = wake;

    results->enqueue(this);
    notify->requestFrame();
}

Decoded::Decoded(ObjPool* owner, ViewApp& app, const Entry& entry, u32 side_, u32 thumbSide_, u64 request_)
    : Job(owner, app, false)
    , path(StringView(entry.path))
    , nameAt(entry.nameAt)
    , side(side_)
    , thumbSide(thumbSide_)
    , request(request_)
{
}

void Decoded::run() {
    struct stat st;

    if (request && stat(path.cStr(), &st) == 0) {
        fileBytes = (i64)st.st_size;
        struct tm tm;
        char stamp[32];

        localtime_r(&st.st_mtime, &tm);
        if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm)) {
            fileModified = Buffer(StringView(stamp));
        }
    }

    try {
        image = decodeFile(*owner, path, nameAt, side, timing);
        if (thumbSide) {
            thumb = shrinkToSide(*owner, image, thumbSide);
        }
    } catch (...) {
        error = Buffer(Exception::current());
    }

    post();
}

Listed::Listed(ObjPool* owner, ViewApp& app, u32 arg_, StringView path_, bool alone_)
    : Job(owner, app, true)
    , arg(arg_)
    , path(path_)
    , alone(alone_)
{
}

void Listed::run() {
    struct stat st;

    if (stat(path.cStr(), &st) != 0) {
        error = Buffer(StringView(strerror(errno)));
        post();

        return;
    }

    directory = S_ISDIR(st.st_mode);

    if (directory || alone) {
        StringView whole(path);
        size_t slash = whole.length() - nameOf(whole).length();

        folder = directory ? Buffer(whole) : Buffer(slash == 0 ? StringView(u8".") : slash == 1 ? StringView(u8"/") : StringView(whole.begin(), whole.begin() + slash - 1));

        try {
            listDir(StringView(folder), [&](const TPathInfo& info) {
                if (!info.isDir && imageName(info.item)) {
                    u8* copy = (u8*)owner->allocate(info.item.length());

                    memcpy(copy, info.item.data(), info.item.length());
                    names.pushBack(StringView(copy, info.item.length()));
                }
            });
        } catch (...) {
            error = Buffer(Exception::current());
        }
    }

    post();
}

void ViewApp::startWorkers(ObjPool& pool) {
    jobs = Channel::create(&pool, workerCount);
    results = Channel::create(&pool, workerCount);

    for (size_t i = 0; i < workerCount; i++) {
        workers[i] = Thread::create(&pool, *pool.make<Worker>(jobs));
    }

    pooledGuard(pool, [this] {
        stopWorkers();
    });
}

void ViewApp::stopWorkers() {
    void* item;

    jobs->close();

    for (Thread* worker : workers) {
        worker->join();
    }

    while (results->tryDequeue(&item)) {
        delete ((Job*)item)->owner;
    }

    if (shown) {
        delete shown->owner;
        shown = nullptr;
    }
}

void ViewApp::open(u32 count, char** given) {
    paths = given;
    pathCount = count;

    if (imageName(StringView(given[0]))) {
        entries.pushBack(makeEntry(*pool, StringView(given[0])));
        select(0);
    }
}

void ViewApp::accept() {
    void* item;

    while (results->tryDequeue(&item)) {
        Job* job = (Job*)item;
        ScopedGuard cleanup = [owner = job->owner] {
            delete owner;
        };

        --inFlight;

        if (job->listing) {
            adopt(*static_cast<Listed*>(job));
        } else {
            take(*static_cast<Decoded*>(job));
        }
    }
}

void ViewApp::take(Decoded& decoded) {
    StringView path(decoded.path);
    StringView name(path.begin() + decoded.nameAt, path.end());
    Image* small = decoded.request ? decoded.thumb : decoded.image;
    u32 side = decoded.request ? decoded.thumbSide : decoded.side;

    ui->timing(StringView(decoded.timing));

    for (Entry* entry : entries) {
        if (StringView(entry->path) != path) {
            continue;
        }

        entry->loading = false;

        if (small) {
            setThumb(*entry, side, *small);
        } else if (!decoded.error.empty() && entry->thumb != Load::Ready) {
            entry->thumb = Load::Failed;
            entry->error = Buffer(StringView(decoded.error));
            TRACE(ui, StringView(StringBuilder() << StringView(u8"no thumbnail ") << name << StringView(u8": ") << StringView(decoded.error)));
        }
    }

    if (!decoded.request) {
        return;
    }

    if (decoded.request != showRequest || entries.empty() || StringView(entries[current]->path) != path) {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"discarded image ") << name));
        return;
    }

    ObjPool* storage = ObjPool::fromMemoryRaw();
    const ShownImage* next = storage->make<ShownImage>(storage, ui, entries[current], decoded);

    replaceShown(next);

    if (!next->error.empty()) {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"cannot show ") << name << StringView(u8": ") << StringView(next->error)));
    } else {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"showing ") << name << StringView(u8" ") << (i64)next->width << StringView(u8"x") << (i64)next->height));
    }
}

void ViewApp::adopt(Listed& listed) {
    constexpr size_t none = (size_t)-1;
    StringView path(listed.path);
    StringView folder(listed.folder);
    Entry* chosen = entries.empty() ? nullptr : entries[current];
    Entry* early = nullptr;
    Vector<Entry*> found;
    size_t named = none;

    if (!listed.error.empty()) {
        StringBuilder text;

        text << path << StringView(u8": ") << StringView(listed.error);
        problems = Buffer(StringView(StringBuilder() << StringView(problems) << (problems.empty() ? StringView() : StringView(u8"\n")) << StringView(text)));
        TRACE(ui, StringView(text));
    }

    for (StringView name : listed.names) {
        StringBuilder joined;

        joined << folder;

        if (!folder.endsWith(StringView(u8"/"))) {
            joined << StringView(u8"/");
        }

        joined << name;
        found.pushBack(makeEntry(*pool, StringView(joined)));
    }

    quickSort(found.mutBegin(), found.mutEnd(), [](const Entry* a, const Entry* b) {
        return a->name() < b->name();
    });

    if (listed.error.empty() && !listed.directory) {
        for (size_t i = 0; i < found.length() && named == none; i++) {
            if (found[i]->name() == nameOf(path)) {
                named = i;
            }
        }

        if (named == none) {
            Vector<Entry*> rest;

            rest.xchg(found);
            found.pushBack(makeEntry(*pool, path));
            found.append(rest.begin(), rest.end());
            named = 0;
        }
    }

    Vector<Entry*> kept;
    size_t at = 0;

    for (Entry* entry : entries) {
        if (entry->arg == listed.arg) {
            early = entry;
            continue;
        }

        at += entry->arg < listed.arg;
        kept.pushBack(entry);
    }

    if (early && named != none) {
        found.mut(named) = early;
    }

    for (Entry* entry : found) {
        entry->arg = listed.arg;
    }

    entries.clear();
    entries.append(kept.begin(), kept.begin() + at);
    entries.append(found.begin(), found.end());
    entries.append(kept.begin() + at, kept.end());
    scrollToCurrent = true;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"listed ") << (i64)entries.length()));

    for (size_t i = 0; i < entries.length(); i++) {
        if (entries[i] == chosen) {
            current = i;
            ui->requestFrame();

            return;
        }
    }

    if (entries.empty()) {
        current = 0;
        ++showRequest;
        showPending = false;
        replaceShown(nullptr);

        return;
    }

    select(named == none ? 0 : at + named);
}

void ViewApp::dispatch(Job* job) {
    ++inFlight;
    jobs->enqueue(static_cast<Runable*>(job));
}

void ViewApp::submit() {
    if (showPending && inFlight < workerCount && !entries[current]->loading) {
        Entry& entry = *entries[current];
        ObjPool* owner = ObjPool::fromMemoryRaw();
        u32 thumbs = entry.thumb == Load::Ready && entry.thumbSide * 4 >= thumbSide * 3 ? 0 : thumbSide;

        entry.loading = true;
        showPending = false;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"loading image ") << entry.name()));
        dispatch(owner->make<Decoded>(owner, *this, entry, maxSide, thumbs, showRequest));
    }

    while (listedNext < pathCount && inFlight < workerCount) {
        ObjPool* owner = ObjPool::fromMemoryRaw();

        dispatch(owner->make<Listed>(owner, *this, listedNext, StringView(paths[listedNext]), pathCount == 1));
        listedNext++;
    }

    // Missing visible images precede resolution upgrades. The list is rebuilt by drawGallery().
    for (int pass = 0; pass < 2; pass++) {
        for (size_t index : wantedThumbs) {
            if (inFlight == workerCount) {
                return;
            }
            Entry& entry = *entries[index];

            if (entry.loading || entry.thumb == Load::Failed || (pass == 0) != (entry.thumb == Load::None)) {
                continue;
            }
            if (entry.thumb == Load::Ready && entry.thumbSide * 4 >= thumbSide * 3) {
                continue;
            }
            ObjPool* owner = ObjPool::fromMemoryRaw();

            entry.loading = true;
            TRACE(ui, StringView(StringBuilder() << StringView(u8"loading thumbnail ") << entry.name()));
            dispatch(owner->make<Decoded>(owner, *this, entry, thumbSide, 0, 0));
        }
    }
}

void ViewApp::setThumb(Entry& entry, u32 side, Image& image) {
    if (entry.thumbW && entry.thumbSide >= side) {
        return;
    }
    ImTextureRef texture = ui->loadTexture(image.width(), image.height(), image.data());

    if (entry.thumbW) {
        ui->releaseTexture(entry.thumbTex);
    }
    entry.thumbTex = texture;
    entry.thumbW = image.width();
    entry.thumbH = image.height();
    entry.thumbSide = side;
    entry.thumb = Load::Ready;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"thumbnail ") << entry.name()));
}

void ViewApp::select(size_t index) {
    current = index;
    ++showRequest;
    showPending = true;
    ui->requestFrame();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"selected ") << entries[index]->name()));
}

void ViewApp::show(size_t index) {
    if (index == current && showRequest != 0 && (!shown || shown->error.empty())) {
        return;
    }
    select(index);
}

void ViewApp::replaceShown(const ShownImage* next) {
    const ShownImage* previous = shown;

    shown = next;
    if (previous) {
        delete previous->owner;
    }
    ui->requestFrame();
}

void ViewApp::step(long delta) {
    if (entries.empty()) {
        return;
    }

    long last = (long)entries.length() - 1;
    long next = (long)current + delta;

    next = next < 0 ? 0 : next > last ? last : next;

    if ((size_t)next != current) {
        scrollToCurrent = true;
        show((size_t)next);
    }
}

void ViewApp::setZoom(float value) {
    zoom = clampf(value, zoomMin, zoomMax);
    fit = false;
    ui->requestFrame();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"zoom ") << (i64)(zoom * 100.f + .5f)));
}

void ViewApp::fitView() {
    ui->requestFrame();
    fit = true;
    panX = 0.f;
    panY = 0.f;
    TRACE(ui, StringView(u8"fit"));
}

void ViewApp::keys() {
    ImGuiIO& io = ImGui::GetIO();

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
        ui->requestFullscreen(fullscreen);
        TRACE(ui, fullscreen ? StringView(u8"fullscreen on") : StringView(u8"fullscreen off"));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        rotation = (rotation + (io.KeyShift ? 3 : 1)) % 4;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"rotated ") << (i64)(rotation * 90)));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        panel = !panel;
        TRACE(ui, panel ? StringView(u8"panel on") : StringView(u8"panel off"));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_I)) {
        info = !info;
        TRACE(ui, info ? StringView(u8"info on") : StringView(u8"info off"));
    }
}

void ViewApp::drawGallery() {
    float g = ui->px(gap);
    float innerW = max(1.f, ImGui::GetWindowWidth() - 2.f * g);
    thumbSide = thumbSideFor((innerW + 2.f * g) * (1.f + bulge));
    float viewH = ImGui::GetWindowHeight();
    size_t count = entries.length();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    bool pointed = mouse.x >= vp->Pos.x && mouse.x < vp->Pos.x + vp->Size.x && mouse.y >= vp->Pos.y && mouse.y < vp->Pos.y + vp->Size.y;
    float reach = ui->px(bulgeReach);
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();
    StringView problem(problems);
    float head = problem.empty() ? 0.f : font->CalcTextSizeA(fontSize, FLT_MAX, innerW, (const char*)problem.begin(), (const char*)problem.end()).y + g;
    float total = g + head;
    float currentTop = total;
    float currentH = 0.f;

    for (size_t i = 0; i < count; i++) {
        float h = rowHeightFor(*entries[i], innerW);

        if (i == current) {
            currentTop = total;
            currentH = h;
        }

        total += h + g;
    }

    ImGui::Dummy(ImVec2(innerW, total));

    if (scrollToCurrent) {
        ImGui::SetScrollY(currentTop - (viewH - currentH) / 2.f);
        scrollToCurrent = false;
    }

    float scrollY = clampf(ImGui::GetScrollY(), 0.f, max(0.f, total - viewH));

    if (scrollY != tracedScrollY) {
        StringBuilder text;

        text << StringView(u8"im scroll: y ") << (i64)scrollY << StringView(u8" dy ") << (i64)(scrollY - tracedScrollY) << StringView(u8" wheel/100 ") << (i64)(ImGui::GetIO().MouseWheel * 100.f);
        ui->timing(StringView(text));
        tracedScrollY = scrollY;
    }
    size_t first = count;
    size_t last = 0;
    float firstTop = 0.f;
    float lastBottom = 0.f;
    size_t nearest = count;
    float nearestD = 0.f;
    float top = g + head;

    if (head > 0.f) {
        dl->AddText(font, fontSize, ImVec2(origin.x + g, origin.y + g), ImGui::GetColorU32(ImGuiCol_Text), (const char*)problem.begin(), (const char*)problem.end(), innerW);
    }

    for (size_t i = 0; i < count; i++) {
        Entry& entry = *entries[i];
        float h = rowHeightFor(entry, innerW);
        float bottom = top + h;
        bool inView = bottom > scrollY && top < scrollY + viewH;

        if (inView) {
            if (first == count) {
                first = i;
                firstTop = top;
            }

            last = i;
            lastBottom = bottom;
        }

        if (inView) {
            wantedThumbs.pushBack(i);
        }

        if (inView) {
            ImVec2 p0(origin.x + g, origin.y + top);
            ImVec2 p1(p0.x + innerW, p0.y + h);

            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID((int)i);

            if (ImGui::InvisibleButton("##row", ImVec2(innerW, h))) {
                show(i);
            }

            ImGui::PopID();

            if (i == current) {
                dl->AddRectFilled(ImVec2(p0.x - g, p0.y - g), ImVec2(p1.x + g, p1.y + g), ImGui::GetColorU32(ImGuiCol_Header));
            }

            if (pointed) {
                float d = distance(mouse, ImVec2((p0.x + p1.x) / 2.f, (p0.y + p1.y) / 2.f));

                if (nearest == count || d < nearestD) {
                    nearest = i;
                    nearestD = d;
                }
            }
        }

        top = bottom + g;
    }

    ImDrawList* fg = ImGui::GetForegroundDrawList();

    auto draw = [&](size_t i, float rowTop, float h) {
        Entry& entry = *entries[i];
        ImVec2 p0(origin.x + g, origin.y + rowTop);
        ImVec2 p1(p0.x + innerW, p0.y + h);

        if (!entry.thumbW && !entry.error.empty()) {
            StringView error(entry.error);
            float pad = g * 2.f;

            dl->PushClipRect(p0, p1, true);
            dl->AddText(font, fontSize, ImVec2(p0.x + pad, p0.y + pad), ImGui::GetColorU32(ImGuiCol_Text), (const char*)error.begin(), (const char*)error.end(), max(1.f, innerW - 2.f * pad));
            dl->PopClipRect();

            return;
        }

        if (!entry.thumbW) {
            const char* mark = entry.thumb == Load::Failed ? "?" : "\xe2\x80\xa6";
            ImVec2 extent = ImGui::CalcTextSize(mark);

            dl->AddText(ImVec2(p0.x + (innerW - extent.x) / 2.f, p0.y + (h - extent.y) / 2.f), dimColor, mark);

            return;
        }

        float scale = 1.f;

        if (pointed) {
            float t = distance(mouse, ImVec2((p0.x + p1.x) / 2.f, (p0.y + p1.y) / 2.f)) / reach;

            if (t < 1.f) {
                scale += bulge * .5f * (1.f + cosf(pi * t));
            }
        }

        ImVec2 centre((p0.x + p1.x) / 2.f, (p0.y + p1.y) / 2.f);
        ImVec2 half((p1.x - p0.x) / 2.f * scale, (p1.y - p0.y) / 2.f * scale);

        fg->AddImage(entry.thumbTex, ImVec2(centre.x - half.x, centre.y - half.y), ImVec2(centre.x + half.x, centre.y + half.y));
    };

    ImVec2 viewportEnd(vp->Pos.x + vp->Size.x, windowPos.y + viewH);

    fg->PushClipRect(windowPos, viewportEnd, false);

    if (first < count) {
        size_t stop = nearest == count ? last + 1 : nearest;
        float y = firstTop;

        for (size_t i = first; i < stop; i++) {
            float h = rowHeightFor(*entries[i], innerW);

            draw(i, y, h);
            y += h + g;
        }

        if (nearest != count) {
            float bottom = lastBottom;

            for (size_t i = last; i > nearest; i--) {
                float h = rowHeightFor(*entries[i], innerW);

                draw(i, bottom - h, h);
                bottom -= h + g;
            }

            draw(nearest, bottom - rowHeightFor(*entries[nearest], innerW), rowHeightFor(*entries[nearest], innerW));
        }
    }

    fg->PopClipRect();
}

void ViewApp::drawInfo() {
    if (!shown) {
        return;
    }
    const Entry& entry = *shown->entry;
    bool ready = shown->error.empty();

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
    auto table = [&](const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame)) {
            return false;
        }

        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.f);

        return true;
    };

    if (ImGui::CollapsingHeader("Image", ImGuiTreeNodeFlags_DefaultOpen) && table("image")) {
        if (!ready) {
            row("Error", StringView(shown->error));
        }

        if (ready) {
            StringBuilder text;
            i64 tenths = ((i64)shown->width * (i64)shown->height + 50000) / 100000;

            text << (i64)shown->width << StringView(u8" \xc3\x97 ") << (i64)shown->height << StringView(u8"   ") << tenths / 10 << StringView(u8".") << tenths % 10 << StringView(u8" MP");
            row("Dimensions", StringView(text));
        }

        {
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

            row("Type", n ? StringView((const u8*)ext, (const u8*)ext + n) : StringView(u8"?"));
        }

        if (ready) {
            key("Zoom");

            {
                StringBuilder text;

                text << (i64)(zoom * 100.f + .5f) << StringView(u8"%");

                if (fit) {
                    text << StringView(u8" (fit)");
                }

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                if (ImGui::BeginCombo("##zoom", text.cStr())) {
                    if (ImGui::Selectable("Fit", fit)) {
                        fitView();
                    }

                    static const int presets[] = {25, 50, 100, 200, 400, 800};

                    for (int preset : presets) {
                        StringBuilder label;

                        label << (i64)preset << StringView(u8"%");

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
                StringBuilder text;

                text << (i64)(rotation * 90) << StringView(u8"\xc2\xb0");
                row("Rotation", StringView(text));
            }
        }

        for (size_t i = 0; i < entries.length(); i++) {
            if (entries[i] == shown->entry) {
                StringBuilder text;

                text << (i64)(i + 1) << StringView(u8" / ") << (i64)entries.length();
                row("Position", StringView(text));
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen) && table("file")) {
        StringView whole = StringView(entry.path);

        row("Name", entry.name());
        row("Folder", entry.nameAt == 0 ? StringView(u8".") : entry.nameAt == 1 ? StringView(u8"/") : StringView(whole.begin(), whole.begin() + entry.nameAt - 1));

        if (shown->fileBytes >= 0) {
            StringBuilder text;

            appendBytes(text, shown->fileBytes);
            row("Size", StringView(text));
        }

        if (!shown->fileModified.empty()) {
            row("Modified", StringView(shown->fileModified));
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

    if (entries.empty()) {
        return;
    }

    if (!shown || !shown->error.empty()) {
        const char* text = shown ? "cannot show this image" : "decoding";
        ImVec2 extent = ImGui::CalcTextSize(text);

        dl->AddText(ImVec2(origin.x + (size.x - extent.x) / 2.f, origin.y + (size.y - extent.y) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);

        return;
    }

    float rw = (float)(rotation & 1 ? shown->height : shown->width);
    float rh = (float)(rotation & 1 ? shown->width : shown->height);

    if (fit) {
        zoom = min(1.f, min(size.x / rw, size.y / rh));
    }

    float dw = rw * zoom;
    float dh = rh * zoom;

    panX = dw <= size.x ? 0.f : clampf(panX, (size.x - dw) / 2.f, (dw - size.x) / 2.f);
    panY = dh <= size.y ? 0.f : clampf(panY, (size.y - dh) / 2.f, (dh - size.y) / 2.f);

    ImVec2 centre(origin.x + size.x / 2.f + panX, origin.y + size.y / 2.f + panY);
    ImGuiIO& io = ImGui::GetIO();

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
    dw = rw * zoom;
    dh = rh * zoom;
    panX = dw <= size.x ? 0.f : clampf(panX, (size.x - dw) / 2.f, (dw - size.x) / 2.f);
    panY = dh <= size.y ? 0.f : clampf(panY, (size.y - dh) / 2.f, (dh - size.y) / 2.f);
    centre = ImVec2(origin.x + size.x / 2.f + panX, origin.y + size.y / 2.f + panY);
    ImVec2 p0(centre.x - dw / 2.f, centre.y - dh / 2.f);
    ImVec2 p1(centre.x + dw / 2.f, centre.y + dh / 2.f);
    const ImVec2 uv[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
    int r = rotation;

    dl->AddImageQuad(shown->texture, p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), uv[(4 - r) & 3], uv[(5 - r) & 3], uv[(6 - r) & 3], uv[(7 - r) & 3]);
}

void ViewApp::draw() {
    wantedThumbs.clear();
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
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
}

int mainView(ObjPool& pool, int argc, char** argv) {
    if (argc < 2) {
        sysE << StringView(u8"usage: im view <file|dir>...") << endL;

        return 2;
    }

    ViewApp& app = *pool.make<ViewApp>();
    Ui& ui = *Ui::create(pool, StringView(u8"view"), UiOptions{windowWidth, windowHeight});

    app.ui = &ui;
    app.pool = &pool;
    app.maxSide = ui.maxTextureSide();
    app.startWorkers(pool);
    app.open((u32)(argc - 1), argv + 1);

    auto body = makeRunable([&] {
        UiEvent event;

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                return;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Q)) {
                return;
            }

            app.keys();
            app.accept();
            app.draw();
            app.submit();
        }
    });
    return ui.run(body);
}
