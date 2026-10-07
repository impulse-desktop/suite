#include "choose.h"

#include "ui.h"
#include "error.h"
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

// libmagic as wasm2c's C (ext/magic, under the name mime): the type of a
// file by its first bytes, for the filters that ask for a type and for the
// thumbnails of the grid. A trap throws out through decoder.cpp's handler.
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
    constexpr Design windowWidth = 820_d;
    constexpr Design windowHeight = 560_d;
    constexpr Design placesWidth = 160_d;
    constexpr Design gap = 4_d;
    constexpr Design cellWidth = 120_d;
    constexpr u32 thumbSide = 128;
    constexpr size_t workerCount = 4;
    constexpr size_t mimeHeadBytes = 4096;
    constexpr size_t mimeTypeLimit = 256;
    constexpr size_t fieldBytes = 4096;

    enum class Mode : u8 {
        Open,
        Save,
        Directory
    };

    enum class Load : u8 {
        None,
        Ready,
        Failed
    };

    // one pattern of a filter: a glob on the name, or a type with a glob
    struct Pattern {
        Buffer text;
        bool type;
    };

    struct Filter {
        Buffer label;
        Vector<Pattern*> patterns;
    };

    // one entry of the directory shown
    struct Item {
        Buffer name;
        Buffer mime;
        bool dir = false;
        bool hidden = false;
        i64 bytes = 0;
        i64 modified = 0;
        bool selected = false;
        bool loading = false;
        Load thumb = Load::None;
        ImTextureRef texture;
        u32 thumbW = 0;
        u32 thumbH = 0;
    };

    struct Place {
        Buffer label;
        Buffer path;
    };

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

        // the type of the file's first bytes, or nothing
        bool file(StringView path, Buffer& out) {
            Buffer name(path);
            size_t got = 0;

            try {
                ScopedFD fd(::open(name.cStr(), O_RDONLY | O_CLOEXEC));

                if (fd.get() < 0) {
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

            return type(StringView((const u8*)head.mutData(), got), out);
        }
    };

    struct ChooseApp;

    // what a worker does, and brings back to the screen
    struct Job: Runable {
        ObjPool* owner;
        ChooseApp* app;
        bool listing;
        Buffer error;

        Job(ObjPool* owner, ChooseApp& app, bool listing);
        void post();
    };

    // a directory read: its entries with their facts and types
    struct Listed final: Job {
        Buffer dir;
        u64 generation;
        Vector<Item*> items;

        Listed(ObjPool* owner, ChooseApp& app, StringView dir, u64 generation);
        void run() override;
    };

    // a thumbnail of an image of the directory shown
    struct Decoded final: Job {
        Buffer path;
        u64 generation;
        size_t index;
        Image* image = nullptr;

        Decoded(ObjPool* owner, ChooseApp& app, StringView path, u64 generation, size_t index);
        void run() override;
    };

    struct Worker final: Runable {
        Channel* jobs;

        explicit Worker(Channel* jobs);
        void run() override;
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

    struct ChooseApp {
        Ui* ui = nullptr;
        ObjPool* pool = nullptr;
        Mode mode = Mode::Open;
        bool multiple = false;
        Buffer title;
        Vector<Filter*> filters;
        size_t filter = 0;
        Vector<Place*> places;
        Channel* jobs = nullptr;
        Channel* results = nullptr;
        Thread* workers[workerCount] = {};
        size_t inFlight = 0;
        // the directory shown, its entries, and the generation of the
        // listing that holds them; a listing that comes back for an older
        // generation is dropped
        Buffer dir;
        u64 generation = 0;
        bool listing = false;
        Buffer problem;
        Vector<Item*> items;
        Vector<size_t> shown;
        Vector<size_t> wanted;
        size_t cursor = (size_t)-1;
        size_t anchor = (size_t)-1;
        bool showHidden = false;
        bool grid = false;
        bool scrollToCursor = false;
        // the one line: the directory, then what is typed after its slash,
        // a name to look for or the name to save under
        char field[fieldBytes] = {};
        Buffer fieldDir;
        bool fieldDirty = false;
        bool fieldFocus = true;
        bool fieldActive = false;
        bool askOverwrite = false;
        bool askingOverwrite = false;
        bool overwriteOk = false;
        bool done = false;
        int code = 1;
        Vector<Buffer*> chosen;

        void startWorkers(ObjPool& pool);
        void stopWorkers();
        void setup(StringView start);
        void readPlaces();
        void go(StringView to);
        void accept();
        void take(Listed& listed);
        void take(Decoded& decoded);
        void submit();
        void dispatch(Job* job);
        bool passes(const Item& item);
        void refilter();
        void setFieldDir(StringView to);
        void setFieldName(StringView name);
        StringView fieldName();
        void syncField();
        void select(size_t index, bool extend, bool toggle);
        void activate(size_t index);
        void enter();
        void complete();
        void finish(bool ok);
        void keys();
        void draw();
        void drawPlaces();
        void drawTable(float height);
        void drawGrid(float height);
        void drawLine();
    };

    StringView nameOf(StringView path) {
        size_t slash = path.length();

        while (slash > 0 && path[slash - 1] != '/') {
            slash--;
        }

        return StringView(path.begin() + slash, path.end());
    }

    bool lowerEq(u8 a, u8 b) {
        return (a | 0x20) == (b | 0x20) || a == b;
    }

    // a glob of * and ? against text, case folded for the ASCII letters
    bool globMatch(StringView glob, StringView text) {
        size_t g = 0;
        size_t t = 0;
        size_t star = (size_t)-1;
        size_t mark = 0;

        while (t < text.length()) {
            if (g < glob.length() && glob[g] == '*') {
                star = g++;
                mark = t;
            } else if (g < glob.length() && (glob[g] == '?' || lowerEq((u8)glob[g], (u8)text[t]))) {
                g++;
                t++;
            } else if (star != (size_t)-1) {
                g = star + 1;
                t = ++mark;
            } else {
                return false;
            }
        }

        while (g < glob.length() && glob[g] == '*') {
            g++;
        }

        return g == glob.length();
    }

    bool containsFolded(StringView text, StringView part) {
        if (part.empty()) {
            return true;
        }

        if (part.length() > text.length()) {
            return false;
        }

        for (size_t i = 0; i + part.length() <= text.length(); i++) {
            bool same = true;

            for (size_t j = 0; j < part.length() && same; j++) {
                same = lowerEq((u8)text[i + j], (u8)part[j]);
            }

            if (same) {
                return true;
            }
        }

        return false;
    }

    void appendBytes(StringBuilder& text, i64 bytes) {
        static const StringView units[] = {StringView(u8"KB"), StringView(u8"MB"), StringView(u8"GB"), StringView(u8"TB")};

        if (bytes < 1024) {
            text << bytes << StringView(u8" B");

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

    void appendStamp(StringBuilder& text, i64 when) {
        struct tm tm;
        char stamp[32];
        time_t t = (time_t)when;

        localtime_r(&t, &tm);

        if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm)) {
            text << StringView(stamp);
        }
    }

    // a path made absolute and plain: ~ is the home, . and .. fold, no
    // doubled or trailing slashes but the root's
    Buffer normalize(StringView given) {
        StringBuilder joined;

        if (given.startsWith(StringView(u8"~")) && (given.length() == 1 || given[1] == '/')) {
            const char* home = getenv("HOME");

            joined << StringView(home ? home : "/") << StringView(given.begin() + 1, given.end());
        } else if (!given.startsWith(StringView(u8"/"))) {
            char cwd[4096];

            joined << StringView(getcwd(cwd, sizeof(cwd)) ? cwd : "/") << StringView(u8"/") << given;
        } else {
            joined << given;
        }

        StringView whole(joined);
        Vector<StringView> parts;
        size_t from = 0;

        for (size_t i = 0; i <= whole.length(); i++) {
            if (i == whole.length() || whole[i] == '/') {
                StringView part(whole.begin() + from, whole.begin() + i);

                from = i + 1;

                if (part.empty() || part == StringView(u8".")) {
                    continue;
                }

                if (part == StringView(u8"..")) {
                    if (!parts.empty()) {
                        parts.popBack();
                    }

                    continue;
                }

                parts.pushBack(part);
            }
        }

        StringBuilder out;

        for (StringView part : parts) {
            out << StringView(u8"/") << part;
        }

        if (parts.empty()) {
            out << StringView(u8"/");
        }

        return Buffer(StringView(out));
    }

    bool isDir(StringView path) {
        struct stat st;
        Buffer name(path);

        return stat(name.cStr(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool exists(StringView path) {
        struct stat st;
        Buffer name(path);

        return stat(name.cStr(), &st) == 0;
    }

    Buffer joinPath(StringView dir, StringView name) {
        StringBuilder text;

        text << dir;

        if (!dir.endsWith(StringView(u8"/"))) {
            text << StringView(u8"/");
        }

        text << name;

        return Buffer(StringView(text));
    }
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

Job::Job(ObjPool* owner_, ChooseApp& app_, bool listing_)
    : owner(owner_)
    , app(&app_)
    , listing(listing_)
{
}

void Job::post() {
    Ui* notify = app->ui;

    app->results->enqueue(this);
    notify->requestFrame();
}

Listed::Listed(ObjPool* owner, ChooseApp& app, StringView dir_, u64 generation_)
    : Job(owner, app, true)
    , dir(dir_)
    , generation(generation_)
{
}

// the entries of the directory with their facts, every file with its type
void Listed::run() {
    try {
        Mime mime;

        listDir(StringView(dir), [&](const TPathInfo& info) {
            Item* item = owner->make<Item>();
            Buffer path = joinPath(StringView(dir), info.item);
            struct stat st;

            item->name = Buffer(info.item);
            item->hidden = !info.item.empty() && info.item[0] == '.';

            if (stat(path.cStr(), &st) == 0) {
                item->dir = S_ISDIR(st.st_mode);
                item->bytes = (i64)st.st_size;
                item->modified = (i64)st.st_mtime;
            } else {
                item->dir = info.isDir;
            }

            if (!item->dir) {
                mime.file(StringView(path), item->mime);
            }

            items.pushBack(item);
        });
    } catch (...) {
        error = Buffer(Exception::current());
    }

    post();
}

Decoded::Decoded(ObjPool* owner, ChooseApp& app, StringView path_, u64 generation_, size_t index_)
    : Job(owner, app, false)
    , path(path_)
    , generation(generation_)
    , index(index_)
{
}

void Decoded::run() {
    try {
        Buffer file;

        readFileContent(path, file);

        Image* whole = decode(*owner, StringView(file), nameOf(StringView(path)));

        image = whole->width() <= thumbSide && whole->height() <= thumbSide ? whole : owner->make<ScaledImage>(*whole, thumbSide);
    } catch (...) {
        error = Buffer(Exception::current());
    }

    post();
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

void ChooseApp::startWorkers(ObjPool& pool) {
    jobs = Channel::create(&pool, workerCount);
    results = Channel::create(&pool, workerCount);

    for (size_t i = 0; i < workerCount; i++) {
        workers[i] = Thread::create(&pool, *pool.make<Worker>(jobs));
    }

    pooledGuard(pool, [this] {
        stopWorkers();
    });
}

void ChooseApp::stopWorkers() {
    void* item;

    jobs->close();

    for (Thread* worker : workers) {
        worker->join();
    }

    while (results->tryDequeue(&item)) {
        delete ((Job*)item)->owner;
    }

    for (Item* item : items) {
        if (item->thumbW) {
            ui->releaseTexture(item->texture);
        }
    }
}

void ChooseApp::setup(StringView start) {
    readPlaces();
    go(start);
}

// The places GNOME and macOS put first: the home, then Desktop,
// Documents, Downloads, Music, Pictures and Videos (Movies on a Mac), each
// where ~/.config/user-dirs.dirs says or under the home by that name, each
// if it is there; the applications on a Mac; then the volumes mounted for
// the user, and the root.
void ChooseApp::readPlaces() {
    const char* home = getenv("HOME");
    Buffer homePath = normalize(StringView(home ? home : "/"));

    auto add = [&](StringView label, StringView path) {
        for (Place* place : places) {
            if (StringView(place->path) == path) {
                return;
            }
        }

        if (!isDir(path)) {
            return;
        }

        Place* place = pool->make<Place>();

        place->label = Buffer(label);
        place->path = Buffer(path);
        places.pushBack(place);
    };

    add(StringView(u8"Home"), StringView(homePath));

    // the user's directories as user-dirs.dirs names them, keyed by XDG_<KEY>_DIR
    struct Named {
        StringView key;
        StringView name;
        StringView macName;
        Buffer path;
    };

    Named standard[] = {
        {StringView(u8"DESKTOP"), StringView(u8"Desktop"), StringView(u8"Desktop"), Buffer()},
        {StringView(u8"DOCUMENTS"), StringView(u8"Documents"), StringView(u8"Documents"), Buffer()},
        {StringView(u8"DOWNLOAD"), StringView(u8"Downloads"), StringView(u8"Downloads"), Buffer()},
        {StringView(u8"MUSIC"), StringView(u8"Music"), StringView(u8"Music"), Buffer()},
        {StringView(u8"PICTURES"), StringView(u8"Pictures"), StringView(u8"Pictures"), Buffer()},
        {StringView(u8"VIDEOS"), StringView(u8"Videos"), StringView(u8"Movies"), Buffer()},
    };

    if (home) {
        Buffer config = joinPath(StringView(homePath), StringView(u8".config/user-dirs.dirs"));
        Buffer text;

        try {
            readFileContent(config, text);
        } catch (...) {
        }

        StringView whole(text);
        size_t from = 0;

        for (size_t i = 0; i <= whole.length(); i++) {
            if (i < whole.length() && whole[i] != '\n') {
                continue;
            }

            StringView line(whole.begin() + from, whole.begin() + i);

            from = i + 1;

            if (!line.startsWith(StringView(u8"XDG_"))) {
                continue;
            }

            size_t eq = 0;

            while (eq < line.length() && line[eq] != '=') {
                eq++;
            }

            if (eq + 2 >= line.length() || line[eq + 1] != '"' || line[line.length() - 1] != '"' || eq < 9) {
                continue;
            }

            StringView key(line.begin() + 4, line.begin() + eq - 4);
            StringView value(line.begin() + eq + 2, line.end() - 1);
            StringBuilder path;

            if (value.startsWith(StringView(u8"$HOME"))) {
                path << StringView(homePath) << StringView(value.begin() + 5, value.end());
            } else {
                path << value;
            }

            for (Named& named : standard) {
                if (named.key == key) {
                    named.path = normalize(StringView(path));
                }
            }
        }
    }

    for (Named& named : standard) {
        if (named.path.empty()) {
            named.path = joinPath(StringView(homePath), named.name);
        }

        if (!isDir(StringView(named.path))) {
            named.path = joinPath(StringView(homePath), named.macName);
        }

        add(nameOf(StringView(named.path)), StringView(named.path));
    }

#if defined(__APPLE__)
    add(StringView(u8"Applications"), StringView(u8"/Applications"));
#endif

    // the volumes mounted for the user, where the desktops put them
    const char* user = getenv("USER");
    StringView roots[] = {
        StringView(u8"/run/media/"),
        StringView(u8"/media/"),
        StringView(u8"/Volumes"),
    };

    for (StringView root : roots) {
        Buffer base(root);

        if (root.endsWith(StringView(u8"/")) && user) {
            base = joinPath(root, StringView(user));
        }

        if (!isDir(StringView(base))) {
            continue;
        }

        try {
            listDir(StringView(base), [&](const TPathInfo& info) {
                if (info.isDir && !info.item.empty() && info.item[0] != '.') {
                    Buffer path = joinPath(StringView(base), info.item);

                    add(info.item, StringView(path));
                }
            });
        } catch (...) {
        }
    }

    add(StringView(u8"/"), StringView(u8"/"));
}

// the directory shown, listed anew by a worker; the line follows it
void ChooseApp::go(StringView to) {
    Buffer plain = normalize(to);

    if (!isDir(StringView(plain))) {
        problem = Buffer(StringView(StringBuilder() << StringView(plain) << StringView(u8": not a directory")));
        TRACE(ui, StringView(problem));
        ui->requestFrame();

        return;
    }

    for (Item* item : items) {
        if (item->thumbW) {
            ui->releaseTexture(item->texture);
        }
    }

    dir = plain;
    items.clear();
    shown.clear();
    cursor = (size_t)-1;
    anchor = (size_t)-1;
    problem = Buffer();
    generation++;
    listing = true;
    setFieldDir(StringView(dir));
    TRACE(ui, StringView(StringBuilder() << StringView(u8"going ") << StringView(dir)));

    ObjPool* owner = ObjPool::fromMemoryRaw();

    dispatch(owner->make<Listed>(owner, *this, StringView(dir), generation));
    ui->requestFrame();
}

void ChooseApp::accept() {
    void* item;

    while (results->tryDequeue(&item)) {
        Job* job = (Job*)item;
        ScopedGuard cleanup = [owner = job->owner] {
            delete owner;
        };

        --inFlight;

        if (job->listing) {
            take(*static_cast<Listed*>(job));
        } else {
            take(*static_cast<Decoded*>(job));
        }
    }
}

void ChooseApp::take(Listed& listed) {
    if (listed.generation != generation) {
        return;
    }

    listing = false;

    if (!listed.error.empty()) {
        problem = Buffer(StringView(StringBuilder() << StringView(dir) << StringView(u8": ") << StringView(listed.error)));
        TRACE(ui, StringView(problem));
        ui->requestFrame();

        return;
    }

    for (Item* got : listed.items) {
        Item* item = pool->make<Item>();

        item->name.xchg(got->name);
        item->mime.xchg(got->mime);
        item->dir = got->dir;
        item->hidden = got->hidden;
        item->bytes = got->bytes;
        item->modified = got->modified;
        items.pushBack(item);
    }

    quickSort(items.mutBegin(), items.mutEnd(), [](const Item* a, const Item* b) {
        if (a->dir != b->dir) {
            return a->dir;
        }

        return StringView(a->name) < StringView(b->name);
    });

    TRACE(ui, StringView(StringBuilder() << StringView(u8"listed ") << (i64)items.length()));
    refilter();
    ui->requestFrame();
}

void ChooseApp::take(Decoded& decoded) {
    if (decoded.generation != generation || decoded.index >= items.length()) {
        return;
    }

    Item& item = *items[decoded.index];

    item.loading = false;

    if (!decoded.error.empty() || !decoded.image) {
        item.thumb = Load::Failed;
        TRACE(ui, StringView(StringBuilder() << StringView(u8"no thumbnail ") << StringView(item.name) << StringView(u8": ") << StringView(decoded.error)));

        return;
    }

    item.texture = ui->loadTexture(decoded.image->width(), decoded.image->height(), decoded.image->data());
    item.thumbW = decoded.image->width();
    item.thumbH = decoded.image->height();
    item.thumb = Load::Ready;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"thumbnail ") << StringView(item.name)));
    ui->requestFrame();
}

void ChooseApp::dispatch(Job* job) {
    ++inFlight;
    jobs->enqueue(job);
}

// the thumbnails the grid wants, in its order
void ChooseApp::submit() {
    if (!grid) {
        return;
    }

    for (size_t index : wanted) {
        if (inFlight == workerCount) {
            return;
        }

        Item& item = *items[index];

        if (item.dir || item.loading || item.thumb != Load::None || !StringView(item.mime).startsWith(StringView(u8"image/"))) {
            continue;
        }

        ObjPool* owner = ObjPool::fromMemoryRaw();

        item.loading = true;
        dispatch(owner->make<Decoded>(owner, *this, StringView(joinPath(StringView(dir), StringView(item.name))), generation, index));
    }
}

// whether the entry is shown: hidden ones on request, directories always
// but in the directory mode nothing else, files through the filter and
// through what is typed after the slash
bool ChooseApp::passes(const Item& item) {
    if (item.hidden && !showHidden) {
        return false;
    }

    StringView name(item.name);
    StringView typed = fieldName();

    if (item.dir) {
        return containsFolded(name, typed);
    }

    if (mode == Mode::Directory) {
        return false;
    }

    if (filter < filters.length()) {
        bool any = false;

        for (Pattern* pattern : filters[filter]->patterns) {
            any = any || (pattern->type ? globMatch(StringView(pattern->text), StringView(item.mime)) : globMatch(StringView(pattern->text), name));
        }

        if (!any) {
            return false;
        }
    }

    return containsFolded(name, typed);
}

void ChooseApp::refilter() {
    size_t was = cursor < items.length() ? cursor : (size_t)-1;

    shown.clear();

    for (size_t i = 0; i < items.length(); i++) {
        if (passes(*items[i])) {
            shown.pushBack(i);
        }
    }

    bool kept = false;

    for (size_t index : shown) {
        kept = kept || index == was;
    }

    if (!kept) {
        cursor = shown.empty() ? (size_t)-1 : shown[0];

        for (Item* item : items) {
            item->selected = false;
        }

        scrollToCursor = true;
    }

    TRACE(ui, StringView(StringBuilder() << StringView(u8"showing ") << (i64)shown.length()));
}

// the line holds the directory with a slash, then the name typed
void ChooseApp::setFieldDir(StringView to) {
    StringView name = fieldName();
    StringBuilder text;
    Buffer kept(name);

    text << to;

    if (!to.endsWith(StringView(u8"/"))) {
        text << StringView(u8"/");
    }

    text << StringView(kept);

    size_t n = min<size_t>(text.length(), fieldBytes - 1);

    memcpy(field, text.data(), n);
    field[n] = 0;
    fieldDir = Buffer(StringView((const u8*)field, strlen(field) - name.length()));
    fieldDirty = true;
}

void ChooseApp::setFieldName(StringView name) {
    StringBuilder text;

    text << StringView(fieldDir) << name;

    size_t n = min<size_t>(text.length(), fieldBytes - 1);

    memcpy(field, text.data(), n);
    field[n] = 0;
    fieldDirty = true;
}

StringView ChooseApp::fieldName() {
    StringView whole(field);
    size_t slash = whole.length();

    while (slash > 0 && whole[slash - 1] != '/') {
        slash--;
    }

    return StringView(whole.begin() + slash, whole.end());
}

// what the line says now: the directory's slash taken away is the
// directory above; a directory part that exists and differs from the one
// shown moves there; the name part filters the entries
void ChooseApp::syncField() {
    StringView whole(field);
    StringView was(fieldDir);

    if (was.length() == whole.length() + 1 && was.startsWith(whole) && was.endsWith(StringView(u8"/")) && StringView(dir).length() > 1) {
        field[0] = 0;
        fieldDir = Buffer();
        go(StringView(joinPath(StringView(dir), StringView(u8".."))));

        return;
    }

    StringView name = fieldName();
    StringView dirPart(whole.begin(), whole.end() - name.length());

    if (dirPart != StringView(fieldDir)) {
        if (!dirPart.empty()) {
            Buffer plain = normalize(dirPart);

            if (isDir(StringView(plain)) && StringView(plain) != StringView(dir)) {
                go(StringView(plain));

                return;
            }
        }

        fieldDir = Buffer(dirPart);
    }

    refilter();
}

void ChooseApp::select(size_t index, bool extend, bool toggle) {
    if (index >= items.length()) {
        return;
    }

    if (!multiple || (!extend && !toggle)) {
        for (Item* item : items) {
            item->selected = false;
        }

        items[index]->selected = mode != Mode::Save || !items[index]->dir;
        anchor = index;
    } else if (toggle) {
        items[index]->selected = !items[index]->selected;
        anchor = index;
    } else {
        size_t from = anchor < items.length() ? anchor : index;
        bool inside = false;

        for (size_t i : shown) {
            if (i == from || i == index) {
                items[i]->selected = true;
                inside = from == index ? false : !inside;
            } else if (inside) {
                items[i]->selected = true;
            }
        }
    }

    cursor = index;

    if (mode == Mode::Save && !items[index]->dir) {
        setFieldName(StringView(items[index]->name));
    }

    // typing goes on in the line after a click in the list
    fieldFocus = true;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"selected ") << StringView(items[index]->name)));
    ui->requestFrame();
}

