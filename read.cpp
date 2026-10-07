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
#include <imgui.h>
#include <string.h>

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
    constexpr Design pageGap = 12_d;
    constexpr Design shadowOffset = 3_d;
    constexpr Design scrollStep = 48_d;
    constexpr float screenShare = .9f;
    constexpr float placeholderAspect = 1.4142f;
    constexpr u32 thumbTexelsStep = 64;
    constexpr u32 thumbTexelsMin = 128;
    constexpr u32 thumbTexelsMax = 512;
    constexpr size_t workerCount = 4;
    constexpr u64 maxBytes = 1u << 30;
    constexpr u32 causeLimit = 256;
    constexpr ImU32 canvasBg = IM_COL32(46, 46, 52, 255);
    constexpr ImU32 paperColor = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 shadowNear = IM_COL32(0, 0, 0, 90);
    constexpr ImU32 shadowFar = IM_COL32(0, 0, 0, 40);

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
        Buffer rgba;
        Buffer timing;

        Rendered(ObjPool* owner, ReadApp& app, u32 page, u32 width, u32 height, bool thumb);
        void work(Document& document) override;
    };

    // the page's row in the list, at the list's width
    struct Thumb {
        Load load = Load::None;
        bool loading = false;
        ImTextureRef texture;
        u32 width = 0;
        u32 height = 0;
        Buffer error;
    };

    // the page on the canvas, at the size the layout gives it; a render
    // in flight, a texture of some size, or the error of the last attempt
    // at the size it was asked
    struct Sheet {
        bool loading = false;
        ImTextureRef texture;
        u32 width = 0;
        u32 height = 0;
        u32 failedWidth = 0;
        Buffer error;
    };

    struct Page {
        float pw = 1.f;
        float ph = 1.f;
        // the layout's size and place for it, in the canvas's content
        u32 width = 1;
        u32 height = 1;
        float top = 0.f;
        Thumb thumb;
        Sheet sheet;
    };

    struct ReadApp {
        Ui* ui = nullptr;
        ObjPool* pool = nullptr;
        Buffer path;
        Source source;
        Channel* jobs = nullptr;
        Channel* results = nullptr;
        Thread* workers[workerCount] = {};
        size_t inFlight = 0;
        bool opening = false;
        bool opened = false;
        Buffer problem;
        Vector<Page*> pages;
        Vector<size_t> wantedThumbs;
        Vector<size_t> wantedSheets;
        u32 thumbSide = thumbTexelsMin;
        u32 maxSide = 0;
        // the canvas: one column of the pages, scrolled as a whole
        float zoom = 0.f;
        float layoutW = 0.f;
        float totalH = 0.f;
        float viewH = 0.f;
        float scrollY = 0.f;
        float scrollTarget = -1.f;
        size_t keepFirst = 0;
        size_t keepLast = 0;
        // the page under the middle of the view; none before the first frame
        size_t current = (size_t)-1;
        bool followCurrent = true;
        bool fullscreen = false;
        bool panel = true;

        void startWorkers(ObjPool& pool);
        void stopWorkers();
        void open(StringView given);
        void accept();
        void take(Opened& opened);
        void take(Rendered& rendered);
        void submit();
        void dispatch(Job* job);
        void thumbSize(u32 page, u32& width, u32& height);
        void setThumb(u32 page, Rendered& result);
        void layout(float width);
        size_t pageAt(float y);
        void scrollTo(float y);
        void goTo(size_t index);
        void keys();
        void draw();
        void drawPages();
        void drawCanvas();
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

Rendered::Rendered(ObjPool* owner, ReadApp& app, u32 page_, u32 width_, u32 height_, bool thumb_)
    : Job(owner, app, false)
    , page(page_)
    , width(width_)
    , height(height_)
    , thumb(thumb_)
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

    for (Page* page : pages) {
        if (page->thumb.width) {
            ui->releaseTexture(page->thumb.texture);
        }

        if (page->sheet.width) {
            ui->releaseTexture(page->sheet.texture);
        }
    }
}

void ReadApp::open(StringView given) {
    path = Buffer(given);
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

    for (u32 i = 0; i < result.pages; i++) {
        Page* page = pool->make<Page>();

        page->pw = max(1.f, result.sizes[(size_t)i * 2]);
        page->ph = max(1.f, result.sizes[(size_t)i * 2 + 1]);
        pages.pushBack(page);
    }

    TRACE(ui, StringView(StringBuilder() << StringView(u8"opened pages=") << (i64)result.pages));
    ui->requestFrame();
}

