#include "ui.h"
#include "error.h"
#include "shader.h"
#include "renderer.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/alg/defer.h>
#include <std/sys/throw.h>
#include <std/lib/vector.h>
#include <std/ptr/scoped.h>
#include <std/str/builder.h>
#include <std/thr/runable.h>
#include <std/mem/obj_pool.h>

#include <math.h>
#include <string.h>
#include <video_codes.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
}

using namespace stl;

namespace {
    constexpr int sampleWidth = 63;
    constexpr int sampleHeight = 47;
    constexpr size_t samplePixels = (size_t)sampleWidth * sampleHeight;
    constexpr double readback = 1. / 512.;

    struct Chromaticity {
        int code;
        double rx, ry, gx, gy, bx, by, wx, wy;
    };

    constexpr Chromaticity chromaticities[] = {
        {1, 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {2, 0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {4, 0.670, 0.330, 0.210, 0.710, 0.140, 0.080, 0.3100, 0.3160},
        {5, 0.640, 0.330, 0.290, 0.600, 0.150, 0.060, 0.3127, 0.3290},
        {6, 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290},
        {7, 0.630, 0.340, 0.310, 0.595, 0.155, 0.070, 0.3127, 0.3290},
        {8, 0.681, 0.319, 0.243, 0.692, 0.145, 0.049, 0.3100, 0.3160},
        {9, 0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290},
        {11, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3140, 0.3510},
        {12, 0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290},
        {22, 0.630, 0.340, 0.295, 0.605, 0.155, 0.077, 0.3127, 0.3290},
    };

    constexpr int lumaBytes[4] = {1, 2, 4, 5};

    struct Matrix {
        double m[3][3];
    };

    struct Ignored final: public Runable {
        void run() override;
    };

    enum class Model {
        Yuv,
        Rgb,
        Gray,
        Xyz,
        Palette,
        Bayer,
    };

    struct Case {
        AVPixelFormat format = AV_PIX_FMT_NONE;
        AVColorSpace matrix = AVCOL_SPC_BT709;
        AVColorRange range = AVCOL_RANGE_MPEG;
        AVColorTransferCharacteristic transfer = AVCOL_TRC_BT709;
        AVColorPrimaries primaries = AVCOL_PRI_BT709;
        AVChromaLocation location = AVCHROMA_LOC_LEFT;
    };

    struct Levels {
        double offset;
        double scale;
    };

    struct CompiledShader {
        VideoShader facts;
        RenderShader* shader;
    };

    struct FormatCheck {
        ObjPool* pool;
        Ui* ui;
        Vector<CompiledShader> compiled;
        Ignored retired;
        Vector<double> expected;
        int checked = 0;
        int failed = 0;
        const char* filter = "bilinear";
        const char* stage = "fragment";
        u32 bucket = 0;
        u32 buckets = 1;

        FormatCheck(ObjPool* pool, Ui* ui);
        VideoShader describe(const AVFrame* frame, const char* output);
        RenderShader& shaderFor(const VideoShader& facts);
        AVFrame* frame(const Case& kase);
        void write(AVFrame* frame, const Case& kase);
        void writePalette(AVFrame* frame);
        void linearize(const Case& kase);
        void shade(AVFrame* frame, const char* output, Vector<double>& out);
        void compare(StringView what, StringView output, const Vector<double>& got, double tolerance, bool relative);
        void verify(AVFrame* frame, StringView what, double tolerance);
        void verifyLinear(AVFrame* frame, StringView what, double tolerance);
        bool mine(StringView what);
        void check(const Case& kase, StringView what);
        void examine(const Case& kase, StringView what);
        void underlay();
        void formats();
        void matrices();
        void locations();
        void systems();
        void transfers();
        void primaries();
    };

    static double sampleValue(double x, double y, int c) {
        double u = x / (sampleWidth - 1);
        double v = y / (sampleHeight - 1);
        const double values[4] = {0.15 + 0.7 * u, 0.2 + 0.6 * v, 0.25 + 0.25 * (u + v), 0.3 + 0.7 * u};

        return values[c];
    }

    static double lanczos3(double x) {
        double a = 3.14159265358979323846 * x;

        return fabs(x) < 1e-9 ? 1. : 3. * sin(a) * sin(a / 3.) / (a * a);
    }

    static double srgbDecode(double v) {
        return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }

    static double srgbEncode(double v) {
        return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1. / 2.4) - 0.055;
    }

    static Matrix multiply(const Matrix& a, const Matrix& b) {
        Matrix out{};

        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                for (int k = 0; k < 3; k++) {
                    out.m[i][j] += a.m[i][k] * b.m[k][j];
                }
            }
        }

