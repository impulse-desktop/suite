#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static NSString* const prelude = @"#include <metal_stdlib>\nusing namespace metal;\n";
static NSString* const frameStruct = @"struct Frame {\n    int2 size;\n    int2 video;\n    uint tilesX;\n    uint first;\n    float white;\n};\n";
static NSString* const hostBody = @"kernel void host(device const uint* tiles [[buffer(4)]], constant Frame& frame [[buffer(5)]], const device uint* words [[buffer(7)]], texture2d<float, access::write> target [[texture(0)]], uint3 group [[threadgroup_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {\n"
                                   @"    uint tile = tiles[frame.first + group.x];\n"
                                   @"    int2 origin = int2(int(tile % frame.tilesX), int(tile / frame.tilesX)) * 24;\n"
                                   @"    int2 pixel = origin + int2(local.xy);\n"
                                   @"    LAYER_SHARED\n"
                                   @"    float4 shown = LAYER_CALL(local.xy, origin - frame.video, words);\n"
                                   @"    if (pixel.x < frame.size.x && pixel.y < frame.size.y) {\n"
                                   @"        target.write(shown, uint2(pixel));\n"
                                   @"    }\n"
                                   @"}\n";
static NSString* const linkedLayer = @"#define LAYER_SHARED\n#define LAYER_CALL(local, origin, words) layer(local, origin, words)\n[[visible]] float4 layer(uint2 local, int2 origin, const device uint* words);\n";

static const int videoX = 13;
static const int videoY = 7;
static const size_t wordsSize = 48u << 20;

typedef struct {
    int32_t size[2];
    int32_t video[2];
    uint32_t tilesX;
    uint32_t first;
    float white;
    uint32_t pad;
} Frame;

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static NSString* problem;

static id<MTLLibrary> compiled(NSString* source) {
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];

    options.languageVersion = MTLLanguageVersion3_0;

    id<MTLLibrary> library = [device newLibraryWithSource:[prelude stringByAppendingString:source] options:options error:&error];

    if (!library) {
        problem = [NSString stringWithFormat:@"MSL: %@", error.localizedDescription];
    }

    return library;
}

static id<MTLLibrary> loaded(NSString* path) {
    NSData* bytes = [NSData dataWithContentsOfFile:path];
    NSError* error = nil;

    if (!bytes) {
        problem = [NSString stringWithFormat:@"missing %@", path];

        return nil;
    }

    dispatch_data_t data = dispatch_data_create(bytes.bytes, bytes.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];

    if (!library) {
        problem = [NSString stringWithFormat:@"AIR library: %@", error.localizedDescription];
    }

    return library;
}

static id<MTLComputePipelineState> pipeline(id<MTLFunction> function, id<MTLFunction> linked) {
    NSError* error = nil;
    MTLComputePipelineDescriptor* descriptor = [MTLComputePipelineDescriptor new];

    if (!function) {
        problem = @"no function";

        return nil;
    }

    descriptor.computeFunction = function;
    descriptor.maxTotalThreadsPerThreadgroup = 24 * 24;

    if (linked) {
        MTLLinkedFunctions* functions = [MTLLinkedFunctions linkedFunctions];

        functions.privateFunctions = @[linked];
        descriptor.linkedFunctions = functions;
    }

    id<MTLComputePipelineState> made = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&error];

    if (!made) {
        problem = [NSString stringWithFormat:@"pipeline: %@", error.localizedDescription];
    }

    return made;
}

static id<MTLFunction> composeFunction(id<MTLLibrary> library, NSString* output) {
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    int encoding = [output isEqualToString:@"srgb"] ? 0 : 2;
    bool wide = [output isEqualToString:@"wide"];
    NSError* error = nil;

    [constants setConstantValue:&encoding type:MTLDataTypeInt atIndex:0];
    [constants setConstantValue:&wide type:MTLDataTypeBool atIndex:1];

    id<MTLFunction> function = [library newFunctionWithName:@"compose" constantValues:constants error:&error];

    if (!function) {
        problem = [NSString stringWithFormat:@"compose host: %@", error.localizedDescription];
    }

    return function;
}

