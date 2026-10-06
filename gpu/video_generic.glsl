struct Facts {
    uint model;
    uint count;
    uint flags;
    uint system;
    uint transfer;
    uint conversion;
    uint target;
    uint tile;
    int luma[5];
    int components[20];
    uint planeOffset[4];
    uint lineSize[4];
    uint size[4];
    float chroma[4];
    float decode[9];
    float bias[4];
    float sites[2];
    float weights[2];
    float curve[11];
    float oetf[11];
    float inverse[11];
    float toOutput[9];
    float light[3];
    float luminance[3];
    float toSignal[9];
    float toLight[9];
    float clNegative[2];
    float clPositive[2];
};

FACTS_DECL

uint genericSwap16(uint v) {
    return ((v & 0xffu) << 8u) | ((v >> 8u) & 0xffu);
}

uint genericSwap32(uint v) {
    return ((v & 0xffu) << 24u) | ((v & 0xff00u) << 8u) | ((v >> 8u) & 0xff00u) | (v >> 24u);
}

int genericFirst(int c ARGS_DECL) {
    int offset = facts.components[c * 5 + 2];
    int shift = facts.components[c * 5 + 3];
    int depth = facts.components[c * 5 + 4];

    return (facts.flags & 1u) != 0u && shift + depth <= 8 ? offset + 1 : offset;
}

float genericValue(int c, uint window ARGS_DECL) {
    int shift = facts.components[c * 5 + 3];
    int depth = facts.components[c * 5 + 4];
    uint mask = depth < 32 ? (1u << uint(depth)) - 1u : 0xffffffffu;
    bool be = (facts.flags & 1u) != 0u;
    bool floating = (facts.flags & 4u) != 0u;
    bool packed = (facts.flags & 8u) != 0u;
    uint swapped32 = genericSwap32(window);
    uint swapped16 = genericSwap16(window);
    uint bits = packed && depth == 10 ? (swapped32 >> uint(facts.components[c * 5 + 2])) & mask : packed ? window : be && shift + depth > 16 ? (swapped32 >> uint(shift)) & mask : be && shift + depth > 8 ? (swapped16 >> uint(shift)) & mask : (window >> uint(shift)) & mask;
    float real = depth == 16 ? HALF(be ? swapped16 : window) : BITS_FLOAT(be ? swapped32 : window);

    return floating ? real : float(bits);
}

uint genericWindow(int c, uint row, uint column ARGS_DECL) {
    int step = max(facts.components[c * 5 + 1], 1);
    int depth = facts.components[c * 5 + 4];
    int first = genericFirst(c ARGS);
    bool packed = (facts.flags & 8u) != 0u;
    bool packed10 = packed && depth == 10;
    bool lumaPath = c == 0 && facts.luma[0] != 0;
    bool narrow = step == 1 || step == 2;
    bool aligned = step % 4 == 0;
    bool spread = !packed && !lumaPath && !narrow && !aligned;
    uint bit = column * uint(step) + uint(facts.components[c * 5 + 2]);
    uint packedByte = bit >> 3u;
    uint packedPlace = ((packedByte & 3u) << 3u) + uint(8 - depth) - (bit & 7u);
    uint lane = column & 3u;
    uint within = lane == 0u ? uint(facts.luma[1]) : lane == 1u ? uint(facts.luma[2]) : lane == 2u ? uint(facts.luma[3]) : uint(facts.luma[4]);
    uint lumaByte = (column >> 2u) * uint(facts.luma[0]) + within;
    uint narrowPlace = (column & uint(4 / step - 1)) * uint(8 * step);
    uint spreadByte = column * uint(step);
    uint spreadPlace = (spreadByte & 3u) << 3u;
    uint index = packed10 ? column : packed ? (packedByte >> 2u) : lumaPath ? (lumaByte >> 2u) : narrow ? (column >> (step == 1 ? 2u : 1u)) : aligned ? column * uint(step / 4) + uint(first / 4) : (spreadByte >> 2u);
    uint low = words[row + index];
    uint high = spread ? words[row + index + 1u] : 0u;
    uint carried = spreadPlace == 0u ? 0u : high << (32u - spreadPlace);
    uint spreadValue = first >= 4 ? (high >> spreadPlace) >> uint(8 * (first - 4)) : ((low >> spreadPlace) | carried) >> uint(8 * first);

    return packed10 ? low : packed ? (low >> packedPlace) & ((1u << uint(depth)) - 1u) : lumaPath ? low >> ((lumaByte & 3u) << 3u) : narrow ? (low >> narrowPlace) >> uint(8 * first) : aligned ? low >> uint(8 * (first % 4)) : spreadValue;
}