void ReadApp::take(Rendered& result) {
    Page& page = *pages[result.page];

    if (!result.timing.empty()) {
        ui->timing(StringView(result.timing));
    }

    if (result.thumb) {
        Thumb& thumb = page.thumb;

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

    Sheet& sheet = page.sheet;

    sheet.loading = false;

    // a page that scrolled out of reach while it was drawn
    if (!opened || result.page < keepFirst || result.page > keepLast) {
        TRACE(ui, StringView(StringBuilder() << StringView(u8"discarded page ") << (i64)(result.page + 1)));

        return;
    }

    if (!result.error.empty()) {
        sheet.failedWidth = result.width;
        sheet.error = Buffer(StringView(result.error));
        TRACE(ui, StringView(StringBuilder() << StringView(u8"cannot show page ") << (i64)(result.page + 1) << StringView(u8": ") << StringView(result.error)));
        ui->requestFrame();

        return;
    }

    ImTextureRef texture = ui->loadTexture(result.width, result.height, result.rgba.data());

    if (sheet.width) {
        ui->releaseTexture(sheet.texture);
    }

    sheet.texture = texture;
    sheet.width = result.width;
    sheet.height = result.height;
    sheet.error = Buffer();
    TRACE(ui, StringView(StringBuilder() << StringView(u8"showing page ") << (i64)(result.page + 1) << StringView(u8" ") << (i64)result.width << StringView(u8"x") << (i64)result.height));
    ui->requestFrame();
}

void ReadApp::dispatch(Job* job) {
    ++inFlight;
    jobs->enqueue(job);
}

// the pages of the canvas at the layout's size, the ones in view first,
// then the thumbnails of the rows in view, missing ones first, then the
// ones drawn for a narrower list
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

    for (size_t index : wantedSheets) {
        if (inFlight == workerCount) {
            return;
        }

        Page& page = *pages[index];
        Sheet& sheet = page.sheet;

        if (sheet.loading || sheet.width == page.width || sheet.failedWidth == page.width) {
            continue;
        }

        ObjPool* owner = ObjPool::fromMemoryRaw();

        sheet.loading = true;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"loading page ") << (i64)(index + 1)));
        dispatch(owner->make<Rendered>(owner, *this, (u32)index, page.width, page.height, false));
    }

    for (int pass = 0; pass < 2; pass++) {
        for (size_t index : wantedThumbs) {
            if (inFlight == workerCount) {
                return;
            }

            Thumb& thumb = pages[index]->thumb;

            if (thumb.loading || thumb.load == Load::Failed || (pass == 0) != (thumb.load == Load::None)) {
                continue;
            }

            if (thumb.load == Load::Ready && thumb.width * 4 >= thumbSide * 3) {
                continue;
            }

            u32 width;
            u32 height;

            thumbSize((u32)index, width, height);

            ObjPool* owner = ObjPool::fromMemoryRaw();

            thumb.loading = true;
            TRACE(ui, StringView(StringBuilder() << StringView(u8"loading thumbnail ") << (i64)(index + 1)));
            dispatch(owner->make<Rendered>(owner, *this, (u32)index, width, height, true));
        }
    }
}

// the page at the list's width, in pixels
void ReadApp::thumbSize(u32 index, u32& width, u32& height) {
    Page& page = *pages[index];

    width = thumbSide;
    height = max<u32>(1, (u32)floorf((float)thumbSide * page.ph / page.pw + .5f));
    height = min<u32>(height, max<u32>(1, maxSide));
}

void ReadApp::setThumb(u32 index, Rendered& result) {
    Thumb& thumb = pages[index]->thumb;

    if (thumb.width && thumb.width >= result.width) {
        return;
    }

    ImTextureRef texture = ui->loadTexture(result.width, result.height, result.rgba.data());

    if (thumb.width) {
        ui->releaseTexture(thumb.texture);
    }

    thumb.texture = texture;
    thumb.width = result.width;
    thumb.height = result.height;
    thumb.load = Load::Ready;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"thumbnail ") << (i64)(index + 1)));
}