// a directory entered, a file chosen
void ChooseApp::activate(size_t index) {
    if (index >= items.length()) {
        return;
    }

    Item& item = *items[index];

    if (item.dir) {
        go(StringView(joinPath(StringView(dir), StringView(item.name))));

        return;
    }

    if (mode == Mode::Directory) {
        return;
    }

    select(index, false, false);
    finish(true);
}

// Enter on the line: a directory typed goes there, a name of an entry
// takes it, a name to save under is the answer; nothing typed is the
// entry under the cursor, or in the directory mode the directory itself
void ChooseApp::enter() {
    StringView name = fieldName();

    if (name.empty()) {
        if (mode == Mode::Directory && !(cursor < items.length() && items[cursor]->selected)) {
            finish(true);
        } else if (cursor < items.length()) {
            activate(cursor);
        }

        return;
    }

    Buffer target = joinPath(StringView(fieldDir), name);

    if (isDir(StringView(target))) {
        go(StringView(target));

        return;
    }

    if (mode == Mode::Save) {
        finish(true);

        return;
    }

    for (size_t index : shown) {
        if (StringView(items[index]->name) == name) {
            activate(index);

            return;
        }
    }

    if (cursor < items.length()) {
        activate(cursor);
    }
}

// Tab on the line: the name grows to the longest start the shown entries share
void ChooseApp::complete() {
    StringView typed = fieldName();
    StringView common;
    bool first = true;

    for (size_t index : shown) {
        StringView name(items[index]->name);

        if (first) {
            common = name;
            first = false;

            continue;
        }

        size_t n = 0;

        while (n < common.length() && n < name.length() && lowerEq((u8)common[n], (u8)name[n])) {
            n++;
        }

        common = StringView(common.begin(), common.begin() + n);
    }

    if (!first && common.length() > typed.length()) {
        Buffer grown(common);

        setFieldName(StringView(grown));
        refilter();
    }
}

