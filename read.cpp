#include "read.h"

#include "ui.h"
#include "error.h"
#include "pooled.h"
#include "timing.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/alg/defer.h>
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
#include <imgui.h>
#include <string.h>
#include <sys/stat.h>

// The engines, PDFium and DjVuLibre, as wasm2c turned their wasm modules
// into C: each instance has a memory of its own, every access checked,
// and a trap throws out of it through the handler decoder.cpp defines.
#define WASM_RT_CORE_TYPES_DEFINED
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

extern "C" {
#include <pdf.h>
#include <djvu.h>
}

using namespace stl;

namespace {
    constexpr Design windowWidth = 1000_d;
    constexpr Design windowHeight = 700_d;
    constexpr float sideShare = .2f;
    constexpr Design gap = 4_d;
    constexpr float placeholderAspect = 1.4142f;
    constexpr u32 thumbTexelsStep = 64;
    constexpr u32 thumbTexelsMin = 128;
    constexpr u32 thumbTexelsMax = 512;
    constexpr size_t workerCount = 4;
    constexpr u64 maxBytes = 1u << 30;
    constexpr u32 causeLimit = 256;

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    enum class Kind : u8 {
        Pdf,
        Djvu
    };

    enum class Load : u8 {
        None,
        Ready,
        Failed
    };

    // the file, read once; every worker's engine takes a copy into its memory
    struct Source {
        Kind kind = Kind::Pdf;
        Buffer bytes;
    };

    // a document open in an engine of a worker's own: its pages, their
    // sizes in points, a page rendered to RGBA at an asked size
    struct Document {
        // an engine that trapped is dead: nothing of it is called again
        bool dead = false;

        virtual ~Document() noexcept = default;
        virtual u32 pages() = 0;
        virtual float width(u32 page) = 0;
        virtual float height(u32 page) = 0;
        // false when the engine drew nothing
        virtual bool render(u32 page, u32 width, u32 height, Buffer& rgba) = 0;
        // what the engine says about its trap, if it says anything
        virtual Buffer cause() = 0;
    };

    // the block an engine answers a render with: the size it drew, then the pixels
    static void takeImage(const u8* block, u32 width, u32 height, Buffer& rgba) {
        u32 header[2];

        memcpy(header, block, sizeof(header));

        if (header[0] != width || header[1] != height) {
            fail(StringView(StringBuilder() << StringView(u8"the engine drew ") << (i64)header[0] << StringView(u8"x") << (i64)header[1] << StringView(u8" for ") << (i64)width << StringView(u8"x") << (i64)height));
        }

        rgba = Buffer(StringView(block + sizeof(header), (size_t)width * height * 4));
    }

    static u64 imageBytes(u32 width, u32 height) {
        u64 bytes = (u64)width * height * 4;

        if (!width || !height || bytes > maxBytes) {
            fail(StringView(u8"a page too large to draw"));
        }

        return 8 + bytes;
    }

    static u32 fileLength(const Source& source) {
        if (source.bytes.length() > 0xffffffffu) {
            fail(StringView(u8"the file is too large for the engine"));
        }

        return (u32)source.bytes.length();
    }

    struct PdfDocument final: Document {
        w2c_pdf wasm;
        u32 data = 0;
        u32 doc = 0;

        explicit PdfDocument(const Source& source);
        ~PdfDocument() noexcept override;

        u32 pages() override;
        float width(u32 page) override;
        float height(u32 page) override;
        bool render(u32 page, u32 width, u32 height, Buffer& rgba) override;
        Buffer cause() override;

        u8* at(u64 offset, u64 length);
    };

    struct DjvuDocument final: Document {
        w2c_djvu wasm;
        u32 doc = 0;

        explicit DjvuDocument(const Source& source);
        ~DjvuDocument() noexcept override;

        u32 pages() override;
        float width(u32 page) override;
        float height(u32 page) override;
        bool render(u32 page, u32 width, u32 height, Buffer& rgba) override;
        Buffer cause() override;

        u8* at(u64 offset, u64 length);
    };

    struct ReadApp;

    // what a worker does with its document, and brings back to the screen
    struct Job {
        ObjPool* owner;
        ReadApp* app;
        bool opening;
        Buffer error;