        return out;
    }

    static Matrix invert(const Matrix& a) {
        const double (&m)[3][3] = a.m;
        double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

        return {{
            {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
            {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
            {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det},
        }};
    }

    static void apply(const Matrix& m, const double* in, double* out) {
        for (int i = 0; i < 3; i++) {
            out[i] = m.m[i][0] * in[0] + m.m[i][1] * in[1] + m.m[i][2] * in[2];
        }
    }

    static Matrix rgbToXyz(const Chromaticity& c) {
        auto xyz = [](double x, double y, double* out) {
            out[0] = x / y;
            out[1] = 1.;
            out[2] = (1. - x - y) / y;
        };
        double r[3], g[3], b[3], w[3], scale[3];

        xyz(c.rx, c.ry, r);
        xyz(c.gx, c.gy, g);
        xyz(c.bx, c.by, b);
        xyz(c.wx, c.wy, w);

        Matrix primaries = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};

        apply(invert(primaries), w, scale);

        Matrix out = multiply(primaries, Matrix{{{scale[0], 0., 0.}, {0., scale[1], 0.}, {0., 0., scale[2]}}});

        if (c.wx != 0.3127 || c.wy != 0.3290) {
            const Matrix bradford = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
            double d65[3], from[3], to[3];

            xyz(0.3127, 0.3290, d65);
            apply(bradford, w, from);
            apply(bradford, d65, to);
            out = multiply(multiply(invert(bradford), multiply(Matrix{{{to[0] / from[0], 0., 0.}, {0., to[1] / from[1], 0.}, {0., 0., to[2] / from[2]}}}, bradford)), out);
        }

        return out;
    }

    static Matrix toBt2020(int code) {
        Matrix fromXyz = invert(rgbToXyz(chromaticities[7]));

        if (code == 10) {
            return fromXyz;
        }

        for (const Chromaticity& c : chromaticities) {
            if (c.code == code) {
                return multiply(fromXyz, rgbToXyz(c));
            }
        }

        fail(StringView(u8"no chromaticities for these primaries"));
    }

    static double pqEncode(double nits) {
        const double m1 = 2610. / 16384.;
        const double m2 = 2523. / 32.;
        const double c1 = 3424. / 4096.;
        const double c2 = 2413. / 128.;
        const double c3 = 2392. / 128.;
        double y = pow(nits / 10000., m1);

        return pow((c1 + c2 * y) / (1. + c3 * y), m2);
    }

    static double hlgEncode(double scene) {
        const double a = 0.17883277;
        const double b = 0.28466892;
        const double c = 0.55991073;

        return scene <= 1. / 12. ? sqrt(3. * scene) : a * log(12. * scene - b) + c;
    }

    static double encodeTransfer(int code, double linear, double white) {
        switch (code) {
            case 4:
                return pow(linear, 1. / 2.2);
            case 5:
                return pow(linear, 1. / 2.8);
            case 8:
                return linear;
            case 9:
                return 1. + log10(linear) / 2.;
            case 10:
                return 1. + log10(linear) / 2.5;
            case 13:
                return srgbEncode(linear);
            case 16:
                return pqEncode(linear * white);
            case 17:
                return pow(linear * 48. / 52.37, 1. / 2.6);
            case 18:
                return hlgEncode(pow(linear * white / 1000., 1. / 1.2));
            default:
                return pow(linear, 1. / 2.4);
        }
    }

    static double pqDecode(double signal) {
        const double m1 = 2610. / 16384.;
        const double m2 = 2523. / 32.;
        const double c1 = 3424. / 4096.;
        const double c2 = 2413. / 128.;
        const double c3 = 2392. / 128.;
        double p = pow(signal, 1. / m2);

        return pow(fmax(p - c1, 0.) / (c2 - c3 * p), 1. / m1) * 10000.;
    }

    static double hlgDecode(double signal) {
        const double a = 0.17883277;
        const double b = 0.28466892;
        const double c = 0.55991073;

        return signal <= 0.5 ? signal * signal / 3. : (exp((signal - c) / a) + b) / 12.;
    }

    static double bt709Encode(double light, double alpha, double beta, double slope) {
        return light < beta ? slope * light : alpha * pow(light, 0.45) - (alpha - 1.);
    }

    static double bt709Decode(double signal, double alpha, double beta, double slope) {
        return signal < slope * beta ? signal / slope : pow((signal + alpha - 1.) / alpha, 1. / 0.45);
    }

    static double oetf(int code, double light) {
        switch (code) {
            case 4:
                return pow(light, 1. / 2.2);
            case 5:
                return pow(light, 1. / 2.8);
            case 7:
                return bt709Encode(light, 1.1115, 0.0228, 4.);
            case 8:
                return light;
            case 9:
                return light < 0.01 ? 0. : 1. + log10(light) / 2.;
            case 10:
                return light < pow(10., -2.5) ? 0. : 1. + log10(light) / 2.5;
            case 13:
                return srgbEncode(light);
            case 16:
                return pqEncode(light * 10000.);
            case 17:
                return pow(light * 48. / 52.37, 1. / 2.6);
            case 18:
                return hlgEncode(light);
            default:
                return bt709Encode(light, 1.099296826809442, 0.018053968510807, 4.5);
        }
    }

    static double inverseOetf(int code, double signal) {
        switch (code) {
            case 4:
                return pow(signal, 2.2);
            case 5:
                return pow(signal, 2.8);
            case 7:
                return bt709Decode(signal, 1.1115, 0.0228, 4.);
            case 8:
                return signal;
            case 9:
                return signal <= 0. ? 0. : pow(10., (signal - 1.) * 2.);
            case 10:
                return signal <= 0. ? 0. : pow(10., (signal - 1.) * 2.5);
            case 13:
                return srgbDecode(signal);
            case 16:
                return pqDecode(signal) / 10000.;
            case 17:
                return pow(signal, 2.6) * 52.37 / 48.;
            case 18:
                return hlgDecode(signal);
            default:
                return bt709Decode(signal, 1.099296826809442, 0.018053968510807, 4.5);
        }
    }

    static Matrix primariesToXyz(int code) {
        if (code == AVCOL_PRI_SMPTE428) {
            return Matrix{{{1., 0., 0.}, {0., 1., 0.}, {0., 0., 1.}}};
        }

        for (const Chromaticity& c : chromaticities) {
            if (c.code == code) {
                return rgbToXyz(c);
            }
        }

        return rgbToXyz(chromaticities[0]);
    }

    static void displayLight(int transfer, int primaries, const double* signal, double* light) {
        double white = RendererOptions{}.sdrWhiteNits;
        Matrix toXyz = primariesToXyz(primaries);

        for (int c = 0; c < 3; c++) {
            double v = fmax(signal[c], 0.);

            switch (transfer) {
                case 4:
                    light[c] = pow(v, 2.2);
                    break;
                case 5:
                    light[c] = pow(v, 2.8);
                    break;
                case 8:
                    light[c] = signal[c];
                    break;
                case 9:
                case 10:
                    light[c] = inverseOetf(transfer, signal[c]);
                    break;
                case 13:
                    light[c] = srgbDecode(v);
                    break;
                case 16:
                    light[c] = pqDecode(v) / white;
                    break;
                case 17:
                    light[c] = pow(v, 2.6) * 52.37 / 48.;
                    break;
                case 18:
                    light[c] = hlgDecode(v);
                    break;
                default:
                    light[c] = pow(v, 2.4);
                    break;
            }
        }

        if (transfer == 18) {
            double luminance = fmax(toXyz.m[1][0] * light[0] + toXyz.m[1][1] * light[1] + toXyz.m[1][2] * light[2], 0.);

            for (int c = 0; c < 3; c++) {
                light[c] = 1000. * pow(luminance, 0.2) * light[c] / white;
            }
        }
    }

    static const VideoLayout* layoutOf(const char* name) {
        for (const VideoFormat& format : videoFormats) {
            if (name && !strcmp(format.name, name)) {
                return &videoLayouts[format.layout];
            }
        }

        return nullptr;
    }

    template <class Entry, size_t N>
    static const Entry& entryOf(const Entry (&entries)[N], int code) {
        for (const Entry& entry : entries) {
            if (entry.code == code) {
                return entry;
            }
        }

        fail(StringView(StringBuilder() << StringView(u8"no table entry for code ") << (i64)code));
    }

    static Model modelOf(const VideoLayout& layout) {
        StringView model(layout.model);

        const StringView names[] = {StringView(u8"yuv"), StringView(u8"rgb"), StringView(u8"gray"), StringView(u8"xyz"), StringView(u8"palette")};
        const Model models[] = {Model::Yuv, Model::Rgb, Model::Gray, Model::Xyz, Model::Palette};

        for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
            if (model == names[i]) {
                return models[i];
            }
        }

        return Model::Bayer;
    }

    static double lumaWeight(const Case& kase, int channel) {
        switch (kase.matrix) {
            case AVCOL_SPC_BT709:
                return channel == 0 ? 0.2126 : 0.0722;
            case AVCOL_SPC_FCC:
                return channel == 0 ? 0.30 : 0.11;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
                return channel == 0 ? 0.299 : 0.114;
            case AVCOL_SPC_SMPTE240M:
                return channel == 0 ? 0.212 : 0.087;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                return channel == 0 ? 0.2627 : 0.0593;
            case AVCOL_SPC_CHROMA_DERIVED_NCL:
            case AVCOL_SPC_CHROMA_DERIVED_CL:
                return primariesToXyz(kase.primaries).m[1][channel == 0 ? 0 : 2];
            default:
                return sampleHeight > 576 ? (channel == 0 ? 0.2126 : 0.0722) : (channel == 0 ? 0.299 : 0.114);
        }
    }

    static void forward(const Case& kase, const double* rgb, double* ycc) {
        double kr = lumaWeight(kase, 0);
        double kb = lumaWeight(kase, 2);
        double kg = 1. - kr - kb;

        switch (kase.matrix) {
            case AVCOL_SPC_RGB:
                ycc[0] = rgb[1];
                ycc[1] = rgb[2];
                ycc[2] = rgb[0];
                return;
            case AVCOL_SPC_YCGCO:
                ycc[0] = rgb[0] / 4. + rgb[1] / 2. + rgb[2] / 4.;
                ycc[1] = -rgb[0] / 4. + rgb[1] / 2. - rgb[2] / 4.;
                ycc[2] = rgb[0] / 2. - rgb[2] / 2.;
                return;
            case AVCOL_SPC_YCGCO_RE:
            case AVCOL_SPC_YCGCO_RO: {
                double co = rgb[0] - rgb[2];
                double t = rgb[2] + co / 2.;
                double cg = rgb[1] - t;

                ycc[0] = t + cg / 2.;
                ycc[1] = cg;
                ycc[2] = co;
                return;
            }
            case AVCOL_SPC_SMPTE2085:
                ycc[0] = rgb[1];
                ycc[1] = (0.986566 * rgb[2] - rgb[1]) / 2.;
                ycc[2] = (rgb[0] - 0.991902 * rgb[1]) / 2.;
                return;
            case AVCOL_SPC_BT2020_CL:
            case AVCOL_SPC_CHROMA_DERIVED_CL: {
                int tf = kase.transfer;
                double y = oetf(tf, kr * inverseOetf(tf, rgb[0]) + kg * inverseOetf(tf, rgb[1]) + kb * inverseOetf(tf, rgb[2]));
                double b = rgb[2] - y;
                double r = rgb[0] - y;

                ycc[0] = y;
                ycc[1] = b / (2. * (b <= 0. ? oetf(tf, 1. - kb) : 1. - oetf(tf, kb)));
                ycc[2] = r / (2. * (r <= 0. ? oetf(tf, 1. - kr) : 1. - oetf(tf, kr)));
                return;
            }
            case AVCOL_SPC_ICTCP: {
                bool hlg = kase.transfer == AVCOL_TRC_ARIB_STD_B67;
                const Matrix toLms = {{{1688. / 4096., 2146. / 4096., 262. / 4096.}, {683. / 4096., 2951. / 4096., 462. / 4096.}, {99. / 4096., 309. / 4096., 3688. / 4096.}}};
                const Matrix hlgToIctcp = {{{0.5, 0.5, 0.}, {3625. / 4096., -7465. / 4096., 3840. / 4096.}, {9500. / 4096., -9212. / 4096., -288. / 4096.}}};
                const Matrix pqToIctcp = {{{0.5, 0.5, 0.}, {6610. / 4096., -13613. / 4096., 7003. / 4096.}, {17933. / 4096., -17390. / 4096., -543. / 4096.}}};
                double light[3];
                double lms[3];

                for (int c = 0; c < 3; c++) {
                    light[c] = hlg ? hlgDecode(rgb[c]) : pqDecode(rgb[c]);
                }

                apply(toLms, light, lms);

                for (int c = 0; c < 3; c++) {
                    lms[c] = hlg ? hlgEncode(lms[c]) : pqEncode(lms[c]);
                }

                apply(hlg ? hlgToIctcp : pqToIctcp, lms, ycc);
                return;
            }
            default:
                ycc[0] = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
                ycc[1] = (rgb[2] - ycc[0]) / (2. * (1. - kb));
                ycc[2] = (rgb[0] - ycc[0]) / (2. * (1. - kr));
                return;
        }
    }

    static bool reversible(const Case& kase) {
        return kase.matrix == AVCOL_SPC_YCGCO_RE || kase.matrix == AVCOL_SPC_YCGCO_RO;
    }

    static Levels levels(const Case& kase, const VideoLayout& layout, int c) {
        Model model = modelOf(layout);
        int depth = layout.components[c][4];
        bool alpha = layout.alpha && c == layout.count - 1;
        bool chroma = model == Model::Yuv && (c == 1 || c == 2) && kase.matrix != AVCOL_SPC_RGB;
        bool full = depth < 8 || kase.range == AVCOL_RANGE_JPEG || (kase.range == AVCOL_RANGE_UNSPECIFIED && model != Model::Yuv);
        double unit = exp2(depth - 8.);

        if (layout.floating) {
            return {0., 1.};
        }

        if (alpha) {
            return {0., exp2(depth) - 1.};
        }

        if (model == Model::Yuv && reversible(kase)) {
            double scale = exp2(depth - (kase.matrix == AVCOL_SPC_YCGCO_RE ? 2 : 1)) - 1.;

            return {chroma ? exp2(depth - 1.) : 0., scale};
        }

        if (chroma) {
            return full ? Levels{exp2(depth - 1.), exp2(depth) - 1.} : Levels{128. * unit, 224. * unit};
        }

        return full ? Levels{0., exp2(depth) - 1.} : Levels{16. * unit, 219. * unit};
    }

    static double within(AVChromaLocation location, int axis) {
        const double sites[7][2] = {{0., 0.5}, {0., 0.5}, {0.5, 0.5}, {0., 0.}, {0.5, 0.}, {0., 1.}, {0.5, 1.}};

        return sites[location][axis];
    }

    static u32 halfBits(double value) {
        u32 bits = __builtin_bit_cast(u32, (float)value);
        u32 sign = (bits >> 16) & 0x8000u;
        int exponent = (int)((bits >> 23) & 255u) - 127 + 15;
        u32 mantissa = bits & 0x7fffffu;
        u32 rest = mantissa & 0x1fffu;
        u32 half = sign | (u32)exponent << 10 | mantissa >> 13;

        if (exponent <= 0) {
            return sign;
        }

        return rest > 0x1000u || (rest == 0x1000u && (half & 1u)) ? half + 1 : half;
    }

    static double halfValue(u32 half) {
        return (1. + (half & 1023u) / 1024.) * exp2((int)((half >> 10) & 31u) - 15) * (half & 0x8000u ? -1. : 1.);
    }

    static u32 encode(double value, Levels levels, const VideoLayout& layout, int c) {
        int depth = layout.components[c][4];

        if (layout.floating) {
            return depth == 16 ? halfBits(value) : __builtin_bit_cast(u32, (float)value);
        }

        double top = exp2(depth) - 1.;
        double code = round(value * levels.scale + levels.offset);

        return (u32)(code < 0. ? 0. : code > top ? top : code);
    }

    static double decode(u32 code, Levels levels, const VideoLayout& layout, int c) {
        if (layout.floating) {
            return layout.components[c][4] == 16 ? halfValue(code) : __builtin_bit_cast(float, code);
        }

        return ((double)code - levels.offset) / levels.scale;
    }

    static int mirrored(int at, int extent) {
        int reflected = at < 0 ? -at : at;
        int distance = extent - 1 - reflected;

        return extent - 1 - (distance < 0 ? -distance : distance);
    }

    static int bayerRed(const AVPixFmtDescriptor* descriptor) {
        const char* pattern = descriptor->name + 6;
        int red = 0;

        while (pattern[red] != 'r') {
            red++;
        }

        return red;
    }

    static int bayerChannel(const AVPixFmtDescriptor* descriptor, int x, int y) {
        int cell = (y & 1) * 2 + (x & 1);
        int red = bayerRed(descriptor);

        return cell == red ? 0 : cell == 3 - red ? 2 : 1;
    }

    static void writeRgb48(AVFrame* frame, int x, int y, const double* rgb) {
        u16* pixel = (u16*)(frame->data[0] + y * frame->linesize[0]) + x * 3;

        for (int c = 0; c < 3; c++) {
            double value = rgb[c] < 0. ? 0. : rgb[c] > 1. ? 1. : rgb[c];

            pixel[c] = (u16)lround(value * 65535.);
        }
    }
}