// the answer: the paths chosen, one a line, or nothing
void ChooseApp::finish(bool ok) {
    if (!ok) {
        code = 1;
        done = true;
        TRACE(ui, StringView(u8"cancelled"));

        return;
    }

    chosen.clear();

    if (mode == Mode::Save) {
        StringView name = fieldName();

        if (name.empty() || !isDir(StringView(fieldDir))) {
            return;
        }

        Buffer target = joinPath(StringView(normalize(StringView(fieldDir))), name);

        if (!overwriteOk && exists(StringView(target))) {
            askOverwrite = true;
            ui->requestFrame();

            return;
        }

        overwriteOk = false;
        chosen.pushBack(pool->make<Buffer>(StringView(target)));
    } else if (mode == Mode::Directory) {
        bool any = false;

        for (Item* item : items) {
            if (item->selected && item->dir) {
                chosen.pushBack(pool->make<Buffer>(StringView(joinPath(StringView(dir), StringView(item->name)))));
                any = true;
            }
        }

        if (!any) {
            chosen.pushBack(pool->make<Buffer>(StringView(dir)));
        }
    } else {
        for (Item* item : items) {
            if (item->selected && !item->dir) {
                chosen.pushBack(pool->make<Buffer>(StringView(joinPath(StringView(dir), StringView(item->name)))));
            }
        }

        if (chosen.empty()) {
            return;
        }
    }

    code = 0;
    done = true;
    TRACE(ui, StringView(StringBuilder() << StringView(u8"chosen ") << (i64)chosen.length()));
}