        Job(ObjPool* owner, ReadApp& app, bool opening);
        virtual ~Job() noexcept = default;
        virtual void work(Document& document) = 0;
        void post();
    };

    struct Opened final: Job {
        u32 pages = 0;
        Vector<float> sizes;

        Opened(ObjPool* owner, ReadApp& app);
        void work(Document& document) override;
    };

    struct Rendered final: Job {
        u32 page;
        u32 width;
        u32 height;
        bool thumb;
        u64 request;
        Buffer rgba;
        Buffer timing;

        Rendered(ObjPool* owner, ReadApp& app, u32 page, u32 width, u32 height, bool thumb, u64 request);
        void work(Document& document) override;
    };

    struct Thumb {
        Load load = Load::None;
        bool loading = false;
        ImTextureRef texture;
        u32 width = 0;
        u32 height = 0;
        u32 side = 0;
        Buffer error;
    };

    struct Shown {
        Ui* ui;
        u32 page;
        ImTextureRef texture;
        u32 width = 0;
        u32 height = 0;
        Buffer error;

        Shown(Ui* ui, Rendered& result);
        ~Shown() noexcept;
    };

    struct ReadApp {
        Ui* ui = nullptr;
        ObjPool* pool = nullptr;
        Buffer path;
        size_t nameAt = 0;
        i64 fileBytes = -1;
        Buffer fileModified;
        Source source;
        Channel* jobs = nullptr;
        Channel* results = nullptr;
        Thread* workers[workerCount] = {};
        size_t inFlight = 0;
        bool opening = false;
        bool opened = false;
        Buffer problem;
        u32 pageCount = 0;
        Vector<float> sizes;
        Vector<Thumb*> thumbs;
        Vector<size_t> wanted;
        u32 thumbSide = thumbTexelsMin;
        u32 maxSide = 0;
        size_t current = 0;
        u64 showRequest = 0;
        bool showPending = false;
        u32 canvasW = 0;
        u32 canvasH = 0;
        Shown* shown = nullptr;
        bool fullscreen = false;
        bool panel = true;
        bool info = true;
        bool scrollToCurrent = true;

        void startWorkers(ObjPool& pool);
        void stopWorkers();
        void open(StringView given);
        void accept();
        void take(Opened& opened);
        void take(Rendered& rendered);
        void submit();
        void dispatch(Job* job);
        void fitSize(u32 page, u32& width, u32& height);
        void thumbSize(u32 page, u32& width, u32& height);
        void setThumb(u32 page, Rendered& result);
        void select(size_t index);
        void show(size_t index);
        void replaceShown(Shown* next);
        void step(long delta);
        void keys();
        void draw();
        void drawPages();
        void drawCanvas();
        void drawInfo();
    };

    // a worker: its own engine with the document open, made on its first job
    struct Worker final: Runable {
        ReadApp* app;

        explicit Worker(ReadApp& app);
        void run() override;
    };

    StringView nameOf(StringView path) {
        size_t slash = path.length();

        while (slash > 0 && path[slash - 1] != '/') {
            slash--;
        }

        return StringView(path.begin() + slash, path.end());
    }

    bool hasSuffix(StringView name, StringView suffix) {
        if (name.length() < suffix.length()) {
            return false;
        }

        for (size_t i = 0; i < suffix.length(); i++) {
            if (((u8)name[name.length() - suffix.length() + i] | 0x20) != (u8)suffix[i]) {
                return false;
            }
        }

        return true;
    }

