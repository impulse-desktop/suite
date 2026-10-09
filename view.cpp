#include "view.h"

#include "ui.h"
#include "error.h"
#include "choose.h"
#include "pooled.h"
#include "timing.h"
#include "decoder.h"

#include <std/sys/fd.h>
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
#include <fcntl.h>
#include <imgui.h>
#include <string.h>
#include <sys/stat.h>
#include <imgui_internal.h>

// libmagic, as wasm2c turned its wasm module into C (ext/magic, under the
// name mime): the MIME type of a file's first bytes, so a listing takes
// every file that is an image of any kind, whatever the decoder then
// makes of it. A trap throws out of the module through the handler
// decoder.cpp defines.
#define WASM_RT_CORE_TYPES_DEFINED
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

extern "C" {
#include <mime.h>
}

using namespace stl;

namespace {
    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    constexpr Design windowWidth = 1000_d;
    constexpr Design windowHeight = 700_d;
    constexpr float sideShare = .2f;
    // the mark of the current row: a frame inside its thumbnail's edge
    constexpr Design mark = 4_d;
    constexpr Design panStep = 48_d;
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

    // the file decoded and fitted to a side: as a thumbnail, its transparent
    // margins cut before it is scaled; as the image to show, whole
    static Image* decodeFile(ObjPool& pool, Buffer& path, size_t nameAt, u32 side, bool asThumbnail, Buffer& timing) {
        u64 began = monotonicNowUs();
        Buffer file;

        readFileContent(path, file);
        StringView whole(path);
        StringView name(whole.begin() + nameAt, whole.end());
        Image* image = decode(pool, StringView(file), name);

        u64 decoded = monotonicNowUs();

        image = asThumbnail ? thumbnail(pool, image, side) : shrink(pool, image, side);

        StringBuilder text;

        text << StringView(u8"im decode ") << name << StringView(u8": read/decode ") << MS{decoded - began};
        text << StringView(u8" shrink ") << MS{monotonicNowUs() - decoded};
        timing = Buffer(StringView(text));
        return image;
    }

    // the first bytes of a file the MIME engine looks at: an image names
    // itself at the start, and the matcher takes an offset past what it
    // was given as no match
    constexpr size_t mimeHeadBytes = 4096;
    constexpr size_t mimeTypeLimit = 256;

    // the MIME engine: an instance of libmagic, made on its first question;
    // one that traps is dropped, and the next question makes another
    struct Mime {
        w2c_mime wasm;
        bool up = false;
        Buffer head;

        Mime()
            : head(mimeHeadBytes)
        {
            head.zero(mimeHeadBytes);
        }

        ~Mime() noexcept {
            if (up) {
                wasm2c_mime_free(&wasm);
            }
        }

        // the type of the bytes; false when the engine has no word
        bool type(StringView bytes, Buffer& out) {
            if (bytes.length() > 0xffffffffu) {
                return false;
            }

            try {
                if (!up) {
                    wasm2c_mime_instantiate(&wasm);
                    up = true;
                }

                u32 length = (u32)bytes.length();
                u32 data = w2c_mime_malloc(&wasm, length ? length : 1);
                wasm_rt_memory_t* memory = w2c_mime_memory(&wasm);

                if (!data || (u64)data + length > memory->size) {
                    return false;
                }

                memcpy(memory->data + data, bytes.data(), length);

                u32 text = w2c_mime_magic_mime(&wasm, data, length);

                w2c_mime_free(&wasm, data);
                memory = w2c_mime_memory(&wasm);

                if (!text || text >= memory->size) {
                    return false;
                }

                const u8* begin = (const u8*)memory->data + text;
                const u8* limit = (const u8*)memory->data + min<u64>(memory->size, (u64)text + mimeTypeLimit);
                const u8* end = begin;

                while (end < limit && *end) {
                    end++;
                }

                out = Buffer(StringView(begin, end));

                return true;
            } catch (...) {
                if (up) {
                    wasm2c_mime_free(&wasm);
                    up = false;
                }

                return false;
            }
        }