void Ignored::run() {
}

FormatCheck::FormatCheck(ObjPool* pool_, Ui* ui_)
    : pool(pool_)
    , ui(ui_)
{
}

VideoShader FormatCheck::describe(const AVFrame* frame, const char* output) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
    const VideoLayout& layout = *layoutOf(descriptor->name);
    Model model = modelOf(layout);
    Case kase;

    kase.format = (AVPixelFormat)frame->format;
    kase.matrix = frame->colorspace;
    kase.range = frame->color_range;
    kase.transfer = frame->color_trc;
    kase.primaries = frame->color_primaries;
    kase.location = frame->chroma_location;

    bool xyz = model == Model::Xyz;
    bool sdr = !strcmp(output, "sdr");
    int transferCode = xyz && kase.transfer == AVCOL_TRC_UNSPECIFIED ? AVCOL_TRC_SMPTE428 : kase.transfer;
    const VideoTransfer& transfer = entryOf(videoTransfers, transferCode);
    const Matrix identity = {{{1., 0., 0.}, {0., 1., 0.}, {0., 0., 1.}}};
    Matrix toXyz = primariesToXyz(xyz ? AVCOL_PRI_SMPTE428 : kase.primaries);
    Matrix toOutput = multiply(invert(primariesToXyz(sdr ? AVCOL_PRI_BT709 : AVCOL_PRI_BT2020)), toXyz);
    bool same = true;

    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
            same = same && fabs(toOutput.m[row][column] - identity.m[row][column]) < 1e-6;
        }
    }

    VideoShader out;

    memset(&out, 0, sizeof(out));

    StringView shape(transfer.shape);

    out.layout = &layout;
    out.system = model == Model::Yuv ? entryOf(videoMatrices, kase.matrix).system : layout.model;
    out.transfer = transfer.shape;
    out.conversion = same ? "same" : "convert";
    out.output = output;
    out.filter = filter;
    out.stage = stage;
    memcpy(out.curve, transfer.eotf, sizeof(out.curve));
    memcpy(out.oetf, transfer.oetf, sizeof(out.oetf));
    memcpy(out.inverse, transfer.inverse, sizeof(out.inverse));

    if (shape == StringView(u8"curve") && same && transferCode == (sdr ? AVCOL_TRC_IEC61966_2_1 : AVCOL_TRC_LINEAR)) {
        out.transfer = "identity";
    }

    double offsets[4] = {};
    double scales[4] = {1., 1., 1., 1.};

    for (int c = 0; c < layout.count && model != Model::Palette; c++) {
        int slot = layout.alpha && c == layout.count - 1 ? 3 : c;
        Levels level = model == Model::Bayer ? Levels{0., exp2(8. * layout.components[0][1]) - 1.} : levels(kase, layout, c);

        offsets[slot] = level.offset;
        scales[slot] = level.scale;
    }

    if (layout.inverted) {
        offsets[0] = 1.;
        scales[0] = -1.;
    }

    Matrix toSignal = identity;

    if (model == Model::Yuv && !strcmp(out.system, "linear")) {
        Matrix fromSignal = {};

        for (int k = 0; k < 3; k++) {
            double rgb[3] = {k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0.};
            double ycc[3];

            forward(kase, rgb, ycc);

            for (int c = 0; c < 3; c++) {
                fromSignal.m[c][k] = ycc[c];
            }
        }

        toSignal = invert(fromSignal);
    } else if (model == Model::Gray) {
        toSignal = Matrix{{{1., 0., 0.}, {1., 0., 0.}, {1., 0., 0.}}};
    }

    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
            out.decode[row][column] = toSignal.m[row][column] / scales[column];
            out.bias[row] -= out.decode[row][column] * offsets[column];
            out.toOutput[row][column] = toOutput.m[row][column];
        }
    }

    out.bias[3] = 1. / scales[3];

    const uint8_t* base = frame->buf[0]->data;

    for (int p = 0; p < 4 && frame->data[p]; p++) {
        out.planeOffset[p] = (u32)(frame->data[p] - base);
        out.lineSize[p] = (u32)frame->linesize[p];
    }

    int shiftX = descriptor->log2_chroma_w;
    int shiftY = descriptor->log2_chroma_h;
    int red = model == Model::Bayer ? bayerRed(descriptor) : 0;
    double white = RendererOptions{}.sdrWhiteNits;

    out.size[0] = (u32)frame->width;
    out.size[1] = (u32)frame->height;
    out.size[2] = (u32)AV_CEIL_RSHIFT(frame->width, shiftX);
    out.size[3] = (u32)AV_CEIL_RSHIFT(frame->height, shiftY);
    out.target[0] = (u32)frame->width;
    out.target[1] = (u32)frame->height;
    out.chroma[0] = exp2(-shiftX);
    out.chroma[1] = exp2(-shiftY);
    out.chroma[2] = within(kase.location, 0) * (exp2(shiftX) - 1.) * exp2(-shiftX);
    out.chroma[3] = within(kase.location, 1) * (exp2(shiftY) - 1.) * exp2(-shiftY);
    out.sites[0] = red % 2;
    out.sites[1] = red / 2;
    out.weights[0] = lumaWeight(kase, 0);
    out.weights[1] = lumaWeight(kase, 2);
    out.light[0] = transfer.decades;
    out.light[1] = 10000. / white;
    out.light[2] = 1000. / white;

    for (int i = 0; i < 3; i++) {
        out.luminance[i] = toXyz.m[1][i];
    }

    return out;
}

