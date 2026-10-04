#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int width, height;
    double* rgb;
} Image;

static void die(const char* what) {
    fprintf(stderr, "corpus: %s\n", what);
    exit(1);
}

static Image make(int width, int height) {
    Image image = {width, height, calloc((size_t)width * height * 3, sizeof(double))};
    if (!image.rgb) die("out of memory");
    return image;
}

static double clamp(double v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static double decode(double v) {
    v = clamp(v);
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double encode(double v) {
    v = clamp(v);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

static FILE* openFile(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (!f) die(path);
    return f;
}

static Image readPfm(const char* path) {
    FILE* f = openFile(path, "rb");
    char kind[3] = {0};
    int width, height;
    double scale;
    if (fscanf(f, "%2s %d %d %lf", kind, &width, &height, &scale) != 4 || strcmp(kind, "PF")) die("not a color PFM");
    fgetc(f);
    Image image = make(width, height);
    size_t row = (size_t)width * 3;
    uint8_t* bytes = malloc(row * 4);
    for (int y = height - 1; y >= 0; y--) {
        if (fread(bytes, 4, row, f) != row) die("short PFM");
        for (size_t i = 0; i < row; i++) {
            const uint8_t* b = bytes + 4 * i;
            uint32_t word = scale < 0 ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24 : (uint32_t)b[3] | (uint32_t)b[2] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[0] << 24;
            float v;
            memcpy(&v, &word, 4);
            image.rgb[(size_t)y * row + i] = v;
        }
    }
    free(bytes);
    fclose(f);
    return image;
}

static Image readPpm(const char* path) {
    FILE* f = openFile(path, "rb");
    char kind[3] = {0};
    int width, height, top;
    if (fscanf(f, "%2s %d %d %d", kind, &width, &height, &top) != 4 || strcmp(kind, "P6")) die("not a color PPM");
    fgetc(f);
    Image image = make(width, height);
    size_t count = (size_t)width * height * 3;
    int wide = top > 255;
    uint8_t* bytes = malloc(count * (wide ? 2 : 1));
    if (fread(bytes, wide ? 2 : 1, count, f) != count) die("short PPM");
    for (size_t i = 0; i < count; i++)
        image.rgb[i] = (wide ? (bytes[2 * i] << 8 | bytes[2 * i + 1]) : bytes[i]) / (double)top;
    free(bytes);
    fclose(f);
    return image;
}

static void writePpm(const char* path, const Image* image) {
    FILE* f = openFile(path, "wb");
    fprintf(f, "P6\n%d %d\n65535\n", image->width, image->height);
    size_t count = (size_t)image->width * image->height * 3;
    uint8_t* bytes = malloc(count * 2);
    for (size_t i = 0; i < count; i++) {
        unsigned v = (unsigned)lround(clamp(image->rgb[i]) * 65535);
        bytes[2 * i] = (uint8_t)(v >> 8);
        bytes[2 * i + 1] = (uint8_t)v;
    }
    fwrite(bytes, 2, count, f);
    free(bytes);
    fclose(f);
}

static void linearize(Image* image) {
    for (size_t i = 0; i < (size_t)image->width * image->height * 3; i++) image->rgb[i] = decode(image->rgb[i]);
}

static void delinearize(Image* image) {
    for (size_t i = 0; i < (size_t)image->width * image->height * 3; i++) image->rgb[i] = encode(image->rgb[i]);
}

static void boxRow(const double* in, size_t inStride, int count, double* out, size_t outStride, int target) {
    double step = (double)count / target;
    for (int i = 0; i < target; i++) {
        double lo = i * step, hi = lo + step;
        double sum[3] = {0, 0, 0};
        for (int j = (int)floor(lo); j < count && j < hi; j++) {
            double a = j > lo ? j : lo, b = j + 1 < hi ? j + 1 : hi;
            for (int c = 0; c < 3; c++) sum[c] += (b - a) * in[j * inStride + c];
        }
        for (int c = 0; c < 3; c++) out[i * outStride + c] = sum[c] / step;
    }
}

static Image box(const Image* in, int width, int height) {
    Image wide = make(width, in->height);
    for (int y = 0; y < in->height; y++) boxRow(in->rgb + (size_t)y * in->width * 3, 3, in->width, wide.rgb + (size_t)y * width * 3, 3, width);
    Image out = make(width, height);
    for (int x = 0; x < width; x++) boxRow(wide.rgb + (size_t)x * 3, (size_t)width * 3, in->height, out.rgb + (size_t)x * 3, (size_t)width * 3, height);
    free(wide.rgb);
    return out;
}

static double luma(const Image* image, int x, int y) {
    const double* p = image->rgb + ((size_t)y * image->width + x) * 3;
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

static int crop(int argc, char** argv) {
    if (argc != 6) die("usage: corpus crop IN.pfm OUT.ppm WIDTH HEIGHT");
    Image source = readPfm(argv[2]);
    int width = atoi(argv[4]), height = atoi(argv[5]);
    double scale = fmax(3840.0 / source.width, 2160.0 / source.height);
    if (scale > 1) scale = 1;
    int sw = (int)lround(source.width * scale), sh = (int)lround(source.height * scale);
    if (sw < width || sh < height) die("too small");
    linearize(&source);
    Image scaled = scale < 1 ? box(&source, sw, sh) : source;
    delinearize(&scaled);
    int bestX = 0, bestY = 0;
    double best = -1;
    for (int y0 = 0; y0 + height <= sh; y0 += height / 6)
        for (int x0 = 0; x0 + width <= sw; x0 += width / 6) {
            double detail = 0;
            for (int y = y0 + 1; y < y0 + height; y += 2)
                for (int x = x0 + 1; x < x0 + width; x += 2)
                    detail += fabs(luma(&scaled, x, y) - luma(&scaled, x - 1, y)) + fabs(luma(&scaled, x, y) - luma(&scaled, x, y - 1));
            if (detail > best) {
                best = detail;
                bestX = x0;
                bestY = y0;
            }
        }
    Image out = make(width, height);
    for (int y = 0; y < height; y++) memcpy(out.rgb + (size_t)y * width * 3, scaled.rgb + ((size_t)(bestY + y) * sw + bestX) * 3, (size_t)width * 3 * sizeof(double));
    writePpm(argv[3], &out);
    printf("crop %d %d %d %d scale %.6f\n", bestX, bestY, width, height, scale);
    return 0;
}

static int input(int argc, char** argv) {
    if (argc != 7) die("usage: corpus input TRUTH.ppm OUT.yuv WIDTH HEIGHT BITS");
    Image truth = readPpm(argv[2]);
    int width = atoi(argv[4]), height = atoi(argv[5]), bits = atoi(argv[6]);
    linearize(&truth);
    Image small = box(&truth, width, height);
    delinearize(&small);
    int cw = (width + 1) / 2, ch = (height + 1) / 2;
    double* ycc = malloc((size_t)width * height * 3 * sizeof(double));
    for (size_t i = 0; i < (size_t)width * height; i++) {
        const double* p = small.rgb + i * 3;
        double y = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        ycc[i * 3] = y;
        ycc[i * 3 + 1] = (p[2] - y) / 1.8556;
        ycc[i * 3 + 2] = (p[0] - y) / 1.5748;
    }
    double unit = (double)(1 << (bits - 8));
    size_t lumaCount = (size_t)width * height, chromaCount = (size_t)cw * ch;
    uint16_t* planes = malloc((lumaCount + 2 * chromaCount) * sizeof(uint16_t));
    for (size_t i = 0; i < lumaCount; i++) planes[i] = (uint16_t)lround((16 + 219 * ycc[i * 3]) * unit);
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++)
            for (int c = 1; c <= 2; c++) {
                double sum = 0;
                for (int r = 0; r < 2; r++) {
                    int y = 2 * j + r < height ? 2 * j + r : height - 1;
                    for (int k = -1; k <= 1; k++) {
                        int x = 2 * i + k < 0 ? 0 : 2 * i + k >= width ? width - 1 : 2 * i + k;
                        sum += (k ? 1 : 2) * ycc[((size_t)y * width + x) * 3 + c];
                    }
                }
                planes[lumaCount + (c - 1) * chromaCount + (size_t)j * cw + i] = (uint16_t)lround((128 + 224 * sum / 8) * unit);
            }
    FILE* f = openFile(argv[3], "wb");
    size_t total = lumaCount + 2 * chromaCount;
    if (bits == 8) {
        uint8_t* bytes = malloc(total);
        for (size_t i = 0; i < total; i++) bytes[i] = (uint8_t)planes[i];
        fwrite(bytes, 1, total, f);
        free(bytes);
    } else {
        fwrite(planes, 2, total, f);
    }
    fclose(f);
    return 0;
}

static float half(uint16_t h) {
    int exponent = (h >> 10) & 31, mantissa = h & 1023;
    double v = exponent == 0 ? ldexp(mantissa, -24) : exponent == 31 ? INFINITY : ldexp(mantissa | 1024, exponent - 25);
    return (float)(h & 0x8000 ? -v : v);
}

static void oklab(const double* srgb, double* lab) {
    double r = decode(srgb[0]), g = decode(srgb[1]), b = decode(srgb[2]);
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    lab[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    lab[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    lab[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

static double ssim(const double* a, const double* b, int width, int height) {
    static const int radius = 5;
    double weights[11], total = 0;
    for (int i = -radius; i <= radius; i++) total += weights[i + radius] = exp(-i * i / (2 * 1.5 * 1.5));
    for (int i = 0; i <= 2 * radius; i++) weights[i] /= total;
    size_t count = (size_t)width * height;
    double* fields[5];
    double* blurred[5];
    for (int k = 0; k < 5; k++) {
        fields[k] = malloc(count * sizeof(double));
        blurred[k] = malloc(count * sizeof(double));
    }
    for (size_t i = 0; i < count; i++) {
        fields[0][i] = a[i];
        fields[1][i] = b[i];
        fields[2][i] = a[i] * a[i];
        fields[3][i] = b[i] * b[i];
        fields[4][i] = a[i] * b[i];
    }
    double* line = malloc(count * sizeof(double));
    for (int k = 0; k < 5; k++) {
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                double s = 0;
                for (int i = -radius; i <= radius; i++) {
                    int at = x + i < 0 ? 0 : x + i >= width ? width - 1 : x + i;
                    s += weights[i + radius] * fields[k][(size_t)y * width + at];
                }
                line[(size_t)y * width + x] = s;
            }
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                double s = 0;
                for (int i = -radius; i <= radius; i++) {
                    int at = y + i < 0 ? 0 : y + i >= height ? height - 1 : y + i;
                    s += weights[i + radius] * line[(size_t)at * width + x];
                }
                blurred[k][(size_t)y * width + x] = s;
            }
    }
    double c1 = 0.01 * 0.01, c2 = 0.03 * 0.03, sum = 0;
    for (size_t i = 0; i < count; i++) {
        double ma = blurred[0][i], mb = blurred[1][i];
        double va = blurred[2][i] - ma * ma, vb = blurred[3][i] - mb * mb, cov = blurred[4][i] - ma * mb;
        sum += (2 * ma * mb + c1) * (2 * cov + c2) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
    }
    for (int k = 0; k < 5; k++) {
        free(fields[k]);
        free(blurred[k]);
    }
    free(line);
    return sum / count;
}

static int metric(int argc, char** argv) {
    if (argc != 4) die("usage: corpus metric TRUTH.ppm OUT.raw");
    Image truth = readPpm(argv[2]);
    int width = truth.width, height = truth.height;
    FILE* f = openFile(argv[3], "rb");
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t count = (size_t)width * height;
    int wide = size == count * 16;
    if (!wide && size != count * 8) die("raw output does not match the truth's size");
    uint8_t* bytes = malloc(size);
    if (fread(bytes, 1, size, f) != size) die("short raw output");
    fclose(f);
    int border = 8;
    double srgb = 0, linear = 0, delta = 0, chroma = 0;
    size_t inside = 0;
    double* ta = malloc(count * sizeof(double));
    double* oa = malloc(count * sizeof(double));
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            size_t i = (size_t)y * width + x;
            double out[3];
            for (int c = 0; c < 3; c++) {
                float v;
                if (wide) {
                    memcpy(&v, bytes + i * 16 + c * 4, 4);
                } else {
                    uint16_t h;
                    memcpy(&h, bytes + i * 8 + c * 2, 2);
                    v = half(h);
                }
                out[c] = clamp(v);
            }
            const double* t = truth.rgb + i * 3;
            ta[i] = 0.2126 * t[0] + 0.7152 * t[1] + 0.0722 * t[2];
            oa[i] = 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
            if (x < border || y < border || x >= width - border || y >= height - border) continue;
            inside++;
            for (int c = 0; c < 3; c++) {
                double d = out[c] - t[c];
                srgb += d * d;
                d = decode(out[c]) - decode(t[c]);
                linear += d * d;
            }
            double la[3], lb[3];
            oklab(t, la);
            oklab(out, lb);
            double dl = la[0] - lb[0], da = la[1] - lb[1], db = la[2] - lb[2];
            delta += sqrt(dl * dl + da * da + db * db);
            chroma += sqrt(da * da + db * db);
        }
    double rmse = sqrt(srgb / (inside * 3.0));
    printf("psnr %.4f linear %.6f delta %.6f chroma %.6f ssim %.6f\n", 20 * log10(1 / rmse), sqrt(linear / (inside * 3.0)), delta / inside, chroma / inside, ssim(ta, oa, width, height));
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "crop")) return crop(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "input")) return input(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "metric")) return metric(argc, argv);
    die("usage: corpus crop|input|metric ...");
    return 1;
}