        // What file calls image/ but is no picture: a document of pages, a
        // drawing or a model of CAD, a volume of data, a texture for a GPU,
        // a sequence, or a thing with a picture somewhere in it. The rest
        // of image/ is the viewer's, decodable or not.
        static bool picture(StringView type) {
            static const StringView notPictures[] = {
                StringView(u8"image/vnd.djvu"),
                StringView(u8"image/x-ms-awd"),
                StringView(u8"image/x-eps"),
                StringView(u8"image/jpm"),
                StringView(u8"image/vnd.dwg"),
                StringView(u8"image/vnd.dxf"),
                StringView(u8"image/x-sld"),
                StringView(u8"image/x-3ds"),
                StringView(u8"image/x.nifti"),
                StringView(u8"image/x.nrrd"),
                StringView(u8"image/ktx"),
                StringView(u8"image/ktx2"),
                StringView(u8"image/x-godot-stex"),
                StringView(u8"image/heic-sequence"),
                StringView(u8"image/heif-sequence"),
                StringView(u8"image/x-epoc-record"),
                StringView(u8"image/x-garmin-exe"),
                StringView(u8"image/x-ulead-tpl"),
                StringView(u8"image/x.sf3-vector"),
            };

            if (!type.startsWith(StringView(u8"image/"))) {
                return false;
            }

            for (StringView other : notPictures) {
                if (type == other) {
                    return false;
                }
            }

            return true;
        }

        // whether the file's first bytes are a picture
        bool image(StringView path) {
            Buffer file(path);
            Buffer kind;
            size_t got = 0;

            try {
                ScopedFD fd(::open(file.cStr(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));

                if (fd.get() < 0) {
                    return false;
                }

                struct stat st;

                if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
                    return false;
                }

                while (got < mimeHeadBytes) {
                    size_t n = fd.read((u8*)head.mutData() + got, mimeHeadBytes - got);

                    if (!n) {
                        break;
                    }

                    got += n;
                }
            } catch (...) {
                return false;
            }

            return type(StringView((const u8*)head.mutData(), got), kind) && picture(StringView(kind));
        }
    };

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
        Vector<StringView> paths;
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
        bool info = false;
        bool leaving = false;
        bool scrollToCurrent = true;
        float tracedScrollY = 0.f;
        float galleryScroll = 0.f;

