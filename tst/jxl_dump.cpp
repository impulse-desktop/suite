// A JPEG XL file's pixels as the scenarios read them: the width and the
// height on a line, then the rows of 16-bit RGB samples, native endian,
// in the image's own colour encoding (a PQ frame stays PQ code values):
//   jxl_dump shot.jxl out.rgb16
#include <jxl/decode.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.jxl out.rgb16\n", argv[0]);
        return 2;
    }
    FILE* const in = fopen(argv[1], "rb");
    if (in == nullptr) {
        perror(argv[1]);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    const long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char* const data = static_cast<unsigned char*>(malloc((size_t)size));
    if (data == nullptr || fread(data, 1, (size_t)size, in) != (size_t)size) {
        fprintf(stderr, "%s: read failed\n", argv[1]);
        return 1;
    }
    fclose(in);
    JxlDecoder* const dec = JxlDecoderCreate(nullptr);
    JxlBasicInfo basic = {};
    unsigned char* pixels = nullptr;
    size_t pixelsSize = 0;
    JxlPixelFormat format = {3, JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    if (dec == nullptr || JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS || JxlDecoderSetInput(dec, data, (size_t)size) != JXL_DEC_SUCCESS) {
        fprintf(stderr, "decoder setup failed\n");
        return 1;
    }
    JxlDecoderCloseInput(dec);
    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec, &basic) != JXL_DEC_SUCCESS) {
                fprintf(stderr, "no basic info\n");
                return 1;
            }
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (JxlDecoderImageOutBufferSize(dec, &format, &pixelsSize) != JXL_DEC_SUCCESS || (pixels = static_cast<unsigned char*>(malloc(pixelsSize))) == nullptr || JxlDecoderSetImageOutBuffer(dec, &format, pixels, pixelsSize) != JXL_DEC_SUCCESS) {
                fprintf(stderr, "out buffer failed\n");
                return 1;
            }
        } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
            break;
        } else if (status == JXL_DEC_ERROR) {
            fprintf(stderr, "decode error\n");
            return 1;
        }
    }
    if (pixels == nullptr) {
        fprintf(stderr, "no image\n");
        return 1;
    }
    FILE* const out = fopen(argv[2], "wb");
    if (out == nullptr) {
        perror(argv[2]);
        return 1;
    }
    fprintf(out, "%u %u\n", basic.xsize, basic.ysize);
    fwrite(pixels, 1, (size_t)basic.xsize * basic.ysize * 3 * 2, out);
    fclose(out);
    JxlDecoderDestroy(dec);
    free(pixels);
    free(data);
    return 0;
}
