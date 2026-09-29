#include "color.h"

extern "C" {
#include <libdisplay-info/info.h>
}

#include <math.h>
#include <string.h>

namespace {
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

        if (fabs(determinant) < 1e-12) {
            return ColorMatrix::identity();
        }

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

    // kelvin is an Output::colorTemp(): 0 when the night light is off, else
    // inside (0, 6500), the backends having folded everything else to 0
    ColorMatrix chromaticAdaptation(double kelvin) {
        if (kelvin <= 0) {
            return ColorMatrix::identity();
        }

        double t = fmax(1667.0, fmin(25000.0, kelvin));
        double x = t <= 4000.0 ? -.2661239e9 / (t * t * t) - .2343580e6 / (t * t) + .8776956e3 / t + .179910 : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + .2226347e3 / t + .240390;
        double y = t <= 2222.0 ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - .20219683 : t <= 4000.0 ? -.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - .16748867 : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - .37001483;
        ColorRgb source{.3127 / .3290, 1.0, (1.0 - .3127 - .3290) / .3290};
        ColorRgb target{x / y, 1.0, (1.0 - x - y) / y};
        ColorMatrix bradford{{
            .8951,
            .2664,
            -.1614,
            -.7502,
            1.7135,
            .0367,
            .0389,
            -.0685,
            1.0296,
        }};
        ColorRgb sourceCone = bradford.apply(source);
        ColorRgb targetCone = bradford.apply(target);
        ColorMatrix scale{{
            targetCone.r / sourceCone.r,
            0,
            0,
            0,
            targetCone.g / sourceCone.g,
            0,
            0,
            0,
            targetCone.b / sourceCone.b,
        }};

        return multiply(inverse(bradford), multiply(scale, bradford));
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

ColorMatrix colorPrimariesTransform(const Chromaticities& from, const Chromaticities& to) {
    return multiply(inverse(rgbToXyz(to)), rgbToXyz(from));
}

Chromaticities Chromaticities::sRgb() {
    return {640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000};
}

Chromaticities Chromaticities::bt2020() {
    return {708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000};
}

Chromaticities Chromaticities::displayP3() {
    return {680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000};
}

bool Chromaticities::valid() const {
    if (ry <= 0 || gy <= 0 || by <= 0 || wy <= 0) {
        return false;
    }

    // with every y at least 1e-6 and every coordinate an i32 millionth, the
    // primaries matrix stays within 4.3e9 a cell and its inverse (taken
    // only past a 1e-12 determinant) within 4e31, so the scaled matrix and
    // its determinant stay far inside double range: never inf or NaN
    ColorMatrix m = rgbToXyz(*this);
    double determinant = m.v[0] * (m.v[4] * m.v[8] - m.v[5] * m.v[7]) - m.v[1] * (m.v[3] * m.v[8] - m.v[5] * m.v[6]) + m.v[2] * (m.v[3] * m.v[7] - m.v[4] * m.v[6]);

    return fabs(determinant) >= 1e-9;
}

bool Chromaticities::operator==(const Chromaticities& o) const {
    return rx == o.rx && ry == o.ry && gx == o.gx && gy == o.gy && bx == o.bx && by == o.by && wx == o.wx && wy == o.wy;
}

ColorDescription ColorDescription::sRgb() {
    return {};
}

ColorDescription ColorDescription::bt2100Pq() {
    ColorDescription d;

    d.transfer = ColorTransfer::pq;
    d.primaries = ColorPrimaries::bt2020;
    d.primary = Chromaticities::bt2020();
    d.minNits = .005;
    d.maxNits = 10000.0;
    d.referenceNits = 203.0;
    d.target = d.primary;
    d.targetMinNits = d.minNits;
    d.targetMaxNits = d.maxNits;

    return d;
}

ColorDescription ColorDescription::bt2100Hlg() {
    ColorDescription d;

    d.transfer = ColorTransfer::hlg;
    d.primaries = ColorPrimaries::bt2020;
    d.primary = Chromaticities::bt2020();
    d.minNits = .005;
    d.maxNits = 1000.0;
    d.referenceNits = 203.0;
    d.target = d.primary;
    d.targetMinNits = d.minNits;
    d.targetMaxNits = d.maxNits;

    return d;
}

ColorDescription ColorDescription::extendedLinear() {
    ColorDescription d;

    d.transfer = ColorTransfer::extendedLinear;

    return d;
}

ColorDescription ColorDescription::bt1886() {
    ColorDescription d;

    d.transfer = ColorTransfer::bt1886;
    d.minNits = .01;
    d.maxNits = 100.0;
    d.referenceNits = 100.0;
    d.targetMinNits = d.minNits;
    d.targetMaxNits = d.maxNits;

    return d;
}

ColorDescription ColorDescription::gamma22() {
    ColorDescription d;

    d.transfer = ColorTransfer::gamma22;

    return d;
}

bool ColorDescription::managed() const {
    return transfer != ColorTransfer::sRgb || primaries != ColorPrimaries::sRgb || directToBt2020;
}

bool ColorDescription::hdr() const {
    return transfer == ColorTransfer::pq || transfer == ColorTransfer::hlg;
}

bool ColorDescription::operator==(const ColorDescription& o) const {
    return transfer == o.transfer && primaries == o.primaries && primary == o.primary && minNits == o.minNits && maxNits == o.maxNits && referenceNits == o.referenceNits && linearOneNits == o.linearOneNits && target == o.target && targetMinNits == o.targetMinNits && targetMaxNits == o.targetMaxNits && maxCll == o.maxCll && maxFall == o.maxFall && maxCllSet == o.maxCllSet && maxFallSet == o.maxFallSet && directToBt2020 == o.directToBt2020 && !memcmp(toBt2020, o.toBt2020, sizeof(toBt2020)) && !memcmp(gamma, o.gamma, sizeof(gamma));
}

bool ColorDescription::operator!=(const ColorDescription& o) const {
    return !(*this == o);
}

OutputColorState OutputColorState::sdr() {
    return {};
}

OutputColorState OutputColorState::hdr10(double white) {
    OutputColorState state;

    state.encoding = ColorDescription::bt2100Pq();
    state.sdrWhiteNits = white;
    state.displayMinNits = .0001;
    state.displayPeakNits = 1000.0;
    state.displayMaxFallNits = 400.0;
    state.bpc = 10;
    state.encoding.referenceNits = white;
    state.encoding.targetMinNits = state.displayMinNits;
    state.encoding.targetMaxNits = state.displayPeakNits;

    return state;
}

void OutputColorState::setSdrWhite(double nits) {
    if (!hdr() || nits <= 0) {
        return;
    }

    sdrWhiteNits = fmin(nits, displayPeakNits);
    encoding.referenceNits = sdrWhiteNits;
}

bool OutputColorState::hdr() const {
    return encoding.hdr();
}

bool OutputColorState::operator==(const OutputColorState& o) const {
    return sameDescription(o) && bpc == o.bpc && range == o.range;
}

bool OutputColorState::sameDescription(const OutputColorState& o) const {
    return encoding == o.encoding && sdrWhiteNits == o.sdrWhiteNits && displayMinNits == o.displayMinNits && displayPeakNits == o.displayPeakNits && displayMaxFallNits == o.displayMaxFallNits;
}

ColorMatrix ColorMatrix::identity() {
    return {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
}

ColorRgb ColorMatrix::apply(const ColorRgb& c) const {
    return {
        v[0] * c.r + v[1] * c.g + v[2] * c.b,
        v[3] * c.r + v[4] * c.g + v[5] * c.b,
        v[6] * c.r + v[7] * c.g + v[8] * c.b,
    };
}

bool parseEdidColorCapabilities(const void* data, size_t size, DisplayColorCapabilities& capabilities) {
    capabilities = {};

    di_info* info = di_info_parse_edid(data, size);

    if (!info) {
        return false;
    }

    capabilities.valid = true;

    const di_hdr_static_metadata* hdr = di_info_get_hdr_static_metadata(info);
    const di_supported_signal_colorimetry* signal = di_info_get_supported_signal_colorimetry(info);
    const di_color_primaries* primaries = di_info_get_default_color_primaries(info);

    capabilities.pq = hdr->pq;
    capabilities.hlg = hdr->hlg;
    capabilities.bt2020Rgb = signal->bt2020_rgb;
    capabilities.minNits = hdr->desired_content_min_luminance;
    capabilities.peakNits = hdr->desired_content_max_luminance;
    capabilities.maxFallNits = hdr->desired_content_max_frame_avg_luminance;
    capabilities.hasPrimaries = primaries->has_primaries && primaries->has_default_white_point;

    if (capabilities.hasPrimaries) {
        auto scaled = [](float value) {
            return (i32)lround((double)value * 1000000.0);
        };

        capabilities.primaries = {
            scaled(primaries->primary[0].x),
            scaled(primaries->primary[0].y),
            scaled(primaries->primary[1].x),
            scaled(primaries->primary[1].y),
            scaled(primaries->primary[2].x),
            scaled(primaries->primary[2].y),
            scaled(primaries->default_white.x),
            scaled(primaries->default_white.y),
        };
    }

    di_info_destroy(info);

    return true;
}

OutputColorState outputColorState(const OutputConfiguration& config, const DisplayColorCapabilities& capabilities) {
    OutputColorState state = config.hdrSdrWhiteNits > 0 ? OutputColorState::hdr10(config.hdrSdrWhiteNits) : OutputColorState::sdr();

    if (state.hdr()) {
        if (capabilities.peakNits > 0) {
            state.displayPeakNits = capabilities.peakNits;
        }

        if (capabilities.maxFallNits > 0) {
            state.displayMaxFallNits = capabilities.maxFallNits;
        }

        if (capabilities.minNits > 0) {
            state.displayMinNits = capabilities.minNits;
        }

        if (capabilities.hasPrimaries) {
            state.encoding.target = capabilities.primaries;
        }
    }

    if (state.hdr()) {
        if (config.displayMinNits > 0) {
            state.displayMinNits = config.displayMinNits;
        }

        if (config.displayPeakNits > 0) {
            state.displayPeakNits = config.displayPeakNits;
        }

        if (config.displayMaxFallNits > 0) {
            state.displayMaxFallNits = config.displayMaxFallNits;
        }

        if (state.displayMaxFallNits > state.displayPeakNits) {
            state.displayMaxFallNits = state.displayPeakNits;
        }
    }

    state.bpc = config.bpc ? config.bpc : (state.hdr() ? 10 : 8);
    state.range = config.range;
    state.encoding.targetMinNits = state.displayMinNits;
    state.encoding.targetMaxNits = state.displayPeakNits;
    state.setSdrWhite(state.sdrWhiteNits);

    return state;
}

OutputMapping outputMapping(const OutputColorState& output, double colorTemperature) {
    OutputMapping mapping;
    Chromaticities target = output.hdr() ? output.encoding.target : Chromaticities::sRgb();
    ColorMatrix sceneToXyz = rgbToXyz(Chromaticities::bt2020());
    ColorMatrix targetToXyz = rgbToXyz(target);

    mapping.toTarget = multiply(inverse(targetToXyz), multiply(chromaticAdaptation(colorTemperature), sceneToXyz));
    mapping.fromTarget = multiply(inverse(sceneToXyz), targetToXyz);
    mapping.targetLuma = {targetToXyz.v[3], targetToXyz.v[4], targetToXyz.v[5]};
    mapping.hdr = output.hdr();
    mapping.peakNits = mapping.hdr ? output.displayPeakNits : 203.0;
    mapping.referenceNits = mapping.hdr ? output.sdrWhiteNits : 100.0;

    // only an hdr output can bring an unknown (zero) peak: sdr has 203 above
    if (mapping.peakNits <= 0) {
        mapping.peakNits = 1000.0;
    }

    if (mapping.referenceNits >= mapping.peakNits) {
        mapping.referenceNits = mapping.peakNits * .5;
    }

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

double surfaceMaxNits(const ColorDescription& color, double sdrWhiteNits) {
    switch (color.transfer) {
        case ColorTransfer::pq:
            return color.maxNits;
        case ColorTransfer::hlg:
            // the fixed 1000-nit OOTF in the scene shader
            return 1000.0;
        case ColorTransfer::extendedLinear:
            // scRGB is the one transfer the scene shader does not clamp
            return 1e9;
        case ColorTransfer::bt1886:
            return color.maxNits;
        case ColorTransfer::gamma22:
        case ColorTransfer::iccGamma:
            return color.referenceNits > 0 ? color.referenceNits : sdrWhiteNits;
        case ColorTransfer::sRgb:
            break;
    }

    // sRGB electrical values are clamped to [0,1] and scaled by SDR white
    return sdrWhiteNits;
}

void HdrContentMetadata::add(const ColorDescription& color, double sdrWhiteNits) {
    if (!color.hdr()) {
        maxCllNits = fmax(maxCllNits, sdrWhiteNits);
        maxFallNits = fmax(maxFallNits, sdrWhiteNits);

        return;
    }

    if (color.maxCllSet) {
        maxCllNits = fmax(maxCllNits, color.maxCll);
    } else {
        maxCllUnknown = true;
    }

    if (color.maxFallSet) {
        maxFallNits = fmax(maxFallNits, color.maxFall);
    } else {
        maxFallUnknown = true;
    }
}

bool HdrOutputMetadata::operator==(const HdrOutputMetadata& o) const {
    return primaries == o.primaries && minNits == o.minNits && maxNits == o.maxNits && maxCll == o.maxCll && maxFall == o.maxFall && hdr == o.hdr;
}

HdrOutputMetadata hdrOutputMetadata(const OutputColorState& output, const HdrContentMetadata& content) {
    HdrOutputMetadata metadata;

    if (!output.hdr()) {
        return metadata;
    }

    metadata.hdr = true;
    metadata.primaries = output.encoding.target;
    metadata.minNits = output.displayMinNits;
    metadata.maxNits = output.displayPeakNits;

    OutputMapping mapping = outputMapping(output);
    auto mappedNeutral = [&](double nits) {
        ColorRgb mapped = mapOutputNits(mapping, {nits, nits, nits});
        ColorRgb target = mapping.toTarget.apply(mapped);

        return mapping.targetLuma.r * target.r + mapping.targetLuma.g * target.g + mapping.targetLuma.b * target.b;
    };

    double sourceCll = content.maxCllUnknown ? output.encoding.maxNits : content.maxCllNits;
    double sourceFall = content.maxFallUnknown ? output.displayMaxFallNits : content.maxFallNits;
    double mappedCll = mappedNeutral(sourceCll);
    double mappedFall = mappedNeutral(sourceFall);

    mappedCll = fmax(output.sdrWhiteNits, mappedCll);
    mappedCll = fmin(output.displayPeakNits, mappedCll);
    mappedFall = fmax(output.sdrWhiteNits, mappedFall);
    mappedFall = fmin(mappedCll, mappedFall);
    metadata.maxCll = (u32)lround(mappedCll);
    metadata.maxFall = (u32)lround(mappedFall);

    return metadata;
}

bool directScanoutColorCompatible(const OutputColorState& output, const ColorDescription& surface) {
    return !output.hdr() && !surface.managed();
}

u32 unorm10To8(u32 value) {
    return (value * 255 + 511) / 1023;
}