    // the kind of file: by its magic, then by its name
    bool sniff(StringView name, const Buffer& bytes, Kind& kind) {
        StringView head(bytes);

        if (head.length() > 1024) {
            head = StringView(head.begin(), head.begin() + 1024);
        }

        // an IFF FORM of a DJVU, DJVM or DJVI chunk, usually behind AT&T
        if (head.startsWith(StringView(u8"AT&T"))) {
            head = StringView(head.begin() + 4, head.end());
        }

        if (head.length() >= 15 && head.startsWith(StringView(u8"FORM")) && StringView(head.begin() + 8, head.begin() + 11) == StringView(u8"DJV")) {
            kind = Kind::Djvu;

            return true;
        }

        for (size_t i = 0; i + 4 <= head.length(); i++) {
            if (StringView(head.begin() + i, head.begin() + i + 4) == StringView(u8"%PDF")) {
                kind = Kind::Pdf;

                return true;
            }
        }

        if (hasSuffix(name, StringView(u8".pdf"))) {
            kind = Kind::Pdf;

            return true;
        }

        if (hasSuffix(name, StringView(u8".djvu")) || hasSuffix(name, StringView(u8".djv"))) {
            kind = Kind::Djvu;

            return true;
        }

        return false;
    }

    u32 thumbSideFor(float innerW) {
        u32 side = (u32)ceilf(innerW / (float)thumbTexelsStep) * thumbTexelsStep;

        return side < thumbTexelsMin ? thumbTexelsMin : side > thumbTexelsMax ? thumbTexelsMax : side;
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

    void appendPoints(StringBuilder& text, float value) {
        i64 tenths = (i64)(value * 10.f + .5f);

        text << tenths / 10 << StringView(u8".") << tenths % 10;
    }
}

PdfDocument::PdfDocument(const Source& source) {
    u32 length = fileLength(source);

    wasm2c_pdf_instantiate(&wasm);
    ScopedGuard cleanup = [&] {
        if (!doc) {
            wasm2c_pdf_free(&wasm);
        }
    };

    data = w2c_pdf_malloc(&wasm, length);

    if (!data) {
        fail(StringView(u8"the engine is out of memory"));
    }

    memcpy(at(data, length), source.bytes.data(), length);
    doc = w2c_pdf_pdf_open(&wasm, data, length);

    if (!doc) {
        fail(StringView(u8"not a PDF the engine opens"));
    }
}

PdfDocument::~PdfDocument() noexcept {
    if (!dead) {
        w2c_pdf_pdf_close(&wasm, doc);
        w2c_pdf_free(&wasm, data);
    }

    wasm2c_pdf_free(&wasm);
}

u32 PdfDocument::pages() {
    return w2c_pdf_pdf_pages(&wasm, doc);
}

float PdfDocument::width(u32 page) {
    return w2c_pdf_pdf_width(&wasm, doc, page);
}

float PdfDocument::height(u32 page) {
    return w2c_pdf_pdf_height(&wasm, doc, page);
}

bool PdfDocument::render(u32 page, u32 width, u32 height, Buffer& rgba) {
    u64 bytes = imageBytes(width, height);
    u32 res = w2c_pdf_pdf_render(&wasm, doc, page, width, height);

    if (!res) {
        return false;
    }

    ScopedGuard release = [&] {
        w2c_pdf_free(&wasm, res);
    };

    takeImage(at(res, bytes), width, height, rgba);

    return true;
}

Buffer PdfDocument::cause() {
    return Buffer();
}

u8* PdfDocument::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_pdf_memory(&wasm);

