#include "svg.h"

#include "error.h"

#include <std/thr/guard.h>
#include <std/thr/mutex.h>
#include <std/lib/buffer.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <lunasvg.h>

using namespace stl;

namespace {
    struct RasterImage final: Image {
        Buffer rgba;
        u32 width_;
        u32 height_;

        RasterImage(u32 width, u32 height);

        const void* data() const override;
        size_t length() const override;
        u32 width() const override;
        u32 height() const override;
    };

    struct DocumentImpl final: SvgDocument {
        Mutex* lock;
        lunasvg::Document* document;
        float width_;
        float height_;

        DocumentImpl(Mutex* lock, lunasvg::Document* document);
        ~DocumentImpl() noexcept;

        float width() const override;
        float height() const override;
        Image* render(ObjPool& pool, u32 width, u32 height) override;
    };

    struct LibraryImpl final: SvgLibrary {
        Mutex* lock;

        explicit LibraryImpl(ObjPool& pool);

        SvgDocument* parse(ObjPool& pool, StringView file) override;
    };

    StringView xmlOf(StringView file) {
        const u8* p = file.begin();
        const u8* end = file.end();

        if (end - p >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
            p += 3;
        }

        const u8* text = p;

        while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }

        return p != end && *p == '<' ? StringView(text, end) : StringView();
    }
}

RasterImage::RasterImage(u32 width, u32 height)
    : width_(width)
    , height_(height)
{
    rgba.zero((size_t)width * height * 4);
}

const void* RasterImage::data() const {
    return rgba.data();
}

size_t RasterImage::length() const {
    return rgba.length();
}

u32 RasterImage::width() const {
    return width_;
}

u32 RasterImage::height() const {
    return height_;
}

DocumentImpl::DocumentImpl(Mutex* lock_, lunasvg::Document* document_)
    : lock(lock_)
    , document(document_)
    , width_(document_->width())
    , height_(document_->height())
{
}

DocumentImpl::~DocumentImpl() noexcept {
    LockGuard guard(lock);

    delete document;
}

float DocumentImpl::width() const {
    return width_;
}

float DocumentImpl::height() const {
    return height_;
}

Image* DocumentImpl::render(ObjPool& pool, u32 width, u32 height) {
    if (!width || !height || (u64)width * 4 > 0x7fffffffu || (u64)height > 0x7fffffffu) {
        raiseError(StringView(StringBuilder() << StringView(u8"cannot draw the SVG at ") << (i64)width << StringView(u8"x") << (i64)height));
    }

    RasterImage* image = pool.make<RasterImage>(width, height);
    lunasvg::Bitmap bitmap((uint8_t*)image->rgba.mutData(), (int)width, (int)height, (int)width * 4);
    lunasvg::Matrix matrix((float)width / width_, 0.f, 0.f, (float)height / height_, 0.f, 0.f);
    LockGuard guard(lock);

    document->render(bitmap, matrix);
    bitmap.convertToRGBA();

    return image;
}

LibraryImpl::LibraryImpl(ObjPool& pool)
    : lock(Mutex::create(&pool))
{
}

SvgDocument* LibraryImpl::parse(ObjPool& pool, StringView file) {
    StringView xml = xmlOf(file);

    if (xml.empty()) {
        return nullptr;
    }

    LockGuard guard(lock);
    lunasvg::Document* document = lunasvg::Document::loadFromData((const char*)xml.data(), xml.length()).release();

    if (!document) {
        return nullptr;
    }

    DocumentImpl* parsed = pool.make<DocumentImpl>(lock, document);

    if (!isfinite(parsed->width_) || !isfinite(parsed->height_) || parsed->width_ <= 0.f || parsed->height_ <= 0.f) {
        raiseError(StringView(u8"the SVG has no size"));
    }

    return parsed;
}

SvgLibrary* SvgLibrary::create(ObjPool& pool) {
    return pool.make<LibraryImpl>(pool);
}
