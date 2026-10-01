#include "view.h"

#include "ui.h"
#include "error.h"
#include "timing.h"
#include "decoder.h"

#include <std/sys/fs.h>
#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/alg/qsort.h>
#include <std/sys/throw.h>
#include <std/alg/minmax.h>
#include <std/lib/vector.h>
#include <std/str/builder.h>
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

    enum class Load : u8 {
        None,
        Ready,
        Failed
    };

    struct Entry {
        Buffer path;
        size_t nameAt = 0;
        Buffer file;
        Buffer error;
        Load thumb = Load::None;
        ImTextureRef thumbTex;
        u32 thumbW = 0;
        u32 thumbH = 0;
        u32 thumbSide = 0;
        StringView name() const;
    };

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

    void readAll(Vector<Entry*>& entries) {
        for (Entry* entry : entries) {
            Buffer path(StringView(entry->path));

            try {
                readFileContent(path, entry->file);
            } catch (...) {
                entry->error = Buffer(Exception::current());
            }
        }
    }

    void decodeEntry(Ui& ui, const Entry& entry, u32 side, DecodedImage& out) {
        u64 began = monotonicNowUs();

        if (!entry.error.empty()) {
            fail(StringView(entry.error));
        }

        decode(StringView(entry.file), entry.name(), out);

        u64 decoded = monotonicNowUs();

        shrinkToSide(out, side);

        StringBuilder text;

        text << StringView(u8"im decode ") << entry.name() << StringView(u8": decode ") << MS{decoded - began};
        text << StringView(u8" shrink ") << MS{monotonicNowUs() - decoded};
        ui.timing(StringView(text));
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

    void addFile(ObjPool& pool, Vector<Entry*>& entries, StringView path) {
        entries.pushBack(makeEntry(pool, path));
    }

    void addDirectory(ObjPool& pool, Vector<Entry*>& entries, StringView dir) {
        Vector<Entry*> found;

        listDir(dir, [&](const TPathInfo& info) {
            if (!info.isDir && imageName(info.item)) {
                StringBuilder path;

                path << dir;

                if (!dir.endsWith(StringView(u8"/"))) {
                    path << StringView(u8"/");
                }

                path << info.item;
                found.pushBack(makeEntry(pool, StringView(path)));
            }
        });
        quickSort(found.mutBegin(), found.mutEnd(), [](const Entry* a, const Entry* b) {
            return a->name() < b->name();
        });
        entries.append(found.begin(), found.end());
    }

    u32 thumbSideFor(float innerW) {
        u32 side = (u32)ceilf(innerW / (float)thumbTexelsStep) * thumbTexelsStep;

        return side < thumbTexelsMin ? thumbTexelsMin : side > thumbTexelsMax ? thumbTexelsMax : side;
    }

    float rowHeightFor(const Entry& entry, float innerW) {
        float aspect = entry.thumb == Load::Ready && entry.thumbW ? (float)entry.thumbH / (float)entry.thumbW : placeholderAspect;

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

    struct ViewApp {
        Ui* ui = nullptr;
        Vector<Entry*> entries;
        size_t current = 0;
        u32 maxSide = 0;
        Load shown = Load::None;
        size_t shownIndex = (size_t)-1;
        ImTextureRef tex;
        u32 texW = 0;
        u32 texH = 0;
        Buffer shownError;
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
        i64 fileBytes = -1;
        Buffer fileModified;

        void loadThumb(size_t index, u32 side);
        void show(size_t index);
        void dropShown();
        void step(long delta);
        void setZoom(float value);
        void fitView();
        void statFile();
        void keys();
        void draw();
        void drawGallery();
        void drawCanvas();
        void drawInfo();
    };

    int showError(Ui& ui, StringView message) {
        UiEvent event;

        ui.open({480_d, 180_d});

        while (ui.next(event)) {
            if (event.kind == UiEvent::Kind::Close || ui.drawErrorPanel(message)) {
                return 0;
            }
        }

        return 0;
    }
}

StringView Entry::name() const {
    StringView whole = StringView(path);

    return StringView(whole.begin() + nameAt, whole.end());
}

void ViewApp::loadThumb(size_t index, u32 side) {
    Entry& entry = *entries[index];
    DecodedImage image;

    try {
        decodeEntry(*ui, entry, side, image);
    } catch (...) {
        Buffer error(Exception::current());

        entry.thumb = Load::Failed;
        ui->trace(StringView(StringBuilder() << StringView(u8"no thumbnail ") << entry.name() << StringView(u8": ") << StringView(error)));

        return;
    }

    if (entry.thumb == Load::Ready) {
        ui->releaseTexture(entry.thumbTex);
    }

    entry.thumbTex = ui->loadTexture(image.width, image.height, image.rgba.data());
    entry.thumbW = image.width;
    entry.thumbH = image.height;
    entry.thumbSide = side;
    entry.thumb = Load::Ready;
    ui->trace(StringView(StringBuilder() << StringView(u8"thumbnail ") << entry.name()));
}

void ViewApp::show(size_t index) {
    current = index;

    Entry& entry = *entries[index];

    ui->trace(StringView(StringBuilder() << StringView(u8"selected ") << entry.name()));
    statFile();

    DecodedImage image;

    try {
        decodeEntry(*ui, entry, maxSide, image);
    } catch (...) {
        dropShown();
        shown = Load::Failed;
        shownIndex = index;
        shownError = Buffer(Exception::current());
        ui->trace(StringView(StringBuilder() << StringView(u8"cannot show ") << entry.name() << StringView(u8": ") << StringView(shownError)));

        return;
    }

    dropShown();
    tex = ui->loadTexture(image.width, image.height, image.rgba.data());
    texW = image.width;
    texH = image.height;
    shown = Load::Ready;
    shownIndex = index;
    ui->trace(StringView(StringBuilder() << StringView(u8"showing ") << entry.name() << StringView(u8" ") << (i64)texW << StringView(u8"x") << (i64)texH));
}

void ViewApp::dropShown() {
    if (shown == Load::Ready) {
        ui->releaseTexture(tex);
    }

    shown = Load::None;
}

void ViewApp::step(long delta) {
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
    ui->trace(StringView(StringBuilder() << StringView(u8"zoom ") << (i64)(zoom * 100.f + .5f)));
}

void ViewApp::fitView() {
    fit = true;
    panX = 0.f;
    panY = 0.f;
    ui->trace(StringView(u8"fit"));
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
        ui->trace(fullscreen ? StringView(u8"fullscreen on") : StringView(u8"fullscreen off"));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        rotation = (rotation + (io.KeyShift ? 3 : 1)) % 4;
        ui->trace(StringView(StringBuilder() << StringView(u8"rotated ") << (i64)(rotation * 90)));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        panel = !panel;
        ui->trace(panel ? StringView(u8"panel on") : StringView(u8"panel off"));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_I)) {
        info = !info;
        ui->trace(info ? StringView(u8"info on") : StringView(u8"info off"));
    }
}

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