    if (offset + length > memory->size) {
        fail(StringView(u8"the engine answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

DjvuDocument::DjvuDocument(const Source& source) {
    u32 length = fileLength(source);

    wasm2c_djvu_instantiate(&wasm);
    ScopedGuard cleanup = [&] {
        if (!doc) {
            wasm2c_djvu_free(&wasm);
        }
    };

    u32 data = w2c_djvu_malloc(&wasm, length);

    if (!data) {
        fail(StringView(u8"the engine is out of memory"));
    }

    memcpy(at(data, length), source.bytes.data(), length);
    // the document copies the file: the engine's own copy goes at once
    doc = w2c_djvu_djvu_open(&wasm, data, length);
    w2c_djvu_free(&wasm, data);

    if (!doc) {
        fail(StringView(u8"not a DjVu the engine opens"));
    }
}

DjvuDocument::~DjvuDocument() noexcept {
    if (!dead) {
        w2c_djvu_djvu_close(&wasm, doc);
    }

    wasm2c_djvu_free(&wasm);
}

u32 DjvuDocument::pages() {
    return w2c_djvu_djvu_pages(&wasm, doc);
}

float DjvuDocument::width(u32 page) {
    return w2c_djvu_djvu_width(&wasm, doc, page);
}

float DjvuDocument::height(u32 page) {
    return w2c_djvu_djvu_height(&wasm, doc, page);
}

bool DjvuDocument::render(u32 page, u32 width, u32 height, Buffer& rgba) {
    u64 bytes = imageBytes(width, height);
    u32 res = w2c_djvu_djvu_render(&wasm, doc, page, width, height);

    if (!res) {
        return false;
    }

    ScopedGuard release = [&] {
        w2c_djvu_free(&wasm, res);
    };

    takeImage(at(res, bytes), width, height, rgba);

    return true;
}

// the engine's cause of its trap: a C string in its memory, read as far
// as the memory and the limit allow
Buffer DjvuDocument::cause() {
    u32 text = w2c_djvu_djvu_error(&wasm);
    wasm_rt_memory_t* memory = w2c_djvu_memory(&wasm);

    if (!text || text >= memory->size) {
        return Buffer();
    }

    const u8* begin = (const u8*)memory->data + text;
    const u8* end = begin;
    const u8* limit = (const u8*)memory->data + min<u64>(memory->size, (u64)text + causeLimit);

    while (end < limit && *end) {
        end++;
    }

    return Buffer(StringView(begin, end));
}

u8* DjvuDocument::at(u64 offset, u64 length) {
    wasm_rt_memory_t* memory = w2c_djvu_memory(&wasm);

    if (offset + length > memory->size) {
        fail(StringView(u8"the engine answered out of its memory"));
    }

    return (u8*)memory->data + offset;
}

Job::Job(ObjPool* owner_, ReadApp& app_, bool opening_)
    : owner(owner_)
    , app(&app_)
    , opening(opening_)
{
}

void Job::post() {
    Ui* notify = app->ui;

    app->results->enqueue(this);
    notify->requestFrame();
}

Opened::Opened(ObjPool* owner, ReadApp& app)
    : Job(owner, app, true)
{
}

void Opened::work(Document& document) {
    pages = document.pages();

    for (u32 page = 0; page < pages; page++) {
        sizes.pushBack(document.width(page));
        sizes.pushBack(document.height(page));
    }
}

Rendered::Rendered(ObjPool* owner, ReadApp& app, u32 page_, u32 width_, u32 height_, bool thumb_, u64 request_)
    : Job(owner, app, false)
    , page(page_)
    , width(width_)
    , height(height_)
    , thumb(thumb_)
    , request(request_)
{
}

void Rendered::work(Document& document) {
    u64 began = monotonicNowUs();

    if (!document.render(page, width, height, rgba)) {
        fail(StringView(u8"the engine drew nothing"));
    }

    StringBuilder text;

    text << StringView(u8"im render page ") << (i64)(page + 1) << StringView(u8" ") << (i64)width << StringView(u8"x") << (i64)height << StringView(u8": ") << MS{monotonicNowUs() - began};
    timing = Buffer(StringView(text));
}

// the page at the size it was asked, or the error in its place: a size
// all the same, so the page is not asked again at that size
Shown::Shown(Ui* ui_, Rendered& result)
    : ui(ui_)
    , page(result.page)
    , width(result.width)
    , height(result.height)
{
    error = Buffer(StringView(result.error));

    if (error.empty()) {
        texture = ui->loadTexture(width, height, result.rgba.data());
    }
}

Shown::~Shown() noexcept {
    if (error.empty()) {
        ui->releaseTexture(texture);
    }
}

Worker::Worker(ReadApp& app_)
    : app(&app_)
{
}

// The worker opens the document in an engine of its own on its first job
// and keeps it; a job that traps the engine takes the engine down with
// it, and the next job opens another. A document that cannot be opened
// fails every job with the reason.
void Worker::run() {
    Document* document = nullptr;
    Buffer failure;
    void* item;

    STD_DEFER {
        delete document;
    };

    while (app->jobs->dequeue(&item)) {
        Job* job = (Job*)item;

        if (!document && failure.empty()) {
            try {
                if (app->source.kind == Kind::Pdf) {
                    document = new PdfDocument(app->source);
                } else {
                    document = new DjvuDocument(app->source);
                }
            } catch (...) {
                failure = Buffer(Exception::current());
            }
        }

        if (!document) {
            job->error = Buffer(StringView(failure));
        } else {
            try {
                job->work(*document);
            } catch (...) {
                StringBuilder text;

                text << Exception::current();
                document->dead = true;

                try {
                    Buffer cause = document->cause();

                    if (!cause.empty()) {
                        text << StringView(u8": ") << StringView(cause);
                    }
                } catch (...) {
                }

                job->error = Buffer(StringView(text));
                delete document;
                document = nullptr;
            }
        }

        job->post();
    }
}

void ReadApp::startWorkers(ObjPool& pool) {
    jobs = Channel::create(&pool, workerCount);
    results = Channel::create(&pool, workerCount);

    for (size_t i = 0; i < workerCount; i++) {
        workers[i] = Thread::create(&pool, *pool.make<Worker>(*this));
    }

    pooledGuard(pool, [this] {
        stopWorkers();
    });
}

void ReadApp::stopWorkers() {
    void* item;

    jobs->close();

    for (Thread* worker : workers) {
        worker->join();
    }

    while (results->tryDequeue(&item)) {
        delete ((Job*)item)->owner;
    }

    replaceShown(nullptr);

    for (Thumb* thumb : thumbs) {
        if (thumb->width) {
            ui->releaseTexture(thumb->texture);
        }
    }
}

void ReadApp::open(StringView given) {
    struct stat st;

    path = Buffer(given);
    nameAt = given.length() - nameOf(given).length();

    if (stat(path.cStr(), &st) == 0) {
        struct tm tm;
        char stamp[32];

        fileBytes = (i64)st.st_size;
        localtime_r(&st.st_mtime, &tm);

        if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm)) {
            fileModified = Buffer(StringView(stamp));
        }
    }

    readFileContent(path, source.bytes);

    if (!sniff(nameOf(given), source.bytes, source.kind)) {
        fail(StringView(u8"not a PDF or a DjVu"));
    }
}

void ReadApp::accept() {
    void* item;

    while (results->tryDequeue(&item)) {
        Job* job = (Job*)item;
        ScopedGuard cleanup = [owner = job->owner] {
            delete owner;
        };

        --inFlight;

        if (job->opening) {
            take(*static_cast<Opened*>(job));
        } else {
            take(*static_cast<Rendered*>(job));
        }
    }
}

void ReadApp::take(Opened& result) {
    opening = false;

    if (!result.error.empty()) {
        problem = Buffer(StringView(result.error));
        TRACE(ui, StringView(StringBuilder() << StringView(u8"cannot open: ") << StringView(problem)));
        ui->requestFrame();

        return;
    }

    if (!result.pages) {
        problem = Buffer(StringView(u8"the document has no pages"));
        TRACE(ui, StringView(problem));
        ui->requestFrame();

        return;
    }

    opened = true;
    pageCount = result.pages;
    sizes.append(result.sizes.begin(), result.sizes.end());

    for (u32 page = 0; page < pageCount; page++) {
        thumbs.pushBack(pool->make<Thumb>());
    }

    TRACE(ui, StringView(StringBuilder() << StringView(u8"opened pages=") << (i64)pageCount));
    select(0);
}

void ReadApp::take(Rendered& result) {
    if (!result.timing.empty()) {
        ui->timing(StringView(result.timing));
    }

    if (result.thumb) {
        Thumb& thumb = *thumbs[result.page];

        thumb.loading = false;

        if (result.error.empty()) {
            setThumb(result.page, result);
        } else if (thumb.load != Load::Ready) {
            thumb.load = Load::Failed;
            thumb.error = Buffer(StringView(result.error));
            TRACE(ui, StringView(StringBuilder() << StringView(u8"no thumbnail ") << (i64)(result.page + 1) << StringView(u8": ") << StringView(result.error)));
        }

        return;
    }

    showPending = false;

    if (result.request != showRequest || result.page != current) {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"discarded page ") << (i64)(result.page + 1)));