uint genericRow(int c, uint line ARGS_DECL) {
    return (facts.planeOffset[facts.components[c * 5]] + line * facts.lineSize[facts.components[c * 5]]) >> 2u;
}

float genericSample(int c, uint column, uint line ARGS_DECL) {
    return genericValue(c, genericWindow(c, genericRow(c, line ARGS), column ARGS) ARGS);
}

float genericBilinear(int c, vec2 at, ivec2 extent ARGS_DECL) {
    vec2 base = floor(at);
    vec2 f = at - base;
    ivec2 whole = ivec2(base);
    uvec2 a = uvec2(clamp(whole, ivec2(0), extent - 1));
    uvec2 b = uvec2(clamp(whole + 1, ivec2(0), extent - 1));
    float left = mix(genericSample(c, a.x, a.y ARGS), genericSample(c, a.x, b.y ARGS), f.y);
    float right = mix(genericSample(c, b.x, a.y ARGS), genericSample(c, b.x, b.y ARGS), f.y);

    return mix(left, right, f.x);
}

vec4 genericPalette(uint column, uint line ARGS_DECL) {
    uint row = genericRow(0, line ARGS);
    uint index = (words[row + (column >> 2u)] >> ((column & 3u) << 3u)) & 0xffu;
    uint entry = words[(facts.planeOffset[1] >> 2u) + index];

    return vec4(float((entry >> 16u) & 0xffu), float((entry >> 8u) & 0xffu), float(entry & 0xffu), float((entry >> 24u) & 0xffu)) / 255.0;
}

float genericMosaic(int x, int y ARGS_DECL) {
    int step = facts.components[1];
    int lastX = int(facts.size[0]) - 1;
    int lastY = int(facts.size[1]) - 1;
    uint inside = uint(lastX - abs(lastX - abs(x)));
    uint line = uint(lastY - abs(lastY - abs(y)));
    uint row = genericRow(0, line ARGS);
    uint texel = words[row + (inside >> (step == 1 ? 2u : 1u))] >> ((inside & (step == 1 ? 3u : 1u)) * uint(8 * step));
    uint bits = step == 1 ? (texel & 0xffu) : ((facts.flags & 1u) != 0u ? genericSwap16(texel) : (texel & 0xffffu));

    return float(bits);
}

vec4 genericBayer(ivec2 at ARGS_DECL) {
    float a0 = genericMosaic(at.x - 1, at.y - 1 ARGS);
    float a1 = genericMosaic(at.x, at.y - 1 ARGS);
    float a2 = genericMosaic(at.x + 1, at.y - 1 ARGS);
    float m0 = genericMosaic(at.x - 1, at.y ARGS);
    float m1 = genericMosaic(at.x, at.y ARGS);
    float m2 = genericMosaic(at.x + 1, at.y ARGS);
    float d0 = genericMosaic(at.x - 1, at.y + 1 ARGS);
    float d1 = genericMosaic(at.x, at.y + 1 ARGS);
    float d2 = genericMosaic(at.x + 1, at.y + 1 ARGS);
    float horizontal = (m0 + m2) * 0.5;
    float vertical = (a1 + d1) * 0.5;
    float cross = (horizontal + vertical) * 0.5;
    float diagonal = (a0 + a2 + d0 + d2) * 0.25;
    float sameX = (uint(at.x) & 1u) == uint(facts.sites[0]) ? 1.0 : 0.0;
    float sameY = (uint(at.y) & 1u) == uint(facts.sites[1]) ? 1.0 : 0.0;
    float onRed = sameX * sameY;
    float onBlue = (1.0 - sameX) * (1.0 - sameY);
    float onGreen = 1.0 - onRed - onBlue;
    vec3 green = vec3(mix(vertical, horizontal, sameY), m1, mix(horizontal, vertical, sameY));
    vec3 redSite = vec3(m1, cross, diagonal);
    vec3 blueSite = vec3(diagonal, cross, m1);

    return vec4(onRed * redSite + onBlue * blueSite + onGreen * green, 0.0);
}