// One zoom for every page, the widest filling the canvas's width, as
// evince lays a document out: the pages one under another with a gap
// between, each centred, no page wider than a texture may be.
void ReadApp::layout(float width) {
    float g = ui->px(pageGap);
    float maxW = 1.f;
    float maxH = 1.f;

    for (Page* page : pages) {
        maxW = max(maxW, page->pw);
        maxH = max(maxH, page->ph);
    }

    float limit = (float)max<u32>(1, maxSide);

    layoutW = width;
    zoom = min(max(1.f, width - 2.f * g) / maxW, min(limit / maxW, limit / maxH));

    float y = g;

    for (Page* page : pages) {
        page->width = max<u32>(1, (u32)floorf(page->pw * zoom + .5f));
        page->height = max<u32>(1, (u32)floorf(page->ph * zoom + .5f));
        page->top = y;
        y += (float)page->height + g;
    }

    totalH = y;
}

// the page under a point of the content, or the one after the gap it is in
size_t ReadApp::pageAt(float y) {
    for (size_t i = 0; i < pages.length(); i++) {
        if (y < pages[i]->top + (float)pages[i]->height) {
            return i;
        }
    }

    return pages.length() - 1;
}

void ReadApp::scrollTo(float y) {
    scrollTarget = clampf(y, 0.f, max(0.f, totalH - viewH));
    ui->requestFrame();
}

// the page's top at the top of the view, its gap above it
void ReadApp::goTo(size_t index) {
    if (!opened) {
        return;
    }

    index = min(index, pages.length() - 1);
    scrollTo(pages[index]->top - ui->px(pageGap));
}

void ReadApp::keys() {
    ImGuiIO& io = ImGui::GetIO();
    float step = ui->px(scrollStep);

    size_t at = current == (size_t)-1 ? 0 : current;

    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_N)) {
        goTo(at + 1);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_P)) {
        goTo(at > 0 ? at - 1 : 0);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_J)) {
        scrollTo(scrollY + step);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_K)) {
        scrollTo(scrollY - step);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        scrollTo(scrollY + viewH * screenShare);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        scrollTo(scrollY - viewH * screenShare);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Home) || (ImGui::IsKeyPressed(ImGuiKey_G) && !io.KeyShift)) {
        scrollTo(0.f);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_End) || (ImGui::IsKeyPressed(ImGuiKey_G) && io.KeyShift)) {
        scrollTo(totalH);
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
}

