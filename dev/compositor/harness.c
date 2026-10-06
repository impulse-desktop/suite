#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

enum { MAX_TEXTURES = 16, TILE = 24, GROUP = 8, TEXTURES = 1024 };
enum { OP_RECT, OP_TEXTURE, OP_TRIANGLE, OP_LAYER };
enum { DECODE_LINEAR = 0, DECODE_SRGB = 1, DECODE_PQ = 2, SOURCE_WIDE = 4, SOLID = 8 };
#define NONE 0xffffffffu
#define FILL 0x80000000u

typedef struct {
    float pos[2];
    float uv[2];
    uint32_t col;
} Vertex;

typedef struct {
    float clip[4];
    uint32_t texture;
    uint32_t vtxOffset;
    uint32_t idxOffset;
    uint32_t elemCount;
    uint32_t vertexBase;
    uint32_t indexBase;
} Command;

typedef struct {
    uint32_t id, width, height;
    uint8_t* pixels;
    int opaque;
} Texture;

typedef struct {
    uint32_t width, height;
    Texture textures[MAX_TEXTURES];
    uint32_t textureCount;
    Vertex* vertices;
    uint32_t vertexCount;
    uint32_t* indices;
    uint32_t indexCount;
    Command* commands;
    uint32_t commandCount;
    int hasVideo;
    float video[4];
} Frame;

typedef struct {
    uint32_t base, start, count, pad;
    float color[4];
} Header;

typedef struct {
    uint32_t kind, index, flags, pad;
    int32_t rect[4];
    float uv[4];
    float map[4];
    float color[4];
} Op;

typedef struct {
    int32_t edges[3][4];
    float colors[3][4];
    float uv[3][2];
    uint32_t texture;
    uint32_t flags;
    int32_t clip[4];
    int64_t offset[3];
    int64_t area;
} Triangle;

_Static_assert(sizeof(Header) == 32, "header");
_Static_assert(sizeof(Op) == 80, "op");
_Static_assert(sizeof(Triangle) == 176, "triangle");

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* map;
} Buffer;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageView srgb;
} Image;

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t family;
static VkCommandPool commandPool;
static VkQueryPool queries;
static float timestampPeriod;
static float srgbLinear[256];

