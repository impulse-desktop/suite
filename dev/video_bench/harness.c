#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkPhysicalDeviceProperties props;
static VkDescriptorSetLayout setLayout;
static VkPipelineLayout pipelineLayout;
static VkCommandPool commandPool;
static VkShaderModule vertexShader;
static PFN_vkGetPipelineExecutableStatisticsKHR getStatistics;
static PFN_vkGetPipelineExecutableInternalRepresentationsKHR getRepresentations;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* map;
} Buffer;

typedef struct {
    VkFormat format;
    uint32_t width, height;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkRenderPass pass;
    VkFramebuffer framebuffer;
} Target;

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
    info.size = size;
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

static VkShaderModule moduleOf(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m;
    CHECK(vkCreateShaderModule(device, &info, NULL, &m));
    return m;
}

static VkShaderModule module(const char* path) {
    size_t size;
    uint32_t* code = readFile(path, &size);
    VkShaderModule m = moduleOf(code, size);
    free(code);
    return m;
}

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9;
}

static Target makeTarget(VkFormat format, uint32_t width, uint32_t height, int readable) {
    Target t = {format, width, height};
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = (VkExtent3D){width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (readable ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    CHECK(vkCreateImage(device, &imageInfo, NULL, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, t.image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(device, &alloc, NULL, &t.memory));
    CHECK(vkBindImageMemory(device, t.image, t.memory, 0));
    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = t.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(device, &viewInfo, NULL, &t.view));
    VkAttachmentDescription attachment = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
        readable ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo passInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    CHECK(vkCreateRenderPass(device, &passInfo, NULL, &t.pass));
    VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = t.pass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &t.view;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    CHECK(vkCreateFramebuffer(device, &fbInfo, NULL, &t.framebuffer));
    return t;
}

static VkPipeline makePipeline(VkShaderModule fragment, const Target* t, int statistics) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexShader, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", NULL},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {0, 0, (float)t->width, (float)t->height, 0, 1};
    VkRect2D scissor = {{0, 0}, {t->width, t->height}};
    VkPipelineViewportStateCreateInfo viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment = {0};
    blendAttachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.flags = statistics ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.layout = pipelineLayout;
    info.renderPass = t->pass;
    VkPipeline pipeline;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
    return pipeline;
}

static void printStatistics(const char* label, VkPipeline pipeline) {
    VkPipelineExecutableInfoKHR exec = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    exec.pipeline = pipeline;
    exec.executableIndex = 1;
    uint32_t count = 0;
    getStatistics(device, &exec, &count, NULL);
    VkPipelineExecutableStatisticKHR stats[64];
    for (uint32_t i = 0; i < count; i++) stats[i] = (VkPipelineExecutableStatisticKHR){VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
    getStatistics(device, &exec, &count, stats);
    printf("stats %s", label);
    for (uint32_t i = 0; i < count; i++) {
        const char* keep[] = {"Instructions", "VALU", "SALU", "VMEM", "SMEM", "Latency", "Inverse Throughput", "VGPRs"};
        for (int k = 0; k < 8; k++)
            if (!strcmp(stats[i].name, keep[k])) printf(" %s=%llu", stats[i].name, (unsigned long long)stats[i].value.u64);
    }
    printf("\n");
}

static void writeAssembly(const char* path, VkPipeline pipeline) {
    VkPipelineExecutableInfoKHR exec = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    exec.pipeline = pipeline;
    exec.executableIndex = 1;
    uint32_t count = 0;
    getRepresentations(device, &exec, &count, NULL);
    VkPipelineExecutableInternalRepresentationKHR representations[8];
    if (count > 8) count = 8;
    for (uint32_t i = 0; i < count; i++) representations[i] = (VkPipelineExecutableInternalRepresentationKHR){VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR};
    getRepresentations(device, &exec, &count, representations);
    for (uint32_t i = 0; i < count; i++) representations[i].pData = calloc(1, representations[i].dataSize + 1);
    getRepresentations(device, &exec, &count, representations);
    FILE* f = fopen(path, "w");
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(representations[i].name, "Assembly")) fputs(representations[i].pData, f);
    fclose(f);
}

static VkDescriptorSet makeSet(Buffer* bytes, Buffer* uniform) {
    static VkDescriptorPool pool;
    if (!pool) {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024}};
        VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1024;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        CHECK(vkCreateDescriptorPool(device, &poolInfo, NULL, &pool));
    }
    VkDescriptorSetAllocateInfo allocSet = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocSet.descriptorPool = pool;
    allocSet.descriptorSetCount = 1;
    allocSet.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &allocSet, &set));
    VkDescriptorBufferInfo storage = {bytes->buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo constants = {uniform->buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &storage, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &constants, NULL},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    return set;
}