RenderShader& FormatCheck::shaderFor(const VideoShader& facts) {
    for (const CompiledShader& known : compiled) {
        if (!memcmp(&known.facts, &facts, sizeof(facts))) {
            return *known.shader;
        }
    }

    ScopedPtr<ObjPool> scratch{ObjPool::fromMemoryRaw()};
    StringView code = compile(*scratch.ptr, facts);
    RenderShader* shader = !strcmp(facts.stage, "kernel") ? ui->compileKernel(*pool, code.data(), code.length(), kernelTile) : ui->compileShader(*pool, code.data(), code.length());

    compiled.pushBack(CompiledShader{facts, shader});

    return *shader;
}

AVFrame* FormatCheck::frame(const Case& kase) {
    AVFrame* frame = av_frame_alloc();

    frame->format = kase.format;
    frame->width = sampleWidth;
    frame->height = sampleHeight;
    frame->colorspace = kase.matrix;
    frame->color_range = kase.range;
    frame->color_trc = kase.transfer;
    frame->color_primaries = kase.primaries;
    frame->chroma_location = kase.location;

    int e = av_frame_get_buffer(frame, 64);

    if (e < 0 || frame->buf[1]) {
        av_frame_free(&frame);
        fail(StringView(u8"a test frame takes one buffer"));
    }

    memZero(frame->buf[0]->data, frame->buf[0]->data + frame->buf[0]->size);

    return frame;
}