        return;
    }

    Shown* next = new Shown(ui, result);

    replaceShown(next);

    if (!next->error.empty()) {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"cannot show page ") << (i64)(result.page + 1) << StringView(u8": ") << StringView(next->error)));
    } else {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"showing page ") << (i64)(result.page + 1) << StringView(u8" ") << (i64)next->width << StringView(u8"x") << (i64)next->height));
    }
}

void ReadApp::dispatch(Job* job) {
    ++inFlight;
    jobs->enqueue(job);
}

// the current page at the size the canvas fits it, and its thumbnail
// at the panel's width; thumbnails of the pages in view, missing ones
// first, then the ones rendered for a narrower panel
void ReadApp::submit() {
    if (!opening && !opened && problem.empty() && inFlight < workerCount) {
        ObjPool* owner = ObjPool::fromMemoryRaw();

        opening = true;
        TRACE(ui, StringView(u8"opening"));
        dispatch(owner->make<Opened>(owner, *this));
    }

    if (!opened) {
        return;
    }

    if (!showPending && inFlight < workerCount && canvasW && canvasH) {
        u32 width;
        u32 height;

        fitSize((u32)current, width, height);

        if (!shown || shown->page != current || shown->width != width || shown->height != height) {
            ObjPool* owner = ObjPool::fromMemoryRaw();

            showPending = true;
            TRACE(ui, StringView(StringBuilder() << StringView(u8"loading page ") << (i64)(current + 1)));
            dispatch(owner->make<Rendered>(owner, *this, (u32)current, width, height, false, showRequest));
        }
    }

    for (int pass = 0; pass < 2; pass++) {
        for (size_t index : wanted) {
            if (inFlight == workerCount) {
                return;
            }

            Thumb& thumb = *thumbs[index];

            if (thumb.loading || thumb.load == Load::Failed || (pass == 0) != (thumb.load == Load::None)) {
                continue;
            }

            if (thumb.load == Load::Ready && thumb.side * 4 >= thumbSide * 3) {
                continue;
            }

            u32 width;
            u32 height;

            thumbSize((u32)index, width, height);

            ObjPool* owner = ObjPool::fromMemoryRaw();

            thumb.loading = true;
            TRACE(ui, StringView(StringBuilder() << StringView(u8"loading thumbnail ") << (i64)(index + 1)));
            dispatch(owner->make<Rendered>(owner, *this, (u32)index, width, height, true, 0));
        }
    }
}