static double run(VkPipeline pipeline, VkDescriptorSet set, const Target* t, uint32_t draws, Buffer* readback) {
    VkCommandBufferAllocateInfo commandInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &commandInfo, &command));
    VkQueryPoolCreateInfo queryInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    VkQueryPool queries;
    CHECK(vkCreateQueryPool(device, &queryInfo, NULL, &queries));
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fenceInfo, NULL, &fence));
    double best = 1e30;
    for (int round = 0; round < 1; round++) {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        vkCmdResetQueryPool(command, queries, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        for (uint32_t i = 0; i < draws; i++) {
            VkRenderPassBeginInfo passBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            passBegin.renderPass = t->pass;
            passBegin.framebuffer = t->framebuffer;
            passBegin.renderArea = (VkRect2D){{0, 0}, {t->width, t->height}};
            vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, NULL);
            vkCmdDraw(command, 3, 1, 0, 0);
            vkCmdEndRenderPass(command);
        }
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        if (readback) {
            VkBufferImageCopy copy = {0};
            copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = (VkExtent3D){t->width, t->height, 1};
            vkCmdCopyImageToBuffer(command, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
        }
        CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(device, 1, &fence));
        uint64_t stamps[2];
        CHECK(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double ns = (double)(stamps[1] - stamps[0]) * props.limits.timestampPeriod / draws;
        if (ns < best) best = ns;
        CHECK(vkResetCommandBuffer(command, 0));
    }
    vkDestroyFence(device, fence, NULL);
    vkDestroyQueryPool(device, queries, NULL);
    vkFreeCommandBuffers(device, commandPool, 1, &command);
    return best;
}

typedef struct {
    uint32_t width, height;
    Target check;
    Target timing[2];
} Size;

typedef struct {
    const char* directory;
    Size* size;
    VkShaderModule shaders[2];
    Buffer bytes;
    Buffer uniforms[2];
    VkDescriptorSet sets[2];
    VkPipeline pipelines[2][2];
    double best[2][2];
} Case;

static char* joined(const char* directory, const char* name) {
    size_t length = strlen(directory) + strlen(name) + 2;
    char* path = malloc(length);
    snprintf(path, length, "%s/%s", directory, name);
    return path;
}

static Size sizes[16];
static int sizeCount;

static Size* sizeOf(uint32_t width, uint32_t height) {
    for (int i = 0; i < sizeCount; i++)
        if (sizes[i].width == width && sizes[i].height == height) return &sizes[i];
    if (sizeCount == 16) {
        fprintf(stderr, "too many target sizes\n");
        exit(1);
    }
    Size* s = &sizes[sizeCount++];
    s->width = width;
    s->height = height;
    s->check = makeTarget(VK_FORMAT_R32G32B32A32_SFLOAT, width, height, 1);
    s->timing[0] = makeTarget(VK_FORMAT_R8_UNORM, width, height, 0);
    s->timing[1] = makeTarget(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, 0);
    return s;
}