void FormatCheck::write(AVFrame* frame, const Case& kase) {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(kase.format);
    const VideoLayout& layout = *layoutOf(descriptor->name);
    Model model = modelOf(layout);
    bool alpha = layout.alpha;
    int count = layout.count;
    AVPixFmtDescriptor written = *descriptor;
    int stepX = 1 << descriptor->log2_chroma_w;
    int stepY = 1 << descriptor->log2_chroma_h;
    int chromaWidth = AV_CEIL_RSHIFT(sampleWidth, (int)descriptor->log2_chroma_w);
    int chromaHeight = AV_CEIL_RSHIFT(sampleHeight, (int)descriptor->log2_chroma_h);
    double siteX = within(kase.location, 0) * (stepX - 1);
    double siteY = within(kase.location, 1) * (stepY - 1);
    auto source = [&](int c, double x, double y) {
        double rgba[4];
        double ycc[3];

        for (int k = 0; k < 4; k++) {
            rgba[k] = sampleValue(x, y, k);
        }

        if (alpha && c == count - 1) {
            return rgba[3];
        }

        if (model == Model::Gray) {
            return 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2];
        }

        if (model == Model::Yuv) {
            forward(kase, rgba, ycc);

            return ycc[c];
        }

        return rgba[c];
    };
    auto filtered = [&](int c, double cx, double cy) {
        double bx = floor(cx);
        double by = floor(cy);
        double wx[6];
        double wy[6];
        double sx = 0.;
        double sy = 0.;
        double total = 0.;

        for (int k = 0; k < 6; k++) {
            wx[k] = lanczos3(k - 2 - (cx - bx));
            wy[k] = lanczos3(k - 2 - (cy - by));
            sx += wx[k];
            sy += wy[k];
        }

        for (int j = 0; j < 6; j++) {
            for (int k = 0; k < 6; k++) {
                double at = fmin(fmax(bx - 2 + k, 0.), chromaWidth - 1.) * stepX + siteX;
                double row = fmin(fmax(by - 2 + j, 0.), chromaHeight - 1.) * stepY + siteY;

                total += wx[k] * wy[j] * source(c, at, row);
            }
        }

        return total / (sx * sy);
    };
    auto stored = [&](int c, double x, double y) {
        Levels scale = levels(kase, layout, c);

        return decode(encode(source(c, x, y), scale, layout, c), scale, layout, c);
    };

    written.flags = (layout.bigEndian ? AV_PIX_FMT_FLAG_BE : 0) | (layout.bits ? AV_PIX_FMT_FLAG_BITSTREAM : 0);

    for (int c = 0; c < count; c++) {
        written.comp[c] = {layout.components[c][0], layout.components[c][1], layout.components[c][2], layout.components[c][3], layout.components[c][4]};
    }

    expected.clear();

    if (model == Model::Bayer) {
        int step = layout.components[0][1];
        bool bigEndian = layout.bigEndian;
        double top = exp2(8 * step) - 1.;
        auto value = [&](int x, int y) {
            x = mirrored(x, sampleWidth);
            y = mirrored(y, sampleHeight);

            return round(sampleValue(x, y, bayerChannel(descriptor, x, y)) * top) / top;
        };

        for (int y = 0; y < sampleHeight; y++) {
            uint8_t* row = frame->data[0] + y * frame->linesize[0];

            for (int x = 0; x < sampleWidth; x++) {
                u32 code = (u32)lround(value(x, y) * top);

                if (step == 1) {
                    row[x] = (uint8_t)code;
                } else {
                    row[2 * x + (bigEndian ? 0 : 1)] = (uint8_t)(code >> 8);
                    row[2 * x + (bigEndian ? 1 : 0)] = (uint8_t)code;
                }
            }
        }

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                int channel = bayerChannel(descriptor, x, y);
                double here = value(x, y);
                double horizontal = (value(x - 1, y) + value(x + 1, y)) / 2.;
                double vertical = (value(x, y - 1) + value(x, y + 1)) / 2.;
                double cross = (horizontal + vertical) / 2.;
                double diagonal = (value(x - 1, y - 1) + value(x + 1, y - 1) + value(x - 1, y + 1) + value(x + 1, y + 1)) / 4.;
                bool redRow = (y & 1) == bayerRed(descriptor) / 2;
                double rgb[3] = {here, cross, diagonal};

                if (channel == 2) {
                    rgb[0] = diagonal;
                    rgb[2] = here;
                } else if (channel == 1) {
                    rgb[0] = redRow ? horizontal : vertical;
                    rgb[1] = here;
                    rgb[2] = redRow ? vertical : horizontal;
                }

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(rgb[c]);
                }

                expected.pushBack(1.);
            }
        }

        return;
    }

    for (int c = 0; c < count; c++) {
        bool chroma = model == Model::Yuv && (c == 1 || c == 2);
        int width = chroma ? chromaWidth : sampleWidth;
        int height = chroma ? chromaHeight : sampleHeight;
        Levels scale = levels(kase, layout, c);
        u32 line[sampleWidth];

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                double at = chroma ? x * stepX + siteX : x;
                double row = chroma ? y * stepY + siteY : y;

                line[x] = encode(source(c, at, row), scale, layout, c);

                if (kase.format == AV_PIX_FMT_MONOWHITE) {
                    line[x] ^= 1u;
                }

                if (kase.format == AV_PIX_FMT_UYYVYY411 && c == 0) {
                    frame->data[0][y * frame->linesize[0] + x / 4 * 6 + lumaBytes[x % 4]] = (uint8_t)line[x];
                }
            }

            if (!(kase.format == AV_PIX_FMT_UYYVYY411 && c == 0)) {
                av_write_image_line2(line, frame->data, frame->linesize, &written, 0, y, c, width, 4);
            }
        }
    }

    Matrix toSignal = {};

    if (model == Model::Yuv && (descriptor->log2_chroma_w || descriptor->log2_chroma_h)) {
        Matrix fromSignal = {};

        for (int k = 0; k < 3; k++) {
            double rgb[3] = {k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0.};
            double ycc[3];

            forward(kase, rgb, ycc);

            for (int c = 0; c < 3; c++) {
                fromSignal.m[c][k] = ycc[c];
            }
        }

        toSignal = invert(fromSignal);
    }

    for (int y = 0; y < sampleHeight; y++) {
        for (int x = 0; x < sampleWidth; x++) {
            double rgba[4] = {sampleValue(x, y, 0), sampleValue(x, y, 1), sampleValue(x, y, 2), alpha ? stored(count - 1, x, y) : 1.};

            if (model == Model::Gray) {
                rgba[0] = rgba[1] = rgba[2] = stored(0, x, y);
            } else if (model != Model::Yuv) {
                for (int c = 0; c < 3; c++) {
                    rgba[c] = stored(c, x, y);
                }
            } else if (descriptor->log2_chroma_w || descriptor->log2_chroma_h) {
                double chromaX = fmin(fmax((x - siteX) / stepX, 0.), chromaWidth - 1.) * stepX + siteX;
                double chromaY = fmin(fmax((y - siteY) / stepY, 0.), chromaHeight - 1.) * stepY + siteY;
                double ycc[3] = {source(0, x, y), source(1, chromaX, chromaY), source(2, chromaX, chromaY)};

                if (!strcmp(filter, "lanczos")) {
                    ycc[1] = filtered(1, (x - siteX) / stepX, (y - siteY) / stepY);
                    ycc[2] = filtered(2, (x - siteX) / stepX, (y - siteY) / stepY);
                }

                apply(toSignal, ycc, rgba);
            }

            for (int c = 0; c < 4; c++) {
                expected.pushBack(rgba[c]);
            }
        }
    }
}