static void* readFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc(*size + 1);
    if (fread(data, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return data;
}

static uint32_t take(const uint8_t** at) {
    uint32_t v;
    memcpy(&v, *at, 4);
    *at += 4;
    return v;
}

static float takeFloat(const uint8_t** at) {
    float v;
    memcpy(&v, *at, 4);
    *at += 4;
    return v;
}

static Frame loadFrame(const char* path) {
    size_t size;
    const uint8_t* data = readFile(path, &size);
    const uint8_t* at = data;
    Frame f = {0};
    if (take(&at) != 0x46434d49u) { fprintf(stderr, "%s: not a frame\n", path); exit(1); }
    f.width = take(&at);
    f.height = take(&at);
    f.textureCount = take(&at);
    if (f.textureCount > MAX_TEXTURES) { fprintf(stderr, "too many textures\n"); exit(1); }
    for (uint32_t i = 0; i < f.textureCount; i++) {
        Texture* t = &f.textures[i];
        t->id = take(&at);
        t->width = take(&at);
        t->height = take(&at);
        t->pixels = (uint8_t*)at;
        t->opaque = 1;
        for (size_t k = 3; k < (size_t)t->width * t->height * 4 && t->opaque; k += 4) t->opaque = t->pixels[k] == 255;
        at += (size_t)t->width * t->height * 4;
    }
    uint32_t lists = take(&at);
    size_t vertexCap = 1 << 16, indexCap = 1 << 16, commandCap = 1 << 10;
    f.vertices = malloc(vertexCap * sizeof(Vertex));
    f.indices = malloc(indexCap * sizeof(uint32_t));
    f.commands = malloc(commandCap * sizeof(Command));
    for (uint32_t l = 0; l < lists; l++) {
        uint32_t vertexBase = f.vertexCount, indexBase = f.indexCount;
        uint32_t nv = take(&at);
        while (f.vertexCount + nv > vertexCap) f.vertices = realloc(f.vertices, (vertexCap *= 2) * sizeof(Vertex));
        for (uint32_t i = 0; i < nv; i++) {
            Vertex* v = &f.vertices[f.vertexCount++];
            v->pos[0] = takeFloat(&at);
            v->pos[1] = takeFloat(&at);
            v->uv[0] = takeFloat(&at);
            v->uv[1] = takeFloat(&at);
            v->col = take(&at);
        }
        uint32_t ni = take(&at);
        while (f.indexCount + ni > indexCap) f.indices = realloc(f.indices, (indexCap *= 2) * sizeof(uint32_t));
        for (uint32_t i = 0; i < ni; i++) f.indices[f.indexCount++] = take(&at);
        uint32_t nc = take(&at);
        while (f.commandCount + nc > commandCap) f.commands = realloc(f.commands, (commandCap *= 2) * sizeof(Command));
        for (uint32_t i = 0; i < nc; i++) {
            Command* c = &f.commands[f.commandCount++];
            for (int k = 0; k < 4; k++) c->clip[k] = takeFloat(&at);
            c->texture = take(&at);
            c->vtxOffset = take(&at);
            c->idxOffset = take(&at);
            c->elemCount = take(&at);
            c->vertexBase = vertexBase;
            c->indexBase = indexBase;
        }
    }
    f.hasVideo = (int)take(&at);
    for (int k = 0; k < 4 && f.hasVideo; k++) f.video[k] = takeFloat(&at);
    return f;
}

static uint32_t textureSlot(const Frame* f, uint32_t id) {
    for (uint32_t i = 0; i < f->textureCount; i++)
        if (f->textures[i].id == id) return i;
    fprintf(stderr, "unknown texture %u\n", id);
    exit(1);
}

static int scissorOf(const Frame* f, const Command* c, int32_t out[4]) {
    float x0 = c->clip[0] < 0 ? 0 : c->clip[0];
    float y0 = c->clip[1] < 0 ? 0 : c->clip[1];
    float x1 = c->clip[2] > (float)f->width ? (float)f->width : c->clip[2];
    float y1 = c->clip[3] > (float)f->height ? (float)f->height : c->clip[3];
    if (x1 <= x0 || y1 <= y0) return 0;
    out[0] = (int32_t)x0;
    out[1] = (int32_t)y0;
    out[2] = out[0] + (int32_t)(uint32_t)(x1 - x0);
    out[3] = out[1] + (int32_t)(uint32_t)(y1 - y0);
    return 1;
}

static uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 16;
    info.usage = usage;
    CHECK(vkCreateBuffer(device, &info, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(device, &alloc, NULL, &b.memory));
    CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
    return b;
}

static VkImageView makeView(VkImage image, VkFormat format) {
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView out;
    CHECK(vkCreateImageView(device, &view, NULL, &out));
    return out;
}

static Image makeImage(uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Image img;
    VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    list.viewFormatCount = 2;
    list.pViewFormats = formats;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &list;
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = (VkExtent3D){w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    CHECK(vkCreateImage(device, &info, NULL, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(device, &alloc, NULL, &img.memory));
    CHECK(vkBindImageMemory(device, img.image, img.memory, 0));
    img.view = makeView(img.image, VK_FORMAT_R8G8B8A8_UNORM);
    img.srgb = makeView(img.image, VK_FORMAT_R8G8B8A8_SRGB);
    return img;
}

static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &info, &cmd));
    VkCommandBufferBeginInfo b = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cmd, &b));
    return cmd;
}

static void finish(VkCommandBuffer cmd) {
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule module(const char* path) {
    size_t size;
    uint32_t* code = readFile(path, &size);
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m;
    CHECK(vkCreateShaderModule(device, &info, NULL, &m));
    free(code);
    return m;
}

static void setup(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &gpu);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    timestampPeriod = props.limits.timestampPeriod;
    fprintf(stderr, "device %s\n", props.deviceName);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, NULL);
    VkQueueFamilyProperties fp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, fp);
    for (family = 0; family < families; family++)
        if (fp[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceShaderClockFeaturesKHR clock = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    clock.shaderSubgroupClock = VK_TRUE;
    VkPhysicalDeviceVulkan12Features twelve = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    twelve.pNext = &clock;
    twelve.descriptorBindingPartiallyBound = VK_TRUE;
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &twelve;
    const char* extensions[1] = {"VK_KHR_shader_clock"};
    features.features.shaderInt64 = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &features;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(gpu, &di, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    CHECK(vkCreateCommandPool(device, &pi, NULL, &commandPool));
    VkQueryPoolCreateInfo qp = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qp.queryCount = 2;
    CHECK(vkCreateQueryPool(device, &qp, NULL, &queries));
}

static double elapsed(void) {
    uint64_t t[2];
    CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    return (double)(t[1] - t[0]) * timestampPeriod / 1000.0;
}

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9;
}

typedef struct {
    Header* headers;
    uint32_t* list;
    uint32_t listCount, listCapacity;
    Op* ops;
    uint32_t opCount;
    Triangle* triangles;
    uint32_t triangleCount;
    uint32_t* tiles;
    uint32_t programs[4][2];
    uint32_t width, height, tilesX, tilesY, tileCount, capacity;
    uint32_t* base;
    float* color;
    uint32_t* head;
    uint32_t* tail;
    uint32_t* count;
    uint8_t* kind;
    uint32_t* nodes;
    uint32_t nodeCount, nodeCapacity;
    float scale[2];
    float half[2];
    uint32_t whiteTexture;
    float whiteUv[2];
    int white;
} Program;

static int64_t nearest(float x) {
    volatile float magic = 12582912.0f;
    return (int64_t)((x + magic) - magic);
}

static int64_t snap(const Program* p, float pos, int axis) {
    float ndc = pos * p->scale[axis] - 1.0f;
    return nearest((ndc * p->half[axis] + p->half[axis]) * 256.0f);
}

static int32_t firstPixel(int64_t fixed) {
    int64_t v = fixed - 128;
    return (int32_t)(v >= 0 ? (v + 255) / 256 : -((-v) / 256));
}

static void edge(const int64_t a[2], const int64_t b[2], int32_t out[4], int64_t* offset) {
    int64_t dx = b[0] - a[0], dy = b[1] - a[1];
    int topLeft = (dy == 0 && dx > 0) || dy < 0;
    out[0] = (int32_t)(-dy);
    out[1] = (int32_t)dx;
    out[2] = topLeft ? 0 : 1;
    out[3] = 0;
    *offset = dy * a[0] - dx * a[1] - (topLeft ? 0 : 1);
}

static void linearColor(uint32_t col, float out[4]) {
    out[0] = srgbLinear[col & 255u];
    out[1] = srgbLinear[(col >> 8) & 255u];
    out[2] = srgbLinear[(col >> 16) & 255u];
    out[3] = (float)(col >> 24) / 255.0f;
}

static int whiteTexel(const Texture* tex, float u, float v) {
    float fx = u * (float)tex->width - 0.5f, fy = v * (float)tex->height - 0.5f;
    int bx = (int)floorf(fx), by = (int)floorf(fy);
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            int sx = bx + dx < 0 ? 0 : bx + dx >= (int)tex->width ? (int)tex->width - 1 : bx + dx;
            int sy = by + dy < 0 ? 0 : by + dy >= (int)tex->height ? (int)tex->height - 1 : by + dy;
            const uint8_t* texel = tex->pixels + ((size_t)sy * tex->width + sx) * 4;
            if (texel[0] != 255 || texel[1] != 255 || texel[2] != 255 || texel[3] != 255) return 0;
        }
    return 1;
}

