#pragma once

#include <std/str/view.h>
#include <std/sys/types.h>
#include <std/lib/buffer.h>

struct DecodedImage {
    u32 width = 0;
    u32 height = 0;
    stl::Buffer rgba;
};

void decode(stl::StringView file, stl::StringView name, DecodedImage& out);