void FormatCheck::writePalette(AVFrame* frame) {
    u32* entries = (u32*)frame->data[1];

    for (u32 i = 0; i < 256; i++) {
        entries[i] = (255 - i) << 24 | i << 16 | ((i * 7) & 255) << 8 | ((i * 13) & 255);
    }

    expected.clear();

    for (int y = 0; y < sampleHeight; y++) {
        for (int x = 0; x < sampleWidth; x++) {
            u32 index = (u32)((x * 5 + y * 3) & 255);
            u32 entry = entries[index];

            frame->data[0][y * frame->linesize[0] + x] = (uint8_t)index;
            expected.pushBack(pow(srgbDecode(((entry >> 16) & 255) / 255.), 1. / 2.4));
            expected.pushBack(pow(srgbDecode(((entry >> 8) & 255) / 255.), 1. / 2.4));
            expected.pushBack(pow(srgbDecode((entry & 255) / 255.), 1. / 2.4));
            expected.pushBack((entry >> 24) / 255.);
        }
    }
}

void FormatCheck::linearize(const Case& kase) {
    Matrix toScene = toBt2020(kase.primaries);

    for (size_t i = 0; i < samplePixels; i++) {
        double signal[3] = {expected[i * 4], expected[i * 4 + 1], expected[i * 4 + 2]};
        double light[3];
        double scene[3];

        displayLight(kase.transfer, kase.primaries, signal, light);
        apply(toScene, light, scene);

        for (int c = 0; c < 3; c++) {
            expected.mut(i * 4 + c) = scene[c];
        }
    }
}

void FormatCheck::shade(AVFrame* frame, const char* output, Vector<double>& out) {
    VideoShader facts = describe(frame, output);
    ScopedPtr<ObjPool> owner{ObjPool::fromMemoryRaw()};
    RenderImage* image = ui->shadeImage(*owner.ptr, shaderFor(facts), (u32)frame->width, (u32)frame->height, frame->buf[0]->data, frame->buf[0]->size, retired);
    ImagePixels pixels;

    image->prepare();
    image->read(0, 0, frame->width, frame->height, pixels);

    const float* got = (const float*)pixels.rgbaf.data();

    out.clear();

    for (size_t i = 0; i < samplePixels * 4; i++) {
        out.pushBack(got[i]);
    }
}

void FormatCheck::compare(StringView what, StringView output, const Vector<double>& got, double tolerance, bool relative) {
    double worst = 0.;
    size_t at = 0;

    for (size_t i = 0; i < samplePixels * 4; i++) {
        double error = fabs(got[i] - expected[i]) / (relative ? fmax(1., fabs(expected[i])) : 1.);

        if (!(error <= worst) && !isnan(worst)) {
            worst = error;
            at = i;
        }
    }

    checked++;

    if (!(worst <= tolerance)) {
        failed++;
        sysE << StringView(u8"video formats: ") << what << StringView(u8" ") << output << StringView(u8" is off by ") << worst << StringView(u8" at ") << (u64)(at / 4 % sampleWidth) << StringView(u8",") << (u64)(at / 4 / sampleWidth) << StringView(u8" channel ") << (u64)(at % 4) << StringView(u8", got ") << got[at] << StringView(u8" for ") << expected[at] << StringView(u8", allowed ") << tolerance << endL;
    }
}

