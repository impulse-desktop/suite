#pragma once

#include <std/sys/types.h>

// The colour of a capture as the compositor described it: SDR, or HDR10
// (BT.2100 PQ) with the white level and the display volume its metadata
// names, and the mapping of that PQ light onto the SDR range for a PNG.

struct OutputColorState {
    bool hdr = false;
    double sdrWhiteNits = 80.0;
    double displayMinNits = .2;
    double displayPeakNits = 80.0;
    double displayMaxFallNits = 80.0;

    static OutputColorState sdr();
    static OutputColorState hdr10(double sdrWhiteNits);
};

struct ColorRgb {
    double r = 0, g = 0, b = 0;
};

struct ColorMatrix {
    double v[9] = {};

    ColorRgb apply(const ColorRgb& color) const;
};

// the SDR display mapping of scene light (linear BT.2020, in nits): a
// knee below SDR white (203 nits) rolls the excess off, the chroma is
// held inside the sRGB gamut, and the result is linear sRGB, in nits
struct OutputMapping {
    ColorMatrix toTarget;
    ColorMatrix fromTarget;
    ColorRgb targetLuma;
    double peakNits = 203.0;
};

OutputMapping sdrMapping();
ColorRgb mapOutputNits(const OutputMapping& mapping, const ColorRgb& color);

// a 10-bit unorm channel at 8 bits, the nearest level: dropping the low
// bits instead would darken by up to a level
u32 unorm10To8(u32 value);