vec4 genericCodes(vec2 at ARGS_DECL) {
    ivec2 full = ivec2(int(facts.size[0]), int(facts.size[1]));
    ivec2 nearest = clamp(ivec2(floor(at + 0.5)), ivec2(0), full - 1);

    if (facts.model == 5u) {
        return genericPalette(uint(nearest.x), uint(nearest.y) ARGS);
    }

    if (facts.model == 4u) {
        return genericBayer(nearest ARGS);
    }

    vec4 codes = vec4(0.0);
    bool yuv = facts.model == 0u;
    bool alpha = (facts.flags & 2u) != 0u;
    int count = int(facts.count);

    codes.x = genericBilinear(0, at, full ARGS);

    if (!yuv && count > 1 && !(alpha && count == 2)) {
        codes.y = genericBilinear(1, at, full ARGS);
    }

    if (!yuv && count > 2) {
        codes.z = genericBilinear(2, at, full ARGS);
    }

    if (alpha) {
        codes.w = count == 2 ? genericBilinear(1, at, full ARGS) : genericBilinear(3, at, full ARGS);
    }

    if (yuv) {
        vec2 chromaAt = vec2(at.x * facts.chroma[0] - facts.chroma[2], at.y * facts.chroma[1] - facts.chroma[3]);
        ivec2 extent = ivec2(int(facts.size[2]), int(facts.size[3]));

        codes.y = genericBilinear(1, chromaAt, extent ARGS);
        codes.z = genericBilinear(2, chromaAt, extent ARGS);
    }

    return codes;
}

float genericTable(int which, int i ARGS_DECL) {
    return which == 0 ? facts.curve[i] : which == 1 ? facts.oetf[i] : facts.inverse[i];
}

float genericSegment(float v, int which, int base ARGS_DECL) {
    float k0 = genericTable(which, base ARGS);
    float k1 = genericTable(which, base + 1 ARGS);
    float k2 = genericTable(which, base + 2 ARGS);
    float k3 = genericTable(which, base + 3 ARGS);
    float k4 = genericTable(which, base + 4 ARGS);

    return k0 * pow(v * k2 + k3, k1) - k4;
}

float genericPiece(float v, int which ARGS_DECL) {
    float threshold = genericTable(which, 10 ARGS);

    if (threshold < 0.0) {
        return genericSegment(v, which, 0 ARGS);
    }

    return v <= threshold ? genericSegment(v, which, 5 ARGS) : genericSegment(v, which, 0 ARGS);
}

float genericPqLight(float signal) {
    float p = pow(clamp(signal, 0.0, 1.0), 32.0 / 2523.0);

    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 16384.0 / 2610.0);
}

float genericHlgScene(float signal) {
    float v = clamp(signal, 0.0, 1.0);

    return v <= 0.5 ? v * v / 3.0 : exp(v * 5.591816310 - 3.130917952) / 12.0 + 0.28466892 / 12.0;
}

vec3 genericHlgDisplay(vec3 scene ARGS_DECL) {
    float luminance = scene.x * facts.luminance[0] + scene.y * facts.luminance[1] + scene.z * facts.luminance[2];

    return scene * (pow(luminance, 0.2) * facts.light[2]);
}

float genericLogLight(float signal ARGS_DECL) {
    return signal <= 0.0 ? 0.0 : exp2((signal - 1.0) * 3.32192809489 * facts.light[0]);
}