void FormatCheck::verify(AVFrame* frame, StringView what, double tolerance) {
    Vector<double> got;

    shade(frame, "sdr", got);

    for (size_t i = 0; i < samplePixels * 4; i++) {
        if (i % 4 != 3) {
            got.mut(i) = pow(srgbDecode(got[i]), 1. / 2.4);
        }
    }

    compare(what, StringView(u8"sdr"), got, tolerance, false);
    shade(frame, "hdr", got);

    Matrix back = invert(toBt2020(1));

    for (size_t i = 0; i < samplePixels; i++) {
        double linear[3] = {got[i * 4], got[i * 4 + 1], got[i * 4 + 2]};
        double signal[3] = {expected[i * 4], expected[i * 4 + 1], expected[i * 4 + 2]};
        double rgb[3];
        double light[3];

        apply(back, linear, rgb);
        displayLight(AVCOL_TRC_BT709, AVCOL_PRI_BT709, signal, light);

        for (int c = 0; c < 3; c++) {
            got.mut(i * 4 + c) = rgb[c];
            expected.mut(i * 4 + c) = light[c];
        }
    }

    compare(what, StringView(u8"hdr"), got, 2.4 * tolerance, true);
}

void FormatCheck::verifyLinear(AVFrame* frame, StringView what, double tolerance) {
    Vector<double> got;

    shade(frame, "hdr", got);
    compare(what, StringView(u8"hdr"), got, tolerance, true);
}

bool FormatCheck::mine(StringView what) {
    u32 hash = 2166136261u;

    for (const u8* c = what.begin(); c != what.end(); c++) {
        hash = (hash ^ *c) * 16777619u;
    }

    return hash % buckets == bucket;
}

void FormatCheck::check(const Case& kase, StringView what) {
    if (!mine(what)) {
        return;
    }

    try {
        examine(kase, what);
    } catch (...) {
        checked++;
        failed++;
        sysE << StringView(u8"video formats: ") << what << StringView(u8": ") << Exception::current() << endL;
    }
}

void FormatCheck::examine(const Case& kase, StringView what) {
    const VideoLayout* layout = layoutOf(av_get_pix_fmt_name(kase.format));

    if (!layout) {
        checked++;
        failed++;
        sysE << StringView(u8"video formats: ") << what << StringView(u8" has no layout") << endL;

        return;
    }

    Model model = modelOf(*layout);
    AVFrame* frame = this->frame(kase);
    STD_DEFER {
        av_frame_free(&frame);
    };

    if (!strcmp(stage, "kernel") && !kernelable(describe(frame, "sdr"))) {
        return;
    }

    if (model == Model::Palette) {
        writePalette(frame);
        frame->color_trc = AVCOL_TRC_IEC61966_2_1;
        verify(frame, what, readback);

        return;
    }

    write(frame, kase);

    if (model == Model::Xyz || kase.transfer != AVCOL_TRC_BT709 || kase.primaries != AVCOL_PRI_BT709) {
        Case shown = kase;

        if (model == Model::Xyz && kase.transfer == AVCOL_TRC_UNSPECIFIED) {
            shown.transfer = AVCOL_TRC_SMPTE428;
        }

        linearize(shown);
        verifyLinear(frame, what, readback);

        return;
    }

    double tolerance = readback;

    if (model == Model::Yuv) {
        Matrix fromSignal = {};
        double steps[3];

        for (int k = 0; k < 3; k++) {
            double rgb[3] = {k == 0 ? 1. : 0., k == 1 ? 1. : 0., k == 2 ? 1. : 0.};
            double ycc[3];

            forward(kase, rgb, ycc);

            for (int c = 0; c < 3; c++) {
                fromSignal.m[c][k] = ycc[c];
            }

            steps[k] = layout->floating ? 0. : 0.5 / levels(kase, *layout, k).scale;
        }

        Matrix toSignal = invert(fromSignal);

        for (int c = 0; c < 3; c++) {
            double bound = readback;

            for (int k = 0; k < 3; k++) {
                bound += fabs(toSignal.m[c][k]) * steps[k];
            }

            tolerance = fmax(tolerance, bound);
        }
    }

    verify(frame, what, tolerance);
}

void FormatCheck::formats() {
    for (const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_next(nullptr); descriptor; descriptor = av_pix_fmt_desc_next(descriptor)) {
        if (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) {
            continue;
        }

        Case kase;
        const VideoLayout* layout = layoutOf(descriptor->name);
        Model model = layout ? modelOf(*layout) : Model::Rgb;

        kase.format = av_pix_fmt_desc_get_id(descriptor);
        kase.matrix = model == Model::Yuv ? AVCOL_SPC_BT709 : AVCOL_SPC_RGB;
        kase.range = model == Model::Yuv && !StringView(descriptor->name).startsWith(StringView(u8"yuvj")) ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;

        if (model == Model::Xyz) {
            kase.transfer = AVCOL_TRC_UNSPECIFIED;
            kase.primaries = AVCOL_PRI_SMPTE428;
        }

        check(kase, StringView(StringBuilder() << StringView(descriptor->name) << StringView(u8" ") << StringView(filter) << StringView(u8" ") << StringView(stage)));
    }
}

void FormatCheck::underlay() {
    if (!mine(StringView(u8"kernel underlay"))) {
        return;
    }

    checked++;

    if (!ui->kernels()) {
        failed++;
        sysE << StringView(u8"video formats: the display takes no kernels") << endL;

        return;
    }

    Case kase;

    kase.format = AV_PIX_FMT_YUV420P;

    AVFrame* frame = this->frame(kase);
    STD_DEFER {
        av_frame_free(&frame);
    };

    write(frame, kase);

    VideoShader facts = describe(frame, "sdr");

    facts.origin[0] = 1;
    facts.origin[1] = 1;

    ScopedPtr<ObjPool> owner{ObjPool::fromMemoryRaw()};
    RenderImage* image = ui->shadeImage(*owner.ptr, shaderFor(facts), (u32)frame->width, (u32)frame->height, frame->buf[0]->data, frame->buf[0]->size, retired);
    UiEvent event;
    int frames = 0;

    image->prepare();
    ui->requestFrame();

    while (ui->next(event)) {
        if (event.kind != UiEvent::Kind::Frame) {
            continue;
        }

        if (frames == 3) {
            break;
        }

        ImVec2 at = ImGui::GetMainViewport()->Pos;

        image->underlay(ImVec2(at.x + 1.f, at.y + 1.f), ImVec2(at.x + 1.f + (float)frame->width, at.y + 1.f + (float)frame->height));
        frames++;
        ui->requestFrame();
    }

    if (frames < 3) {
        failed++;
        sysE << StringView(u8"video formats: the kernel underlay saw ") << (u64)frames << StringView(u8" frames") << endL;
    }
}

