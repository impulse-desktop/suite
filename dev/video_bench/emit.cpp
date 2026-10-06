#include "codes.h"
#include "error.h"
#include "shader.h"
#include "renderer.h"

#include <std/ios/sys.h>
#include <std/sys/crt.h>
#include <std/str/view.h>
#include <std/str/builder.h>
#include <std/mem/obj_pool.h>

#include <string.h>

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

    void sizeOf(const char* text, u32 (&out)[2]) {
        int at = 0;

        out[0] = out[1] = 0;

        for (const char* c = text; *c; c++) {
            if (*c == 'x' && !at) {
                at = 1;
            } else if (*c >= '0' && *c <= '9') {
                out[at] = out[at] * 10 + (u32)(*c - '0');
            } else {
                raiseError(StringView(u8"video_shader: --size is WxH"));
            }
        }

        if (!at || !out[0] || !out[1]) {
            raiseError(StringView(u8"video_shader: --size is WxH"));
        }
    }
}

int main(int argc, char** argv) {
    bool generic = argc > 9 && !strcmp(argv[9], "--generic");
    int names = generic ? 10 : 9;

    if (argc < names + 5) {
        sysE << StringView(u8"usage: video_shader --target spirv|air --output srgb|pq|linear|wide --tiles inside|edge|mixed --size WxH [--generic] FORMAT SYSTEM TRANSFER CONVERSION OUTPUT HEX...") << endL;

        return 2;
    }

    static const char* const targets[2] = {"spirv", "air"};
    static const char* const outputs[4] = {"srgb", "pq", "linear", "wide"};
    static const char* const tiles[3] = {"inside", "edge", "mixed"};
    ObjPool::Ref owner = ObjPool::fromMemory();
    VideoShader shader;
    ShaderOptions options{(ShaderTarget)pick("--target", argv[1], argv[2], targets, 2), (ShaderOutput)pick("--output", argv[3], argv[4], outputs, 4), (ShaderTiles)pick("--tiles", argv[5], argv[6], tiles, 3), {0, 0}, generic};
    Arguments numbers{argv + names + 5, argv + argc};

    if (strcmp(argv[7], "--size")) {
        raiseError(StringView(u8"video_shader: --size comes fourth"));
    }

    sizeOf(argv[8], options.size);

    memset(&shader, 0, sizeof(shader));
    shader.layout = layoutOf(argv[names]);
    shader.system = argv[names + 1];
    shader.transfer = argv[names + 2];
    shader.conversion = argv[names + 3];
    shader.output = argv[names + 4];
    numbers.words(shader.planeOffset, 4);
    numbers.words(shader.lineSize, 4);
    numbers.words(shader.size, 4);
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

    CompiledShader compiled = compile(*owner, shader, options);

    sysO << (generic ? compiled.constants : compiled.code);

    return 0;
}
