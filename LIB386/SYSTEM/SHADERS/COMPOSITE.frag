#version 450
/* Presents the frame: GPU colour wherever a GPU surface owns the pixel, the
   software frame elsewhere. When the frame is shown larger than its pixels,
   software pixels go through a pixel-art upscaler (xBR level 2 with blending,
   after Hyllian) and a gentle debanding pass that fills the palette's steps
   with in-between colours. */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_frame;
layout(set = 2, binding = 1) uniform sampler2D u_objColor;
layout(set = 2, binding = 2) uniform sampler2D u_objId; // R32F draw ids
layout(set = 2, binding = 3) uniform sampler2D u_tags;  // R32F draw ids per pixel
layout(set = 2, binding = 4) uniform sampler2D u_objLight; // dynamic light, halved

layout(set = 3, binding = 0) uniform Params {
    float debugTint;   // > 0: tint GPU pixels green (LBA2_GPU_DEBUG)
    float supersample; // > 0: the GPU targets are larger than the screen area
    float pixelFilter; // > 0: upscaled software pixels go through xBR
    float deband;      // > 0: upscaled software pixels are debanded
};

// --- Software frame -----------------------------------------------------------
ivec2 g_frameSize;

vec3 Src(ivec2 p) {
    return texelFetch(u_frame, clamp(p, ivec2(0), g_frameSize - ivec2(1)), 0).rgb;
}

/* Perceptual distance used by xBR, on a 0..255-ish luma-weighted scale. */
float Luma(vec3 c) {
    return dot(c, vec3(65.0, 129.0, 25.0)) * 1.2;
}

vec4 Df(vec4 a, vec4 b) {
    return abs(a - b);
}

bvec4 Eq(vec4 a, vec4 b) {
    return lessThan(Df(a, b), vec4(15.0));
}

bvec4 NotEq(vec4 a, vec4 b) {
    return greaterThanEqual(Df(a, b), vec4(15.0));
}

vec4 WeightedDistance(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return Df(a, b) + Df(a, c) + Df(d, e) + Df(d, f) + 4.0 * Df(g, h);
}

bvec4 And(bvec4 a, bvec4 b) {
    return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w);
}

bvec4 Or(bvec4 a, bvec4 b) {
    return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w);
}

float ColorDistance(vec3 a, vec3 b) {
    vec3 d = abs(a - b);
    return d.r + d.g + d.b;
}

vec3 XbrUpscale(vec2 uv) {
    vec2 pos = uv * vec2(g_frameSize);
    ivec2 c = ivec2(floor(pos));
    vec2 fp = fract(pos);

    vec3 A1 = Src(c + ivec2(-1, -2)), B1 = Src(c + ivec2(0, -2)), C1 = Src(c + ivec2(1, -2));
    vec3 A0 = Src(c + ivec2(-2, -1)), A = Src(c + ivec2(-1, -1)), B = Src(c + ivec2(0, -1)), C = Src(c + ivec2(1, -1)),
         C4 = Src(c + ivec2(2, -1));
    vec3 D0 = Src(c + ivec2(-2, 0)), D = Src(c + ivec2(-1, 0)), E = Src(c), F = Src(c + ivec2(1, 0)),
         F4 = Src(c + ivec2(2, 0));
    vec3 G0 = Src(c + ivec2(-2, 1)), G = Src(c + ivec2(-1, 1)), H = Src(c + ivec2(0, 1)), I = Src(c + ivec2(1, 1)),
         I4 = Src(c + ivec2(2, 1));
    vec3 G5 = Src(c + ivec2(-1, 2)), H5 = Src(c + ivec2(0, 2)), I5 = Src(c + ivec2(1, 2));

    vec4 b = vec4(Luma(B), Luma(D), Luma(H), Luma(F));
    vec4 cc = vec4(Luma(C), Luma(A), Luma(G), Luma(I));
    vec4 d = b.yzwx;
    vec4 e = vec4(Luma(E));
    vec4 f = b.wxyz;
    vec4 g = cc.zwxy;
    vec4 h = b.zwxy;
    vec4 i = cc.wxyz;
    vec4 i4 = vec4(Luma(I4), Luma(C1), Luma(A0), Luma(G5));
    vec4 i5 = vec4(Luma(I5), Luma(C4), Luma(A1), Luma(G0));
    vec4 h5 = vec4(Luma(H5), Luma(F4), Luma(B1), Luma(D0));
    vec4 f4 = h5.yzwx;

    const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0);
    const vec4 Bo = vec4(1.0, 1.0, -1.0, -1.0);
    const vec4 Co = vec4(1.5, 0.5, -0.5, 0.5);
    const vec4 Ax = vec4(1.0, -1.0, -1.0, 1.0);
    const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0);
    const vec4 Cx = vec4(1.0, 1.0, -0.5, 0.0);
    const vec4 Ay = vec4(1.0, -1.0, -1.0, 1.0);
    const vec4 By = vec4(2.0, 0.5, -2.0, -0.5);
    const vec4 Cy = vec4(2.0, 0.0, -1.0, 0.5);

    vec4 fx = Ao * fp.y + Bo * fp.x;
    vec4 fxLeft = Ax * fp.y + Bx * fp.x;
    vec4 fxUp = Ay * fp.y + By * fp.x;

    /* Edge softness: half an output pixel, in source-pixel units. */
    vec2 fw = fwidth(pos);
    vec4 delta = vec4(max(0.5 * max(fw.x, fw.y), 1e-3));
    vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) * delta;
    vec4 deltaU = deltaL.yxwz;

    vec4 fx45 = clamp((fx + delta - Co) / (2.0 * delta), 0.0, 1.0);
    vec4 fx30 = clamp((fxLeft + deltaL - Cx) / (2.0 * deltaL), 0.0, 1.0);
    vec4 fx60 = clamp((fxUp + deltaU - Cy) / (2.0 * deltaU), 0.0, 1.0);

    bvec4 lv1 = And(And(NotEq(e, f), NotEq(e, h)),
                    Or(Or(And(NotEq(f, b), NotEq(h, d)), And(And(Eq(e, i), NotEq(f, i4)), NotEq(h, i5))),
                       Or(Eq(e, g), Eq(e, cc))));
    bvec4 lv2Left = And(NotEq(e, g), NotEq(d, g));
    bvec4 lv2Up = And(NotEq(e, cc), NotEq(b, cc));

    bvec4 edr = And(lessThan(WeightedDistance(e, cc, g, i, h5, f4, h, f), WeightedDistance(h, d, i5, f, i4, b, e, i)),
                    lv1);
    bvec4 edrLeft = And(lessThanEqual(2.0 * Df(f, g), Df(h, cc)), lv2Left);
    bvec4 edrUp = And(greaterThanEqual(Df(f, g), 2.0 * Df(h, cc)), lv2Up);

    vec4 nc45 = vec4(edr) * fx45;
    vec4 nc30 = vec4(And(edr, edrLeft)) * fx30;
    vec4 nc60 = vec4(And(edr, edrUp)) * fx60;
    vec4 blend = max(max(nc30, nc60), nc45);

    bvec4 px = lessThanEqual(Df(e, f), Df(e, h));
    vec3 pX = px.x ? F : H;
    vec3 pY = px.y ? B : F;
    vec3 pZ = px.z ? D : B;
    vec3 pW = px.w ? H : D;

    vec3 res1 = E;
    res1 = mix(res1, pX, blend.x);
    res1 = mix(res1, pY, blend.y);
    res1 = mix(res1, pZ, blend.z);
    res1 = mix(res1, pW, blend.w);

    vec3 res2 = E;
    res2 = mix(res2, pW, blend.w);
    res2 = mix(res2, pZ, blend.z);
    res2 = mix(res2, pY, blend.y);
    res2 = mix(res2, pX, blend.x);

    return ColorDistance(E, res1) >= ColorDistance(E, res2) ? res2 : res1;
}

