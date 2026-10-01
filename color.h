#pragma once

#include <std/sys/types.h>

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

struct OutputMapping {
    ColorMatrix toTarget;
    ColorMatrix fromTarget;
    ColorRgb targetLuma;
    double peakNits = 203.0;
};

OutputMapping sdrMapping();
ColorRgb mapOutputNits(const OutputMapping& mapping, const ColorRgb& color);

u32 unorm10To8(u32 value);