static int whiteAt(Program* p, const Frame* f, uint32_t slot, float u, float v) {
    if (p->whiteTexture != slot || p->whiteUv[0] != u || p->whiteUv[1] != v) {
        p->whiteTexture = slot;
        p->whiteUv[0] = u;
        p->whiteUv[1] = v;
        p->white = whiteTexel(&f->textures[slot], u, v);
    }
    return p->white;
}

static void append(Program* p, uint32_t tile, uint32_t entry) {
    if (p->nodeCount == p->nodeCapacity) {
        p->nodeCapacity = p->nodeCapacity ? p->nodeCapacity * 2 : 1 << 16;
        p->nodes = realloc(p->nodes, (size_t)p->nodeCapacity * 2 * sizeof(uint32_t));
    }
    uint32_t node = p->nodeCount++;
    p->nodes[node * 2] = entry;
    p->nodes[node * 2 + 1] = NONE;
    if (p->tail[tile] == NONE) p->head[tile] = node;
    else p->nodes[p->tail[tile] * 2 + 1] = node;
    p->tail[tile] = node;
    p->count[tile]++;
}

static void cover(Program* p, uint32_t tile, const Op* op, uint32_t index, int opaque) {
    float* c = &p->color[tile * 4];
    if (opaque) {
        p->head[tile] = p->tail[tile] = NONE;
        p->count[tile] = 0;
        if (op->kind == OP_RECT) {
            p->base[tile] = NONE;
            c[0] = op->color[0];
            c[1] = op->color[1];
            c[2] = op->color[2];
            c[3] = 1.0f;
        } else {
            p->base[tile] = index;
            c[0] = c[1] = c[2] = c[3] = 0.0f;
        }
        return;
    }
    if (p->count[tile]) {
        append(p, tile, index | FILL);
        return;
    }
    float a = op->color[3];
    for (int k = 0; k < 3; k++) c[k] = op->color[k] * a + c[k] * (1.0f - a);
    c[3] = a + c[3] * (1.0f - a);
}

static void place(Program* p, const Op* op, uint32_t index, const int32_t box[4], int fills, int opaque) {
    if (box[2] <= box[0] || box[3] <= box[1]) return;
    int32_t tx0 = box[0] / TILE, ty0 = box[1] / TILE, tx1 = (box[2] - 1) / TILE, ty1 = (box[3] - 1) / TILE;
    int32_t lastX = (int32_t)p->tilesX - 1, lastY = (int32_t)p->tilesY - 1;
    tx1 = tx1 > lastX ? lastX : tx1;
    ty1 = ty1 > lastY ? lastY : ty1;
    int32_t fx0 = (box[0] + TILE - 1) / TILE, fy0 = (box[1] + TILE - 1) / TILE;
    int32_t fx1 = box[2] >= (int32_t)p->width ? lastX : box[2] / TILE - 1;
    int32_t fy1 = box[3] >= (int32_t)p->height ? lastY : box[3] / TILE - 1;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        int inside = fills && ty >= fy0 && ty <= fy1;
        uint32_t row = (uint32_t)ty * p->tilesX;
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            uint32_t tile = row + (uint32_t)tx;
            if (inside && tx >= fx0 && tx <= fx1) cover(p, tile, op, index, opaque);
            else append(p, tile, index);
        }
    }
}