        void startWorkers(ObjPool& pool);
        void stopWorkers();
        void open();
        void accept();
        void take(Decoded& decoded);
        void adopt(Listed& listed);
        void submit();
        void dispatch(Job* job);
        void setThumb(Entry& entry, u32 side, Image& image);
        void select(size_t index);
        void show(size_t index);
        void replaceShown(const ShownImage* next);
        void setZoom(float value);
        void fitView();
        void keys();
        void step(long delta);
        void draw();
        void drawTools(float width);
        void drawGallery(ImVec2 size);
        void galleryKeys(ImGuiID owner);
        void drawCanvas(ImVec2 size);
        void canvasKeys(ImGuiID owner);
        void drawInfo();
    };

    struct Worker final: Runable {
        Channel* jobs;

        explicit Worker(Channel* jobs);
        void run() override;
    };
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
        image = decodeFile(*owner, path, nameAt, side, request == 0, timing);
        if (thumbSide) {
            thumb = thumbnail(*owner, image, thumbSide);
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
            Mime mime;

            listDir(StringView(folder), [&](const TPathInfo& info) {
                if (!info.isDir && mime.image(StringView(StringBuilder() << StringView(folder) << StringView(u8"/") << info.item))) {
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

void ViewApp::open() {
    Mime mime;

    if (mime.image(paths[0])) {
        entries.pushBack(makeEntry(*pool, paths[0]));
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

    while (listedNext < paths.length() && inFlight < workerCount) {
        ObjPool* owner = ObjPool::fromMemoryRaw();

        dispatch(owner->make<Listed>(owner, *this, listedNext, paths[listedNext], paths.length() == 1));
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
    ui->requestTitle(entries[index]->name());
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

void ViewApp::galleryKeys(ImGuiID owner) {
    bool next = ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, owner);

    next |= ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, owner);
    next |= ImGui::Shortcut(ImGuiKey_Space, ImGuiInputFlags_Repeat, owner);
    next |= ImGui::Shortcut(ImGuiKey_PageDown, ImGuiInputFlags_Repeat, owner);
    next |= ImGui::Shortcut(ImGuiKey_J, ImGuiInputFlags_Repeat, owner);
    next |= ImGui::Shortcut(ImGuiKey_N, ImGuiInputFlags_Repeat, owner);

    if (next) {
        step(1);
    }

    bool previous = ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, owner);

    previous |= ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, owner);
    previous |= ImGui::Shortcut(ImGuiKey_Backspace, ImGuiInputFlags_Repeat, owner);
    previous |= ImGui::Shortcut(ImGuiKey_PageUp, ImGuiInputFlags_Repeat, owner);
    previous |= ImGui::Shortcut(ImGuiKey_K, ImGuiInputFlags_Repeat, owner);
    previous |= ImGui::Shortcut(ImGuiKey_P, ImGuiInputFlags_Repeat, owner);

    if (previous) {
        step(-1);
    }

    bool first = ImGui::Shortcut(ImGuiKey_Home, ImGuiInputFlags_None, owner);

    first |= ImGui::Shortcut(ImGuiKey_G, ImGuiInputFlags_None, owner);

    if (first) {
        step(-(long)entries.length());
    }

    bool last = ImGui::Shortcut(ImGuiKey_End, ImGuiInputFlags_None, owner);

    last |= ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_G, ImGuiInputFlags_None, owner);

    if (last) {
        step((long)entries.length());
    }
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
    if (ImGui::Shortcut(ImGuiKey_Q)) {
        leaving = true;
    }

    bool fitting = ImGui::Shortcut(ImGuiKey_W);

    fitting |= ImGui::Shortcut(ImGuiKey_0);
    fitting |= ImGui::Shortcut(ImGuiKey_Keypad0);

    if (fitting) {
        fitView();
    }

    bool actual = ImGui::Shortcut(ImGuiKey_1);

    actual |= ImGui::Shortcut(ImGuiKey_Keypad1);

    if (actual) {
        panX = 0.f;
        panY = 0.f;
        setZoom(1.f);
    }

    bool closer = ImGui::Shortcut(ImGuiKey_Equal, ImGuiInputFlags_Repeat);

    closer |= ImGui::Shortcut(ImGuiKey_KeypadAdd, ImGuiInputFlags_Repeat);

    if (closer) {
        setZoom(zoom * zoomStep);
    }

    bool farther = ImGui::Shortcut(ImGuiKey_Minus, ImGuiInputFlags_Repeat);

    farther |= ImGui::Shortcut(ImGuiKey_KeypadSubtract, ImGuiInputFlags_Repeat);

    if (farther) {
        setZoom(zoom / zoomStep);
    }

    bool flip = ImGui::Shortcut(ImGuiKey_F);

    flip |= ImGui::Shortcut(ImGuiKey_F11);

    if (flip) {
        fullscreen = !fullscreen;
        ui->requestFullscreen(fullscreen);
        TRACE(ui, fullscreen ? StringView(u8"fullscreen on") : StringView(u8"fullscreen off"));
    }

    bool clockwise = ImGui::Shortcut(ImGuiKey_R);
    bool counter = ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_R);

    if (clockwise || counter) {
        rotation = (rotation + (counter ? 3 : 1)) % 4;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"rotated ") << (i64)(rotation * 90)));
    }

    if (ImGui::Shortcut(ImGuiKey_I)) {
        info = !info;
        TRACE(ui, info ? StringView(u8"info on") : StringView(u8"info off"));
    }

    if (!ImGui::GetIO().NavVisible) {
        galleryKeys(ImGuiKeyOwner_Any);
    }
}

static bool holdBehavior(const ImRect& bb, ImGuiID id, bool* hovered, bool* keyboard, ImGuiButtonFlags flags) {
    ImGuiContext& context = *GImGui;
    ImGuiWindow* window = context.CurrentWindow;
    bool pressed = false;

    *hovered = ImGui::ItemHoverable(bb, id, context.LastItemData.ItemFlags);

    for (int button = ImGuiMouseButton_Left; button <= ImGuiMouseButton_Middle; button++) {
        if (*hovered && (flags & (ImGuiButtonFlags_MouseButtonLeft << button)) && ImGui::IsMouseClicked(button)) {
            ImGui::SetActiveID(id, window);
            context.ActiveIdMouseButton = button;
            pressed = true;
        }
    }

    if (context.NavActivateId == id) {
        ImGui::SetActiveID(id, window);
    }

    if (pressed || context.NavActivateId == id) {
        ImGui::SetFocusID(id, window);
        ImGui::FocusWindow(window);
    }

    if (context.ActiveId == id && context.ActiveIdSource == ImGuiInputSource_Mouse && !ImGui::IsMouseDown(context.ActiveIdMouseButton)) {
        ImGui::ClearActiveID();
    }

    if (context.ActiveId == id && context.ActiveIdSource != ImGuiInputSource_Mouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::ClearActiveID();
    }

    *keyboard = context.ActiveId == id && context.ActiveIdSource != ImGuiInputSource_Mouse;

    if (*keyboard) {
        context.ActiveIdAllowOverlap = true;
    }

    return pressed;
}

