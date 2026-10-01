#include "color.h"

#include <math.h>

namespace {
    struct Chromaticities {
        i32 rx, ry, gx, gy, bx, by, wx, wy;
    };

    constexpr Chromaticities sRgb = {640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000};
    constexpr Chromaticities bt2020 = {708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000};

    ColorMatrix multiply(const ColorMatrix& a, const ColorMatrix& b) {
        ColorMatrix result;

        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                for (int k = 0; k < 3; k++) {
                    result.v[row * 3 + col] += a.v[row * 3 + k] * b.v[k * 3 + col];
                }
            }
        }

        return result;
    }

    ColorMatrix inverse(const ColorMatrix& m) {
        double determinant = m.v[0] * (m.v[4] * m.v[8] - m.v[5] * m.v[7]) - m.v[1] * (m.v[3] * m.v[8] - m.v[5] * m.v[6]) + m.v[2] * (m.v[3] * m.v[7] - m.v[4] * m.v[6]);
        double d = 1.0 / determinant;

        return {{
            (m.v[4] * m.v[8] - m.v[5] * m.v[7]) * d,
            (m.v[2] * m.v[7] - m.v[1] * m.v[8]) * d,
            (m.v[1] * m.v[5] - m.v[2] * m.v[4]) * d,
            (m.v[5] * m.v[6] - m.v[3] * m.v[8]) * d,
            (m.v[0] * m.v[8] - m.v[2] * m.v[6]) * d,
            (m.v[2] * m.v[3] - m.v[0] * m.v[5]) * d,
            (m.v[3] * m.v[7] - m.v[4] * m.v[6]) * d,
            (m.v[1] * m.v[6] - m.v[0] * m.v[7]) * d,
            (m.v[0] * m.v[4] - m.v[1] * m.v[3]) * d,
        }};
    }

    ColorMatrix rgbToXyz(const Chromaticities& c) {
        auto unit = [](i32 value) {
            return (double)value / 1000000.0;
        };
        double xr = unit(c.rx), yr = unit(c.ry);
        double xg = unit(c.gx), yg = unit(c.gy);
        double xb = unit(c.bx), yb = unit(c.by);
        double xw = unit(c.wx), yw = unit(c.wy);
        ColorMatrix primaries{{
            xr / yr,
            xg / yg,
            xb / yb,
            1,
            1,
            1,
            (1 - xr - yr) / yr,
            (1 - xg - yg) / yg,
            (1 - xb - yb) / yb,
        }};
        ColorRgb white{xw / yw, 1, (1 - xw - yw) / yw};
        ColorRgb scale = inverse(primaries).apply(white);

        for (int row = 0; row < 3; row++) {
            primaries.v[row * 3] *= scale.r;
            primaries.v[row * 3 + 1] *= scale.g;
            primaries.v[row * 3 + 2] *= scale.b;
        }

        return primaries;
    }

    double toneMap(double value, const OutputMapping& mapping) {
        value = value > 0 ? value : 0;
        double knee = mapping.peakNits * .9;

        if (value <= knee || mapping.peakNits <= knee) {
            return value < mapping.peakNits ? value : mapping.peakNits;
        }

        double headroom = mapping.peakNits - knee;
        double excess = value - knee;

        return mapping.peakNits - headroom * headroom / (headroom + excess);
    }
}

OutputColorState OutputColorState::sdr() {
    return {};
}

OutputColorState OutputColorState::hdr10(double white) {
    OutputColorState state;

    state.hdr = true;
    state.sdrWhiteNits = white;
    state.displayMinNits = .0001;
    state.displayPeakNits = 1000.0;
    state.displayMaxFallNits = 400.0;

    return state;
}

ColorRgb ColorMatrix::apply(const ColorRgb& c) const {
    return {
        v[0] * c.r + v[1] * c.g + v[2] * c.b,
        v[3] * c.r + v[4] * c.g + v[5] * c.b,
        v[6] * c.r + v[7] * c.g + v[8] * c.b,
    };
}

OutputMapping sdrMapping() {
    OutputMapping mapping;
    ColorMatrix sceneToXyz = rgbToXyz(bt2020);
    ColorMatrix targetToXyz = rgbToXyz(sRgb);

    mapping.toTarget = multiply(inverse(targetToXyz), sceneToXyz);
    mapping.fromTarget = multiply(inverse(sceneToXyz), targetToXyz);
    mapping.targetLuma = {targetToXyz.v[3], targetToXyz.v[4], targetToXyz.v[5]};

    return mapping;
}

ColorRgb mapOutputNits(const OutputMapping& mapping, const ColorRgb& color) {
    ColorRgb target = mapping.toTarget.apply(color);
    double luminance = mapping.targetLuma.r * target.r + mapping.targetLuma.g * target.g + mapping.targetLuma.b * target.b;
    double mappedLuminance = toneMap(luminance, mapping);

    if (luminance > 1e-9) {
        double scale = mappedLuminance / luminance;

        target.r *= scale;
        target.g *= scale;
        target.b *= scale;
    } else {
        target = {};
        mappedLuminance = 0;
    }

    double chromaScale = 1.0;
    const double channels[] = {target.r, target.g, target.b};

    for (double channel : channels) {
        if (channel < 0) {
            chromaScale = fmin(chromaScale, mappedLuminance / (mappedLuminance - channel));
        } else if (channel > mapping.peakNits) {
            chromaScale = fmin(chromaScale, (mapping.peakNits - mappedLuminance) / (channel - mappedLuminance));
        }
    }

    chromaScale = fmax(0.0, fmin(1.0, chromaScale));
    target.r = mappedLuminance + (target.r - mappedLuminance) * chromaScale;
    target.g = mappedLuminance + (target.g - mappedLuminance) * chromaScale;
    target.b = mappedLuminance + (target.b - mappedLuminance) * chromaScale;

    return mapping.fromTarget.apply(target);
}

u32 unorm10To8(u32 value) {
    return (value * 255 + 511) / 1023;
}