static void buildProgram(const Frame* f, Program* p) {
    uint32_t tilesX = (f->width + TILE - 1) / TILE, tilesY = (f->height + TILE - 1) / TILE;
    uint32_t tileCount = tilesX * tilesY;
    uint32_t total = 2;
    for (uint32_t c = 0; c < f->commandCount; c++) total += f->commands[c].elemCount / 3;
    if (p->tileCount != tileCount || p->capacity < total) {
        p->capacity = total;
        p->tileCount = tileCount;
        p->ops = realloc(p->ops, total * sizeof(Op));
        p->triangles = realloc(p->triangles, total * sizeof(Triangle));
        p->headers = realloc(p->headers, tileCount * sizeof(Header));
        p->tiles = realloc(p->tiles, tileCount * sizeof(uint32_t));
        p->base = realloc(p->base, tileCount * sizeof(uint32_t));
        p->color = realloc(p->color, tileCount * 4 * sizeof(float));
        p->head = realloc(p->head, tileCount * sizeof(uint32_t));
        p->tail = realloc(p->tail, tileCount * sizeof(uint32_t));
        p->count = realloc(p->count, tileCount * sizeof(uint32_t));
        p->kind = realloc(p->kind, tileCount);
    }
    p->width = f->width;
    p->height = f->height;
    p->tilesX = tilesX;
    p->tilesY = tilesY;
    p->nodeCount = 0;
    p->whiteTexture = NONE;
    p->opCount = 0;
    p->triangleCount = 0;
    p->scale[0] = 2.0f / (float)f->width;
    p->scale[1] = 2.0f / (float)f->height;
    p->half[0] = (float)f->width * 0.5f;
    p->half[1] = (float)f->height * 0.5f;
    memset(p->head, 0xff, tileCount * sizeof(uint32_t));
    memset(p->tail, 0xff, tileCount * sizeof(uint32_t));
    memset(p->base, 0xff, tileCount * sizeof(uint32_t));
    memset(p->count, 0, tileCount * sizeof(uint32_t));
    memset(p->kind, 0, tileCount);
    float clear = srgbLinear[25];
    for (uint32_t i = 0; i < tileCount; i++) {
        p->color[i * 4] = p->color[i * 4 + 1] = p->color[i * 4 + 2] = clear;
        p->color[i * 4 + 3] = 1.0f;
    }
    if (f->hasVideo) {
        Op* op = &p->ops[p->opCount];
        memset(op, 0, sizeof(*op));
        op->kind = OP_LAYER;
        for (int k = 0; k < 4; k++) {
            op->rect[k] = (int32_t)f->video[k];
            op->color[k] = 1.0f;
        }
        place(p, op, p->opCount, op->rect, 0, 0);
        p->opCount++;
    }
    for (uint32_t c = 0; c < f->commandCount; c++) {
        const Command* cmd = &f->commands[c];
        int32_t clip[4];
        if (!scissorOf(f, cmd, clip)) continue;
        uint32_t slot = textureSlot(f, cmd->texture);
        const Texture* tex = &f->textures[slot];
        const uint32_t* ids = &f->indices[cmd->indexBase + cmd->idxOffset];
        const Vertex* base = &f->vertices[cmd->vertexBase + cmd->vtxOffset];
        for (uint32_t e = 0; e < cmd->elemCount; e += 3) {
            const uint32_t* id = ids + e;
            Op* op = &p->ops[p->opCount];
            if (e + 3 < cmd->elemCount && id[3] == id[0] && id[4] == id[2]) {
                const Vertex* v[4] = {&base[id[0]], &base[id[1]], &base[id[2]], &base[id[5]]};
                int rect = v[0]->pos[1] == v[1]->pos[1] && v[1]->pos[0] == v[2]->pos[0] && v[2]->pos[1] == v[3]->pos[1] && v[3]->pos[0] == v[0]->pos[0];
                rect = rect && v[0]->uv[1] == v[1]->uv[1] && v[1]->uv[0] == v[2]->uv[0] && v[2]->uv[1] == v[3]->uv[1] && v[3]->uv[0] == v[0]->uv[0];
                rect = rect && v[0]->col == v[1]->col && v[0]->col == v[2]->col && v[0]->col == v[3]->col;
                if (rect) {
                    int64_t x0 = snap(p, v[0]->pos[0], 0), x1 = snap(p, v[2]->pos[0], 0), y0 = snap(p, v[0]->pos[1], 1), y1 = snap(p, v[2]->pos[1], 1);
                    float u0 = v[0]->uv[0], u1 = v[2]->uv[0], w0 = v[0]->uv[1], w1 = v[2]->uv[1];
                    if (x0 > x1) { int64_t t = x0; x0 = x1; x1 = t; float q = u0; u0 = u1; u1 = q; }
                    if (y0 > y1) { int64_t t = y0; y0 = y1; y1 = t; float q = w0; w0 = w1; w1 = q; }
                    int32_t box[4] = {firstPixel(x0), firstPixel(y0), firstPixel(x1), firstPixel(y1)};
                    box[0] = box[0] < clip[0] ? clip[0] : box[0];
                    box[1] = box[1] < clip[1] ? clip[1] : box[1];
                    box[2] = box[2] > clip[2] ? clip[2] : box[2];
                    box[3] = box[3] > clip[3] ? clip[3] : box[3];
                    e += 3;
                    if (box[2] <= box[0] || box[3] <= box[1]) continue;
                    op->kind = u0 == u1 && w0 == w1 && whiteAt(p, f, slot, u0, w0) ? OP_RECT : OP_TEXTURE;
                    op->index = slot;
                    op->flags = DECODE_LINEAR;
                    op->pad = 0;
                    linearColor(v[0]->col, op->color);
                    memcpy(op->rect, box, sizeof(box));
                    op->uv[0] = u0;
                    op->uv[1] = w0;
                    op->uv[2] = u1;
                    op->uv[3] = w1;
                    op->map[0] = (float)x0 * (1.0f / 256.0f);
                    op->map[1] = (float)y0 * (1.0f / 256.0f);
                    op->map[2] = x1 > x0 ? 256.0f / (float)(x1 - x0) : 0.0f;
                    op->map[3] = y1 > y0 ? 256.0f / (float)(y1 - y0) : 0.0f;
                    int opaque = (v[0]->col >> 24) == 255u && (op->kind == OP_RECT || tex->opaque);
                    place(p, op, p->opCount, box, op->kind == OP_RECT || opaque, opaque);
                    p->opCount++;
                    continue;
                }
            }
            const Vertex* v[3] = {&base[id[0]], &base[id[1]], &base[id[2]]};
            int64_t q0[3][2];
            for (int k = 0; k < 3; k++)
                for (int d = 0; d < 2; d++) q0[k][d] = snap(p, v[k]->pos[d], d);
            int64_t area = (q0[1][0] - q0[0][0]) * (q0[2][1] - q0[0][1]) - (q0[1][1] - q0[0][1]) * (q0[2][0] - q0[0][0]);
            if (area == 0) continue;
            int order[3] = {0, 1, 2};
            if (area < 0) {
                order[1] = 2;
                order[2] = 1;
                area = -area;
            }
            Triangle* t = &p->triangles[p->triangleCount];
            int64_t q[3][2];
            for (int k = 0; k < 3; k++) {
                q[k][0] = q0[order[k]][0];
                q[k][1] = q0[order[k]][1];
                t->uv[k][0] = v[order[k]]->uv[0];
                t->uv[k][1] = v[order[k]]->uv[1];
                linearColor(v[order[k]]->col, t->colors[k]);
            }
            edge(q[1], q[2], t->edges[0], &t->offset[0]);
            edge(q[2], q[0], t->edges[1], &t->offset[1]);
            edge(q[0], q[1], t->edges[2], &t->offset[2]);
            t->area = area;
            t->texture = slot;
            int solid = t->uv[0][0] == t->uv[1][0] && t->uv[0][0] == t->uv[2][0] && t->uv[0][1] == t->uv[1][1] && t->uv[0][1] == t->uv[2][1] && whiteAt(p, f, slot, t->uv[0][0], t->uv[0][1]);
            t->flags = DECODE_LINEAR | (solid ? SOLID : 0);
            memcpy(t->clip, clip, sizeof(clip));
            int64_t lo[2], hi[2];
            for (int d = 0; d < 2; d++) {
                lo[d] = hi[d] = q[0][d];
                for (int k = 1; k < 3; k++) {
                    lo[d] = q[k][d] < lo[d] ? q[k][d] : lo[d];
                    hi[d] = q[k][d] > hi[d] ? q[k][d] : hi[d];
                }
            }
            int32_t box[4] = {firstPixel(lo[0]), firstPixel(lo[1]), firstPixel(hi[0] + 1), firstPixel(hi[1] + 1)};
            box[0] = box[0] < clip[0] ? clip[0] : box[0];
            box[1] = box[1] < clip[1] ? clip[1] : box[1];
            box[2] = box[2] > clip[2] ? clip[2] : box[2];
            box[3] = box[3] > clip[3] ? clip[3] : box[3];
            if (box[2] <= box[0] || box[3] <= box[1]) continue;
            memset(op, 0, sizeof(*op));
            op->kind = OP_TRIANGLE;
            op->index = p->triangleCount;
            memcpy(op->rect, box, sizeof(box));
            place(p, op, p->opCount, box, 0, 0);
            p->opCount++;
            p->triangleCount++;
        }
    }
    uint32_t listCount = 0;
    for (uint32_t i = 0; i < tileCount; i++) listCount += p->count[i];
    if (listCount > p->listCapacity || !p->list) {
        p->listCapacity = listCount ? listCount : 1;
        p->list = realloc(p->list, p->listCapacity * sizeof(uint32_t));
    }
    p->listCount = listCount;
    uint32_t at = 0, sizes[4] = {0};
    for (uint32_t i = 0; i < tileCount; i++) {
        Header* h = &p->headers[i];
        const Op* video = NULL;
        int alone = p->base[i] == NONE;
        h->base = p->base[i];
        h->start = at;
        h->count = p->count[i];
        h->pad = 0;
        memcpy(h->color, &p->color[i * 4], sizeof(h->color));
        for (uint32_t node = p->head[i]; node != NONE; node = p->nodes[node * 2 + 1]) {
            uint32_t entry = p->nodes[node * 2];
            int layer = !(entry & FILL) && p->ops[entry & ~FILL].kind == OP_LAYER;
            video = layer ? &p->ops[entry & ~FILL] : video;
            alone = alone && layer;
            p->list[at++] = entry;
        }
        p->kind[i] = 0;
        if (video) {
            int32_t x0 = (int32_t)(i % p->tilesX * TILE), y0 = (int32_t)(i / p->tilesX * TILE);
            int32_t x1 = x0 + TILE < (int32_t)p->width ? x0 + TILE : (int32_t)p->width;
            int32_t y1 = y0 + TILE < (int32_t)p->height ? y0 + TILE : (int32_t)p->height;
            int inside = video->rect[0] <= x0 && video->rect[1] <= y0 && video->rect[2] >= x1 && video->rect[3] >= y1;
            p->kind[i] = (uint8_t)(!alone ? 3 : inside ? 1 : 2);
        }
        sizes[p->kind[i]]++;
    }
    for (uint32_t k = 0, first = 0; k < 4; k++) {
        p->programs[k][0] = first;
        p->programs[k][1] = 0;
        first += sizes[k];
    }
    for (uint32_t i = 0; i < tileCount; i++) {
        uint32_t k = p->kind[i];
        p->tiles[p->programs[k][0] + p->programs[k][1]++] = i;
    }
}

