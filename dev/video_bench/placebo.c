#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#include <libplacebo/utils/upload.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t spent;

static void account(void* priv, const struct pl_render_info* info) {
    (void)priv;
    spent += info->pass->last;
}

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9;
}

static char* joined(const char* directory, const char* name) {
    size_t length = strlen(directory) + strlen(name) + 2;
    char* path = malloc(length);
    snprintf(path, length, "%s/%s", directory, name);
    return path;
}

static void* readFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc(*size + 1);
    if (fread(data, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    ((char*)data)[*size] = 0;
    return data;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: placebo fast|default|high_quality ROUNDS CASE_DIRECTORY...\n");
        return 2;
    }
    const struct pl_render_params* preset = !strcmp(argv[1], "fast") ? &pl_render_fast_params : !strcmp(argv[1], "default") ? &pl_render_default_params : &pl_render_high_quality_params;
    int rounds = atoi(argv[2]);
    pl_log log = pl_log_create(PL_API_VER, &(struct pl_log_params) {.log_cb = pl_log_color, .log_level = PL_LOG_WARN});
    struct pl_vulkan_params vulkan = pl_vulkan_default_params;
    vulkan.get_proc_addr = vkGetInstanceProcAddr;
    pl_vulkan vk = pl_vulkan_create(log, &vulkan);
    if (!vk) { fprintf(stderr, "no vulkan\n"); return 1; }
    pl_gpu gpu = vk->gpu;
    pl_renderer renderer = pl_renderer_create(log, gpu);
    struct pl_render_params params = *preset;
    params.info_callback = account;

    for (int i = 3; i < argc; i++) {
        const char* directory = argv[i];
        size_t size;
        uint8_t* data = readFile(joined(directory, "data.bin"), &size);
        char* text = readFile(joined(directory, "placebo.txt"), &size);
        int width, height, tw, th, planes, rgb, full, subsampled;
        int consumed;
        if (sscanf(text, "%d %d %d %d %d %d %d %d%n", &width, &height, &tw, &th, &planes, &rgb, &full, &subsampled, &consumed) != 8) {
            fprintf(stderr, "%s: bad placebo.txt\n", directory);
            return 1;
        }
        char* at = text + consumed;
        struct pl_frame image = {0};
        pl_tex textures[4] = {0};
        image.num_planes = planes;
        for (int p = 0; p < planes; p++) {
            int pw, ph, offset, line, component, step;
            if (sscanf(at, "%d %d %d %d %d %d%n", &pw, &ph, &offset, &line, &component, &step, &consumed) != 6) {
                fprintf(stderr, "%s: bad plane %d\n", directory, p);
                return 1;
            }
            at += consumed;
            struct pl_plane_data plane = {
                .type = PL_FMT_UNORM,
                .width = pw,
                .height = ph,
                .component_size = {8},
                .component_map = {component},
                .pixel_stride = (size_t)step,
                .row_stride = (size_t)line,
                .pixels = data + offset,
            };
            if (!pl_upload_plane(gpu, &image.planes[p], &textures[p], &plane)) {
                fprintf(stderr, "%s: upload of plane %d failed\n", directory, p);
                return 1;
            }
        }
        image.repr = rgb ? pl_color_repr_rgb : pl_color_repr_hdtv;
        image.repr.levels = full ? PL_COLOR_LEVELS_FULL : PL_COLOR_LEVELS_LIMITED;
        image.color = rgb ? pl_color_space_srgb : pl_color_space_bt709;
        if (subsampled) {
            pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
        }

        pl_tex out = pl_tex_create(gpu, &(struct pl_tex_params) {
            .w = tw,
            .h = th,
            .format = pl_find_named_fmt(gpu, "rgba16f"),
            .renderable = true,
            .host_readable = true,
        });
        struct pl_frame target = {
            .num_planes = 1,
            .planes = {{.texture = out, .components = 4, .component_mapping = {0, 1, 2, 3}}},
            .repr = pl_color_repr_rgb,
            .color = pl_color_space_srgb,
        };

        double bestWall = 1e30;
        double bestGpu = 1e30;
        for (int r = 0; r < rounds + 3; r++) {
            spent = 0;
            double start = seconds();
            if (!pl_render_image(renderer, &image, &target, &params)) {
                fprintf(stderr, "%s: render failed\n", directory);
                return 1;
            }
            pl_gpu_finish(gpu);
            double wall = seconds() - start;
            if (r >= 3) {
                if (wall < bestWall) bestWall = wall;
                if (spent && spent < bestGpu) bestGpu = (double)spent;
            }
        }

        size_t bytes = (size_t)tw * th * out->params.format->texel_size;
        uint8_t* pixels = malloc(bytes);
        if (!pl_tex_download(gpu, &(struct pl_tex_transfer_params) {.tex = out, .ptr = pixels})) {
            fprintf(stderr, "%s: download failed\n", directory);
            return 1;
        }
        char name[64];
        snprintf(name, sizeof(name), "placebo_%s.raw", argv[1]);
        FILE* dump = fopen(joined(directory, name), "wb");
        fwrite(pixels, 1, bytes, dump);
        fclose(dump);
        printf("placebo %s %s gpu %.1f wall %.1f format %s\n", directory, argv[1], bestGpu / 1e3, bestWall * 1e6, out->params.format->name);
        fflush(stdout);
        free(pixels);
        pl_tex_destroy(gpu, &out);
        for (int p = 0; p < planes; p++) {
            pl_tex_destroy(gpu, &textures[p]);
        }
        free(data);
        free(text);
    }

    pl_renderer_destroy(&renderer);
    pl_vulkan_destroy(&vk);
    pl_log_destroy(&log);
    return 0;
}