// the rows down the list, the list's width each and the image's shape,
// one against the next, the current one framed inside its edge in the
// colour of an active item; the thumbnails bulge towards the pointer,
// over their rows
void ViewApp::drawGallery(ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiID id = window->GetID("##gallery");
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImRect bb(origin, ImVec2(origin.x + size.x, origin.y + size.y));

    ImGui::ItemSize(size);

    if (!ImGui::ItemAdd(bb, id)) {
        return;
    }

    bool hovered = false;
    bool keyboard = false;
    bool clicked = holdBehavior(bb, id, &hovered, &keyboard, ImGuiButtonFlags_MouseButtonLeft);

    if (keyboard) {
        galleryKeys(id);
    }

    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    ImGui::RenderNavCursor(bb, id);

    float g = ImGui::GetStyle().ItemSpacing.y;
    float frame = ui->px(mark);
    float innerW = max(1.f, size.x);
    thumbSide = thumbSideFor(innerW * (1.f + bulge));
    float viewH = max(1.f, size.y);
    size_t count = entries.length();
    ImDrawList* dl = window->DrawList;
    ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    bool pointed = mouse.x >= vp->Pos.x && mouse.x < vp->Pos.x + vp->Size.x && mouse.y >= vp->Pos.y && mouse.y < vp->Pos.y + vp->Size.y;
    float reach = ui->px(bulgeReach);
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();
    StringView problem(problems);
    float head = problem.empty() ? 0.f : font->CalcTextSizeA(fontSize, FLT_MAX, innerW, (const char*)problem.begin(), (const char*)problem.end()).y + g;
    float total = head;
    float currentTop = total;
    float currentH = 0.f;

    for (size_t i = 0; i < count; i++) {
        float h = rowHeightFor(*entries[i], innerW);

        if (i == current) {
            currentTop = total;
            currentH = h;
        }

        total += h;
    }

    if (scrollToCurrent) {
        galleryScroll = currentTop - (viewH - currentH) / 2.f;
        scrollToCurrent = false;
    }

    if (hovered && io.MouseWheel != 0.f) {
        galleryScroll -= io.MouseWheel * min(5.f * fontSize, viewH * .67f);
    }

    galleryScroll = clampf(galleryScroll, 0.f, max(0.f, total - viewH));

    float scrollY = galleryScroll;
    ImVec2 content(origin.x, origin.y - scrollY);
    if (scrollY != tracedScrollY) {
        StringBuilder text;

        text << StringView(u8"im scroll: y ") << (i64)scrollY << StringView(u8" dy ") << (i64)(scrollY - tracedScrollY) << StringView(u8" wheel/100 ") << (i64)(io.MouseWheel * 100.f);
        ui->timing(StringView(text));
        tracedScrollY = scrollY;
    }
    size_t first = count;
    size_t last = 0;
    float firstTop = 0.f;
    float lastBottom = 0.f;
    size_t nearest = count;
    float nearestD = 0.f;
    float top = head;

    dl->PushClipRect(bb.Min, bb.Max, true);

    if (head > 0.f) {
        dl->AddText(font, fontSize, content, ImGui::GetColorU32(ImGuiCol_Text), (const char*)problem.begin(), (const char*)problem.end(), innerW);
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
            ImVec2 p0(content.x, content.y + top);
            ImVec2 p1(p0.x + innerW, p0.y + h);

            if (clicked && mouse.y >= p0.y && mouse.y < p1.y) {
                clicked = false;
                show(i);
            }

            if (pointed) {
                float d = distance(mouse, ImVec2((p0.x + p1.x) / 2.f, (p0.y + p1.y) / 2.f));

                if (nearest == count || d < nearestD) {
                    nearest = i;
                    nearestD = d;
                }
            }
        }

        top = bottom;
    }

    ImDrawList* fg = ImGui::GetForegroundDrawList();

    // the frame of the current row, inside the edge of what it is drawn on:
    // four filled bands flush with the edge, as a stroked rectangle keeps
    // half a pixel and its anti-aliasing inside it
    auto framed = [&](ImDrawList* list, ImVec2 a, ImVec2 b) {
        ImU32 color = ImGui::GetColorU32(ImGuiCol_HeaderActive);

        list->AddRectFilled(a, ImVec2(b.x, a.y + frame), color);
        list->AddRectFilled(ImVec2(a.x, b.y - frame), b, color);
        list->AddRectFilled(a, ImVec2(a.x + frame, b.y), color);
        list->AddRectFilled(ImVec2(b.x - frame, a.y), b, color);
    };

    auto draw = [&](size_t i, float rowTop, float h) {
        Entry& entry = *entries[i];
        ImVec2 p0(content.x, content.y + rowTop);
        ImVec2 p1(p0.x + innerW, p0.y + h);

        if (!entry.thumbW && !entry.error.empty()) {
            StringView error(entry.error);
            float pad = g;

            dl->PushClipRect(p0, p1, true);
            dl->AddText(font, fontSize, ImVec2(p0.x + pad, p0.y + pad), ImGui::GetColorU32(ImGuiCol_Text), (const char*)error.begin(), (const char*)error.end(), max(1.f, innerW - 2.f * pad));
            dl->PopClipRect();

            if (i == current) {
                framed(dl, p0, p1);
            }

            return;
        }

        if (!entry.thumbW) {
            const char* text = entry.thumb == Load::Failed ? "?" : "\xe2\x80\xa6";
            ImVec2 extent = ImGui::CalcTextSize(text);

            dl->AddText(ImVec2(p0.x + (innerW - extent.x) / 2.f, p0.y + (h - extent.y) / 2.f), dimColor, text);

            if (i == current) {
                framed(dl, p0, p1);
            }

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

        ImVec2 a(centre.x - half.x, centre.y - half.y);
        ImVec2 b(centre.x + half.x, centre.y + half.y);

        fg->AddImage(entry.thumbTex, a, b);

        if (i == current) {
            framed(fg, a, b);
        }
    };

    ImVec2 viewportEnd(vp->Pos.x + vp->Size.x, bb.Max.y);

    fg->PushClipRect(bb.Min, viewportEnd, false);

    if (first < count) {
        size_t stop = nearest == count ? last + 1 : nearest;
        float y = firstTop;

        for (size_t i = first; i < stop; i++) {
            float h = rowHeightFor(*entries[i], innerW);

            draw(i, y, h);
            y += h;
        }

        if (nearest != count) {
            float bottom = lastBottom;

            for (size_t i = last; i > nearest; i--) {
                float h = rowHeightFor(*entries[i], innerW);

                draw(i, bottom - h, h);
                bottom -= h;
            }

            draw(nearest, bottom - rowHeightFor(*entries[nearest], innerW), rowHeightFor(*entries[nearest], innerW));
        }
    }

    fg->PopClipRect();
    dl->PopClipRect();
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
        ImGui::TextDisabled("%s", name);
        ImGui::TableSetColumnIndex(1);
    };
    auto row = [&](const char* name, StringView value) {
        key(name);
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

void ViewApp::canvasKeys(ImGuiID owner) {
    float delta = ui->px(panStep);

    if (ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, owner)) {
        panX += delta;
    }

    if (ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, owner)) {
        panX -= delta;
    }

    if (ImGui::Shortcut(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, owner)) {
        panY += delta;
    }

    if (ImGui::Shortcut(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, owner)) {
        panY -= delta;
    }
}