void ChooseApp::keys() {
    ImGuiIO& io = ImGui::GetIO();

    if (askingOverwrite) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        finish(false);
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_H)) {
        showHidden = !showHidden;
        TRACE(ui, showHidden ? StringView(u8"hidden on") : StringView(u8"hidden off"));
        refilter();
    }

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
        grid = !grid;
        TRACE(ui, grid ? StringView(u8"grid on") : StringView(u8"grid off"));
        scrollToCursor = true;
    }

    // with the line active, Backspace and Enter are its own; elsewhere
    // Backspace on an empty name is the directory above, Enter the entry
    if (!fieldActive && ImGui::IsKeyPressed(ImGuiKey_Backspace) && fieldName().empty() && !io.KeyCtrl && StringView(dir).length() > 1) {
        go(StringView(joinPath(StringView(dir), StringView(u8".."))));
    }

    if (!fieldActive && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
        enter();
    }

    // the arrows walk the entries shown: the first press takes the one
    // under the cursor, the next ones move it
    bool downKey = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    bool upKey = ImGui::IsKeyPressed(ImGuiKey_UpArrow);

    if ((downKey || upKey) && !shown.empty()) {
        size_t at = 0;
        bool standing = false;

        for (size_t i = 0; i < shown.length(); i++) {
            if (shown[i] == cursor) {
                at = i;
                standing = items[cursor]->selected;
            }
        }

        if (standing && downKey && at + 1 < shown.length()) {
            at++;
        } else if (standing && upKey && at > 0) {
            at--;
        }

        select(shown[at], io.KeyShift, false);
        scrollToCursor = true;
    }
}