void ViewApp::drawGallery() {
    float g = ui->px(gap);
    float innerW = max(1.f, ImGui::GetWindowWidth() - 2.f * g);
    u32 side = thumbSideFor((innerW + 2.f * g) * (1.f + bulge));
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
    float top = g;

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

        if (inView && entry.thumb == Load::Ready && entry.thumbSide * 4 < side * 3) {
            loadThumb(i, side);
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

        if (entry.thumb != Load::Ready) {
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
    const Entry& entry = *entries[current];
    bool ready = shown == Load::Ready && shownIndex == current;

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
        if (shown == Load::Failed && shownIndex == current) {
            row("Error", StringView(shownError));
        }

        if (ready) {
            StringBuilder text;
            i64 tenths = ((i64)texW * (i64)texH + 50000) / 100000;

            text << (i64)texW << StringView(u8" \xc3\x97 ") << (i64)texH << StringView(u8"   ") << tenths / 10 << StringView(u8".") << tenths % 10 << StringView(u8" MP");
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

        {
            StringBuilder text;

            text << (i64)(current + 1) << StringView(u8" / ") << (i64)entries.length();
            row("Position", StringView(text));
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
        zoom = min(1.f, min(size.x / rw, size.y / rh));
    }

    float dw = rw * zoom;
    float dh = rh * zoom;

    panX = dw <= size.x ? 0.f : clampf(panX, (size.x - dw) / 2.f, (dw - size.x) / 2.f);
    panY = dh <= size.y ? 0.f : clampf(panY, (size.y - dh) / 2.f, (dh - size.y) / 2.f);

    ImVec2 centre(origin.x + size.x / 2.f + panX, origin.y + size.y / 2.f + panY);
    ImVec2 p0(centre.x - dw / 2.f, centre.y - dh / 2.f);
    ImVec2 p1(centre.x + dw / 2.f, centre.y + dh / 2.f);
    const ImVec2 uv[4] = {ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1)};
    int r = rotation;

    dl->AddImageQuad(tex, p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), uv[(4 - r) & 3], uv[(5 - r) & 3], uv[(6 - r) & 3], uv[(7 - r) & 3]);

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
}

void ViewApp::draw() {
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

int mainView(ObjPool& pool, Ui& ui, int argc, char** argv) {
    if (argc < 2) {
        sysE << StringView(u8"usage: im view <file|dir>...") << endL;

        return 2;
    }

    ViewApp& app = *pool.make<ViewApp>();

    app.ui = &ui;

    for (int i = 1; i < argc; i++) {
        StringView arg(argv[i]);
        struct stat st;

        if (stat(argv[i], &st) != 0) {
            sysE << StringView(u8"im view: ") << arg << StringView(u8": ") << StringView(strerror(errno)) << endL;

            continue;
        }

        try {
            if (S_ISDIR(st.st_mode)) {
                addDirectory(pool, app.entries, arg);
            } else if (argc == 2) {
                size_t slash = arg.length();

                while (slash > 0 && arg[slash - 1] != '/') {
                    slash--;
                }

                StringView dir = slash == 0 ? StringView(u8".") : slash == 1 ? StringView(u8"/") : StringView(arg.begin(), arg.begin() + slash - 1);
                StringView name(arg.begin() + slash, arg.end());

                addDirectory(pool, app.entries, dir);

                bool listed = false;

                for (size_t j = 0; j < app.entries.length() && !listed; j++) {
                    if (app.entries[j]->name() == name) {
                        app.current = j;
                        listed = true;
                    }
                }

                if (!listed) {
                    Vector<Entry*> rest;

                    rest.xchg(app.entries);
                    addFile(pool, app.entries, arg);
                    app.entries.append(rest.begin(), rest.end());
                    app.current = 0;
                }
            } else {
                addFile(pool, app.entries, arg);
            }
        } catch (...) {
            sysE << StringView(u8"im view: ") << arg << StringView(u8": ") << Exception::current() << endL;
        }
    }

    ui.trace(StringView(StringBuilder() << StringView(u8"listed ") << (i64)app.entries.length()));

    if (app.entries.empty()) {
        sysE << StringView(u8"im view: no images to show") << endL;

        return showError(ui, StringView(u8"no images to show"));
    }

    readAll(app.entries);

    ui.open({windowWidth, windowHeight});
    app.maxSide = ui.maxTextureSide();

    u32 side = thumbSideFor(floorf(ui.px(windowWidth) * sideShare) * (1.f + bulge));

    for (size_t i = 0; i < app.entries.length(); i++) {
        app.loadThumb(i, side);
    }

    app.show(app.current);

    UiEvent event;

    while (ui.next(event)) {
        if (event.kind == UiEvent::Kind::Close) {
            return 0;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Q)) {
            return 0;
        }

        app.keys();
        app.draw();
    }

    return 0;
}