// the pages down the list, each a row of the list's width and the page's
// shape, the current one highlighted and kept in view; a click goes to
// the page
void ReadApp::drawPages() {
    float g = ui->px(gap);
    float innerW = max(1.f, ImGui::GetWindowWidth() - 2.f * g);
    float listH = ImGui::GetWindowHeight();
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
        return max(1.f, floorf(innerW * (opened ? pages[i]->ph / pages[i]->pw : placeholderAspect) + .5f));
    };

    size_t count = pages.length();
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

    float listScroll = clampf(ImGui::GetScrollY(), 0.f, max(0.f, total - listH));

    if (followCurrent && count) {
        if (currentTop - g < listScroll) {
            ImGui::SetScrollY(currentTop - g);
        } else if (currentTop + currentH + g > listScroll + listH) {
            ImGui::SetScrollY(currentTop + currentH + g - listH);
        }

        followCurrent = false;
    }

    float top = g;

    for (size_t i = 0; i < count; i++) {
        Thumb& thumb = pages[i]->thumb;
        float h = rowHeight(i);
        float bottom = top + h;
        bool inView = bottom > listScroll && top < listScroll + listH;

        if (inView) {
            ImVec2 p0(origin.x + g, origin.y + top);
            ImVec2 p1(p0.x + innerW, p0.y + h);

            wantedThumbs.pushBack(i);
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID((int)i);

            if (ImGui::InvisibleButton("##row", ImVec2(innerW, h))) {
                goTo(i);
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

// The pages one under another, scrolled as one column: each a sheet with
// a shadow, blank until its render arrives. The pages in view and a
// view's worth around them are kept drawn; the rest let their textures
// go. A change of width lays the column out again and keeps the view on
// the same spot of the same page.
void ReadApp::drawCanvas() {
    ImVec2 win = ImGui::GetWindowPos();
    float viewW = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    viewH = ImGui::GetWindowHeight();

    if (!opened) {
        const char* text = !problem.empty() ? "cannot open this document" : "opening";
        ImVec2 extent = ImGui::CalcTextSize(text);

        dl->AddText(ImVec2(win.x + (viewW - extent.x) / 2.f, win.y + (viewH - extent.y) / 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);

        return;
    }

    float shadow = ui->px(shadowOffset);
    float before = ImGui::GetScrollY();

    if (viewW != layoutW) {
        // the same spot of the same page after the new layout
        bool again = layoutW > 0.f;
        size_t anchor = again ? pageAt(before) : 0;
        float within = again ? (before - pages[anchor]->top) / max(1.f, (float)pages[anchor]->height) : 0.f;

        layout(viewW);

        if (again && scrollTarget < 0.f) {
            scrollTarget = clampf(pages[anchor]->top + within * (float)pages[anchor]->height, 0.f, max(0.f, totalH - viewH));
        }

        // the sheets drawn for the old width stay until the new ones come
        for (Page* page : pages) {
            page->sheet.failedWidth = 0;
        }
    }

    ImGui::Dummy(ImVec2(viewW, totalH));

    if (scrollTarget >= 0.f) {
        ImGui::SetScrollY(scrollTarget);
        before = scrollTarget;
        scrollTarget = -1.f;
    }

    scrollY = clampf(before, 0.f, max(0.f, totalH - viewH));

    size_t middle = pageAt(scrollY + viewH / 2.f);

    if (middle != current) {
        current = middle;
        followCurrent = true;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"page ") << (i64)(current + 1)));
    }

    // what is drawn: the pages in view, then the ones a view away
    float reachTop = scrollY - viewH;
    float reachBottom = scrollY + 2.f * viewH;

    keepFirst = pages.length();
    keepLast = 0;

    for (size_t i = 0; i < pages.length(); i++) {
        Page& page = *pages[i];
        float top = page.top;
        float bottom = top + (float)page.height;
        bool inView = bottom > scrollY && top < scrollY + viewH;
        bool inReach = bottom > reachTop && top < reachBottom;

        if (inReach) {
            keepFirst = min(keepFirst, i);
            keepLast = max(keepLast, i);
        } else {
            if (page.sheet.width) {
                ui->releaseTexture(page.sheet.texture);
                page.sheet.width = 0;
                page.sheet.height = 0;
            }

            page.sheet.error = Buffer();
            page.sheet.failedWidth = 0;
        }

        if (!inView) {
            continue;
        }

        float x = win.x + floorf((viewW - (float)page.width) / 2.f);
        float y = win.y + top - scrollY;
        ImVec2 p0(x, y);
        ImVec2 p1(x + (float)page.width, y + (float)page.height);

        dl->AddRectFilled(ImVec2(p0.x + 2.f * shadow, p0.y + 2.f * shadow), ImVec2(p1.x + 2.f * shadow, p1.y + 2.f * shadow), shadowFar);
        dl->AddRectFilled(ImVec2(p0.x + shadow, p0.y + shadow), ImVec2(p1.x + shadow, p1.y + shadow), shadowNear);

        if (page.sheet.width) {
            dl->AddImage(page.sheet.texture, p0, p1);
        } else {
            dl->AddRectFilled(p0, p1, paperColor);
        }
    }

    for (size_t i = 0; i < pages.length(); i++) {
        Page& page = *pages[i];
        float top = page.top;
        float bottom = top + (float)page.height;

        if (bottom > scrollY && top < scrollY + viewH) {
            wantedSheets.pushBack(i);
        }
    }

    for (size_t i = keepFirst; i <= keepLast && i < pages.length(); i++) {
        Page& page = *pages[i];
        float top = page.top;
        float bottom = top + (float)page.height;

        if (!(bottom > scrollY && top < scrollY + viewH)) {
            wantedSheets.pushBack(i);
        }
    }
}

void ReadApp::draw() {
    wantedThumbs.clear();
    wantedSheets.clear();
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##read", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    float sideW = floorf(vp->Size.x * sideShare);

    if (panel && !fullscreen) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("pages", ImVec2(sideW, 0.f), 0, ImGuiWindowFlags_NoScrollbar);
        drawPages();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    // the scrollbar is always there, so the width the pages fit does not
    // depend on whether they overflow
    ImGui::PushStyleColor(ImGuiCol_ChildBg, canvasBg);
    ImGui::BeginChild("canvas", ImVec2(0.f, 0.f), 0, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    drawCanvas();
    ImGui::EndChild();
    ImGui::PopStyleColor();

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