void ChooseApp::drawPlaces() {
    for (size_t i = 0; i < places.length(); i++) {
        Place& place = *places[i];
        bool here = StringView(place.path) == StringView(dir);

        ImGui::PushID((int)i);

        if (ImGui::Selectable((const char*)place.label.cStr(), here)) {
            go(StringView(place.path));
        }

        ImGui::PopID();
    }
}

// the entries as rows: name, size, modified; a click selects, a double
// click enters or takes
void ChooseApp::drawTable(float height) {
    if (!ImGui::BeginTable("entries", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0.f, height))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 5.f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableHeadersRow();

    ImGuiIO& io = ImGui::GetIO();
    wanted.clear();

    for (size_t index : shown) {
        Item& item = *items[index];

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID((int)index);

        StringBuilder label;

        label << StringView(item.name);

        if (item.dir) {
            label << StringView(u8"/");
        }

        if (ImGui::Selectable((const char*)label.cStr(), item.selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                activate(index);
            } else {
                select(index, io.KeyShift, io.KeyCtrl);
            }
        }

        if (scrollToCursor && index == cursor) {
            ImGui::SetScrollHereY();
            scrollToCursor = false;
        }

        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);

        if (!item.dir) {
            StringBuilder size;

            appendBytes(size, item.bytes);
            ImGui::TextUnformatted((const char*)size.data(), (const char*)size.data() + size.length());
        }

        ImGui::TableSetColumnIndex(2);

        StringBuilder stamp;

        appendStamp(stamp, item.modified);
        ImGui::TextUnformatted((const char*)stamp.data(), (const char*)stamp.data() + stamp.length());
    }

    ImGui::EndTable();
}