static id<MTLBuffer> shared(const void* bytes, size_t size) {
    return [device newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
}

static void run(id<MTLComputePipelineState> state, NSArray<id<MTLBuffer>>* buffers, id<MTLBuffer> words, const Frame* frame, id<MTLTexture> target, NSUInteger tiles, id<MTLBuffer> out) {
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    const NSUInteger slots[4] = {0, 1, 2, 4};

    [encoder setComputePipelineState:state];

    for (NSUInteger i = 0; i < 4; i++) {
        [encoder setBuffer:buffers[i] offset:0 atIndex:slots[i]];
    }

    [encoder setBytes:frame length:sizeof(*frame) atIndex:5];
    [encoder setBuffer:words offset:0 atIndex:7];
    [encoder setTexture:target atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(tiles, 1, 1) threadsPerThreadgroup:MTLSizeMake(24, 24, 1)];
    [encoder endEncoding];

    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];

    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(target.width, target.height, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:target.width * 16 destinationBytesPerImage:target.width * target.height * 16];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];

    if (commands.error) {
        problem = [NSString stringWithFormat:@"dispatch: %@", commands.error.localizedDescription];
    }
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: check SAMPLES\n");

            return 2;
        }

        NSString* dir = [NSString stringWithUTF8String:argv[1]];
        NSString* manifest = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"manifest.txt"] encoding:NSUTF8StringEncoding error:nil];
        NSString* compose = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"compose.metal"] encoding:NSUTF8StringEncoding error:nil];

        device = MTLCreateSystemDefaultDevice();
        queue = [device newCommandQueue];

        if (!device || !manifest || !compose) {
            fprintf(stderr, "no device, manifest or host\n");

            return 2;
        }

        printf("device %s\n", device.name.UTF8String);

        id<MTLBuffer> words = [device newBufferWithLength:wordsSize options:MTLResourceStorageModeShared];
        uint32_t* filled = (uint32_t*)words.contents;
        uint32_t state = 0x9e3779b9u;

        for (size_t i = 0; i < wordsSize / 4; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            filled[i] = state;
        }

        id<MTLLibrary> airHost = compiled([NSString stringWithFormat:@"%@%@%@", frameStruct, linkedLayer, hostBody]);
        id<MTLLibrary> composeHost = compiled([@"#define GROUP 24\n#define LAYER 1\n" stringByAppendingString:compose]);

        if (!airHost || !composeHost) {
            fprintf(stderr, "hosts: %s\n", problem.UTF8String);

            return 1;
        }

        int failed = 0;
        int checked = 0;
        double worst = 0;
        NSString* worstName = @"";

        for (NSString* line in [manifest componentsSeparatedByString:@"\n"]) {
            NSArray<NSString*>* fields = [line componentsSeparatedByString:@" "];

            if (fields.count != 5) {
                continue;
            }

            @autoreleasepool {
                NSString* name = fields[0];
                NSString* kind = fields[1];
                NSString* output = fields[2];
                int width = fields[3].intValue;
                int height = fields[4].intValue;
                bool mixed = [kind isEqualToString:@"mixed"];
                int frameWidth = width + videoX + 11;
                int frameHeight = height + videoY + 17;
                uint32_t tilesX = (uint32_t)(frameWidth + 23) / 24;
                uint32_t tilesY = (uint32_t)(frameHeight + 23) / 24;
                NSMutableData* tileList = [NSMutableData data];

                problem = nil;

                for (uint32_t ty = 0; ty < tilesY; ty++) {
                    for (uint32_t tx = 0; tx < tilesX; tx++) {
                        int x0 = (int)tx * 24;
                        int y0 = (int)ty * 24;
                        bool touches = x0 < videoX + width && x0 + 24 > videoX && y0 < videoY + height && y0 + 24 > videoY;
                        bool within = x0 >= videoX && x0 + 24 <= videoX + width && y0 >= videoY && y0 + 24 <= videoY + height;
                        bool wanted = [kind isEqualToString:@"inside"] ? within : [kind isEqualToString:@"edge"] ? touches && !within : touches;

                        if (wanted) {
                            uint32_t tile = ty * tilesX + tx;

                            [tileList appendBytes:&tile length:4];
                        }
                    }
                }

                NSUInteger tiles = tileList.length / 4;

                if (!tiles) {
                    printf("%s: no tiles\n", name.UTF8String);
                    continue;
                }

                NSMutableData* headers = [NSMutableData dataWithLength:tilesX * tilesY * 32];
                const float backdrop[4] = {0.1f, 0.2f, 0.3f, 0.5f};

                for (uint32_t t = 0; t < tilesX * tilesY; t++) {
                    uint32_t* header = (uint32_t*)headers.mutableBytes + t * 8;

                    header[2] = 1;
                    memcpy(header + 4, backdrop, sizeof(backdrop));
                }

                uint32_t list[1] = {0};
                uint32_t ops[20] = {3, 0, 0, 0, (uint32_t)videoX, (uint32_t)videoY, (uint32_t)(videoX + width), (uint32_t)(videoY + height)};
                NSArray<id<MTLBuffer>>* buffers = @[shared(headers.bytes, headers.length), shared(list, sizeof(list)), shared(ops, sizeof(ops)), shared(tileList.bytes, tileList.length)];
                Frame frame = {{frameWidth, frameHeight}, {videoX, videoY}, tilesX, 0, 203.0f, 0};
                id<MTLLibrary> air = loaded([dir stringByAppendingPathComponent:[name stringByAppendingString:@".metallib"]]);
                NSString* text = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".metal"]] encoding:NSUTF8StringEncoding error:nil];
                id<MTLComputePipelineState> ours = nil;
                id<MTLComputePipelineState> theirs = nil;

                if (air && mixed) {
                    id<MTLFunction> layer = [air newFunctionWithName:@"layer"];

                    ours = pipeline([airHost newFunctionWithName:@"host"], layer);

                    if (ours && !pipeline(composeFunction(composeHost, output), layer)) {
                        ours = nil;
                    }
                } else if (air) {
                    ours = pipeline([air newFunctionWithName:@"compose"], nil);
                }

                if (ours) {
                    id<MTLLibrary> msl = compiled(mixed ? [NSString stringWithFormat:@"%@%@\n%@", frameStruct, text, hostBody] : text);

                    theirs = msl ? pipeline([msl newFunctionWithName:mixed ? @"host" : @"compose"], nil) : nil;
                }

                if (!ours || !theirs) {
                    printf("%s: FAIL %s\n", name.UTF8String, problem.UTF8String);
                    failed++;
                    continue;
                }

                MTLTextureDescriptor* format = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:(NSUInteger)frameWidth height:(NSUInteger)frameHeight mipmapped:NO];

                format.usage = MTLTextureUsageShaderWrite;
                format.storageMode = MTLStorageModePrivate;

                id<MTLTexture> targets[2] = {[device newTextureWithDescriptor:format], [device newTextureWithDescriptor:format]};
                id<MTLBuffer> pixels[2] = {[device newBufferWithLength:(NSUInteger)frameWidth * frameHeight * 16 options:MTLResourceStorageModeShared], [device newBufferWithLength:(NSUInteger)frameWidth * frameHeight * 16 options:MTLResourceStorageModeShared]};

                run(ours, buffers, words, &frame, targets[0], tiles, pixels[0]);
                run(theirs, buffers, words, &frame, targets[1], tiles, pixels[1]);

                if (problem) {
                    printf("%s: FAIL %s\n", name.UTF8String, problem.UTF8String);
                    failed++;
                    continue;
                }

                const float* a = (const float*)pixels[0].contents;
                const float* b = (const float*)pixels[1].contents;
                const uint32_t* listed = (const uint32_t*)tileList.bytes;
                double most = 0;
                double relative = 0;
                float pair[3] = {0, 0, 0};
                int where[3] = {0, 0, 0};
                long over = 0;
                long nans = 0;
                long compared = 0;

                for (NSUInteger t = 0; t < tiles; t++) {
                    int x0 = (int)(listed[t] % tilesX) * 24;
                    int y0 = (int)(listed[t] / tilesX) * 24;

                    for (int y = y0; y < y0 + 24 && y < frameHeight; y++) {
                        for (int x = x0; x < x0 + 24 && x < frameWidth; x++) {
                            if (mixed && (x < videoX || x >= videoX + width || y < videoY || y >= videoY + height)) {
                                continue;
                            }

                            compared++;

                            for (int c = 0; c < 4; c++) {
                                float p = a[((size_t)y * frameWidth + x) * 4 + c];
                                float q = b[((size_t)y * frameWidth + x) * 4 + c];

                                if (isnan(p) || isnan(q)) {
                                    nans += isnan(p) != isnan(q);
                                    continue;
                                }

                                double d = fabs((double)p - (double)q);
                                double r = d / fmax(1.0, fabs((double)q));

                                if (d > most) {
                                    most = d;
                                    pair[0] = p;
                                    pair[1] = q;
                                    pair[2] = b[((size_t)y * frameWidth + x) * 4 + 3];
                                    where[0] = x;
                                    where[1] = y;
                                    where[2] = c;
                                }

                                relative = r > relative ? r : relative;
                                over += r > 1.0 / 1024;
                            }
                        }
                    }
                }

                bool good = !nans && relative <= 0.1 && over * 1000 <= compared * 4;

                checked++;
                failed += !good;

                if (most > worst) {
                    worst = most;
                    worstName = name;
                }

                printf("%s: %s tiles %lu pixels %ld max %.6f relative %.6f over %ld nan %ld at (%d, %d).%d air %g msl %g alpha %g\n", name.UTF8String, good ? "ok" : "FAIL", (unsigned long)tiles, compared, most, relative, over, nans, where[0], where[1], where[2], pair[0], pair[1], pair[2]);
            }
        }

        printf("%d checked, %d failed, worst %.6f in %s\n", checked, failed, worst, worstName.UTF8String);

        return failed ? 1 : 0;
    }
}