static double buildTime(const char* path, const Target* t) {
    size_t size;
    uint32_t* code = readFile(path, &size);
    double best = 1e30;
    for (int i = 0; i < 5; i++) {
        double start = seconds();
        VkShaderModule fragment = moduleOf(code, size);
        VkPipeline pipeline = makePipeline(fragment, t, 0);
        double spent = seconds() - start;
        if (spent < best) best = spent;
        vkDestroyPipeline(device, pipeline, NULL);
        vkDestroyShaderModule(device, fragment, NULL);
    }
    free(code);
    return best;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: harness VERT DRAWS ROUNDS CASE_DIRECTORY...\n");
        return 2;
    }
    uint32_t draws = atoi(argv[2]);
    int rounds = atoi(argv[3]);
    int count = argc - 4;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&instanceInfo, NULL, &instance));
    uint32_t devices = 1;
    vkEnumeratePhysicalDevices(instance, &devices, &gpu);
    vkGetPhysicalDeviceProperties(gpu, &props);
    float priority = 1.f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executables = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    executables.pipelineExecutableInfo = VK_TRUE;
    const char* extensions[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &executables;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(gpu, &deviceInfo, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    getStatistics = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR");
    getRepresentations = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(device, "vkGetPipelineExecutableInternalRepresentationsKHR");

    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    CHECK(vkCreateDescriptorSetLayout(device, &setInfo, NULL, &setLayout));
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    CHECK(vkCreatePipelineLayout(device, &layoutInfo, NULL, &pipelineLayout));
    VkCommandPoolCreateInfo commandPoolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHECK(vkCreateCommandPool(device, &commandPoolInfo, NULL, &commandPool));
    vertexShader = module(argv[1]);

    const char* labels[2] = {"optimum", "template"};
    const char* names[2] = {"r8", "rgba16f"};
    Case* cases = calloc(count, sizeof(Case));
    VkDeviceSize largest = 0;
    for (int i = 0; i < count; i++) {
        Case* c = &cases[i];
        c->directory = argv[4 + i];
        size_t length;
        char* text = readFile(joined(c->directory, "size"), &length);
        text[length] = 0;
        unsigned width, height;
        if (sscanf(text, "%u %u", &width, &height) != 2) {
            fprintf(stderr, "%s: no target size\n", c->directory);
            return 1;
        }
        free(text);
        c->size = sizeOf(width, height);
        if ((VkDeviceSize)width * height * 16 > largest) largest = (VkDeviceSize)width * height * 16;
    }
    Buffer pixels[2];
    for (int k = 0; k < 2; k++)
        pixels[k] = makeBuffer(largest, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (int i = 0; i < count; i++) {
        Case* c = &cases[i];
        const Target* check = &c->size->check;
        const Target* targets = c->size->timing;
        size_t dataSize;
        void* data = readFile(joined(c->directory, "data.bin"), &dataSize);
        c->bytes = makeBuffer(dataSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        memcpy(c->bytes.map, data, dataSize);
        free(data);
        for (int k = 0; k < 2; k++) {
            char shader[32], uniform[32];
            snprintf(shader, sizeof(shader), "%s.spv", labels[k]);
            snprintf(uniform, sizeof(uniform), "%s.ubo", labels[k]);
            c->shaders[k] = module(joined(c->directory, shader));
            size_t size;
            void* u = readFile(joined(c->directory, uniform), &size);
            c->uniforms[k] = makeBuffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            memset(c->uniforms[k].map, 0, 1024);
            memcpy(c->uniforms[k].map, u, size);
            free(u);
            c->sets[k] = makeSet(&c->bytes, &c->uniforms[k]);
            VkPipeline pipeline = makePipeline(c->shaders[k], check, 0);
            run(pipeline, c->sets[k], check, 1, &pixels[k]);
            vkDestroyPipeline(device, pipeline, NULL);
        }
        double worst = 0;
        double squares = 0;
        for (size_t p = 0; p < (size_t)check->width * check->height * 4; p++) {
            double d = ((float*)pixels[0].map)[p] - ((float*)pixels[1].map)[p];
            if (p % 4 != 3) squares += d * d;
            d = d < 0 ? -d : d;
            if (d > worst) worst = d;
        }
        printf("case %s\n", c->directory);
        printf("difference %s %g\n", c->directory, worst);
        printf("rmse %s %g\n", c->directory, sqrt(squares / ((double)check->width * check->height * 3)));
        for (int k = 0; k < 2; k++) {
            char shader[32];
            snprintf(shader, sizeof(shader), "%s.spv", labels[k]);
            printf("build %s %s %.3f\n", c->directory, labels[k], buildTime(joined(c->directory, shader), &targets[1]) * 1e3);
        }
        for (int k = 0; k < 2; k++) {
            VkPipeline statistics = makePipeline(c->shaders[k], &targets[0], 1);
            printf("%s ", c->directory);
            printStatistics(labels[k], statistics);
            char assembly[32];
            snprintf(assembly, sizeof(assembly), "%s.s", labels[k]);
            writeAssembly(joined(c->directory, assembly), statistics);
            vkDestroyPipeline(device, statistics, NULL);
        }
        for (int t = 0; t < 2; t++)
            for (int k = 0; k < 2; k++) {
                c->pipelines[t][k] = makePipeline(c->shaders[k], &targets[t], 0);
                c->best[t][k] = 1e30;
            }
        fflush(stdout);
    }
    for (int round = 0; round < rounds; round++)
        for (int i = 0; i < count; i++)
            for (int t = 0; t < 2; t++)
                for (int k = 0; k < 2; k++) {
                    double ns = run(cases[i].pipelines[t][k], cases[i].sets[k], &cases[i].size->timing[t], draws, NULL);
                    if (ns < cases[i].best[t][k]) cases[i].best[t][k] = ns;
                }
    for (int i = 0; i < count; i++)
        for (int t = 0; t < 2; t++)
            for (int k = 0; k < 2; k++)
                printf("time %s %s %s %.2f\n", cases[i].directory, names[t], labels[k], cases[i].best[t][k] / 1000.);
    return 0;
}