// the entries as cells: a thumbnail for an image, its type for another
// file, a folder for a directory, the name under each
void ChooseApp::drawGrid(float height) {
    float g = ui->px(gap);
    float cell = ui->px(cellWidth);
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float cellH = cell + lineH + g;

    ImGui::BeginChild("grid", ImVec2(0.f, height), 0, 0);

    float innerW = max(1.f, ImGui::GetContentRegionAvail().x);
    size_t columns = max<size_t>(1, (size_t)(innerW / (cell + g)));
    size_t rows = (shown.length() + columns - 1) / columns;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float scrollY = ImGui::GetScrollY();
    float viewH = ImGui::GetWindowHeight();
    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 frame = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 chosenColor = ImGui::GetColorU32(ImGuiCol_Header);

    ImGui::Dummy(ImVec2(innerW, rows * cellH + g));
    wanted.clear();

    for (size_t n = 0; n < shown.length(); n++) {
        size_t index = shown[n];
        Item& item = *items[index];
        float x = origin.x + g + (float)(n % columns) * (cell + g);
        float y = origin.y + g + (float)(n / columns) * cellH;
        bool inView = y + cellH > origin.y + scrollY && y < origin.y + scrollY + viewH;

        if (scrollToCursor && index == cursor) {
            ImGui::SetScrollY(max(0.f, y - origin.y - (viewH - cellH) / 2.f));
            scrollToCursor = false;
        }

        if (!inView) {
            continue;
        }

        wanted.pushBack(index);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID((int)index);

        if (ImGui::InvisibleButton("##cell", ImVec2(cell, cellH))) {
            select(index, io.KeyShift, io.KeyCtrl);
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            activate(index);
        }

        ImGui::PopID();

        ImVec2 p0(x, y);
        ImVec2 p1(x + cell, y + cell);

        if (item.selected) {
            dl->AddRectFilled(ImVec2(p0.x - 2.f, p0.y - 2.f), ImVec2(p1.x + 2.f, y + cellH), chosenColor, 3.f);
        }

        dl->AddRectFilled(p0, p1, frame, 3.f);

        if (item.thumbW) {
            float scale = min(cell / (float)item.thumbW, cell / (float)item.thumbH);
            float w = (float)item.thumbW * scale;
            float h = (float)item.thumbH * scale;
            ImVec2 t0(x + (cell - w) / 2.f, y + (cell - h) / 2.f);

            dl->AddImage(item.texture, t0, ImVec2(t0.x + w, t0.y + h));
        } else {
            const char* mark = item.dir ? "folder" : item.thumb == Load::Failed ? "?" : StringView(item.mime).startsWith(StringView(u8"image/")) ? "\xe2\x80\xa6" : (const char*)item.mime.cStr();
            ImVec2 extent = ImGui::CalcTextSize(mark);

            dl->AddText(ImVec2(x + max(0.f, (cell - extent.x) / 2.f), y + (cell - extent.y) / 2.f), dim, mark);
        }

        ImVec2 extent = ImGui::CalcTextSize((const char*)item.name.data(), (const char*)item.name.data() + item.name.length());
        float nameX = x + max(0.f, (cell - extent.x) / 2.f);

        dl->PushClipRect(ImVec2(x, y + cell), ImVec2(x + cell, y + cellH), true);
        dl->AddText(ImVec2(nameX, y + cell + g / 2.f), ImGui::GetColorU32(ImGuiCol_Text), (const char*)item.name.data(), (const char*)item.name.data() + item.name.length());
        dl->PopClipRect();
    }

    ImGui::EndChild();
}