// the page as the canvas fits it whole, in pixels, within what a texture may be
void ReadApp::fitSize(u32 page, u32& width, u32& height) {
    float pw = max(1.f, sizes[(size_t)page * 2]);
    float ph = max(1.f, sizes[(size_t)page * 2 + 1]);
    float scale = min((float)canvasW / pw, (float)canvasH / ph);
    float limit = (float)max<u32>(1, maxSide);

    scale = min(scale, min(limit / pw, limit / ph));
    width = max<u32>(1, (u32)floorf(pw * scale + .5f));
    height = max<u32>(1, (u32)floorf(ph * scale + .5f));
}

// the page at the panel's width, in pixels
void ReadApp::thumbSize(u32 page, u32& width, u32& height) {
    float pw = max(1.f, sizes[(size_t)page * 2]);
    float ph = max(1.f, sizes[(size_t)page * 2 + 1]);

    width = thumbSide;
    height = max<u32>(1, (u32)floorf((float)thumbSide * ph / pw + .5f));
    height = min<u32>(height, max<u32>(1, maxSide));
}

void ReadApp::setThumb(u32 page, Rendered& result) {
    Thumb& thumb = *thumbs[page];

    if (thumb.width && thumb.side >= result.width) {
        return;
    }

    ImTextureRef texture = ui->loadTexture(result.width, result.height, result.rgba.data());

    if (thumb.width) {
        ui->releaseTexture(thumb.texture);
    }

    thumb.texture = texture;
    thumb.width = result.width;
    thumb.height = result.height;
    thumb.side = result.width;
    thumb.load = Load::Ready;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"thumbnail ") << (i64)(page + 1)));
}

void ReadApp::select(size_t index) {
    current = index;
    ++showRequest;
    showPending = false;
    scrollToCurrent = true;
    ui->requestFrame();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"selected page ") << (i64)(index + 1)));
}

