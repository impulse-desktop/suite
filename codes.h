#pragma once

#include <std/sys/types.h>

struct VideoLayout {
    const char* name;
    const char* shape;
    const char* model;
    bool bigEndian;
    bool alpha;
    bool floating;
    bool bits;
    bool inverted;
    int count;
    int components[4][5];
    int padding[4];
    int luma[5];
};

struct VideoFormat {
    const char* name;
    int layout;
};

struct VideoMatrix {
    u8 code;
    const char* system;
    const char* weights;
    double kr;
    double kb;
    double toSignal[3][3];
    int lumaBits;
};

struct VideoTransfer {
    u8 code;
    const char* shape;
    double eotf[11];
    double oetf[11];
    double inverse[11];
    double decades;
};

struct VideoPrimaries {
    u8 code;
    double toXyz[3][3];
};

struct VideoOutput {
    const char* name;
    double fromXyz[3][3];
};

struct VideoLocation {
    u8 code;
    double site[2];
};

template <class T>
struct VideoTable {
    const T* items;
    size_t count;

    const T* begin() const {
        return items;
    }

    const T* end() const {
        return items + count;
    }

    const T& operator[](size_t i) const {
        return items[i];
    }
};

extern const VideoTable<VideoLayout> videoLayouts;
extern const VideoTable<VideoFormat> videoFormats;
extern const VideoTable<VideoMatrix> videoMatrices;
extern const VideoTable<VideoTransfer> videoTransfers;
extern const VideoTable<VideoPrimaries> videoPrimaries;
extern const VideoTable<VideoOutput> videoOutputs;
extern const VideoTable<VideoLocation> videoLocations;
extern const VideoTable<u8> videoRanges;