// The line is ImGui's while it is active: what it typed comes to the app
// on every edit, and what the app set (a directory entered, a name taken,
// a completion) goes back through the callback, never behind its back.
int fieldCallback(ImGuiInputTextCallbackData* data) {
    ChooseApp* app = (ChooseApp*)data->UserData;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        size_t n = min<size_t>((size_t)data->BufTextLen, fieldBytes - 1);

        memcpy(app->field, data->Buf, n);
        app->field[n] = 0;
        app->fieldDirty = false;
        app->syncField();
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        app->complete();
    }

    if (app->fieldDirty) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, app->field);
        data->CursorPos = data->BufTextLen;
        data->SelectionStart = data->SelectionEnd = data->CursorPos;
        app->fieldDirty = false;
    }

    return 0;
}

// the line: the path, then the filter and the buttons
void ChooseApp::drawLine() {
    ImGuiStyle& style = ImGui::GetStyle();
    const char* action = mode == Mode::Save ? "Save" : mode == Mode::Directory ? "Choose" : "Open";
    float buttons = ImGui::CalcTextSize(action).x + ImGui::CalcTextSize("Cancel").x + 4.f * style.FramePadding.x + 2.f * style.ItemSpacing.x;
    float filterW = 0.f;

    if (filters.length() > 1) {
        for (Filter* f : filters) {
            filterW = max(filterW, ImGui::CalcTextSize((const char*)f->label.cStr()).x);
        }

        filterW += 2.f * style.FramePadding.x + ImGui::GetFrameHeight() + style.ItemSpacing.x;
    }

    ImGui::SetNextItemWidth(max(80.f, ImGui::GetContentRegionAvail().x - buttons - filterW));

    if (fieldFocus) {
        ImGui::SetKeyboardFocusHere();
        fieldFocus = false;
    }

    // Up and Down stay the list's: the line's history would take them
    bool entered = ImGui::InputText("##path", field, sizeof(field), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways, fieldCallback, this);

    fieldActive = ImGui::IsItemActive();

    if (entered) {
        enter();
        fieldFocus = true;
    }

    if (filters.length() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(filterW - style.ItemSpacing.x);

        if (ImGui::BeginCombo("##filter", (const char*)filters[filter]->label.cStr())) {
            for (size_t i = 0; i < filters.length(); i++) {
                if (ImGui::Selectable((const char*)filters[i]->label.cStr(), i == filter)) {
                    filter = i;
                    TRACE(ui, StringView(StringBuilder() << StringView(u8"filter ") << StringView(filters[i]->label)));
                    refilter();
                }
            }

            ImGui::EndCombo();
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel")) {
        finish(false);
    }

    ImGui::SameLine();

    if (ImGui::Button(action)) {
        enter();
    }
}

// the blocks are the gap from the edges and from each other; the places
// are padded by it too, as the entries are by their cells
void ChooseApp::draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    float g = ui->px(gap);

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(g, g));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(g, g));
    ImGui::Begin("##choose", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

    if (!title.empty()) {
        ImGui::TextUnformatted((const char*)title.data(), (const char*)title.data() + title.length());
        ImGui::Spacing();
    }

    float lineH = ImGui::GetFrameHeight() + g;
    float bodyH = max(1.f, ImGui::GetContentRegionAvail().y - lineH);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("places", ImVec2(ui->px(placesWidth), bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    drawPlaces();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginGroup();

    if (!problem.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted((const char*)problem.data(), (const char*)problem.data() + problem.length());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.f, max(0.f, bodyH - ImGui::GetTextLineHeightWithSpacing() * 2.f)));
    } else if (grid) {
        drawGrid(bodyH);
    } else {
        drawTable(bodyH);
    }

    ImGui::EndGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    drawLine();
    ImGui::PopStyleVar();

    if (askOverwrite) {
        ImGui::OpenPopup("Replace");
        askOverwrite = false;
        askingOverwrite = true;
        TRACE(ui, StringView(u8"asking to replace"));
    }

    if (ImGui::BeginPopupModal("Replace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        StringView name = fieldName();

        ImGui::Text("%.*s is there already. Replace it?", (int)name.length(), (const char*)name.begin());
        ImGui::Spacing();

        bool yes = ImGui::Button("Replace") || ImGui::IsKeyPressed(ImGuiKey_Enter);

        ImGui::SameLine();

        bool no = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);

        if (yes) {
            askingOverwrite = false;
            overwriteOk = true;
            ImGui::CloseCurrentPopup();
            finish(true);
        } else if (no) {
            askingOverwrite = false;
            ImGui::CloseCurrentPopup();
            fieldFocus = true;
        }

        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

int mainChoose(ObjPool& pool, int argc, char** argv) {
    ChooseApp& app = *pool.make<ChooseApp>();
    StringView start;
    bool usage = false;

    app.pool = &pool;

    for (int i = 1; i < argc && !usage; i++) {
        StringView arg(argv[i]);

        if (arg == StringView(u8"--save")) {
            app.mode = Mode::Save;
        } else if (arg == StringView(u8"--directory")) {
            app.mode = Mode::Directory;
        } else if (arg == StringView(u8"--multiple")) {
            app.multiple = true;
        } else if ((arg == StringView(u8"--title") || arg == StringView(u8"--name") || arg == StringView(u8"--filter")) && i + 1 < argc) {
            StringView value(argv[++i]);

            if (arg == StringView(u8"--title")) {
                app.title = Buffer(value);
            } else if (arg == StringView(u8"--name")) {
                app.setFieldName(value);
            } else {
                // Label|*.png|*.jpg|image/*: a pattern with a slash is a type
                Filter* f = pool.make<Filter>();
                size_t from = 0;

                for (size_t k = 0; k <= value.length(); k++) {
                    if (k == value.length() || value[k] == '|') {
                        StringView part(value.begin() + from, value.begin() + k);

                        from = k + 1;

                        if (f->label.empty() && f->patterns.empty()) {
                            f->label = Buffer(part);
                        } else if (!part.empty()) {
                            Pattern* p = pool.make<Pattern>();

                            p->text = Buffer(part);
                            p->type = false;

                            for (size_t c = 0; c < part.length(); c++) {
                                p->type = p->type || part[c] == '/';
                            }

                            f->patterns.pushBack(p);
                        }
                    }
                }

                if (f->patterns.empty()) {
                    usage = true;
                } else {
                    app.filters.pushBack(f);
                }
            }
        } else if (!arg.startsWith(StringView(u8"-")) && start.empty()) {
            start = arg;
        } else {
            usage = true;
        }
    }

    if (usage) {
        sysE << StringView(u8"usage: im choose [--save] [--directory] [--multiple] [--title T] [--name N] [--filter 'Label|*.ext|type/*']... [DIR]") << endL;

        return 2;
    }

    Ui& ui = *Ui::create(pool, StringView(u8"choose"), UiOptions{windowWidth, windowHeight});

    app.ui = &ui;
    app.startWorkers(pool);
    app.setup(start.empty() ? StringView(u8".") : start);

    auto body = makeRunable([&] {
        UiEvent event;

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close) {
                return;
            }

            app.keys();
            app.accept();
            app.draw();
            app.submit();

            if (app.done) {
                return;
            }
        }
    });

    int result = ui.run(body);

    if (result != 0) {
        return result;
    }

    for (Buffer* path : app.chosen) {
        sysO << StringView(*path) << endL;
    }

    return app.code;
}
