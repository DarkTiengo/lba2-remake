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
    float glow;        // > 0: bloom halo around emissive surfaces (fire, lamp globes)
    float glowPad0;
    float waterStorm;  // > 0: wider, faster breakers while visible rain is active
    float glowPad2;
    float flameTime;   // seconds
    float flameCount;  // boxes in flameBoxes
    float waterTime;   // game seconds, frozen with the simulation
    float waterEnabled;
    vec4 flameBoxes[16]; // burning polygons' screen boxes in frame uv (x0, y0, x1, y1)
    vec4 waterImpacts[8]; // screen x/y, start time, strength
};

// --- Flames ----------------------------------------------------------------------
float FlameHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float FlameNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(FlameHash(i), FlameHash(i + vec2(1.0, 0.0)), u.x),
               mix(FlameHash(i + vec2(0.0, 1.0)), FlameHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float FlameFbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int k = 0; k < 4; k++) {
        sum += amp * FlameNoise(p);
        p = p * 2.07 + vec2(3.1, 7.7);
        amp *= 0.5;
    }
    return sum;
}

/* A soft volumetric flame rising from each burning box: taller than the
   polygons, licking tongues, transparent edges. Premultiplied, added to the frame. */
vec3 Flames(vec2 uv) {
    vec3 sum = vec3(0.0);
    for (int k = 0; k < int(flameCount); k++) {
        vec4 b = flameBoxes[k];
        float w = max(b.z - b.x, 0.004) * 1.35;
        float h = clamp((b.w - b.y) * 2.0, w * 1.6, w * 3.0);
        float cx = (b.x + b.z) * 0.5;
        float base = b.w + (b.w - b.y) * 0.05;
        float u = (uv.x - cx) / (w * 0.5);
        float v = (base - uv.y) / h; // 0 at the roots, 1 at the tips
        if (v < -0.08 || v > 1.0 || abs(u) > 1.6) {
            continue;
        }
        float seed = float(k) * 17.3;
        float t = flameTime;
        /* Sway and turbulence grow with height. */
        float sway = (FlameFbm(vec2(v * 2.0 - t * 1.3, seed)) - 0.5) * 0.9 * v;
        float uu = u + sway;
        float width = pow(clamp(1.0 - v, 0.0, 1.0), 0.55) * (0.85 + 0.15 * sin(t * 7.0 + seed));
        float body = 1.0 - smoothstep(width * 0.35, width, abs(uu));
        float n = FlameFbm(vec2(uu * 2.5, v * 3.5 - t * 3.2) + seed);
        float heat = body * (1.15 - v * 1.05) + (n - 0.5) * 0.9 * body;
        heat *= smoothstep(-0.08, 0.06, v);
        heat = clamp(heat, 0.0, 1.0);
        vec3 c = mix(vec3(0.5, 0.04, 0.0), vec3(1.0, 0.3, 0.02), smoothstep(0.05, 0.35, heat));
        c = mix(c, vec3(1.0, 0.65, 0.12), smoothstep(0.3, 0.6, heat));
        c = mix(c, vec3(1.0, 0.85, 0.42), smoothstep(0.6, 0.9, heat));
        float alpha = smoothstep(0.04, 0.45, heat);
        sum += c * alpha * 0.9;
    }
    return sum;
}

/* Adds light that approaches white instead of clipping at it. */
vec3 Screen(vec3 base, vec3 add) {
    base = clamp(base, 0.0, 1.0);
    return base + add * (1.0 - base) / (1.0 + add * 0.25);
}

/* Soft halo from the emissive surfaces around this pixel: two rings of taps on
   the GPU targets, each weighting the surface's colour by its emissive mask. */
