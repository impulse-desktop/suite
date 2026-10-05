#include "error.h"
#include "shader.h"
#include "renderer.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <string.h>
#include <video_codes.h>

using namespace stl;

namespace {
    struct Arguments {
        char** at;
        char** end;

        u64 next() {
            if (at == end) {
                raiseError(StringView(u8"video_shader: too few numbers"));
            }

            u64 value = 0;

            for (const char* c = *at++; *c; c++) {
                value = value * 16 + (u64)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
            }

            return value;
        }

        void words(u32* out, int count) {
            for (int i = 0; i < count; i++) {
                out[i] = (u32)next();
            }
        }

        void numbers(double* out, int count) {
            for (int i = 0; i < count; i++) {
                u64 bits = next();

                memcpy(out + i, &bits, sizeof(bits));
            }
        }
    };

    const VideoLayout* layoutOf(const char* name) {
        for (const VideoFormat& format : videoFormats) {
            if (!strcmp(format.name, name)) {
                return &videoLayouts[format.layout];
            }
        }

        raiseError(StringView(u8"video_shader: unknown format"));
    }

    int pick(const char* flag, const char* given, const char* wanted, const char* const* names, int count) {
        if (!given || strcmp(given, flag)) {
            raiseError(StringView(StringBuilder() << StringView(u8"video_shader: ") << StringView(flag) << StringView(u8" comes first")));
        }

        for (int i = 0; i < count; i++) {
            if (wanted && !strcmp(wanted, names[i])) {
                return i;
            }
        }

        raiseError(StringView(StringBuilder() << StringView(u8"video_shader: unknown ") << StringView(flag)));
    }
}

int main(int argc, char** argv) {
    if (argc < 10) {
        sysE << StringView(u8"usage: video_shader --output srgb|pq|linear|wide --tiles inside|edge|mixed FORMAT SYSTEM TRANSFER CONVERSION OUTPUT HEX...") << endL;

        return 2;
    }

    static const char* const outputs[4] = {"srgb", "pq", "linear", "wide"};
    static const char* const tiles[3] = {"inside", "edge", "mixed"};
    ObjPool::Ref owner = ObjPool::fromMemory();
    VideoShader shader;
    ShaderOptions options{(ShaderOutput)pick("--output", argv[1], argv[2], outputs, 4), (ShaderTiles)pick("--tiles", argv[3], argv[4], tiles, 3)};
    Arguments numbers{argv + 10, argv + argc};

    memset(&shader, 0, sizeof(shader));
    shader.layout = layoutOf(argv[5]);
    shader.system = argv[6];
    shader.transfer = argv[7];
    shader.conversion = argv[8];
    shader.output = argv[9];
    numbers.words(shader.planeOffset, 4);
    numbers.words(shader.lineSize, 4);
    numbers.words(shader.size, 4);
    numbers.words(shader.target, 2);
    numbers.words(shader.origin, 2);
    numbers.words(&shader.tile, 1);
    numbers.numbers(shader.chroma, 4);
    numbers.numbers(&shader.decode[0][0], 9);
    numbers.numbers(shader.bias, 4);
    numbers.numbers(shader.sites, 2);
    numbers.numbers(shader.weights, 2);
    numbers.numbers(shader.curve, 11);
    numbers.numbers(shader.oetf, 11);
    numbers.numbers(shader.inverse, 11);
    numbers.numbers(&shader.toOutput[0][0], 9);
    numbers.numbers(shader.light, 3);
    numbers.numbers(shader.luminance, 3);

    if (numbers.at != numbers.end) {
        raiseError(StringView(u8"video_shader: too many numbers"));
    }

    u64 start = monotonicNowUs();

    for (int i = 0; i < 100; i++) {
        ObjPool::Ref scratch = ObjPool::fromMemory();

        compile(*scratch, shader, options);
    }

    sysE << StringView(u8"compile ") << (monotonicNowUs() - start) * 10 << StringView(u8" ns") << endL;
    sysO << compile(*owner, shader, options);

    return 0;
}
