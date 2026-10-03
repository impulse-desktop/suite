#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static VkShaderModule module(const char* path) {
    size_t size;
    uint32_t* code = readFile(path, &size);
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m;
    CHECK(vkCreateShaderModule(device, &info, NULL, &m));
    return m;
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
    info.flags = statistics ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0;
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

static VkDescriptorSet makeSet(Buffer* bytes, Buffer* uniform) {
    static VkDescriptorPool pool;
    if (!pool) {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}};
        VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 8;
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
    for (int round = 0; round < (readback ? 1 : 7); round++) {
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

int main(int argc, char** argv) {
    if (argc != 8) {
        fprintf(stderr, "usage: harness VERT DATA OPTIMUM_SPV OPTIMUM_UBO TEMPLATE_SPV TEMPLATE_UBO DRAWS\n");
        return 2;
    }
    uint32_t draws = atoi(argv[7]);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&instanceInfo, NULL, &instance));
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &gpu);
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

    size_t dataSize;
    void* data = readFile(argv[2], &dataSize);
    Buffer bytes = makeBuffer(dataSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(bytes.map, data, dataSize);
    const char* labels[2] = {"optimum", "template"};
    VkShaderModule shaders[2] = {module(argv[3]), module(argv[5])};
    Buffer uniforms[2];
    VkDescriptorSet sets[2];
    for (int k = 0; k < 2; k++) {
        size_t size;
        void* u = readFile(argv[4 + 2 * k], &size);
        uniforms[k] = makeBuffer(1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        memset(uniforms[k].map, 0, 1024);
        memcpy(uniforms[k].map, u, size);
        sets[k] = makeSet(&bytes, &uniforms[k]);
    }

    Target check = makeTarget(VK_FORMAT_R32G32B32A32_SFLOAT, 960, 540, 1);
    Buffer pixels[2];
    for (int k = 0; k < 2; k++) {
        pixels[k] = makeBuffer((VkDeviceSize)check.width * check.height * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkPipeline pipeline = makePipeline(shaders[k], &check, 0);
        run(pipeline, sets[k], &check, 1, &pixels[k]);
    }
    double worst = 0;
    for (size_t i = 0; i < (size_t)check.width * check.height * 4; i++) {
        double d = ((float*)pixels[0].map)[i] - ((float*)pixels[1].map)[i];
        d = d < 0 ? -d : d;
        if (d > worst) worst = d;
    }
    printf("difference %g\n", worst);

    Target targets[2] = {makeTarget(VK_FORMAT_R8_UNORM, 3840, 2160, 0), makeTarget(VK_FORMAT_R16G16B16A16_SFLOAT, 3840, 2160, 0)};
    const char* names[2] = {"r8", "rgba16f"};
    for (int k = 0; k < 2; k++) {
        VkPipeline statistics = makePipeline(shaders[k], &targets[0], 1);
        printStatistics(labels[k], statistics);
    }
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 2; k++) {
            VkPipeline pipeline = makePipeline(shaders[k], &targets[t], 0);
            printf("time %s %s %.2f\n", names[t], labels[k], run(pipeline, sets[k], &targets[t], draws, NULL) / 1000.);
        }
    return 0;
}