vec3 Glow(vec2 uv) {
    ivec2 size = textureSize(u_objLight, 0);
    vec2 centre = uv * vec2(size);
    float scale = float(size.y) / 540.0;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    const int TAPS = 12;
    for (int ring = 1; ring <= 3; ring++) {
        float radius = scale * (float(ring) * float(ring) * 2.2);
        float weight = 1.0 / float(ring);
        for (int k = 0; k < TAPS; k++) {
            float a = (float(k) + float(ring) * 0.5) * 6.2831853 / float(TAPS);
            ivec2 q = clamp(ivec2(centre + vec2(cos(a), sin(a)) * radius), ivec2(0), size - ivec2(1));
            float e = texelFetch(u_objLight, q, 0).a;
            if (e > 0.02) {
                sum += texelFetch(u_objColor, q, 0).rgb * e * weight;
            }
            total += weight;
        }
    }
    float e0 = texelFetch(u_objLight, clamp(ivec2(centre), ivec2(0), size - ivec2(1)), 0).a;
    if (e0 > 0.02) {
        sum += texelFetch(u_objColor, clamp(ivec2(centre), ivec2(0), size - ivec2(1)), 0).rgb * e0 * 2.0;
    }
    total += 2.0;
    return sum / total * 1.6;
}

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

bool WaterMarker(float alpha) {
    return alpha > 0.002 && alpha < 0.006;
}

bool SkyMarker(float alpha) {
    return alpha >= 0.006 && alpha < 0.012;
}

/* Distance to the actual visible water boundary. Sky markers exclude the
   horizon; terrain, beaches and 3D pier geometry produce the breaking edge. */
vec2 ShoreFoam(ivec2 p) {
    if (waterEnabled < 0.5) {
        return vec2(0.0);
    }
    ivec2 size = textureSize(u_objLight, 0);
    float current = texelFetch(u_objLight, p, 0).a;
    bool onWater = WaterMarker(current);
    if (SkyMarker(current)) {
        return vec2(0.0);
    }
    float nearest = 30.0;
    for (int ring = 1; ring <= 5; ring++) {
        float radius = float(ring * ring);
        int r = ring * ring;
        for (int k = 0; k < 4; k++) {
            ivec2 offset = k == 0 ? ivec2(r, 0) :
                           k == 1 ? ivec2(-r, 0) :
                           k == 2 ? ivec2(0, r) : ivec2(0, -r);
            ivec2 q = clamp(p + offset, ivec2(0), size - ivec2(1));
            float marker = texelFetch(u_objLight, q, 0).a;
            bool boundary = onWater ? (!WaterMarker(marker) && !SkyMarker(marker))
                                    : WaterMarker(marker);
            if (boundary) {
                nearest = min(nearest, radius);
            }
        }
    }
    float reach = mix(13.0, 26.0, waterStorm);
    if (nearest > reach) {
        return vec2(0.0);
    }
    float envelope = 1.0 - smoothstep(1.0, reach, nearest);
    float signedDistance = onWater ? nearest : -nearest;
    float phase = signedDistance * (1.08 + waterStorm * 0.18)
                - waterTime * (4.2 + waterStorm * 3.6)
                + float(p.x + p.y) * 0.035;
    float breaker = smoothstep(0.28, 0.86, 0.5 + 0.5 * sin(phase));
    float backwash = smoothstep(0.58, 0.92, 0.5 + 0.5 * sin(phase * 0.47 + 1.7));
    float foam = envelope * (0.22 + breaker * 0.68 + backwash * 0.22 * waterStorm);
    return onWater ? vec2(foam, 0.0) : vec2(0.0, foam);
}

float WaterImpactAge(float start) {
    return mod(waterTime - start + 256.0, 256.0);
}

/* Short crown and ballistic droplets above a contact. The expanding rings
   themselves live in WATER.glsl and therefore follow the water surface. */