/* Softens the palette's 16-step ramps: blends toward neighbours close enough
   in colour to be the same surface, never across an edge. */
vec3 Deband(vec2 uv, vec3 color) {
    ivec2 c = ivec2(floor(uv * vec2(g_frameSize)));
    vec3 centre = Src(c);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 n = Src(c + ivec2(x, y));
            float w = 1.0 - smoothstep(0.03, 0.09, ColorDistance(n, centre));
            sum += n * w;
            weight += w;
        }
    }
    vec3 average = sum / max(weight, 1e-4);
    return mix(color, average, 0.35);
}

vec3 SoftwarePixel(vec2 uv) {
    vec3 c = pixelFilter > 0.0 ? XbrUpscale(uv) : texture(u_frame, uv).rgb;
    return deband > 0.0 ? Deband(uv, c) : c;
}

// -----------------------------------------------------------------------------
void main() {
    g_frameSize = textureSize(u_frame, 0);
    vec4 frame = texture(u_frame, v_uv) * v_color;

    /* Tags are per software-frame pixel; the GPU targets may be larger (the
       window's resolution), so each is addressed in its own texels. */
    ivec2 tagSize = textureSize(u_tags, 0);
    ivec2 tp = clamp(ivec2(v_uv * vec2(tagSize)), ivec2(0), tagSize - ivec2(1));
    ivec2 objSize = textureSize(u_objId, 0);
    ivec2 p = clamp(ivec2(v_uv * vec2(objSize)), ivec2(0), objSize - ivec2(1));
    float tag = texelFetch(u_tags, tp, 0).r;
    float id = texelFetch(u_objId, p, 0).r;

    /* LBA2_GPU_DEBUG=2: the GPU image alone, wherever it drew. */
    if (debugTint > 1.5) {
        o_color = id > 0.0 ? vec4(texelFetch(u_objColor, p, 0).rgb, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    /* Scene tags accept any scene surface: the GPU's depth picks it. */
    const float SCENE_BIT = 8388608.0;
    bool match = tag > 0.0 && (abs(tag - id) < 0.5 || (tag >= SCENE_BIT && id >= SCENE_BIT));
    if (!match) {
        o_color = vec4(SoftwarePixel(v_uv), 1.0) * v_color;
        return;
    }

    vec4 nearest = texelFetch(u_objColor, p, 0);
    vec3 light = texelFetch(u_objLight, p, 0).rgb * 2.0;
    if (nearest.a < 0.5) {
        /* A surface whose colour is the software frame (interior bricks). */
        vec3 c = SoftwarePixel(v_uv);
        o_color = vec4(c * (vec3(1.0) + light), 1.0) * v_color;
        return;
    }

    /* Colour is cleared to alpha 0 where no body is drawn, so a filtered read
       divided by its alpha averages only body texels at the edge. */
    vec3 gpu;
    if (supersample > 0.0) {
        vec4 c = textureLod(u_objColor, v_uv, 0.0);
        gpu = c.a > 0.0 ? c.rgb / c.a : nearest.rgb;
    } else {
        gpu = nearest.rgb;
    }
    gpu *= vec3(1.0) + light;
    gpu = mix(gpu, vec3(0.0, 1.0, 0.0), min(debugTint, 1.0) * 0.5);
    o_color = vec4(gpu, frame.a);
}
