#pragma once

#include <std/sys/types.h>

enum class ColorTransfer {
    sRgb,
    pq,
    hlg,
    extendedLinear,
    bt1886,
    gamma22,
    iccGamma,
};

enum class ColorPrimaries {
    sRgb,
    bt2020,
    displayP3,
    custom,
};

enum class OutputRange {
    automatic,
    full,
    limited,
};

struct Chromaticities {
    i32 rx = 0, ry = 0;
    i32 gx = 0, gy = 0;
    i32 bx = 0, by = 0;
    i32 wx = 0, wy = 0;

    static Chromaticities sRgb();
    static Chromaticities bt2020();
    static Chromaticities displayP3();
    // false for coordinates whose primaries matrix math degenerates
    // (zero y components, collapsed triangle): they divide by zero and
    // would poison the shader with NaN/Inf
    bool valid() const;
    bool operator==(const Chromaticities& other) const;
};

struct ColorDescription {
    ColorTransfer transfer = ColorTransfer::sRgb;
    ColorPrimaries primaries = ColorPrimaries::sRgb;
    Chromaticities primary = Chromaticities::sRgb();
    double minNits = .2;
    double maxNits = 80.0;
    double referenceNits = 80.0;
    double linearOneNits = 80.0;
    Chromaticities target = Chromaticities::sRgb();
    double targetMinNits = .2;
    double targetMaxNits = 80.0;
    u32 maxCll = 0;
    u32 maxFall = 0;
    bool maxCllSet = false;
    bool maxFallSet = false;
    bool directToBt2020 = false;
    double toBt2020[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double gamma[3] = {1, 1, 1};

    static ColorDescription sRgb();
    static ColorDescription bt2100Pq();
    static ColorDescription bt2100Hlg();
    static ColorDescription extendedLinear();
    static ColorDescription bt1886();
    static ColorDescription gamma22();
    bool managed() const;
    bool hdr() const;
    bool operator==(const ColorDescription& other) const;
    bool operator!=(const ColorDescription& other) const;
};

struct DisplayColorCapabilities {
    bool valid = false;
    bool pq = false;
    bool hlg = false;
    bool bt2020Rgb = false;
    bool hasPrimaries = false;
    Chromaticities primaries;
    double minNits = 0;
    double peakNits = 0;
    double maxFallNits = 0;
};

struct OutputConfiguration {
    double hdrSdrWhiteNits = 0;
    double displayMinNits = 0;
    double displayPeakNits = 0;
    double displayMaxFallNits = 0;
    u32 bpc = 0;
    OutputRange range = OutputRange::automatic;
};

struct OutputColorState {
    ColorDescription encoding;
    double sdrWhiteNits = 80.0;
    double displayMinNits = .2;
    double displayPeakNits = 80.0;
    double displayMaxFallNits = 80.0;
    u32 bpc = 8;
    OutputRange range = OutputRange::automatic;

    static OutputColorState sdr();
    static OutputColorState hdr10(double sdrWhiteNits);
    void setSdrWhite(double nits);
    bool hdr() const;
    bool operator==(const OutputColorState& other) const;
    // what an image description of the output says is the same: the
    // encoding and the luminances, not the link depth or the RGB range
    bool sameDescription(const OutputColorState& other) const;
};

struct ColorRgb {
    double r = 0, g = 0, b = 0;
};

struct ColorMatrix {
    double v[9] = {};

    static ColorMatrix identity();
    ColorRgb apply(const ColorRgb& color) const;
};

ColorMatrix colorPrimariesTransform(const Chromaticities& from, const Chromaticities& to);

struct OutputMapping {
    ColorMatrix toTarget;
    ColorMatrix fromTarget;
    ColorRgb targetLuma;
    double referenceNits = 100;
    double peakNits = 203;
    bool hdr = false;
};

struct HdrContentMetadata {
    double maxCllNits = 0;
    double maxFallNits = 0;
    bool maxCllUnknown = false;
    bool maxFallUnknown = false;

    void add(const ColorDescription& color, double sdrWhiteNits);
};

struct HdrOutputMetadata {
    Chromaticities primaries;
    double minNits = 0;
    double maxNits = 0;
    u32 maxCll = 0;
    u32 maxFall = 0;
    bool hdr = false;

    bool operator==(const HdrOutputMetadata& other) const;
};

bool parseEdidColorCapabilities(const void* data, size_t size, DisplayColorCapabilities& capabilities);
OutputColorState outputColorState(const OutputConfiguration& config, const DisplayColorCapabilities& capabilities);
OutputMapping outputMapping(const OutputColorState& output, double colorTemperature = 0);
ColorRgb mapOutputNits(const OutputMapping& mapping, const ColorRgb& color);

// The brightest scene value a surface with this description can produce,
// mirroring the renderer's per-transfer scale. Bounds the display tone map:
// when nothing on screen can exceed the output peak, the roll-off knee must
// not touch in-range content.
double surfaceMaxNits(const ColorDescription& color, double sdrWhiteNits);

// a 10-bit unorm channel at 8 bits, the nearest level: dropping the low
// bits instead would darken by up to a level
u32 unorm10To8(u32 value);
HdrOutputMetadata hdrOutputMetadata(const OutputColorState& output, const HdrContentMetadata& content);

bool directScanoutColorCompatible(const OutputColorState& output, const ColorDescription& surface);