void FormatCheck::matrices() {
    const AVPixelFormat formats[] = {AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUYV422, AV_PIX_FMT_Y210LE};
    const AVColorRange ranges[] = {AVCOL_RANGE_UNSPECIFIED, AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG};

    for (const VideoMatrix& matrix : videoMatrices) {
        if (strcmp(matrix.system, "linear")) {
            continue;
        }

        for (AVColorRange range : ranges) {
            for (AVPixelFormat format : formats) {
                Case kase;

                kase.format = format;
                kase.matrix = (AVColorSpace)matrix.code;
                kase.range = range;
                check(kase, StringView(StringBuilder() << StringView(av_get_pix_fmt_name(format)) << StringView(u8" matrix ") << (i64)matrix.code << StringView(u8" range ") << (i64)range));
            }
        }
    }
}

void FormatCheck::locations() {
    const AVPixelFormat formats[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV420P10LE,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21,
        AV_PIX_FMT_P010LE,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_UYVY422,
        AV_PIX_FMT_Y210LE,
        AV_PIX_FMT_UYYVYY411,
    };

    for (AVPixelFormat format : formats) {
        for (int location = AVCHROMA_LOC_UNSPECIFIED; location <= AVCHROMA_LOC_BOTTOM; location++) {
            Case kase;

            kase.format = format;
            kase.location = (AVChromaLocation)location;
            check(kase, StringView(StringBuilder() << StringView(av_get_pix_fmt_name(format)) << StringView(u8" chroma location ") << (i64)location));
        }
    }
}

void FormatCheck::systems() {
    for (const VideoMatrix& matrix : videoMatrices) {
        bool cl = !strcmp(matrix.system, "cl");

        if (!cl && strcmp(matrix.system, "ictcp")) {
            continue;
        }

        for (const VideoTransfer& transfer : videoTransfers) {
            if (!cl && transfer.code != AVCOL_TRC_SMPTE2084 && transfer.code != AVCOL_TRC_ARIB_STD_B67) {
                continue;
            }

            Case kase;

            kase.format = AV_PIX_FMT_YUV444P16LE;
            kase.matrix = (AVColorSpace)matrix.code;
            kase.range = AVCOL_RANGE_MPEG;
            kase.transfer = (AVColorTransferCharacteristic)transfer.code;
            kase.primaries = AVCOL_PRI_BT2020;
            check(kase, StringView(StringBuilder() << StringView(u8"yuv444p16le matrix ") << (i64)matrix.code << StringView(u8" transfer ") << (i64)transfer.code));
        }
    }
}

void FormatCheck::transfers() {
    float white = RendererOptions{}.sdrWhiteNits;

    for (const VideoTransfer& entry : videoTransfers) {
        Case kase;

        kase.format = AV_PIX_FMT_RGB48LE;
        kase.matrix = AVCOL_SPC_RGB;
        kase.range = AVCOL_RANGE_JPEG;
        kase.transfer = (AVColorTransferCharacteristic)entry.code;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };
        bool absolute = entry.code == AVCOL_TRC_SMPTE2084 || entry.code == AVCOL_TRC_ARIB_STD_B67;
        bool gray = entry.code == AVCOL_TRC_ARIB_STD_B67;
        Matrix toScene = toBt2020(1);

        expected.clear();

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                double linear[3];
                double encoded[3];
                double scene[3];

                for (int c = 0; c < 3; c++) {
                    double value = gray ? sampleValue(x, 0, 0) * sampleValue(0, y, 1) : sampleValue(x, y, c);

                    linear[c] = absolute ? value * 1000. / white : value;
                    encoded[c] = encodeTransfer(entry.code, linear[c], white);
                }

                writeRgb48(frame, x, y, encoded);
                apply(toScene, linear, scene);

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(scene[c]);
                }

                expected.pushBack(1.);
            }
        }

        verifyLinear(frame, StringView(StringBuilder() << StringView(u8"rgb48le transfer ") << (i64)entry.code), 0.02);
    }
}

void FormatCheck::primaries() {
    for (const VideoPrimaries& entry : videoPrimaries) {
        int code = entry.code;
        Case kase;

        kase.format = AV_PIX_FMT_RGB48LE;
        kase.matrix = AVCOL_SPC_RGB;
        kase.range = AVCOL_RANGE_JPEG;
        kase.transfer = AVCOL_TRC_LINEAR;
        kase.primaries = (AVColorPrimaries)code;

        AVFrame* frame = this->frame(kase);
        STD_DEFER {
            av_frame_free(&frame);
        };
        Matrix toScene = toBt2020(code);

        expected.clear();

        for (int y = 0; y < sampleHeight; y++) {
            for (int x = 0; x < sampleWidth; x++) {
                double linear[3] = {sampleValue(x, y, 0), sampleValue(x, y, 1), sampleValue(x, y, 2)};
                double scene[3];

                writeRgb48(frame, x, y, linear);
                apply(toScene, linear, scene);

                for (int c = 0; c < 3; c++) {
                    expected.pushBack(scene[c]);
                }

                expected.pushBack(1.);
            }
        }

        verifyLinear(frame, StringView(StringBuilder() << StringView(u8"rgb48le primaries ") << (i64)code), 0.005);
    }
}

int main(int argc, char** argv) {
    auto number = [](const char* text) {
        u32 value = 0;

        for (; *text >= '0' && *text <= '9'; text++) {
            value = value * 10 + (u32)(*text - '0');
        }

        return value;
    };

    if (argc != 3 || !number(argv[2]) || number(argv[1]) >= number(argv[2])) {
        sysE << StringView(u8"usage: video_test BUCKET BUCKETS") << endL;

        return 2;
    }

    ObjPool::Ref owner = ObjPool::fromMemory();
    ObjPool& pool = *owner;
    Ui& ui = *Ui::create(pool, StringView(u8"video-test"), {64_d, 64_d});
    FormatCheck& check = *pool.make<FormatCheck>(&pool, &ui);
    bool crashed = false;

    check.bucket = number(argv[1]);
    check.buckets = number(argv[2]);

    auto body = makeRunable([&] {
        try {
            check.formats();
            check.filter = "lanczos";
            check.formats();
            check.stage = "kernel";
            check.formats();
            check.underlay();
            check.stage = "fragment";
            check.filter = "bilinear";
            check.matrices();
            check.locations();
            check.systems();
            check.transfers();
            check.primaries();
        } catch (...) {
            crashed = true;
            sysE << StringView(u8"video formats: ") << Exception::current() << endL;
        }
    });
    int result = ui.run(body);

    if (crashed || check.failed) {
        return 1;
    }

    sysO << StringView(u8"OK: ") << (u64)check.checked << StringView(u8" video color checks") << endL;

    return result;
}