float genericEncode(float light ARGS_DECL) {
    if (facts.transfer == 0u) {
        return genericPiece(max(light, 0.0), 1 ARGS);
    }

    if (facts.transfer == 1u) {
        float v = max(light, 1e-30);

        return v < exp2(-3.32192809489 * facts.light[0]) ? 0.0 : log2(v) * (0.30102999566 / facts.light[0]) + 1.0;
    }

    if (facts.transfer == 2u) {
        float y = pow(clamp(light, 0.0, 1.0), 2610.0 / 16384.0);

        return pow((y * 18.8515625 + 0.8359375) / (y * 18.6875 + 1.0), 2523.0 / 32.0);
    }

    float v = max(light, 0.0);

    return v <= 1.0 / 12.0 ? sqrt(v * 3.0) : log(max(v * 12.0 - 0.28466892, 1e-6)) * 0.17883277 + 0.55991073;
}

float genericDecodeLight(float signal ARGS_DECL) {
    if (facts.transfer == 0u) {
        return genericPiece(max(signal, 0.0), 2 ARGS);
    }

    if (facts.transfer == 1u) {
        return genericLogLight(signal ARGS);
    }

    if (facts.transfer == 2u) {
        return genericPqLight(signal);
    }

    return genericHlgScene(signal);
}

vec3 genericConstantLuminance(vec3 signal ARGS_DECL) {
    float kr = facts.weights[0];
    float kb = facts.weights[1];
    float chroma0 = signal.y * 2.0 * (signal.y <= 0.0 ? facts.clNegative[0] : facts.clPositive[0]);
    float chroma1 = signal.z * 2.0 * (signal.z <= 0.0 ? facts.clNegative[1] : facts.clPositive[1]);
    float r = signal.x + chroma1;
    float b = signal.x + chroma0;
    float lightY = genericDecodeLight(signal.x ARGS);
    float lightR = genericDecodeLight(r ARGS);
    float lightB = genericDecodeLight(b ARGS);
    float green = (lightY - lightR * kr - lightB * kb) / (1.0 - kr - kb);

    return vec3(r, genericEncode(green ARGS), b);
}

vec4 genericSignal(vec4 codes ARGS_DECL) {
    if (facts.model == 5u) {
        return codes;
    }

    vec3 signal = vec3(facts.bias[0], facts.bias[1], facts.bias[2]);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            signal[r] += codes[c] * facts.decode[r * 3 + c];
        }
    }

    if (facts.system == 1u) {
        signal = genericConstantLuminance(signal ARGS);
    }

    float alpha = (facts.flags & 2u) != 0u ? codes.w * facts.bias[3] : 1.0;

    return vec4(signal, alpha);
}

vec3 genericRows(int which, vec3 v ARGS_DECL) {
    vec3 result = vec3(0.0);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float m = which == 0 ? facts.toSignal[r * 3 + c] : which == 1 ? facts.toLight[r * 3 + c] : facts.toOutput[r * 3 + c];

            result[r] += v[c] * m;
        }
    }

    return result;
}

vec3 genericLight(vec3 signal ARGS_DECL) {
    vec3 light = vec3(0.0);

    if (facts.system == 2u) {
        bool pq = facts.transfer == 2u;
        vec3 lms = genericRows(0, signal ARGS);

        for (int i = 0; i < 3; i++) {
            lms[i] = pq ? genericPqLight(lms[i]) : genericHlgScene(lms[i]);
        }

        vec3 scene = genericRows(1, lms ARGS);

        return pq ? scene * facts.light[1] : genericHlgDisplay(scene ARGS);
    }

    if (facts.transfer == 0u) {
        for (int i = 0; i < 3; i++) {
            light[i] = genericPiece(max(signal[i], 0.0), 0 ARGS);
        }

        return light;
    }

    if (facts.transfer == 1u) {
        for (int i = 0; i < 3; i++) {
            light[i] = genericLogLight(signal[i] ARGS);
        }

        return light;
    }

    if (facts.transfer == 2u) {
        for (int i = 0; i < 3; i++) {
            light[i] = genericPqLight(signal[i]) * facts.light[1];
        }

        return light;
    }

    for (int i = 0; i < 3; i++) {
        light[i] = genericHlgScene(signal[i]);
    }

    return genericHlgDisplay(light ARGS);
}