static void writePpm(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

static void readback(Image* img, uint32_t w, uint32_t h, VkImageLayout layout, uint8_t* out) {
    Buffer b = makeBuffer((VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cmd = begin();
    barrier(cmd, img->image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyImageToBuffer(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &copy);
    finish(cmd);
    memcpy(out, b.map, (size_t)w * h * 4);
    vkDestroyBuffer(device, b.buffer, NULL);
    vkFreeMemory(device, b.memory, NULL);
}

typedef struct {
    int32_t size[2];
    int32_t video[2];
    uint32_t tilesX;
    uint32_t first;
    float white;
    uint32_t pad;
    int32_t box[2];
} Push;

static VkPipeline composePipeline(VkPipelineLayout layout, const char* path, uint32_t edge) {
    VkSpecializationMapEntry entries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    uint32_t data[3] = {0, VK_FALSE, edge};
    VkSpecializationInfo spec = {3, entries, sizeof(data), data};
    VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cp.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module(path), "main", &spec};
    cp.layout = layout;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, NULL, &pipeline));
    return pipeline;
}

int main(int argc, char** argv) {
    if (argc != 5 && argc != 7) {
        fprintf(stderr, "usage: harness PLAIN.spv ROUNDS FRAME.bin OUTDIR [LAYERS VIDEO.bin]\n");
        return 2;
    }
    const char* plain = argv[1];
    int rounds = atoi(argv[2]);
    Frame f = loadFrame(argv[3]);
    const char* outdir = argv[4];
    const char* layers = argc == 7 ? argv[5] : NULL;
    const char* frameWords = argc == 7 ? argv[6] : NULL;
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        srgbLinear[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    if (f.hasVideo != (argc == 7)) {
        fprintf(stderr, "a frame with video needs its programs, and only it\n");
        return 2;
    }
    setup();

    VkSampler sampler;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = -1000;
    si.maxLod = 1000;
    si.maxAnisotropy = 1.0f;
    CHECK(vkCreateSampler(device, &si, NULL, &sampler));

    Image textures[MAX_TEXTURES];
    for (uint32_t i = 0; i < f.textureCount; i++) {
        const Texture* t = &f.textures[i];
        textures[i] = makeImage(t->width, t->height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        Buffer staging = makeBuffer((VkDeviceSize)t->width * t->height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        memcpy(staging.map, t->pixels, (size_t)t->width * t->height * 4);
        VkCommandBuffer cmd = begin();
        barrier(cmd, textures[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0};
        copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = (VkExtent3D){t->width, t->height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, textures[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cmd, textures[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        finish(cmd);
    }

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURES}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 64;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &dp, NULL, &pool));

    Buffer video = {0};
    Buffer facts = {0};
    int generic = 0;
    if (f.hasVideo) {
        size_t size;
        void* words = readFile(frameWords, &size);
        video = makeBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        memcpy(video.map, words, size);
        free(words);
        char path[1024];
        snprintf(path, sizeof(path), "%s/facts.bin", layers);
        FILE* probe = fopen(path, "rb");
        if (probe) {
            fclose(probe);
            void* bytes = readFile(path, &size);
            facts = makeBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            memcpy(facts.map, bytes, size);
            free(bytes);
            generic = 1;
        }
    }

    Program program;
    memset(&program, 0, sizeof(program));
    double building = 1e30;
    for (int r = 0; r < rounds; r++) {
        double t0 = seconds();
        buildProgram(&f, &program);
        double us = (seconds() - t0) * 1e6;
        building = us < building ? us : building;
    }
    printf("program cpu %.1f us, %u ops (%u triangles), %u list entries, %u tiles: %u plain, %u inside, %u edge, %u mixed\n", building, program.opCount, program.triangleCount, program.listCount, program.tileCount, program.programs[0][1], program.programs[1][1], program.programs[2][1], program.programs[3][1]);

    Buffer headerBuffer = makeBuffer((VkDeviceSize)program.tileCount * sizeof(Header), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(headerBuffer.map, program.headers, (size_t)program.tileCount * sizeof(Header));
    Buffer listBuffer = makeBuffer((VkDeviceSize)(program.listCount ? program.listCount : 1) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(listBuffer.map, program.list, (size_t)program.listCount * 4);
    Buffer opBuffer = makeBuffer((VkDeviceSize)(program.opCount ? program.opCount : 1) * sizeof(Op), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(opBuffer.map, program.ops, (size_t)program.opCount * sizeof(Op));
    Buffer triBuffer = makeBuffer((VkDeviceSize)(program.triangleCount ? program.triangleCount : 1) * sizeof(Triangle), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(triBuffer.map, program.triangles, (size_t)program.triangleCount * sizeof(Triangle));
    Buffer tileBuffer = makeBuffer((VkDeviceSize)program.tileCount * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(tileBuffer.map, program.tiles, (size_t)program.tileCount * 4);
    Buffer profile = makeBuffer(64 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(profile.map, 0, 64 * 4);

    Image composed = makeImage(f.width, f.height, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkDescriptorSetLayout setLayouts[2];
    VkPipelineLayout composeLayout;
    VkPipeline pipelines[4] = {VK_NULL_HANDLE};
    VkDescriptorSet sets[2];
    {
        VkDescriptorSetLayoutBinding bindings[7] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURES, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        };
        VkDescriptorBindingFlags flags[7] = {0, 0, 0, 0, 0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 7;
        bf.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo cl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        cl.pNext = &bf;
        cl.bindingCount = 7;
        cl.pBindings = bindings;
        CHECK(vkCreateDescriptorSetLayout(device, &cl, NULL, &setLayouts[0]));
        VkDescriptorSetLayoutBinding videoBindings[3] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        };
        VkDescriptorSetLayoutCreateInfo wl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        wl.bindingCount = 3;
        wl.pBindings = videoBindings;
        CHECK(vkCreateDescriptorSetLayout(device, &wl, NULL, &setLayouts[1]));
        VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo cpl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cpl.setLayoutCount = 2;
        cpl.pSetLayouts = setLayouts;
        cpl.pushConstantRangeCount = 1;
        cpl.pPushConstantRanges = &range;
        CHECK(vkCreatePipelineLayout(device, &cpl, NULL, &composeLayout));
        pipelines[0] = composePipeline(composeLayout, plain, 0);
        if (f.hasVideo && generic) {
            char kernel[1024], layer[1024];
            snprintf(kernel, sizeof(kernel), "%s/generic.spv", layers);
            snprintf(layer, sizeof(layer), "%s/generic_layer.spv", layers);
            pipelines[1] = composePipeline(composeLayout, kernel, 0);
            pipelines[2] = composePipeline(composeLayout, kernel, 1);
            pipelines[3] = composePipeline(composeLayout, layer, 0);
        }
        const char* kinds[4] = {"", "inside", "edge", "mixed"};
        for (int k = 1; f.hasVideo && !generic && k < 4; k++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s.spv", layers, kinds[k]);
            pipelines[k] = composePipeline(composeLayout, path, 0);
        }
        for (int s = 0; s < 2; s++) {
            VkDescriptorSetAllocateInfo ca = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ca.descriptorPool = pool;
            ca.descriptorSetCount = 1;
            ca.pSetLayouts = &setLayouts[s];
            CHECK(vkAllocateDescriptorSets(device, &ca, &sets[s]));
        }
        VkDescriptorBufferInfo bufs[5] = {{headerBuffer.buffer, 0, VK_WHOLE_SIZE}, {listBuffer.buffer, 0, VK_WHOLE_SIZE}, {opBuffer.buffer, 0, VK_WHOLE_SIZE}, {triBuffer.buffer, 0, VK_WHOLE_SIZE}, {tileBuffer.buffer, 0, VK_WHOLE_SIZE}};
        VkDescriptorImageInfo storage = {VK_NULL_HANDLE, composed.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo sampled[MAX_TEXTURES];
        for (uint32_t i = 0; i < f.textureCount; i++) sampled[i] = (VkDescriptorImageInfo){sampler, textures[i].srgb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[10];
        for (int i = 0; i < 7; i++) {
            writes[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = sets[0];
            writes[i].dstBinding = (uint32_t)i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = bindings[i].descriptorType;
            if (i < 5) writes[i].pBufferInfo = &bufs[i];
        }
        writes[5].pImageInfo = &storage;
        writes[6].descriptorCount = f.textureCount;
        writes[6].pImageInfo = sampled;
        VkDescriptorBufferInfo words = {f.hasVideo ? video.buffer : headerBuffer.buffer, 0, VK_WHOLE_SIZE};
        writes[7] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[7].dstSet = sets[1];
        writes[7].dstBinding = 0;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[7].pBufferInfo = &words;
        VkDescriptorBufferInfo constants = {generic ? facts.buffer : headerBuffer.buffer, 0, VK_WHOLE_SIZE};
        writes[8] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[8].dstSet = sets[1];
        writes[8].dstBinding = 1;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8].pBufferInfo = &constants;
        VkDescriptorBufferInfo profileInfo = {profile.buffer, 0, VK_WHOLE_SIZE};
        writes[9] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[9].dstSet = sets[1];
        writes[9].dstBinding = 2;
        writes[9].descriptorCount = 1;
        writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[9].pBufferInfo = &profileInfo;
        vkUpdateDescriptorSets(device, 10, writes, 0, NULL);
    }

    uint8_t* composedPixels = malloc((size_t)f.width * f.height * 4);
    double gpuBest = 1e30;
    for (int r = 0; r < rounds; r++) {
        VkCommandBuffer cmd = begin();
        vkCmdResetQueryPool(cmd, queries, 0, 2);
        barrier(cmd, composed.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout, 0, 2, sets, 0, NULL);
        Push push = {{(int32_t)f.width, (int32_t)f.height}, {(int32_t)f.video[0], (int32_t)f.video[1]}, program.tilesX, 0, 203.0f, 0, {(int32_t)(f.video[2] - f.video[0]), (int32_t)(f.video[3] - f.video[1])}};
        for (int k = 0; k < 4; k++) {
            if (!program.programs[k][1]) continue;
            push.first = program.programs[k][0];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[k]);
            vkCmdPushConstants(cmd, composeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, program.programs[k][1], k ? 1 : (TILE / GROUP) * (TILE / GROUP), 1);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        finish(cmd);
        double us = elapsed();
        gpuBest = us < gpuBest ? us : gpuBest;
    }
    readback(&composed, f.width, f.height, VK_IMAGE_LAYOUT_GENERAL, composedPixels);
    printf("compose gpu %.1f us, cpu %.1f us, sum %.1f us\n", gpuBest, building, gpuBest + building);
    {
        const uint32_t* counts = profile.map;
        int any = 0;
        for (int i = 0; i < 64; i++) any = any || counts[i];
        if (any) {
            printf("profile cycles per round:");
            for (int i = 0; i < 64; i++)
                if (counts[i]) printf(" [%d]=%.0f", i, (double)counts[i] / rounds);
            printf("\n");
        }
    }
    if (f.hasVideo) {
        double layerBest = 1e30;
        for (int r = 0; r < rounds; r++) {
            VkCommandBuffer cmd = begin();
            vkCmdResetQueryPool(cmd, queries, 0, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout, 0, 2, sets, 0, NULL);
            Push push = {{(int32_t)f.width, (int32_t)f.height}, {(int32_t)f.video[0], (int32_t)f.video[1]}, program.tilesX, 0, 203.0f, 0, {(int32_t)(f.video[2] - f.video[0]), (int32_t)(f.video[3] - f.video[1])}};
            for (int k = 1; k < 4; k++) {
                if (!program.programs[k][1]) continue;
                push.first = program.programs[k][0];
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[k]);
                vkCmdPushConstants(cmd, composeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, program.programs[k][1], 1, 1);
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            finish(cmd);
            double us = elapsed();
            layerBest = us < layerBest ? us : layerBest;
        }
        printf("alone: video tiles %.1f us\n", layerBest);
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/composed.ppm", outdir);
    writePpm(path, composedPixels, f.width, f.height);
    return 0;
}
