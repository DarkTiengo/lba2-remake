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
layout(set = 2, binding = 5) uniform sampler2D u_objShadow; // soft shadow darkness
layout(set = 2, binding = 6) uniform sampler2D u_objHeight; // scene-space surface height
/* Ray-traced sun and light shadows and ambient occlusion at half resolution
   (the HALF_PASS build of this shader writes it): r sun, g lights, b occlusion. */
layout(set = 2, binding = 7) uniform sampler2D u_rtHalf;

/* The frame's bodies for sun rays (GPUBVH.H): nodes are two vec4 (min, first;
   max, leaf count), triangles three (a vertex, two edges). */
layout(std430, set = 2, binding = 8) readonly buffer BvhNodes {
    vec4 bvhNodes[];
};
layout(std430, set = 2, binding = 9) readonly buffer BvhTris {
    vec4 bvhTris[];
};

layout(set = 3, binding = 0) uniform Params {
    float debugTint;   // > 0: tint GPU pixels green (LBA2_GPU_DEBUG)
    float supersample; // > 0: the GPU targets are larger than the screen area
    float pixelFilter; // > 0: upscaled software pixels go through xBR
    float deband;      // > 0: upscaled software pixels are debanded
    float glow;        // > 0: bloom halo around emissive surfaces (fire, lamp globes)
    float sunlight;    // > 0: global light grading, sun bloom, ambient occlusion
    float waterStorm;  // > 0: wider, faster breakers while visible rain is active
    float halfReady;   // > 0: u_rtHalf holds this present's shadows and occlusion
    float flameTime;   // seconds
    float flameCount;  // boxes in flameBoxes
    float waterTime;   // game seconds, frozen with the simulation
    float waterEnabled;
    vec4 flameBoxes[16]; // burning polygons' screen boxes in frame uv (x0, y0, x1, y1)
    vec4 waterImpacts[8]; // screen x/y, start time, strength
    vec4 sunTint;  // lit surfaces lean to the sun's colour; w: grading strength
    vec4 skyTint;  // shaded ones to the sky's; w: sun bloom strength
    vec4 aoParams; // x strength, y pixels per unit (3D: at distance 1), z world radius, w 1 in 3D
    vec4 rtSun;    // toward the sun, w: ray-traced shadow strength (0: off)
    vec4 rtProj;   // XCentre, YCentre, FRatioX, FRatioY: pixel and distance back to a scene point
    vec4 rtInfo;   // target pixels per frame pixel (x, y), ray start, ray length
    vec4 rtLightCfg;       // x lights to trace toward, y clearance kept around each
    vec4 rtLightPos[8];    // scene position, radius
    vec4 rtLightWeight[2]; // intensity times luma, four a vec4
    vec4 stormRain;  // x rain falls in view, y seconds, z density, w flash brightness
    vec4 stormBolt;  // x bolt and y horizon in frame uv, z seed, w bolt visibility
    vec4 stormLight; // xyz toward the flash in scene space, w exterior
    vec4 stormUp;    // xyz world up, w seconds since the strike
    vec4 stormView;  // the scene's window in frame uv (x0, y0, x1, y1): cinema bars outside
    vec4 skyFog;     // rgb the fog colour at the horizon, w: 1 the GPU draws the sky, 2 above the clouds
    vec4 skyUp;      // xyz world up in scene space, w daylight
    vec4 skyX;       // xyz the world's X axis, w cloud cover
    vec4 skyZ;       // xyz the world's Z axis, w storm
    vec4 skySun;     // xyz toward the sun, w seconds
    vec4 skyFogRange; // x view depth where the fog starts, y where it is total
    vec4 skyCamera;  // x height above the cloud ceiling, yz world X and Z, w 1 space, 2 heavy gas
    vec4 skyPlanet;  // toward the planet seen from space (world frame), w its angular radius
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
            } else if (skyTint.w > 0.0) {
                /* Sunlit highlights bloom softly too. */
                vec3 c = texelFetch(u_objColor, q, 0).rgb;
                float bright = smoothstep(0.84, 0.98, dot(c, vec3(0.3, 0.59, 0.11)));
                sum += c * bright * skyTint.w * weight;
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

// --- Global light ------------------------------------------------------------------
float UnpackDistance(ivec2 q) {
    vec2 ba = texelFetch(u_objShadow, q, 0).ba;
    float t = ba.x + ba.y / 255.0;
    return t > 0.0 ? t * 131072.0 : -1.0;
}

/* Ambient occlusion from the view distance: each pair of opposite neighbours
   predicts where a flat surface through them would put this pixel (harmonic in
   perspective, where 1/distance is linear on screen); lying behind that plane
   means a crease, a corner, the foot of a wall. Within one draw only sharp
   corners count: the low-poly terrain's gentle folds are smooth in the art and
   must stay so. Returns the light kept. */
float AmbientOcclusion(ivec2 p) {
    float d = UnpackDistance(p);
    if (d <= 0.0 || aoParams.x <= 0.0) {
        return 1.0;
    }
    ivec2 size = textureSize(u_objShadow, 0);
    float radius = aoParams.z;
    float px = aoParams.w > 0.5 ? aoParams.y * radius / max(d, 1.0) : aoParams.y * radius;
    px = clamp(px, 2.0, 48.0);
    float self = texelFetch(u_objId, clamp(ivec2(vec2(p) * vec2(textureSize(u_objId, 0)) / vec2(size)), ivec2(0),
                                           textureSize(u_objId, 0) - ivec2(1)), 0).r;
    float occ = 0.0;
    const int PAIRS = 6;
    for (int ring = 0; ring < 2; ring++) {
        float r = px * (ring == 0 ? 0.5 : 1.0);
        for (int k = 0; k < PAIRS; k++) {
            float a = (float(k) + 0.5 * float(ring)) * 3.14159265 / float(PAIRS);
            ivec2 o = ivec2(vec2(cos(a), sin(a)) * r);
            float da = UnpackDistance(clamp(p + o, ivec2(0), size - ivec2(1)));
            float db = UnpackDistance(clamp(p - o, ivec2(0), size - ivec2(1)));
            if (da <= 0.0 || db <= 0.0) {
                continue;
            }
            float ia = texelFetch(u_objId, clamp(p + o, ivec2(0), size - ivec2(1)), 0).r;
            float ib = texelFetch(u_objId, clamp(p - o, ivec2(0), size - ivec2(1)), 0).r;
            bool same = abs(ia - self) < 0.5 && abs(ib - self) < 0.5;
            float plane = aoParams.w > 0.5 ? 2.0 / (1.0 / da + 1.0 / db) : 0.5 * (da + db);
            float crease = (d - plane) / radius;
            /* A neighbour far in front or behind is another object (or a horizon
               cube drawn from a shifted camera), not this surface's corner. */
            float near = 1.0 - smoothstep(radius * 1.2, radius * 2.5, max(abs(d - da), abs(d - db)));
            occ += (same ? smoothstep(0.4, 1.1, crease) : clamp(crease, 0.0, 1.0)) * near;
        }
    }
    return 1.0 - aoParams.x * clamp(occ / float(PAIRS * 2) * 2.5, 0.0, 1.0);
}

/* Whether anything of the frame's bodies lies along o + t d, tMin < t < tMax. */
bool RayBlocked(vec3 o, vec3 d, float tMin, float tMax) {
    vec3 inv = 1.0 / (sign(d) * max(abs(d), vec3(1e-6)) + vec3(equal(d, vec3(0.0))) * 1e-6);
    int stack[40];
    int sp = 0;
    stack[sp++] = 0;
    for (int guard = 0; guard < 400 && sp > 0; guard++) {
        int node = stack[--sp];
        vec4 a = bvhNodes[node * 2];
        vec4 b = bvhNodes[node * 2 + 1];
        vec3 t0 = (a.xyz - o) * inv;
        vec3 t1 = (b.xyz - o) * inv;
        vec3 lo = min(t0, t1);
        vec3 hi = max(t0, t1);
        float enter = max(max(lo.x, lo.y), max(lo.z, tMin));
        float leave = min(min(hi.x, hi.y), min(hi.z, tMax));
        if (enter > leave) {
            continue;
        }
        int first = int(a.w);
        int count = int(b.w);
        if (count == 0) {
            if (sp < 38) {
                stack[sp++] = first;
                stack[sp++] = first + 1;
            }
            continue;
        }
        for (int i = first; i < first + count; i++) {
            vec3 v0 = bvhTris[i * 3].xyz;
            vec3 e1 = bvhTris[i * 3 + 1].xyz;
            vec3 e2 = bvhTris[i * 3 + 2].xyz;
            vec3 pv = cross(d, e2);
            float det = dot(e1, pv);
            if (abs(det) < 1e-6) {
                continue;
            }
            float id = 1.0 / det;
            vec3 s = o - v0;
            float u = dot(s, pv) * id;
            if (u < 0.0 || u > 1.0) {
                continue;
            }
            vec3 q = cross(s, e1);
            float v = dot(d, q) * id;
            if (v < 0.0 || u + v > 1.0) {
                continue;
            }
            float t = dot(e2, q) * id;
            if (t > tMin && t < tMax) {
                return true;
            }
        }
    }
    return false;
}

/* The scene point under target pixel q at view distance d. */
vec3 ScenePoint(ivec2 q, float d) {
    vec2 frame = (vec2(q) + 0.5) / rtInfo.xy;
    return vec3((frame.x - rtProj.x) * d / rtProj.z, (frame.y - rtProj.y) * d / (rtProj.z * rtProj.w), -d);
}

/* The surface under pixel p: its scene point and a normal from the neighbours
   on the same surface (the nearer in distance on each axis), facing the camera. */
bool SurfaceAt(ivec2 p, out vec3 pos, out vec3 n) {
    float d = UnpackDistance(p);
    if (d <= 0.0) {
        return false;
    }
    pos = ScenePoint(p, d);
    ivec2 size = textureSize(u_objShadow, 0);
    vec3 axis[2];
    for (int a = 0; a < 2; a++) {
        ivec2 step = a == 0 ? ivec2(2, 0) : ivec2(0, 2);
        ivec2 qa = clamp(p + step, ivec2(0), size - ivec2(1));
        ivec2 qb = clamp(p - step, ivec2(0), size - ivec2(1));
        float da = UnpackDistance(qa);
        float db = UnpackDistance(qb);
        bool useA = da > 0.0 && (db <= 0.0 || abs(da - d) <= abs(db - d));
        if (!useA && db <= 0.0) {
            axis[a] = vec3(0.0);
            continue;
        }
        axis[a] = useA ? ScenePoint(qa, da) - pos : pos - ScenePoint(qb, db);
    }
    vec3 c = cross(axis[0], axis[1]);
    n = dot(c, c) > 1e-6 ? normalize(c) : normalize(-pos);
    if (dot(n, -pos) < 0.0) {
        n = -n;
    }
    return true;
}

/* Sun shadow ray traced from a surface through every body and the terrain:
   four rays over the sun's disc, turned per pixel, for a penumbra. A surface
   turned away from the sun is left to its own shading. */
float SunShadowRT(ivec2 p, vec3 pos, vec3 n) {
    if (rtSun.w <= 0.0) {
        return 0.0;
    }
    vec3 l = normalize(rtSun.xyz);
    if (dot(n, l) < 0.05) {
        return 0.0;
    }
    vec3 o = pos + n * 6.0;
    vec3 side = normalize(cross(l, abs(l.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 other = cross(l, side);
    float turn = fract(sin(dot(vec2(p), vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    float blocked = 0.0;
    for (int k = 0; k < 4; k++) {
        float a = turn + float(k) * 1.5707963;
        vec3 dir = normalize(l + (side * cos(a) + other * sin(a)) * 0.025);
        blocked += RayBlocked(o, dir, rtInfo.z, rtInfo.w) ? 1.0 : 0.0;
    }
    return rtSun.w * blocked * 0.25;
}

/* The share of the dynamic light a surface loses to what stands between it and
   each light (lamps, fires, the magic ball): one ray per light in reach, weighed
   as the shaders weigh the lights, stopping short of the light's own lamp. */
float LightShadowRT(vec3 pos, vec3 n) {
    int count = int(rtLightCfg.x);
    if (count <= 0) {
        return 0.0;
    }
    vec3 o = pos + n * 6.0;
    float total = 0.0;
    float blocked = 0.0;
    for (int k = 0; k < count; k++) {
        vec3 toLight = rtLightPos[k].xyz - o;
        float dist2 = dot(toLight, toLight);
        float range = rtLightPos[k].w;
        float f = clamp(1.0 - dist2 / (range * range), 0.0, 1.0);
        float w = f * f * rtLightWeight[k / 4][k % 4];
        if (w <= 0.0) {
            continue;
        }
        total += w;
        float dist = sqrt(dist2);
        vec3 dir = toLight / max(dist, 1.0);
        if (dot(n, dir) <= 0.0) {
            continue;
        }
        if (dist > rtLightCfg.y + rtInfo.z && RayBlocked(o, dir, rtInfo.z, dist - rtLightCfg.y)) {
            blocked += w;
        }
    }
    return total > 0.0 ? blocked / total : 0.0;
}

/* Lit surfaces lean toward the sun's colour, shaded ones toward the sky's. */
vec3 Grade(vec3 c) {
    float luma = dot(c, vec3(0.3, 0.59, 0.11));
    vec3 tone = mix(skyTint.rgb, sunTint.rgb, smoothstep(0.12, 0.7, luma));
    return mix(c, c * tone, sunTint.w);
}

/* The shadow target softened: silhouettes are drawn sharp, a penumbra is not. */
vec2 SoftShadow(vec2 uv) {
    vec2 texel = 1.0 / vec2(textureSize(u_objShadow, 0));
    float r = float(textureSize(u_objShadow, 0).y) / 540.0 * 1.6;
    vec2 sum = textureLod(u_objShadow, uv, 0.0).rg * 2.0;
    for (int k = 0; k < 8; k++) {
        float a = float(k) * 0.785398;
        sum += textureLod(u_objShadow, uv + vec2(cos(a), sin(a)) * r * texel, 0.0).rg;
    }
    return sum / 10.0;
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

const float CONTACT_HEIGHT_TOLERANCE = 96.0;
const float CONTACT_DEPTH_TOLERANCE = 4096.0;
const float SHORE_SEARCH_RADIUS = 100.0;

/* Water and a shoreline fragment must be the same visible surface scale. A
   height-only test lets a quay or a foreground body borrow a distant sea's
   edge; the packed view distance closes that projection ambiguity. */
bool WaterContactPair(ivec2 a, ivec2 b) {
    float ah = texelFetch(u_objHeight, a, 0).r;
    float bh = texelFetch(u_objHeight, b, 0).r;
    if (abs(ah - bh) > CONTACT_HEIGHT_TOLERANCE) {
        return false;
    }
    if (aoParams.w > 0.5) {
        float ad = UnpackDistance(a);
        float bd = UnpackDistance(b);
        if (ad <= 0.0 || bd <= 0.0 || abs(ad - bd) > CONTACT_DEPTH_TOLERANCE) {
            return false;
        }
    }
    return true;
}

/* Convert the short screen search into a scene-space shoreline distance. The
   same conversion is used for every resolution and camera distance, so the
   breaker does not become a fixed-pixel neon rim when the view changes. */
float ShoreSceneDistance(float pixels, float depth, float edgeDepth) {
    float d = max(1.0, 0.5 * (depth + edgeDepth));
    if (aoParams.w > 0.5) {
        return pixels * d / max(aoParams.y, 1.0);
    }
    return pixels / max(aoParams.y, 1.0);
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
    float surfaceHeight = texelFetch(u_objHeight, p, 0).r;
    /* A projected body can sit beside the sea while its feet are well above
       the surface. Require the fragment and the nearby water edge to share a
       world-up height before allowing foam on either side of the boundary. */
    float surfaceDepth = UnpackDistance(p);
    float nearest = SHORE_SEARCH_RADIUS + 1.0;
    float nearestDepth = -1.0;
    float nearestEdgeHeight = surfaceHeight;
    for (int ring = 1; ring <= 10; ring++) {
        float radius = float(ring * ring);
        int r = ring * ring;
        for (int k = 0; k < 4; k++) {
            ivec2 offset = k == 0 ? ivec2(r, 0) :
                           k == 1 ? ivec2(-r, 0) :
                           k == 2 ? ivec2(0, r) : ivec2(0, -r);
            if (radius > nearest) {
                continue;
            }
            ivec2 q = clamp(p + offset, ivec2(0), size - ivec2(1));
            float marker = texelFetch(u_objLight, q, 0).a;
            bool boundary = onWater ? (!WaterMarker(marker) && !SkyMarker(marker))
                                    : WaterMarker(marker);
            if (boundary) {
                /* The squared search radii are deliberately sparse. Refine
                   the marker transition so a narrow quay lip is tested
                   instead of jumping directly onto its taller wall. */
                ivec2 lo = p;
                ivec2 hi = q;
                for (int refine = 0; refine < 5; refine++) {
                    ivec2 mid = (lo + hi) / 2;
                    float midMarker = texelFetch(u_objLight, mid, 0).a;
                    bool midBoundary = onWater ? (!WaterMarker(midMarker) && !SkyMarker(midMarker))
                                               : WaterMarker(midMarker);
                    if (midBoundary) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
                q = hi;
                boundary = WaterContactPair(p, q);
            }
            if (boundary) {
                float refined = length(vec2(q - p));
                if (refined < nearest) {
                    nearest = refined;
                    nearestDepth = UnpackDistance(q);
                    nearestEdgeHeight = texelFetch(u_objHeight, q, 0).r;
                }
            }
        }
    }
    if (nearest > SHORE_SEARCH_RADIUS || nearestDepth <= 0.0 || surfaceDepth <= 0.0) {
        return vec2(0.0);
    }
    float sceneDistance = ShoreSceneDistance(nearest, surfaceDepth, nearestDepth);
    float storm = clamp(waterStorm, 0.0, 1.0);
    float crestWidth = 220.0 + storm * 110.0;
    float washWidth = 620.0 + storm * 420.0;
    /* A travelling crest reaches the edge, recedes, then leaves a weaker
       backwash. Its phase is in scene units, not framebuffer pixels. */
    float phase = sceneDistance * (0.014 - storm * 0.002)
                - waterTime * (1.25 + storm * 0.45);
    float pulse = 0.5 + 0.5 * sin(phase);
    float crest = exp(-pow(sceneDistance / crestWidth, 2.0));
    /* Even between breaker crests the wet edge catches a restrained highlight;
       otherwise a deterministic capture can make a real quay contact vanish. */
    crest *= 0.30 + 0.70 * smoothstep(0.28, 0.78, pulse);
    float backwash = exp(-sceneDistance / washWidth) *
                     smoothstep(0.38, 0.82, 0.5 + 0.5 * sin(phase * 0.53 + 1.4));
    if (onWater) {
        /* Whitewater is strongest on the water side and falls off quickly. */
        return vec2(crest * (0.34 + 0.52 * pulse) + backwash * (0.06 + 0.06 * storm), 0.0);
    }
    /* Land wash is restricted to a shallow rise above the water plane. */
    float rise = surfaceHeight - nearestEdgeHeight;
    float shallow = 1.0 - smoothstep(12.0, CONTACT_HEIGHT_TOLERANCE, rise);
    float wash = shallow * exp(-sceneDistance / washWidth) *
                 (0.18 + 0.20 * backwash + 0.06 * pulse + 0.04 * storm);
    return vec2(0.0, wash);
}

float WaterImpactAge(float start) {
    return mod(waterTime - start + 256.0, 256.0);
}

/* An impact is allowed to cross from water onto a quay only when its projected
   base is visible water and the receiving pixel is the same shallow surface.
   This keeps a foreground hero from receiving a screen-space spray halo. */
bool WaterImpactContact(ivec2 p, ivec2 base) {
    float baseMarker = texelFetch(u_objLight, base, 0).a;
    if (!WaterMarker(baseMarker)) {
        return false;
    }
    float marker = texelFetch(u_objLight, p, 0).a;
    if (SkyMarker(marker)) {
        return false;
    }
    if (WaterMarker(marker)) {
        return true;
    }
    return WaterContactPair(p, base);
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
        if (!WaterImpactContact(clamp(ivec2(pixel), ivec2(0), size - ivec2(1)), bp)) {
            continue;
        }
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
        sum += sprayColor * spray * 0.34;
    }
    return sum;
}

// -----------------------------------------------------------------------------
// --- Storm ---------------------------------------------------------------------
float StormHash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

/* Rain streaks over target pixel q: three layers at increasing depth, slanted
   by the wind and blurred along their fall, each hidden behind a nearer surface
   (d, the surface's view distance, <= 0 for the sky). s scales to 1080 lines. */
float RainStreaks(vec2 q, float d, float s, float t) {
    float sum = 0.0;
    for (int i = 0; i < 3; i++) {
        float depth = i == 0 ? 700.0 : (i == 1 ? 2200.0 : 6000.0);
        if (d > 0.0 && d < depth) {
            continue;
        }
        float cell = (i == 0 ? 34.0 : (i == 1 ? 20.0 : 12.0)) * s;
        float speed = (i == 0 ? 2600.0 : (i == 1 ? 1800.0 : 1200.0)) * s;
        float len = (i == 0 ? 120.0 : (i == 1 ? 66.0 : 36.0)) * s;
        float width = (i == 0 ? 1.4 : (i == 1 ? 1.0 : 0.8)) * s;
        float period = (i == 0 ? 560.0 : (i == 1 ? 380.0 : 250.0)) * s;
        float bright = i == 0 ? 0.42 : (i == 1 ? 0.3 : 0.2);
        float x = q.x - q.y * 0.14;
        float col = floor(x / cell);
        float r1 = StormHash(vec2(col, float(i) * 7.1));
        float r2 = StormHash(vec2(col * 1.7, float(i) * 3.3 + 1.0));
        if (StormHash(vec2(col * 0.7, float(i) * 5.9 + 2.0)) > 0.9) {
            continue;
        }
        float xc = (col + 0.15 + 0.7 * r2) * cell;
        float across = 1.0 - smoothstep(width * 0.5, width * 1.5, abs(x - xc));
        if (across <= 0.0) {
            continue;
        }
        float head = mod(t * speed * (0.85 + 0.3 * r1) + r1 * period, period);
        float v = mod(head - q.y, period);
        float along = v < len ? 1.0 - v / len : 0.0;
        sum += bright * across * along * along;
    }
    return sum;
}

/* Raindrops hitting ground turned up: rings spreading on a grid of the ground
   plane (scene units), two drops a cell at their own pace. */
float RainSplash(vec3 pos, vec3 n, float t) {
    vec3 up = normalize(stormUp.xyz);
    if (dot(n, up) < 0.75) {
        return 0.0;
    }
    vec3 a = normalize(cross(up, abs(up.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(up, a);
    vec2 g = vec2(dot(pos, a), dot(pos, b));
    const float CELL = 300.0;
    vec2 cell = floor(g / CELL);
    float sum = 0.0;
    for (int k = 0; k < 2; k++) {
        vec2 id = cell + vec2(float(k) * 37.0, float(k) * 11.0);
        float r1 = StormHash(id);
        float r2 = StormHash(id + 11.3);
        float r3 = StormHash(id + 23.7);
        vec2 c = (cell + 0.3 + 0.4 * vec2(r1, r2)) * CELL;
        float period = 0.6 + 0.6 * r3;
        float age = fract(t / period + r1 * 5.0);
        float dist = length(g - c);
        float radius = 8.0 + age * 80.0;
        float fade = (1.0 - age) * (1.0 - age);
        sum += exp(-abs(dist - radius) / 6.0) * fade * 0.55;
        if (age < 0.12) {
            sum += exp(-dist / 9.0) * (1.0 - age / 0.12) * 0.7;
        }
    }
    return sum;
}

float SegmentDist2D(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
    return length(pa - ba * h);
}

/* Distance from target pixel q to the bolt's main channel (x) and its branches
   (y): a jagged line from the top of the frame down to the horizon. */
vec2 Bolt(vec2 q, vec2 size) {
    float seed = stormBolt.z;
    /* Down past the horizon: the terrain hides what is below it. */
    float bottom = size.y * 0.95;
    const int N = 14;
    float step = bottom / float(N);
    vec2 pts[N + 1];
    pts[0] = vec2(stormBolt.x * size.x, -step * 0.5);
    for (int k = 1; k <= N; k++) {
        float jit = (StormHash(vec2(seed, float(k))) - 0.5) * 1.1 * step;
        pts[k] = vec2(pts[k - 1].x + jit, float(k) * step);
    }
    float d = 1e9;
    for (int k = 0; k < N; k++) {
        d = min(d, SegmentDist2D(q, pts[k], pts[k + 1]));
    }
    float db = 1e9;
    for (int br = 0; br < 3; br++) {
        int k0 = 2 + int(StormHash(vec2(seed + 5.0, float(br))) * 9.0);
        float dir = StormHash(vec2(seed + 9.0, float(br))) < 0.5 ? -1.0 : 1.0;
        vec2 a = pts[k0];
        for (int j = 0; j < 4; j++) {
            float r = StormHash(vec2(seed + float(br) * 13.0, float(j)));
            vec2 b = a + vec2(dir * (0.3 + 0.7 * r) * step, step * (0.6 + 0.3 * r));
            db = min(db, SegmentDist2D(q, a, b));
            a = b;
        }
    }
    return vec2(d, db);
}

/* The storm over a finished colour: the flash lighting the scene from the
   strike (with shadows when rays are traced), the lit clouds and the bolt in
   the sky, the rain streaks and the splashes. lit: the surface takes light;
   scene: the pixel shows the GPU's surface (not the frame's letterbox or HUD
   over it), so rain falls on it. */
vec3 Storm(vec3 c, ivec2 p, bool lit, bool scene) {
    float flash = stormRain.w;
    bool raining = stormRain.x > 0.5;
    if (!raining && flash < 0.001 && stormBolt.w <= 0.0) {
        return c;
    }
    vec2 size = vec2(textureSize(u_objShadow, 0));
    float s = size.y / 1080.0;
    vec2 q = vec2(p) + 0.5;
    float d = UnpackDistance(p);
    bool exterior = stormLight.w > 0.5;
    vec3 pos = vec3(0.0);
    vec3 n = vec3(0.0, 0.0, 1.0);
    bool surf = exterior && d > 0.0 && rtProj.z > 0.0 && SurfaceAt(p, pos, n);
    bool waterSurface = WaterMarker(texelFetch(u_objLight, p, 0).a);
    vec3 tint = vec3(0.78, 0.86, 1.0);

    if (flash > 0.001 && lit && d > 0.0) {
        /* A room lit through its windows takes less. */
        float gain = exterior ? 0.7 : 0.45;
        if (surf) {
            vec3 l = normalize(stormLight.xyz);
            float direct = max(dot(n, l), 0.0);
            if (direct > 0.0 && rtInfo.w > 0.0 && RayBlocked(pos + n * 6.0, l, rtInfo.z, rtInfo.w)) {
                direct = 0.0;
            }
            gain = 0.3 + 1.3 * direct;
        }
        c *= vec3(1.0) + tint * flash * gain;
    }
    /* The sky, or land far enough for the strike to stand in front of it. */
    if (exterior && (d <= 0.0 || d > 45000.0) && scene) {
        /* Clouds lit around the strike, then the bolt itself. */
        vec2 top = vec2(stormBolt.x * size.x, 0.0);
        float cloud = 0.2 + 0.7 * exp(-length(q - top) / (0.35 * size.x));
        c = Screen(c, vec3(0.6, 0.68, 0.88) * flash * cloud);
        if (stormBolt.w > 0.0) {
            vec2 bd = Bolt(q, size);
            float core = exp(-bd.x / (2.0 * s)) + 0.7 * exp(-bd.y / (1.3 * s));
            float halo = 0.9 * exp(-bd.x / (26.0 * s)) + 0.4 * exp(-bd.y / (14.0 * s));
            float vis = stormBolt.w * clamp(flash * 1.4, 0.25, 1.0);
            c = Screen(c, (vec3(1.0) * min(core, 1.0) + vec3(0.55, 0.65, 1.0) * halo) * vis);
        }
    }
    if (raining && scene) {
        float streak = RainStreaks(q, d, s, stormRain.y) * stormRain.z;
        vec3 drop = vec3(0.62, 0.68, 0.78) * (1.0 + 2.5 * flash);
        c = Screen(c, drop * streak);
        /* Sea pixels have their world-anchored ripple material in WATER.glsl;
           the screen-space ground splash grid is for dry upward surfaces only. */
        if (surf && !waterSurface && d < 12000.0) {
            float splash = RainSplash(pos, n, stormRain.y) * (1.0 - smoothstep(5000.0, 12000.0, d));
            c = Screen(c, vec3(0.7, 0.76, 0.86) * splash * 0.45 * (1.0 + 2.0 * flash));
        }
    }
    return c;
}

// --- Sky -----------------------------------------------------------------------
/* A hash that stays even for large cell numbers (the sky's far cells). */
float SkyHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float SkyNoise(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);
    /* Quintic: its slope is continuous too, so the shading of the clouds
       (a difference of two samples) shows no creases along the cells. */
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = SkyHash(i);
    float b = SkyHash(i + vec2(1.0, 0.0));
    float c = SkyHash(i + vec2(0.0, 1.0));
    float d = SkyHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float SkyFbm(vec2 x) {
    float v = 0.0;
    float a = 0.5;
    const mat2 turn = mat2(1.6, 1.2, -1.2, 1.6);
    for (int i = 0; i < 5; i++) {
        v += a * SkyNoise(x);
        x = turn * x;
        a *= 0.5;
    }
    return v;
}

/* The view ray through target pixel p, in the world's frame (y up). */
vec3 SkyRay(ivec2 p) {
    vec2 frame = (vec2(p) + 0.5) / rtInfo.xy;
    vec3 v = normalize(vec3((frame.x - rtProj.x) / rtProj.z, (frame.y - rtProj.y) / (rtProj.z * rtProj.w), -1.0));
    return vec3(dot(v, skyX.xyz), dot(v, skyUp.xyz), dot(v, skyZ.xyz));
}

/* Whether target pixel p shows the sky: the textured ceiling, or the fog-
   coloured background inside the scene's window. */
bool IsSky(ivec2 p, bool match, float tag) {
    if (skyFog.w < 0.5 || rtProj.z <= 0.0 || UnpackDistance(p) > 0.0) {
        return false;
    }
    /* Not over the cinema bars. */
    if (any(lessThan(v_uv, stormView.xy)) || any(greaterThanEqual(v_uv, stormView.zw))) {
        return false;
    }
    if (match) {
        return SkyMarker(texelFetch(u_objLight, p, 0).a);
    }
    if (texelFetch(u_objId, p, 0).r > 0.0) {
        bool ceiling = SkyMarker(texelFetch(u_objLight, p, 0).a);
        /* A GPU body the ceiling hides (a tower reaching into the clouds): the
           GPU's depth says the sky is in front. Or the software drew the same
           ceiling there itself: its pixel matches the GPU's. */
        if (ceiling) {
            if (tag > 0.0) {
                return true;
            }
            ivec2 fs = textureSize(u_frame, 0);
            vec3 soft = texelFetch(u_frame, clamp(ivec2(v_uv * vec2(fs)), ivec2(0), fs - ivec2(1)), 0).rgb;
            vec3 gpu = texelFetch(u_objColor, p, 0).rgb;
            vec3 d = abs(soft - gpu);
            if (max(max(d.r, d.g), d.b) < 0.12) {
                return true;
            }
        }
        /* Otherwise only the software's fog is sky: a sprite drawn over the
           ceiling stays. */
        if (!ceiling) {
            return false;
        }
    }
    ivec2 fs = textureSize(u_frame, 0);
    vec3 f = texelFetch(u_frame, clamp(ivec2(v_uv * vec2(fs)), ivec2(0), fs - ivec2(1)), 0).rgb;
    vec3 diff = abs(f - skyFog.rgb);
    return max(max(diff.r, diff.g), diff.b) < 0.07;
}


/* The haze on the horizon: the island's fog, a touch paler and warmer by day.
   Far land fades into it, so it stays close to the fog the art was made for. */
vec3 SkyHaze() {
    return mix(skyFog.rgb, vec3(1.0, 0.97, 0.92), 0.07 * skyUp.w) * 1.02;
}

/* The sky along the ray through p: the island's fog at the horizon deepening
   toward the zenith, the sun, clouds drifting on a dome anchored to the world
   (lit from the sun's side, dark underneath, closed in the storm), stars at
   night. The islands are flat and bounded: the background seen above the
   land lies a little below the true horizon, so the sky's horizon is lowered
   to where the land ends. Below it the sky stays the fog the land fades into,
   or, on an island above the clouds, becomes a sea of clouds. */
vec3 CloudSea(vec3 w) {
    float down = -w.y;
    vec3 fog = skyFog.rgb;
    float day = skyUp.w;
    float t = skySun.w;
    /* On the ceiling's plane under the camera, anchored to the world, so the
       clouds keep their size looking straight down and stay put as it moves. */
    float h = max(skyCamera.x, 200.0);
    float dist = h / max(down, 0.02);
    vec2 ground = w.xz * dist + skyCamera.yz;
    vec2 uv = ground / 4000.0 + vec2(t * 0.02, t * 0.007);
    float n = SkyFbm(uv * 1.3);
    float n2 = SkyFbm(uv * 1.3 + vec2(0.11, 0.06));
    /* Billows: more contrast than the dome's thin clouds. */
    n = clamp((n - 0.5) * 1.8 + 0.5, 0.0, 1.0);
    /* Billowing tops: bright where they rise toward the sun, blue in the dips. */
    float top = clamp(0.55 + (n - n2) * 2.5, 0.0, 1.0);
    vec3 lit = mix(vec3(1.0, 0.99, 0.96), fog, 0.2) * (0.35 + 0.6 * day);
    vec3 shade = mix(fog, vec3(0.55, 0.66, 0.82), 0.5) * (0.45 + 0.55 * day);
    vec3 c = mix(shade, lit, smoothstep(0.3, 0.75, n) * (0.55 + 0.45 * top));
    /* Haze toward the horizon, far across the clouds. */
    float haze = 1.0 - exp(-dist / 90000.0);
    return mix(c, SkyHaze(), max(haze, 1.0 - smoothstep(0.0, 0.12, down)));
}

/* The sky's colour alone along a ray at elevation el (the lowered horizon at
   0): haze on the horizon deepening to the zenith, continuous across the
   horizon, with no clouds, sun or stars. */
vec3 SkyGradient(float el) {
    vec3 fog = skyFog.rgb;
    float day = skyUp.w;
    vec3 zenith = fog * mix(vec3(0.5), vec3(0.62, 0.78, 1.05) * 0.88, day);
    /* A pale, warm haze on the horizon, deepening fast: little of the sky is
       seen above these islands. */
    return mix(SkyHaze(), zenith, smoothstep(0.05, 0.45, max(el, 0.0)));
}

/* A star field over direction w: one star in some cells of a grid wrapped on
   the sphere, a few bright ones, colours from blue-white to warm. */
vec3 Stars(vec3 w, float density, float twinkleTime) {
    vec2 g = vec2(atan(w.z, w.x) * 260.0, asin(clamp(w.y, -1.0, 1.0)) * 260.0);
    vec2 cell = floor(g);
    float h = SkyHash(cell);
    if (h < 1.0 - density) {
        return vec3(0.0);
    }
    vec2 at = cell + 0.5 + 0.7 * (vec2(SkyHash(cell + 3.1), SkyHash(cell + 7.7)) - 0.5);
    float r = SkyHash(cell + 11.3);
    float size = r > 0.97 ? 0.9 : 0.45;
    float twinkle = twinkleTime > 0.0 ? 0.7 + 0.3 * sin(twinkleTime * (2.0 + 4.0 * h) + h * 50.0) : 1.0;
    float star = exp(-length(g - at) / size * 2.2) * (0.35 + 0.65 * r) * twinkle;
    vec3 tint = mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.85, 0.65), SkyHash(cell + 5.9));
    return tint * star;
}

/* Space above the Emerald Moon: black, a dense star field, the Milky Way's
   band, and Twinsun hanging in the sky, lit from one side, with its blue
   atmosphere. Below the horizon the moon's fog (its dark ground) stays. */
vec3 SpaceSky(vec3 w) {
    vec3 fog = skyFog.rgb;
    float el = w.y + 0.11;
    if (el <= 0.0) {
        return fog;
    }
    vec3 c = fog * 0.4 + vec3(0.004, 0.006, 0.014);
    /* The galaxy: a tilted band of faint light and dust. */
    vec3 axis = normalize(vec3(0.35, 0.55, 0.75));
    float band = exp(-pow(dot(w, axis) / 0.2, 2.0));
    float dust = SkyFbm(vec2(atan(w.z, w.x) * 3.0, w.y * 6.0) + 7.0);
    c += vec3(0.1, 0.1, 0.14) * band * (0.4 + 0.8 * dust);
    c += Stars(w, 0.012 + 0.02 * band, 0.0) * 1.3;
    /* Twinsun: a sun over each pole warms the two hemispheres (oceans and
       islands), and between them the equator is a frozen, mountainous band. */
    vec3 planet = normalize(skyPlanet.xyz);
    float radius = skyPlanet.w;
    float cosA = dot(w, planet);
    vec3 right = normalize(cross(planet, vec3(0.0, 1.0, 0.0)));
    vec3 vup = cross(right, planet);
    vec2 q = vec2(dot(w, right), dot(w, vup)) / radius;
    /* The poles' poles, a little tilted and toward us, in (right, up, us). */
    vec3 poles = normalize(vec3(0.62, 0.72, 0.3));
    float front = step(0.0, cosA);
    float d = length(q);
    if (cosA > 0.0 && d < 1.0) {
        /* The disc as a sphere. */
        vec3 n = vec3(q, sqrt(max(1.0 - d * d, 0.0)));
        float lat = dot(n, poles);
        /* Longitude-like coordinates for the noise, turning with the planet. */
        vec3 e1 = normalize(cross(poles, vec3(0.0, 0.0, 1.0)));
        vec3 e2 = cross(poles, e1);
        vec2 geo = vec2(atan(dot(n, e2), dot(n, e1)) * 1.6 + skySun.w * 0.004, lat * 3.0);
        float land = SkyFbm(geo * 2.2 + 1.3);
        vec3 ocean = vec3(0.07, 0.24, 0.55);
        vec3 ground = mix(vec3(0.3, 0.5, 0.2), vec3(0.72, 0.6, 0.38), smoothstep(0.6, 0.72, land));
        vec3 surface = mix(ocean, ground, smoothstep(0.55, 0.6, land));
        /* The frozen equator: ice and snowy ridges, ragged at its edges. */
        float ridge = SkyFbm(geo * 5.0 + 9.0);
        float ice = 1.0 - smoothstep(0.2, 0.36, abs(lat) + (ridge - 0.5) * 0.18);
        vec3 snow = mix(vec3(0.72, 0.8, 0.9), vec3(0.97, 0.98, 1.0), ridge);
        surface = mix(surface, snow, ice);
        /* Clouds over the warm hemispheres. */
        surface = mix(surface, vec3(0.96), smoothstep(0.62, 0.8, SkyFbm(geo * 3.5 + 4.0)) * 0.7 * (1.0 - ice));
        /* Lit from both poles: the equator gets the least light. */
        vec3 s1 = normalize(poles + vec3(0.0, 0.0, 0.5));
        vec3 s2 = normalize(-poles + vec3(0.0, 0.0, 0.5));
        float lit = max(dot(n, s1), 0.0) + max(dot(n, s2), 0.0);
        vec3 disc = surface * (0.12 + 0.95 * lit) + vec3(0.3, 0.55, 1.0) * pow(1.0 - n.z, 3.0) * 0.5;
        c = mix(c, disc, smoothstep(1.0, 0.985, d));
    }
    /* Its atmosphere glowing past the limb. */
    c += vec3(0.25, 0.45, 1.0) * exp(-max(d - 1.0, 0.0) * radius / 0.012) * front * 0.35;
    return c;
}

/* The bottom of Zeelich's gas: a heavy cover closing the whole sky, rolling
   and churning slowly, lilac-grey with darker folds and paler swells, over a
   band of the island's glowing haze on the horizon. */
vec3 GasSky(vec3 w) {
    vec3 fog = skyFog.rgb;
    float t = skySun.w;
    float el = w.y + 0.11;
    if (el <= 0.0) {
        return fog;
    }
    /* On a deck overhead: compressed toward the horizon. */
    vec2 uv = w.xz / (el + 0.06) * 0.32;
    /* Domain warping twists the gas into swirls; it drifts and churns. */
    vec2 drift = vec2(t * 0.008, t * 0.003);
    vec2 warp = vec2(SkyFbm(uv * 1.3 + drift + 3.1), SkyFbm(uv * 1.3 - drift + 7.7));
    float n = SkyFbm(uv * 2.0 + warp * 1.6 + drift * 0.5);
    float fine = SkyFbm(uv * 6.0 + warp * 2.5 - drift);
    float g = clamp(n * 0.75 + (fine - 0.5) * 0.55 + 0.12, 0.0, 1.0);
    vec3 deep = mix(fog, vec3(0.3, 0.26, 0.38), 0.8);
    vec3 swell = mix(fog, vec3(0.68, 0.63, 0.76), 0.7);
    vec3 gas = mix(deep, swell, smoothstep(0.35, 0.72, g));
    /* The folds catch the haze's glow from below near the horizon. */
    gas += fog * 0.25 * (1.0 - smoothstep(0.05, 0.35, el)) * smoothstep(0.45, 0.8, g);
    /* Heavier and darker overhead. */
    gas *= mix(1.0, 0.72, smoothstep(0.15, 0.7, el));
    /* The cover's ragged underside over the band of haze. */
    float edge = 0.17 + (n - 0.5) * 0.08;
    return mix(fog, gas, smoothstep(edge, edge + 0.05, el));
}

vec3 Sky(ivec2 p) {
    vec3 w = SkyRay(p);
    if (skyCamera.w > 1.5) {
        return GasSky(w);
    }
    if (skyCamera.w > 0.5) {
        return SpaceSky(w);
    }
    bool above = skyFog.w > 1.5;
    if (above && w.y < 0.0) {
        return CloudSea(w);
    }
    float el = above ? w.y : w.y + 0.11;
    float day = skyUp.w;
    float storm = skyZ.w;
    float t = skySun.w;
    vec3 fog = skyFog.rgb;
    vec3 zenith = fog * mix(vec3(0.5), vec3(0.62, 0.78, 1.05) * 0.88, day);
    vec3 sky = SkyGradient(el);
    if (el <= 0.0) {
        return sky;
    }

    vec3 sunW = normalize(vec3(dot(skySun.xyz, skyX.xyz), dot(skySun.xyz, skyUp.xyz), dot(skySun.xyz, skyZ.xyz)));
    float mu = max(dot(normalize(w), sunW), 0.0);
    float sunUp = smoothstep(-0.05, 0.1, sunW.y) * day * (1.0 - storm);
    vec3 sunColor = vec3(1.0, 0.94, 0.82);

    /* Clouds on a dome: compressed toward the horizon, carried by the wind. */
    vec2 uv = w.xz / (el + 0.08) * 0.45 + vec2(t * 0.012, t * 0.005);
    float cover = skyX.w;
    float n = SkyFbm(uv * 2.2);
    float dens = smoothstep(1.0 - cover, 1.0 - cover + 0.3, n);
    vec2 toSun = length(sunW.xz) > 1e-3 ? normalize(sunW.xz) : vec2(1.0, 0.0);
    float n2 = SkyFbm(uv * 2.2 + toSun * 0.18);
    float shade = clamp(0.55 + (n - n2) * 2.5, 0.0, 1.0) * (1.0 - 0.6 * storm);
    vec3 lit = mix(vec3(1.0), fog, 0.25) * (0.25 + 0.85 * day);
    vec3 dark = mix(fog * 0.65, zenith * 0.7, 0.5) * (1.0 - 0.4 * storm) + vec3(0.02, 0.024, 0.032);
    vec3 cloud = mix(dark, lit, shade);
    /* Thin edges glow when the sun is behind them. */
    cloud += sunColor * pow(mu, 8.0) * dens * (1.0 - dens) * 2.0 * sunUp;

    /* Stars in the gaps at night. */
    if (day < 0.5) {
        sky += Stars(w, 0.01, t + 0.001) * (1.0 - day * 2.0) * (1.0 - storm);
    }

    /* The sun and its glow, veiled by the clouds. */
    sky += sunColor * (smoothstep(0.9994, 0.9997, mu) * 2.5 + pow(mu, 24.0) * 0.35 + pow(mu, 4.0) * 0.06) * sunUp *
           (1.0 - dens * 0.9);
    /* Clouds thin into the haze toward the horizon. */
    return mix(sky, cloud, dens * smoothstep(0.0, 0.12, el) * 0.95);
}

/* A scene surface fades into the fog with distance; with the GPU's sky it
   fades into the sky behind it instead (aerial perspective), so the land meets
   the sky without a seam. */
vec3 FogToSky(vec3 c, ivec2 p) {
    if (skyFog.w < 0.5 || rtProj.z <= 0.0 || skyFogRange.y <= skyFogRange.x) {
        return c;
    }
    float d = UnpackDistance(p);
    if (d <= 0.0) {
        return c;
    }
    float f = clamp((d - skyFogRange.x) / (skyFogRange.y - skyFogRange.x), 0.0, 1.0);
    if (f <= 0.0) {
        return c;
    }
    /* Toward the sky's colour behind it (not its clouds, which lie beyond the
       land); the gradient is continuous, so no line crosses what fades. */
    vec3 w = SkyRay(p);
    vec3 behind = skyCamera.w > 0.5 ? skyFog.rgb : SkyGradient(skyFog.w > 1.5 ? w.y : w.y + 0.11);
    /* What the fog has all but swallowed becomes the sky itself, clouds and
       all, so it meets the sky around it. */
    if (f > 0.85) {
        behind = mix(behind, Sky(p), smoothstep(0.85, 1.0, f));
    }
    return c + (behind - skyFog.rgb) * f;
}

/* The half-resolution shadows and occlusion at full-resolution pixel p: the
   four half texels around it, each weighed by how close its surface's distance
   is to this pixel's, so shadows do not bleed across depth edges. */
vec3 HalfLight(ivec2 p) {
    ivec2 hs = textureSize(u_rtHalf, 0);
    ivec2 fs = textureSize(u_objShadow, 0);
    vec2 scale = vec2(fs) / vec2(hs);
    vec2 hp = (vec2(p) + 0.5) / scale - 0.5;
    ivec2 base = ivec2(floor(hp));
    vec2 f = hp - vec2(base);
    float d = UnpackDistance(p);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 nearest = vec3(0.0, 0.0, 1.0);
    float best = 1e9;
    for (int k = 0; k < 4; k++) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 q = clamp(base + o, ivec2(0), hs - ivec2(1));
        ivec2 at = clamp(ivec2((vec2(q) + 0.5) * scale), ivec2(0), fs - ivec2(1));
        float dq = UnpackDistance(at);
        float gap = d > 0.0 && dq > 0.0 ? abs(dq - d) : (d > 0.0 || dq > 0.0 ? 1e6 : 0.0);
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        w *= exp(-gap / (d * 0.02 + 40.0));
        vec3 v = texelFetch(u_rtHalf, q, 0).rgb;
        sum += v * w;
        total += w;
        if (gap < best) {
            best = gap;
            nearest = v;
        }
    }
    return total > 1e-4 ? sum / total : nearest;
}

#ifdef HALF_PASS
/* The half-resolution pass: what the composite would trace per pixel. */
void main() {
    ivec2 objSize = textureSize(u_objId, 0);
    ivec2 p = clamp(ivec2(v_uv * vec2(objSize)), ivec2(0), objSize - ivec2(1));
    bool scenePixel = texelFetch(u_objId, p, 0).r >= 8388608.0;
    float sunRT = 0.0;
    float lightRT = 0.0;
    float ao = 1.0;
    if (scenePixel) {
        vec3 pos;
        vec3 n;
        if ((rtSun.w > 0.0 || rtLightCfg.x > 0.5) && SurfaceAt(p, pos, n)) {
            sunRT = SunShadowRT(p, pos, n);
            lightRT = LightShadowRT(pos, n);
        }
        if (sunlight > 0.0) {
            ao = AmbientOcclusion(p);
        }
    }
    o_color = vec4(sunRT, lightRT, ao, 1.0);
}
#else
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

    /* Ray-traced shadows of the surface under this pixel (exteriors). */
    vec3 surfacePos;
    vec3 surfaceNormal;
    /* Only scene surfaces: a menu's bodies (the behaviour menu's Twinsens) have
       their own projection, their distances are not scene points. */
    bool scenePixel = id >= 8388608.0;
    float sunRT = 0.0;
    float lightRT = 0.0;
    float sceneAO = 1.0;
    if (halfReady > 0.5) {
        /* Traced at half resolution by the HALF_PASS build. */
        if (scenePixel) {
            vec3 h = HalfLight(p);
            sunRT = h.r;
            lightRT = h.g;
            sceneAO = h.b;
        }
    } else {
        bool traced = scenePixel && (rtSun.w > 0.0 || rtLightCfg.x > 0.5) && SurfaceAt(p, surfacePos, surfaceNormal);
        sunRT = traced ? SunShadowRT(p, surfacePos, surfaceNormal) : 0.0;
        lightRT = traced ? LightShadowRT(surfacePos, surfaceNormal) : 0.0;
        sceneAO = scenePixel && sunlight > 0.0 ? AmbientOcclusion(p) : 1.0;
    }

    /* LBA2_GPU_DEBUG=6: the ray-traced shadows alone: grey the sun's, red the lights'. */
    if (debugTint > 5.5 && debugTint < 6.5) {
        o_color = vec4((1.0 - sunRT / max(rtSun.w, 0.01)) * vec3(1.0, 1.0 - lightRT, 1.0 - lightRT), 1.0);
        return;
    }
    /* LBA2_GPU_DEBUG=5: the ambient occlusion alone. */
    if (debugTint > 4.5 && debugTint < 5.5) {
        o_color = vec4(vec3(AmbientOcclusion(p)), 1.0);
        return;
    }
    /* LBA2_GPU_DEBUG=3: the soft shadow target. */
    if (debugTint > 2.5 && debugTint < 3.5) {
        vec2 s = SoftShadow(v_uv);
        o_color = vec4((1.0 - s.r) * vec3(1.0, 1.0 - s.g, 1.0 - s.g), 1.0);
        return;
    }
    /* LBA2_GPU_DEBUG=2: the GPU image alone, wherever it drew. */
    if (debugTint > 1.5 && debugTint < 2.5) {
        o_color = id > 0.0 ? vec4(texelFetch(u_objColor, p, 0).rgb, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }

    /* Scene tags accept any scene surface: the GPU's depth picks it. */
    const float SCENE_BIT = 8388608.0;
    bool match = tag > 0.0 && (abs(tag - id) < 0.5 || (tag >= SCENE_BIT && id >= SCENE_BIT));
    /* The software drew another scene surface here (the terrain's original
       edge) but the GPU's only surface is the sky: a smoothed crest lying just
       below the classic silhouette. Keep the software's pixel. */
    bool hole = match && abs(tag - id) >= 0.5 && SkyMarker(texelFetch(u_objLight, p, 0).a);
    if (hole) {
        match = false;
    }
    vec3 halo = (glow > 0.0 || skyTint.w > 0.0) ? Glow(v_uv) : vec3(0.0);
    if (flameCount > 0.5) {
        halo += Flames(v_uv);
    }
    halo += WaterSpray(v_uv);
    /* The GPU's sky over the sky pixels outdoors, then the storm over it. */
    if (!hole && IsSky(p, match, tag)) {
        o_color = vec4(Screen(Storm(Sky(p), p, true, true), halo), 1.0) * v_color;
        return;
    }
    if (!match) {
        vec3 c = SoftwarePixel(v_uv);
        /* A soft shadow reaches the untouched frame too (an interior room's
           floor) where the GPU knows the surface, so it stays behind walls. */
        if (texelFetch(u_objId, p, 0).r > 0.0) {
            c *= 1.0 - max(SoftShadow(v_uv).r, sunRT);
            if (sunlight > 0.0) {
                c = Grade(c * sceneAO);
            }
            c = Storm(FogToSky(c, p), p, true, false);
        } else if (stormLight.w > 0.5 && all(greaterThanEqual(v_uv, stormView.xy)) &&
                   all(lessThan(v_uv, stormView.zw))) {
            /* No GPU surface in the scene's window outdoors: the software sky. */
            c = Storm(c, p, true, true);
        }
        o_color = vec4(Screen(c, halo), 1.0) * v_color;
        return;
    }

    vec4 nearest = texelFetch(u_objColor, p, 0);
    vec4 lightTexel = texelFetch(u_objLight, p, 0);
    /* Soft shadow, filtered at the GPU's resolution. */
    vec2 shadow = SoftShadow(v_uv);
    float shade = 1.0 - max(shadow.r, sunRT);
    /* An emissive surface is the light source: it is not lit again by itself. */
    float emissive = lightTexel.a > 0.02 ? lightTexel.a : 0.0;
    /* Bodies block the dynamic light behind them. */
    vec3 light = lightTexel.rgb * 2.0 * (1.0 - emissive) * (1.0 - max(shadow.g, lightRT));
    if (nearest.a < 0.5) {
        /* A surface whose colour is the software frame (interior bricks). */
        vec3 c = SoftwarePixel(v_uv);
        if (sunlight > 0.0) {
            c = Grade(c * sceneAO);
        }
        o_color = vec4(Screen(Storm(FogToSky(c * (vec3(1.0) + light) * shade, p), p, true, true), halo), 1.0) * v_color;
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
    if (sunlight > 0.0 && emissive <= 0.0) {
        gpu = Grade(gpu * sceneAO);
    }
    gpu *= (vec3(1.0) + light) * shade;
    vec2 shore = ShoreFoam(p);
    float crest = max(max(gpu.r, gpu.g), gpu.b);
    vec3 foamColor = min(gpu * 1.30 + vec3(0.10 + crest * 0.10), vec3(0.96));
    float waterFoam = shore.x * mix(0.66, 0.82, waterStorm);
    float landWash = shore.y * mix(0.84, 0.96, waterStorm);
    gpu = mix(gpu, foamColor, clamp(waterFoam + landWash, 0.0, 0.78));
    gpu = Storm(scenePixel ? FogToSky(gpu, p) : gpu, p, emissive <= 0.0, true);
    gpu = Screen(gpu, halo);
    gpu = mix(gpu, vec3(0.0, 1.0, 0.0), min(debugTint, 1.0) * 0.5);
    o_color = vec4(gpu, frame.a);
}
#endif
