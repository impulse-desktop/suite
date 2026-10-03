#pragma once

#include <std/sys/types.h>

namespace stl {
    class ObjPool;
    class StringView;
}

struct VideoLayout;

struct VideoShader {
    const VideoLayout* layout;
    const char* system;
    const char* transfer;
    const char* conversion;
    const char* output;
    const char* filter;
    u32 planeOffset[4];
    u32 lineSize[4];
    u32 size[4];
    u32 target[2];
    u32 dither;
    u32 phase;
    double chroma[4];
    double decode[3][3];
    double bias[4];
    double sites[2];
    double weights[2];
    double curve[11];
    double oetf[11];
    double inverse[11];
    double toOutput[3][3];
    double light[3];
    double luminance[3];
};

stl::StringView compile(stl::ObjPool& pool, const VideoShader& shader);