vec4 genericLayer(uvec2 local, ivec2 origin ARGS_DECL) {
    ivec2 box = ivec2(max(frame.box.x, 1), max(frame.box.y, 1));
    ivec2 start = min(origin, box - 1);
    ivec2 pixel = clamp(start + ivec2(local), ivec2(0), box - 1);
    vec2 at = (vec2(pixel) + 0.5) * vec2(float(int(facts.size[0])), float(int(facts.size[1]))) / vec2(box) - 0.5;
    vec4 signal = genericSignal(genericCodes(at ARGS) ARGS);
    vec3 straight = genericLight(signal.xyz ARGS);

    if (facts.conversion == 1u) {
        straight = genericRows(2, straight ARGS);
    }

    if (facts.target == 0u) {
        straight = clamp(straight, 0.0, 1.0);
    }

    return vec4(straight, (facts.flags & 2u) != 0u ? clamp(signal.w, 0.0, 1.0) : 1.0);
}

vec3 genericWiden(vec3 c) {
    return vec3(0.627404 * c.x + 0.329283 * c.y + 0.043313 * c.z, 0.069097 * c.x + 0.919540 * c.y + 0.011362 * c.z, 0.016391 * c.x + 0.088013 * c.y + 0.895595 * c.z);
}

vec3 genericNarrow(vec3 c) {
    return vec3(1.660491 * c.x - 0.587641 * c.y - 0.072850 * c.z, -0.124550 * c.x + 1.132900 * c.y - 0.008349 * c.z, -0.018151 * c.x - 0.100579 * c.y + 1.118730 * c.z);
}

vec4 genericShown(uvec2 local, ivec2 origin ARGS_DECL) {
    vec4 layer = genericLayer(local, origin ARGS);
    bool layerWide = facts.target != 0u;

    if (layerWide) {
        return vec4(WIDE ? layer.xyz : clamp(genericNarrow(layer.xyz), 0.0, 1.0), layer.w);
    }

    return vec4(WIDE ? genericWiden(layer.xyz) : layer.xyz, layer.w);
}

float genericNoise(ivec2 pixel) {
    vec2 at = vec2(pixel) + 0.5;
    float inner = at.x * 0.06711056 + at.y * 0.00583715;

    return fract(52.9829189 * fract(inner)) - 0.5;
}

void genericKernel(uvec2 local, uint group ARGS_DECL) {
    uint tile = tiles[frame.first + group];
    ivec2 origin = ivec2(int(tile % frame.tilesX), int(tile / frame.tilesX)) * int(facts.tile);
    ivec2 pixel = origin + ivec2(local);
    vec4 shown = genericShown(local, origin - frame.video ARGS);
    float inside = 1.0;

    if (EDGE) {
        uint video = list[headers[tile].start] & ~FILL;
        ivec4 rect = ops[video].rect;

        inside = pixel.x >= rect.x && pixel.y >= rect.y && pixel.x < rect.z && pixel.y < rect.w ? 1.0 : 0.0;
    }

    if (pixel.x >= frame.size.x || pixel.y >= frame.size.y) {
        return;
    }

    float cover = shown.w * inside;
    vec4 color = headers[tile].color;
    vec4 acc = vec4(shown.xyz * cover, cover) + color * (1.0 - cover);
    float noise = genericNoise(pixel);

    if (OUTPUT == 0) {
        vec3 v = clamp(acc.xyz, 0.0, 1.0);
        vec3 coded;

        for (int c = 0; c < 3; c++) {
            coded[c] = v[c] <= 0.0031308 ? v[c] * 12.92 : pow(v[c], 1.0 / 2.4) * 1.055 - 0.055;
        }

        STORE(pixel, vec4(coded + noise / 255.0, 1.0));
    } else if (OUTPUT == 1) {
        vec3 coded;

        for (int c = 0; c < 3; c++) {
            float y = pow(clamp(acc[c] * frame.white / 10000.0, 0.0, 1.0), 2610.0 / 16384.0);

            coded[c] = pow((y * 18.8515625 + 0.8359375) / (y * 18.6875 + 1.0), 2523.0 / 32.0);
        }

        STORE(pixel, vec4(coded + noise / 1023.0, 1.0));
    } else {
        STORE(pixel, acc.w > 0.0 ? vec4(acc.xyz / acc.w, acc.w) : vec4(0.0));
    }
}