void ReadApp::show(size_t index) {
    if (index == current && (!shown || shown->error.empty())) {
        return;
    }

    select(index);
}

void ReadApp::replaceShown(Shown* next) {
    Shown* previous = shown;

    shown = next;
    delete previous;
    ui->requestFrame();
}

void ReadApp::step(long delta) {
    if (!opened) {
        return;
    }

    long last = (long)pageCount - 1;
    long next = (long)current + delta;

    next = next < 0 ? 0 : next > last ? last : next;

    if ((size_t)next != current) {
        show((size_t)next);
    }
}

void ReadApp::keys() {
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_N)) {
        step(1);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_PageUp) || ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_P)) {
        step(-1);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Home) || (ImGui::IsKeyPressed(ImGuiKey_G) && !io.KeyShift)) {
        step(-(long)pageCount);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_End) || (ImGui::IsKeyPressed(ImGuiKey_G) && io.KeyShift)) {
        step((long)pageCount);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_F11)) {
        fullscreen = !fullscreen;
        ui->requestFullscreen(fullscreen);
        TRACE(ui, fullscreen ? StringView(u8"fullscreen on") : StringView(u8"fullscreen off"));
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

// the pages down the panel, each a row of the panel's width and the
// page's shape, the current one highlighted; a click selects a row
void ReadApp::drawPages() {
    float g = ui->px(gap);
    float innerW = max(1.f, ImGui::GetWindowWidth() - 2.f * g);
    float viewH = ImGui::GetWindowHeight();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();
    StringView trouble(problem);

    thumbSide = thumbSideFor(innerW);

    if (!trouble.empty()) {
        float h = font->CalcTextSizeA(fontSize, FLT_MAX, innerW, (const char*)trouble.begin(), (const char*)trouble.end()).y;

        ImGui::Dummy(ImVec2(innerW, h + 2.f * g));
        dl->AddText(font, fontSize, ImVec2(origin.x + g, origin.y + g), ImGui::GetColorU32(ImGuiCol_Text), (const char*)trouble.begin(), (const char*)trouble.end(), innerW);

        return;
    }

    auto rowHeight = [&](size_t i) {
        float pw = max(1.f, sizes[i * 2]);
        float ph = max(1.f, sizes[i * 2 + 1]);

        return max(1.f, floorf(innerW * (opened ? ph / pw : placeholderAspect) + .5f));
    };

    size_t count = thumbs.length();
    float total = g;
    float currentTop = total;
    float currentH = 0.f;

    for (size_t i = 0; i < count; i++) {
        float h = rowHeight(i);

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
    float top = g;

    for (size_t i = 0; i < count; i++) {
        Thumb& thumb = *thumbs[i];
        float h = rowHeight(i);
        float bottom = top + h;
        bool inView = bottom > scrollY && top < scrollY + viewH;

        if (inView) {
            ImVec2 p0(origin.x + g, origin.y + top);
            ImVec2 p1(p0.x + innerW, p0.y + h);

            wanted.pushBack(i);
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID((int)i);

            if (ImGui::InvisibleButton("##row", ImVec2(innerW, h))) {
                show(i);
            }

            ImGui::PopID();

            if (i == current) {
                dl->AddRectFilled(ImVec2(p0.x - g, p0.y - g), ImVec2(p1.x + g, p1.y + g), ImGui::GetColorU32(ImGuiCol_Header));
            }

            if (thumb.width) {
                dl->AddImage(thumb.texture, p0, p1);
            } else {
                const char* mark = thumb.load == Load::Failed ? "?" : "\xe2\x80\xa6";
                ImVec2 extent = ImGui::CalcTextSize(mark);

                dl->AddText(ImVec2(p0.x + (innerW - extent.x) / 2.f, p0.y + (h - extent.y) / 2.f), dimColor, mark);
            }
        }

        top = bottom + g;
    }
}

void ReadApp::drawInfo() {
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

    if (ImGui::CollapsingHeader("Document", ImGuiTreeNodeFlags_DefaultOpen) && table("document")) {
        row("Type", source.kind == Kind::Pdf ? StringView(u8"PDF") : StringView(u8"DjVu"));

        if (!problem.empty()) {
            row("Error", StringView(problem));
        }

        if (opened) {
            StringBuilder text;

            text << (i64)pageCount;
            row("Pages", StringView(text));
        }

        ImGui::EndTable();
    }

    if (opened) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Page", ImGuiTreeNodeFlags_DefaultOpen) && table("page")) {
            StringBuilder position;

            position << (i64)(current + 1) << StringView(u8" / ") << (i64)pageCount;
            row("Position", StringView(position));

            StringBuilder size;

            appendPoints(size, sizes[current * 2]);
            size << StringView(u8" \xc3\x97 ");
            appendPoints(size, sizes[current * 2 + 1]);
            size << StringView(u8" pt");
            row("Size", StringView(size));

            if (shown && shown->page == current) {
                if (!shown->error.empty()) {
                    row("Error", StringView(shown->error));
                } else {
                    StringBuilder text;

                    text << (i64)shown->width << StringView(u8" \xc3\x97 ") << (i64)shown->height;
                    row("Drawn", StringView(text));
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("File", ImGuiTreeNodeFlags_DefaultOpen) && table("file")) {
        StringView whole = StringView(path);

        row("Name", nameOf(whole));
        row("Folder", nameAt == 0 ? StringView(u8".") : nameAt == 1 ? StringView(u8"/") : StringView(whole.begin(), whole.begin() + nameAt - 1));

        if (fileBytes >= 0) {
            StringBuilder text;

            appendBytes(text, fileBytes);
            row("Size", StringView(text));
        }

        if (!fileModified.empty()) {
            row("Modified", StringView(fileModified));
        }

        ImGui::EndTable();
    }
}

// the current page, fitted whole into the canvas; the wheel turns pages
void ReadApp::drawCanvas() {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();

    if (size.x < 1.f || size.y < 1.f) {
        return;
    }

    canvasW = (u32)size.x;
    canvasH = (u32)size.y;
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft);

    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsItemHovered() && io.MouseWheel != 0.f) {
        step(io.MouseWheel < 0.f ? 1 : -1);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!opened || !shown || !shown->error.empty() || shown->page != current) {
        const char* text = !problem.empty() ? "cannot open this document" : shown && shown->page == current ? "cannot show this page" : "rendering";
        ImVec2 extent = ImGui::CalcTextSize(text);

        dl->AddText(ImVec2(origin.x + (size.x - extent.x) / 2.f, origin.y + (size.y - extent.y) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);

        return;
    }

    // the page as the canvas fits it now; the texture may be of another
    // size for a moment, until the render for this size arrives
    float pw = max(1.f, sizes[current * 2]);
    float ph = max(1.f, sizes[current * 2 + 1]);
    float scale = min(size.x / pw, size.y / ph);
    float dw = floorf(pw * scale + .5f);
    float dh = floorf(ph * scale + .5f);
    ImVec2 p0(origin.x + floorf((size.x - dw) / 2.f), origin.y + floorf((size.y - dh) / 2.f));
    ImVec2 p1(p0.x + dw, p0.y + dh);

    dl->AddImage(shown->texture, p0, p1);
}

void ReadApp::draw() {
    wanted.clear();
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::Begin("##read", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

    float sideW = floorf(vp->Size.x * sideShare);
    bool left = panel && !fullscreen;
    bool right = info && !fullscreen;

    if (left) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("pages", ImVec2(sideW, 0.f), 0, ImGuiWindowFlags_NoScrollbar);
        drawPages();
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

int mainRead(ObjPool& pool, int argc, char** argv) {
    if (argc != 2) {
        sysE << StringView(u8"usage: im read <file>") << endL;

        return 2;
    }

    ReadApp& app = *pool.make<ReadApp>();

    app.pool = &pool;
    app.open(StringView(argv[1]));

    Ui& ui = *Ui::create(pool, StringView(u8"read"), UiOptions{windowWidth, windowHeight});

    app.ui = &ui;
    app.maxSide = ui.maxTextureSide();
    app.startWorkers(pool);

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
