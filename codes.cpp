#include "codes.h"

namespace {
#include "codes.inc"

    template <class T, size_t N>
    constexpr VideoTable<T> table(const T (&items)[N]) {
        return VideoTable<T>{items, N};
    }
}

const VideoTable<VideoLayout> videoLayouts = table(layouts);
const VideoTable<VideoFormat> videoFormats = table(formats);
const VideoTable<VideoMatrix> videoMatrices = table(matrices);
const VideoTable<VideoTransfer> videoTransfers = table(transfers);
const VideoTable<VideoPrimaries> videoPrimaries = table(primaries);
const VideoTable<VideoOutput> videoOutputs = table(outputs);
const VideoTable<VideoLocation> videoLocations = table(locations);
const VideoTable<u8> videoRanges = table(ranges);