void ViewApp::drawCanvas(ImVec2 size) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiID id = window->GetID("##canvas");
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImRect bb(origin, ImVec2(origin.x + size.x, origin.y + size.y));

    ImGui::ItemSize(size);

    if (!ImGui::ItemAdd(bb, id)) {
        return;
    }

    bool hovered = false;
    bool keyboard = false;

    holdBehavior(bb, id, &hovered, &keyboard, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

    if (keyboard) {
        canvasKeys(id);
    }

    if (hovered) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }

    ImGui::RenderNavCursor(bb, id);

    ImDrawList* dl = window->DrawList;

    if (entries.empty() || size.x < 1.f || size.y < 1.f) {
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

    dl->PushClipRect(bb.Min, bb.Max, true);
    dl->AddImageQuad(shown->texture, p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), uv[(4 - r) & 3], uv[(5 - r) & 3], uv[(6 - r) & 3], uv[(7 - r) & 3]);
    dl->PopClipRect();
}

// the toolbar over the list: the info panel's switch, zoom in and out by
// the wheel's step, and the zoom as a list of steps with the fit first
void ViewApp::drawTools(float width) {
    static const i64 steps[] = {25, 50, 75, 100, 150, 200, 300, 400};
    i64 percent = (i64)(zoom * 100.f + .5f);
    StringBuilder current;

    current << percent << StringView(u8"%");

    float left = ImGui::GetCursorScreenPos().x;

    if (ImGui::Checkbox("I", &info)) {
        TRACE(ui, info ? StringView(u8"info on") : StringView(u8"info off"));
    }

    ImGui::SameLine();

    if (ImGui::Button("+")) {
        setZoom(zoom * zoomStep);
    }

    ImGui::SameLine();

    if (ImGui::Button("-")) {
        setZoom(zoom / zoomStep);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(max(1.f, left + width - ImGui::GetCursorScreenPos().x));

    if (ImGui::BeginCombo("##zoom", fit ? "Fit" : (const char*)current.cStr())) {
        if (ImGui::Selectable("Fit", fit)) {
            fitView();
        }

        for (i64 step : steps) {
            StringBuilder label;

            label << step << StringView(u8"%");

            if (ImGui::Selectable((const char*)label.cStr(), !fit && percent == step)) {
                setZoom((float)step / 100.f);
            }
        }

        ImGui::EndCombo();
    }
}

void ViewApp::draw() {
    wantedThumbs.clear();
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    keys();

    float sideW = floorf(vp->Size.x * sideShare);
    bool left = panel && !fullscreen;
    bool right = info && !fullscreen;

    // the toolbar and the list are one column, a group, so that the canvas
    // stands beside both
    if (left) {
        ImGui::BeginGroup();
        drawTools(sideW);
        drawGallery(ImVec2(sideW, ImGui::GetContentRegionAvail().y));
        ImGui::EndGroup();
        ImGui::SameLine();
    }

    // the canvas leaves the info panel its width and the spacing before it
    ImVec2 room = ImGui::GetContentRegionAvail();

    drawCanvas(ImVec2(right ? room.x - sideW - ImGui::GetStyle().ItemSpacing.x : room.x, room.y));

    if (right) {
        ImGui::SameLine();
        ImGui::BeginChild("info", ImVec2(sideW, 0.f), 0, ImGuiWindowFlags_NoScrollbar);
        drawInfo();
        ImGui::EndChild();
    }

    ImGui::End();
}

int mainView(ObjPool& pool, int argc, char** argv) {
    ViewApp& app = *pool.make<ViewApp>();
    Ui& ui = *Ui::create(pool, StringView(u8"view"), UiOptions{windowWidth, windowHeight});

    app.ui = &ui;
    app.pool = &pool;
    app.maxSide = ui.maxTextureSide();
    app.startWorkers(pool);

    for (int i = 1; i < argc; i++) {
        app.paths.pushBack(StringView(argv[i]));
    }

    auto body = makeRunable([&] {
        UiEvent event;

        if (app.paths.empty()) {
            ChooseOptions options;

            options.multiple = true;
            options.filters.pushBack(StringView(u8"Images|image/*"));
            options.filters.pushBack(StringView(u8"All|*"));

            bool chosen = chooseInWindow(ui, options, [&](StringView path) {
                app.paths.pushBack(pool.intern(path));
            });

            if (!chosen || app.paths.empty()) {
                return;
            }
        }

        app.open();

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                return;
            }

            app.accept();
            app.draw();
            app.submit();

            if (app.leaving) {
                return;
            }
        }
    });
    return ui.run(body);
}
