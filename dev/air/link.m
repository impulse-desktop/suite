#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static id<MTLLibrary> loadLibrary(id<MTLDevice> device, NSString* path) {
    NSData* bytes = [NSData dataWithContentsOfFile:path];

    if (!bytes) {
        printf("%s: missing\n", path.UTF8String);

        return nil;
    }

    dispatch_data_t data = dispatch_data_create(bytes.bytes, bytes.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];

    printf("%s: %s\n", path.UTF8String, library ? "loaded" : error.description.UTF8String);

    return library;
}

static void check(id<MTLDevice> device, id<MTLComputePipelineState> pipeline, bool exact) {
    MTLTextureDescriptor* format = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:48 height:48 mipmapped:NO];

    format.usage = MTLTextureUsageShaderWrite;
    format.storageMode = MTLStorageModePrivate;

    id<MTLTexture> target = [device newTextureWithDescriptor:format];
    uint32_t tiles[4] = {0, 1, 2, 3};
    struct {
        int32_t size[2];
        int32_t video[2];
        uint32_t tilesX;
        uint32_t first;
        float white;
        uint32_t pad;
    } frame = {{48, 48}, {5, 7}, 2, 0, 1.0f, 0};
    uint32_t words[576];

    for (uint32_t i = 0; i < 576; i++) {
        words[i] = i;
    }

    id<MTLBuffer> out = [device newBufferWithLength:48 * 48 * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];

    [encoder setComputePipelineState:pipeline];
    [encoder setBytes:tiles length:sizeof(tiles) atIndex:4];
    [encoder setBytes:&frame length:sizeof(frame) atIndex:5];
    [encoder setBytes:words length:sizeof(words) atIndex:7];
    [encoder setTexture:target atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(24, 24, 1)];
    [encoder endEncoding];

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];

    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(48, 48, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:48 * 16 destinationBytesPerImage:48 * 48 * 16];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];

    if (commands.error) {
        printf("  dispatch: %s\n", commands.error.description.UTF8String);

        return;
    }

    const float* pixels = (const float*)out.contents;
    int wrong = 0;

    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            const float* p = pixels + (y * 48 + x) * 4;

            int lane = y % 24 * 24 + x % 24;

            if (p[1] != (float)(y / 24 * 24 - 7) || p[2] != 0.0f || p[3] != 1.0f || (exact && p[0] != (float)((lane + 1) % 576 + x / 24 * 24 - 5))) {
                wrong++;
            }
        }
    }

    printf("  dispatch: %d of 2304 pixels wrong; pixel (30, 30) = %g %g %g %g\n", wrong, pixels[(30 * 48 + 30) * 4], pixels[(30 * 48 + 30) * 4 + 1], pixels[(30 * 48 + 30) * 4 + 2], pixels[(30 * 48 + 30) * 4 + 3]);
}

static void linkLayer(id<MTLDevice> device, NSString* text, NSString* label, id<MTLFunction> layer, bool exact) {
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];

    options.languageVersion = MTLLanguageVersion3_0;

    id<MTLLibrary> hosts = [device newLibraryWithSource:text options:options error:&error];

    printf("host with '%s': %s\n", label.UTF8String, hosts ? "compiled" : error.description.UTF8String);

    if (!hosts || !layer) {
        return;
    }

    MTLComputePipelineDescriptor* descriptor = [MTLComputePipelineDescriptor new];
    MTLLinkedFunctions* linked = [MTLLinkedFunctions linkedFunctions];

    linked.privateFunctions = @[layer];
    descriptor.computeFunction = [hosts newFunctionWithName:@"host"];
    descriptor.linkedFunctions = linked;

    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&error];

    printf("  linked pipeline: %s\n", pipeline ? "built" : error.description.UTF8String);

    if (pipeline) {
        check(device, pipeline, exact);
    }
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();

        if (!device) {
            printf("no Metal device\n");

            return 1;
        }

        printf("device %s, function pointers %d\n", device.name.UTF8String, (int)device.supportsFunctionPointers);

        NSError* error = nil;
        id<MTLLibrary> kernels = loadLibrary(device, @"probe_kernel.metallib");

        if (kernels) {
            id<MTLFunction> probe = [kernels newFunctionWithName:@"probe"];
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:probe error:&error];

            printf("probe pipeline: %s\n", pipeline ? "built" : error.description.UTF8String);
        }

        id<MTLLibrary> layers = loadLibrary(device, @"probe_layer.metallib");
        id<MTLFunction> layer = [layers newFunctionWithName:@"layer"];

        printf("layer function: %s, type %d\n", layer ? "found" : "missing", layer ? (int)layer.functionType : -1);

        NSString* source = [NSString stringWithContentsOfFile:@"probe_host.metal" encoding:NSUTF8StringEncoding error:&error];
        NSArray<NSString*>* variants = @[@"extern float4 layer", @"[[visible]] float4 layer", @"[[visible]] extern float4 layer"];

        for (NSString* declaration in variants) {
            linkLayer(device, [source stringByReplacingOccurrencesOfString:@"extern float4 layer" withString:declaration], declaration, layer, false);
        }

        id<MTLLibrary> owned = loadLibrary(device, @"probe_owned.metallib");
        NSString* text = [source stringByReplacingOccurrencesOfString:@"extern float4 layer(uint2 local, int2 origin, const device uint* words, threadgroup float* sharedA, threadgroup float* sharedB, threadgroup float* sharedW)" withString:@"[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words)"];

        text = [text stringByReplacingOccurrencesOfString:@"words, sharedA, sharedB, sharedW)" withString:@"words)"];
        linkLayer(device, text, @"a layer owning its threadgroup memory", [owned newFunctionWithName:@"layer"], true);
    }

    return 0;
}
