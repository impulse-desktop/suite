#include "shader.h"
#include "error.h"

#include <std/ios/sys.h>
#include <std/str/view.h>
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
}

int main(int argc, char** argv) {
    if (argc < 6) {
        sysE << StringView(u8"usage: video_shader FORMAT SYSTEM TRANSFER CONVERSION OUTPUT HEX...") << endL;

        return 2;
    }

    ObjPool::Ref owner = ObjPool::fromMemory();
    VideoShader shader;
    Arguments numbers{argv + 6, argv + argc};

    memset(&shader, 0, sizeof(shader));
    shader.layout = layoutOf(argv[1]);
    shader.system = argv[2];
    shader.transfer = argv[3];
    shader.conversion = argv[4];
    shader.output = argv[5];
    numbers.words(shader.planeOffset, 4);
    numbers.words(shader.lineSize, 4);
    numbers.words(shader.size, 4);
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

    sysO << compile(*owner, shader);

    return 0;
}