vec3 WaterSpray(vec2 uv) {
    if (waterEnabled < 0.5) {
        return vec3(0.0);
    }
    ivec2 size = textureSize(u_objColor, 0);
    vec2 pixel = uv * vec2(size);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 8; k++) {
        vec4 impact = waterImpacts[k];
        if (impact.w <= 0.0) {
            continue;
        }
        float age = WaterImpactAge(impact.z);
        if (age >= 1.75) {
            continue;
        }
        float scale = (6.0 + impact.w * 6.5) * float(size.y) / 540.0;
        vec2 base = impact.xy * vec2(size);
        vec2 local = pixel - base;
        if (abs(local.x) > scale * 8.0 || local.y < -scale * 8.0 || local.y > scale * 3.5) {
            continue;
        }
        ivec2 bp = clamp(ivec2(base), ivec2(0), size - ivec2(1));
        vec3 waterColor = texelFetch(u_objColor, bp, 0).rgb;
        float waterCrest = max(max(waterColor.r, waterColor.g), waterColor.b);
        vec3 sprayColor = min(waterColor * 1.25 + vec3(waterCrest) * 0.08, vec3(0.9));
        float crownRadius = age * scale * 5.0;
        float crown = exp(-pow((length(vec2(local.x, local.y * 2.2)) - crownRadius) / max(scale * 0.55, 1.0), 2.0));
        crown *= 1.0 - smoothstep(0.25, 0.8, age);
        float churnRadius = max(scale * (0.72 + age * 3.6), 1.0);
        float churn = 1.0 - smoothstep(0.48, 1.0,
                                     length(vec2(local.x, local.y * 2.6)) / churnRadius);
        churn *= (1.0 - smoothstep(0.68, 1.5, age)) * smoothstep(0.35, 0.9, impact.w);
        float plumeAge = min(age, 0.72);
        vec2 plumeCentre = vec2(0.0, -scale * (0.7 + plumeAge * (4.2 + impact.w)));
        vec2 plumeShape = vec2(max(scale * (0.72 - plumeAge * 0.35), 1.0),
                               max(scale * (1.15 + plumeAge * 1.4), 1.0));
        float plume = 1.0 - smoothstep(0.55, 1.0, length((local - plumeCentre) / plumeShape));
        plume *= (1.0 - smoothstep(0.32, 0.82, age)) * smoothstep(0.7, 1.4, impact.w);
        float drops = 0.0;
        for (int j = 0; j < 12; j++) {
            float seed = fract(sin(float(k * 17 + j * 31) * 12.9898) * 43758.5453);
            float vx = (seed * 2.0 - 1.0) * (2.6 + impact.w * 1.25);
            float vy = 3.5 + fract(seed * 7.13) * (2.8 + impact.w * 1.15);
            vec2 centre = vec2(vx * age, -vy * age + 4.15 * age * age) * scale;
            float radius = max(1.0, scale * (0.13 + seed * 0.08));
            drops += 1.0 - smoothstep(radius * 0.35, radius, length(local - centre));
        }
        float fade = 1.0 - smoothstep(0.9, 1.75, age);
        float spray = clamp(churn * 0.42 + crown * 0.58 + plume * 0.5
                            + drops * fade * 0.65, 0.0, 0.85);
        sum += sprayColor * spray * 0.55;
    }
    return sum;
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
    vec3 halo = glow > 0.0 ? Glow(v_uv) : vec3(0.0);
    if (flameCount > 0.5) {
        halo += Flames(v_uv);
    }
    halo += WaterSpray(v_uv);
    if (!match) {
        o_color = vec4(Screen(SoftwarePixel(v_uv), halo), 1.0) * v_color;
        return;
    }

    vec4 nearest = texelFetch(u_objColor, p, 0);
    vec4 lightTexel = texelFetch(u_objLight, p, 0);
    /* An emissive surface is the light source: it is not lit again by itself. */
    float emissive = lightTexel.a > 0.02 ? lightTexel.a : 0.0;
    vec3 light = lightTexel.rgb * 2.0 * (1.0 - emissive);
    if (nearest.a < 0.5) {
        /* A surface whose colour is the software frame (interior bricks). */
        vec3 c = SoftwarePixel(v_uv);
        o_color = vec4(Screen(c * (vec3(1.0) + light), halo), 1.0) * v_color;
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
    vec2 shore = ShoreFoam(p);
    float crest = max(max(gpu.r, gpu.g), gpu.b);
    vec3 foamColor = min(gpu * 1.45 + vec3(crest) * 0.22, vec3(1.0));
    float waterFoam = shore.x * mix(0.72, 0.94, waterStorm);
    float landWash = shore.y * mix(0.24, 0.78, waterStorm);
    gpu = mix(gpu, foamColor, clamp(waterFoam + landWash, 0.0, 0.96));
    gpu = Screen(gpu, halo);
    gpu = mix(gpu, vec3(0.0, 1.0, 0.0), min(debugTint, 1.0) * 0.5);
    o_color = vec4(gpu, frame.a);
}
