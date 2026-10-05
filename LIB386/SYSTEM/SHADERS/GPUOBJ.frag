#version 450
#extension GL_GOOGLE_include_directive : require
/* 3D body fragment: the original palette ramps and CLUTs stay the source of
   colour, but the shade index is computed per pixel from interpolated normals
   and blended between neighbouring ramp entries, then lifted with a specular
   highlight. */

layout(location = 0) in vec4 v_normal;
layout(location = 1) flat in vec4 v_light;
layout(location = 2) in vec4 v_vpos;
layout(location = 3) flat in vec4 v_mat;
layout(location = 4) in vec4 v_uv;
layout(location = 5) flat in vec2 v_slice;
layout(location = 6) in float v_waterCoast;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_id;
layout(location = 2) out vec4 o_light; // dynamic light at this surface, halved
layout(location = 3) out vec4 o_shadow; // r: contact shadow darkness, g: dynamic light blocked,
                                        // ba: view distance, 16 bits (ambient occlusion)
layout(location = 4) out float o_height; // scene-space height for water contact

layout(set = 2, binding = 0) uniform sampler2D u_palette;    // 256x1 RGBA
layout(set = 2, binding = 1) uniform sampler2D u_lut;        // 256xN R8: logical palettes, CLUT blocks
layout(set = 2, binding = 2) uniform sampler2DArray u_pages; // 256x256xN R8 texture pages
layout(set = 2, binding = 3) uniform sampler2D u_atlas;       // interior bricks: index, coverage
layout(set = 2, binding = 4) uniform sampler2D u_lava;        // project-authored lava albedo
layout(set = 2, binding = 5) uniform sampler2DArray u_replacements; // 1024x1024 RGBA, alpha = strength
layout(set = 2, binding = 6) uniform sampler2DArray u_replacementMasks; // 256x256 R8, 1 = original
layout(set = 2, binding = 7) uniform sampler2D u_replacementPalette; // palette the RGBA was exported with
layout(set = 2, binding = 8) uniform sampler2DArray u_vehicleMetal; // project-authored Citadel object albedos

layout(set = 3, binding = 0) uniform Draw {
    float drawId;
    float lutRow;   // fog remap row, -1 when none
    float clutRow;  // first row of the 16-row Gouraud CLUT
    float page;     // texture page layer
    float fogStart; // view depth where distance fog begins
    float fogEnd;   // and where it is total; <= fogStart disables it
    float fogColor; // palette index fog fades to
    float specular; // specular strength, 0 turns highlights off
    float glassPass;  // 1: the blended pass drawing only lamp glass; 0: everything else
    float replacement; // replacement layer + 1, or 0
    float textureDetail; // 0 current, 1 enhanced, 2 replacement with enhanced fallback
    float terrainMaterial; // replacement layer + 1 for world-projected earth/sand, or 0
};

layout(set = 3, binding = 1) uniform Lights {
    vec4 lightPos[16];   // xyz in the scene's space, w radius
    vec4 lightColor[16]; // rgb, w intensity
    float lightCount;
    vec4 sunDir;   // toward the global light, w: sun shadow strength (0: global light off)
    vec4 upDir;    // the world's up, w: sky fill strength
    vec4 sunColor; // rgb, w: rim light strength
};

layout(set = 3, binding = 2) uniform Fire {
    vec4 fireRects[4]; // texel x, y, w, h in the fire page
    vec4 fireInfo;     // count, page slot, time in seconds, modern lava enabled
};

/* Light reaching a point, soft quadratic falloff; normals shade bodies,
   baked surfaces (terrain, bricks) take it omnidirectionally. */
vec3 DynamicLight(vec3 pos, vec3 normal, bool useNormal) {
    vec3 sum = vec3(0.0);
    int count = int(lightCount);
    for (int k = 0; k < count; k++) {
        vec3 toLight = lightPos[k].xyz - pos;
        float dist2 = dot(toLight, toLight);
        float radius = lightPos[k].w;
        float falloff = clamp(1.0 - dist2 / (radius * radius), 0.0, 1.0);
        falloff *= falloff;
        if (falloff <= 0.0) {
            continue;
        }
        float facing = 1.0;
        if (useNormal) {
            facing = 0.35 + 0.65 * max(dot(normalize(normal), toLight * inversesqrt(max(dist2, 1.0))), 0.0);
        }
        sum += lightColor[k].rgb * lightColor[k].w * falloff * facing;
    }
    return sum;
}

/* Distance between segments p0-p1 and q0-q1; s is where along p0-p1. */
float SegmentDistance(vec3 p0, vec3 p1, vec3 q0, vec3 q1, out float s) {
    vec3 d1 = p1 - p0;
    vec3 d2 = q1 - q0;
    vec3 r = p0 - q0;
    float a = dot(d1, d1);
    float e = dot(d2, d2);
    float f = dot(d2, r);
    float c = dot(d1, r);
    float b = dot(d1, d2);
    float denom = a * e - b * b;
    s = denom > 1e-6 ? clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
    float t = clamp((b * s + f) / max(e, 1e-6), 0.0, 1.0);
    s = clamp((b * t - c) / max(a, 1e-6), 0.0, 1.0);
    return length(p0 + d1 * s - (q0 + d2 * t));
}

/* The soft contact blob under a body; its shape-true shadows away from the sun
   and the lights are the silhouettes (MODE_SILHOUETTE). */
vec2 ShadowOf() {
    vec2 e = v_uv.zw / v_mat.z;
    float r = length(e);
    float body = 1.0 - smoothstep(0.62, 1.08, r);
    float core = 1.0 - smoothstep(0.0, 0.5, r);
    float blob = v_mat.y * (0.75 * body + 0.25 * core);

    return vec2(blob, 0.0);
}

const int MODE_PASS = 0;
const int MODE_SOLID = 1;
const int MODE_SHADED = 2;
const int MODE_TEX = 3;
const int MODE_TEXSHADED = 4;
const int MODE_DISC = 5;
const int MODE_CLUT = 6;
const int MODE_BRICK = 7;
const int MODE_ORB = 8;
const int MODE_RGB = 12;
const int MODE_SHADOW = 9;
const int MODE_SILHOUETTE = 10;
const int MODE_SPRITE = 11;

/* Must match ISO_DEPTH_RANGE in AFF_GPU.CPP: bricks and iso bodies share depth. */
const float ISO_DEPTH_RANGE = 524288.0;

const int FLAG_CHROMAKEY = 1;
const int FLAG_ISO = 2;
const int FLAG_BAKED = 4;
const int FLAG_EMISSIVE = 8;
const int FLAG_WATER = 16;
const int FLAG_SKY = 32;
const int FLAG_WATER_TERRAIN = 64;
const int FLAG_GRASS = 512;
const int FLAG_TERRAIN = 2048;
const int FLAG_LAVA = 4096;
const int FLAG_FOLIAGE = 8192;
const int FLAG_CITADEL_VEHICLE = 16384;
const int FLAG_CITADEL_OBJECT = 32768;
const int FLAG_CITADEL_BOAT_GLASS = 65536;
const int FLAG_CITADEL_BOAT_INTERIOR = 131072;

vec3 Pal(int i) {
    return texelFetch(u_palette, ivec2(i & 255, 0), 0).rgb;
}

/* The Citadel's brown and ochre land has palette colour but no authored UVs.
   Mirror tiling keeps the material continuous across every cube boundary. */
vec3 TerrainAlbedo(vec3 original, int base) {
    vec2 tile = 1.0 - abs(fract(v_uv.zw / 8.0) * 2.0 - 1.0);
    float y = base == 107 ? 512.0 : 0.0;
    vec2 atlasUv = (vec2(0.5, y + 0.5) + tile * 511.0) / 1024.0;
    vec3 material = texture(u_replacements, vec3(atlasUv, terrainMaterial - 1.0)).rgb;
    vec3 mean = base == 107 ? vec3(217.0, 167.0, 103.0) / 255.0
                            : vec3(127.0, 89.0, 58.0) / 255.0;
    return original * mix(vec3(1.0), clamp(material / mean, vec3(0.55), vec3(1.5)), 0.88);
}

/* Flags ride in an interpolated attribute, so a constant 4 can arrive as
   3.9999: round, never truncate. */
int Flags() {
    return int(floor(v_vpos.w + 0.5));
}

int Lut(int row, int i) {
    return int(texelFetch(u_lut, ivec2(i & 255, row), 0).r * 255.0 + 0.5);
}

int Logical(int i) {
    int row = int(lutRow);
    return row >= 0 ? Lut(row, i) : (i & 255);
}

int TexelAddress(int u, int v) {
    int offset = int(v_mat.y);
    int mask = int(v_mat.z);
    return (offset + ((((v & 255) << 8) | (u & 255)) & mask)) & 65535;
}

int Texel(int u, int v) {
    int index = TexelAddress(u, v);
    return int(texelFetch(u_pages, ivec3(index & 255, index >> 8, int(page)), 0).r * 255.0 + 0.5);
}

vec3 ReplacementBasePal(int i) {
    return texelFetch(u_replacementPalette, ivec2(i & 255, 0), 0).rgb;
}

// --- Procedural fire -----------------------------------------------------------
float FireHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float FireNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(FireHash(i), FireHash(i + vec2(1.0, 0.0)), u.x),
               mix(FireHash(i + vec2(0.0, 1.0)), FireHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float FireFbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int k = 0; k < 5; k++) {
        sum += amp * FireNoise(p);
        p = p * 2.03 + vec2(1.7, 9.2);
        amp *= 0.5;
    }
    return sum;
}

/* Where this fragment samples a fire region of the page, its position in it
   (0..1, y down: the flames' roots are at the bottom). */
bool FireLocal(out vec2 local) {
    local = vec2(0.0);
    if (fireInfo.x < 0.5 || int(page) != int(fireInfo.y)) {
        return false;
    }
    int offset = int(v_mat.y);
    int mask = int(v_mat.z);
    vec2 t = v_uv.xy;
    ivec2 i = ivec2(floor(t));
    int index = (offset + ((((i.y & 255) << 8) | (i.x & 255)) & mask)) & 65535;
    vec2 texel = vec2(float(index & 255), float(index >> 8)) + fract(t);
    for (int k = 0; k < int(fireInfo.x); k++) {
        vec4 r = fireRects[k];
        if (texel.x >= r.x && texel.x < r.x + r.z && texel.y >= r.y && texel.y < r.y + r.w) {
            local = (texel - r.xy) / r.zw;
            return true;
        }
    }
    return false;
}

/* Rising turbulent flames: hot and white at the roots, orange, then red at
   ragged tips; alpha 0 where the fire gives out. */
vec4 ProceduralFire(vec2 local) {
    float time = fireInfo.z;
    float x = local.x;
    float rise = local.y; // 1 at the roots
    float n = FireFbm(vec2(x * 3.2, rise * 2.2 + time * 2.1));
    float lick = FireFbm(vec2(x * 6.0 - time * 0.7, rise * 4.0 + time * 3.3));
    float heat = rise * 1.25 + (n - 0.5) * 1.1 + (lick - 0.5) * 0.45 - 0.2;
    heat -= pow(abs(x - 0.5) * 2.0, 3.0) * 0.35;
    heat = clamp(heat, 0.0, 1.0);
    vec3 c = mix(vec3(0.25, 0.02, 0.0), vec3(0.95, 0.18, 0.03), smoothstep(0.08, 0.35, heat));
    c = mix(c, vec3(1.0, 0.55, 0.08), smoothstep(0.3, 0.6, heat));
    c = mix(c, vec3(1.0, 0.88, 0.4), smoothstep(0.55, 0.85, heat));
    c = mix(c, vec3(1.0, 1.0, 0.85), smoothstep(0.85, 1.0, heat));
    return vec4(c * 1.15, smoothstep(0.06, 0.18, heat));
}

/* Value noise over the tree's leaves (v_uv.zw, where a leaf lies on it). */
float LeafNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float ShadeValue(out float spec) {
    if ((Flags() & FLAG_BAKED) != 0) {
        /* Authored intensity, interpolated across the polygon. */
        spec = 0.0;
        return clamp(v_normal.w, 0.0, 15.0);
    }
    vec3 n = normalize(v_normal.xyz);
    vec3 l = normalize(v_light.xyz);
    float ndl = dot(n, l);

    vec3 view;
    if ((Flags() & FLAG_ISO) != 0) {
        view = normalize(vec3(1.0, 0.8, 1.0));
    } else {
        view = normalize(-v_vpos.xyz);
    }
    vec3 h = normalize(l + view);
    spec = pow(max(dot(n, h), 0.0), 28.0) * step(0.0, ndl);

    if ((Flags() & FLAG_FOLIAGE) != 0) {
        /* Leaves: light wraps round the crown, which is never lit to a hard
           terminator, and clumps of them sit a little lighter or darker than
           their neighbours, within the palette's own ramp. No gloss. */
        spec *= 0.15;
        float wrapped = clamp((ndl + 0.5) / 1.5, 0.0, 1.0);
        float tuft = (LeafNoise(v_uv.zw / 240.0) - 0.5) * 1.0;
        return clamp(v_normal.w * wrapped + tuft, 0.0, 15.0);
    }
    return clamp(v_normal.w * max(ndl, 0.0), 0.0, 15.0);
}

/* Ramp colour at a fractional shade, never leaving the 16-entry ramp. */
vec3 Ramp(int base, float shade) {
    int i = base + int(floor(shade));
    int top = base | 15;
    int a = min(i, top);
    int b = min(i + 1, top);
    return mix(Pal(Logical(a)), Pal(Logical(b)), fract(shade));
}

vec4 TexColor(int texel, float shade, bool shaded, int clut) {
    if ((Flags() & FLAG_CHROMAKEY) != 0 && texel == 0) {
        return vec4(0.0);
    }
    if (!shaded) {
        return vec4(Pal(Logical(texel)), 1.0);
    }
    int row = int(floor(shade));
    int rowA = clut + min(row, 15);
    int rowB = clut + min(row + 1, 15);
    return vec4(mix(Pal(Lut(rowA, texel)), Pal(Lut(rowB, texel)), fract(shade)), 1.0);
}

vec4 OriginalTexturedAt(float shade, bool shaded, vec2 uvOffset, bool enhanced) {
    int clut = int(clutRow);
    vec2 t = v_uv.xy + uvOffset - 0.5;
    vec2 f = fract(t);
    if (enhanced) {
        /* Hold either texel a little longer across the interpolation. This
           removes the permanently soft look without ringing across palette
           edges as a cubic reconstruction would. */
        f = f * f * (3.0 - 2.0 * f);
    }
    ivec2 i = ivec2(floor(t));
    vec4 c00 = TexColor(Texel(i.x, i.y), shade, shaded, clut);
    vec4 c10 = TexColor(Texel(i.x + 1, i.y), shade, shaded, clut);
    vec4 c01 = TexColor(Texel(i.x, i.y + 1), shade, shaded, clut);
    vec4 c11 = TexColor(Texel(i.x + 1, i.y + 1), shade, shaded, clut);
    vec4 c = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    if (c.a < 0.5) {
        discard;
    }
    return vec4(c.rgb / c.a, 1.0);
}

vec4 OriginalTextured(float shade, bool shaded, vec2 uvOffset) {
    bool enhanced = textureDetail > 0.5;
    vec2 footprint = fwidth(v_uv.xy);
    float span = max(footprint.x, footprint.y);
    if (enhanced && span > 1.5 && (Flags() & FLAG_CHROMAKEY) == 0) {
        /* Four seam-safe samples suppress distant shimmer. Texel() applies the
           page's offset and repeat mask independently to every tap. */
        vec2 d = min(footprint * 0.22, vec2(1.5));
        return (OriginalTexturedAt(shade, shaded, uvOffset + vec2(-d.x, -d.y), true) +
                OriginalTexturedAt(shade, shaded, uvOffset + vec2( d.x, -d.y), true) +
                OriginalTexturedAt(shade, shaded, uvOffset + vec2(-d.x,  d.y), true) +
                OriginalTexturedAt(shade, shaded, uvOffset + vec2( d.x,  d.y), true)) * 0.25;
    }
    return OriginalTexturedAt(shade, shaded, uvOffset, enhanced);
}

/* Keep mode 0 on the exact sampling path shipped before texture packs. Apart
   from being the compatibility promise, this avoids making retained terrain
   depend on derivatives that only the enhanced modes need. */
vec4 LegacyTextured(float shade, bool shaded, vec2 uvOffset) {
    int clut = int(clutRow);
    vec2 t = v_uv.xy + uvOffset - 0.5;
    vec2 f = fract(t);
    ivec2 i = ivec2(floor(t));
    vec4 c00 = TexColor(Texel(i.x, i.y), shade, shaded, clut);
    vec4 c10 = TexColor(Texel(i.x + 1, i.y), shade, shaded, clut);
    vec4 c01 = TexColor(Texel(i.x, i.y + 1), shade, shaded, clut);
    vec4 c11 = TexColor(Texel(i.x + 1, i.y + 1), shade, shaded, clut);
    vec4 c = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    if (c.a < 0.5) {
        discard;
    }
    return vec4(c.rgb / c.a, 1.0);
}

ivec2 ReplacementAddress(int highU, int highV) {
    int u = int(floor(float(highU) * 0.25));
    int v = int(floor(float(highV) * 0.25));
    int subU = highU - u * 4;
    int subV = highV - v * 4;
    int index = TexelAddress(u, v);
    return ivec2((index & 255) * 4 + subU, (index >> 8) * 4 + subV);
}

vec4 ReplacementTexel(int highU, int highV) {
    int layer = int(replacement) - 1;
    ivec2 at = ReplacementAddress(highU, highV);
    int source = TexelAddress(int(floor(float(highU) * 0.25)),
                              int(floor(float(highV) * 0.25)));
    if (texelFetch(u_replacementMasks, ivec3(source & 255, source >> 8, layer), 0).r > 0.5) {
        return vec4(0.0);
    }
    return texelFetch(u_replacements, ivec3(at, layer), 0);
}

vec4 ReplacementTexturedAt(vec2 uvOffset) {
    vec2 t = (v_uv.xy + uvOffset) * 4.0 - 0.5;
    vec2 f = fract(t);
    ivec2 i = ivec2(floor(t));
    vec4 c00 = ReplacementTexel(i.x, i.y);
    vec4 c10 = ReplacementTexel(i.x + 1, i.y);
    vec4 c01 = ReplacementTexel(i.x, i.y + 1);
    vec4 c11 = ReplacementTexel(i.x + 1, i.y + 1);
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

vec4 ReplacementTextured(vec2 uvOffset) {
    vec2 footprint = fwidth(v_uv.xy);
    if (max(footprint.x, footprint.y) * 4.0 > 1.5 && (Flags() & FLAG_CHROMAKEY) == 0) {
        vec2 d = min(footprint * 0.22, vec2(1.5));
        return (ReplacementTexturedAt(uvOffset + vec2(-d.x, -d.y)) +
                ReplacementTexturedAt(uvOffset + vec2( d.x, -d.y)) +
                ReplacementTexturedAt(uvOffset + vec2(-d.x,  d.y)) +
                ReplacementTexturedAt(uvOffset + vec2( d.x,  d.y))) * 0.25;
    }
    return ReplacementTexturedAt(uvOffset);
}

vec4 Textured(float shade, bool shaded, vec2 uvOffset) {
    if (textureDetail < 0.5) {
        return LegacyTextured(shade, shaded, uvOffset);
    }
    vec4 original = OriginalTextured(shade, shaded, uvOffset);
    if (replacement < 0.5) {
        return original;
    }

    vec4 authored = ReplacementTextured(uvOffset);
    if (authored.a <= 0.0) {
        return original;
    }

    ivec2 centre = ivec2(floor(v_uv.xy + uvOffset));
    int index = Texel(centre.x, centre.y);
    int logical = Logical(index);
    vec3 currentBase = Pal(logical);
    vec3 sourceBase = ReplacementBasePal(index);
    vec3 referenceBase = ReplacementBasePal(logical);
    float currentLuma = dot(currentBase, vec3(0.299, 0.587, 0.114));
    float referenceLuma = dot(referenceBase, vec3(0.299, 0.587, 0.114));
    vec3 colour = (authored.rgb + referenceBase - sourceBase) *
                  (currentLuma / max(referenceLuma, 1.0 / 255.0));
    float paletteEffect = clamp(length(currentBase - referenceBase) * 1.5, 0.0, 1.0);
    colour = mix(colour, currentBase, paletteEffect * 0.65);

    if (shaded) {
        vec3 shadeRatio = original.rgb / max(currentBase, vec3(0.06));
        colour *= clamp(shadeRatio, vec3(0.0), vec3(2.5));
    }
    return vec4(mix(original.rgb, colour, authored.a), 1.0);
}

/* Brick coverage from the nearest texel, so neighbouring bricks meet exactly
   where their sprites do. The colour itself is left to the software frame (the
   composite reads it where a brick is the nearest surface): the sprites overlap
   by design and the painter's result is already there, exact. */
void BrickCoverage() {
    if (texelFetch(u_atlas, ivec2(floor(v_uv.xy)), 0).g < 0.5) {
        discard;
    }
}

/* Depth of the brick surface under this pixel: the isometric view ray through
   the frame position meets the brick's grid cell, and the visible surface is
   where it leaves the cell toward the camera. Same depth formula as iso bodies. */
float BrickDepth(out vec3 surface) {
    const vec3 dir = vec3(1.0, 0.8, 1.0);
    float a = (v_normal.x - v_mat.x) * 64.0 / 3.0;         // x - z
    float b = (v_normal.y - v_mat.y - 1.0) * 256.0 / 6.0;  // x + z at y = 0
    vec3 p0 = vec3((a + b) * 0.5, 0.0, (b - a) * 0.5);
    vec3 bmin = v_vpos.xyz;
    vec3 bmax = bmin + vec3(512.0, 256.0, 512.0);
    vec3 t1 = (bmin - p0) / dir;
    vec3 t2 = (bmax - p0) / dir;
    vec3 tNear = min(t1, t2);
    vec3 tFar = max(t1, t2);
    float tEnter = max(max(tNear.x, tNear.y), tNear.z);
    float tExit = min(min(tFar.x, tFar.y), tFar.z);
    float t = tExit >= tEnter ? tExit : 0.5 * (tEnter + tExit);
    vec3 p = p0 + t * dir;
    surface = p;
    float d = clamp(0.5 - (p.x + 0.8 * p.y + p.z) / ISO_DEPTH_RANGE, 0.0, 1.0);
    return v_slice.x + d * 0.999 * v_slice.y;
}

/* The global light on a body, over its palette shading: the sky fills the side
   turned up but away from the sun (the palette ramp leaves it flat dark), the
   ground bounces a little warmth under it, and the sun rims the silhouette
   when it shines from behind. */
vec3 GlobalLight(vec3 color, vec3 normal) {
    vec3 n = normalize(normal);
    vec3 l = normalize(sunDir.xyz);
    vec3 view = (Flags() & FLAG_ISO) != 0 ? normalize(vec3(1.0, 0.8, 1.0)) : normalize(-v_vpos.xyz);
    float lit = clamp(dot(n, l), 0.0, 1.0);
    float up = dot(n, upDir.xyz) * 0.5 + 0.5;
    vec3 sky = fogEnd > fogStart ? mix(Pal(int(fogColor)), vec3(0.6, 0.7, 0.85), 0.5) : vec3(0.62, 0.66, 0.75);
    vec3 ground = vec3(0.55, 0.45, 0.35);
    vec3 fill = mix(ground * 0.4, sky, up) * upDir.w * (1.0 - lit);
    color *= vec3(1.0) + fill * 1.6;
    float rim = pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 3.0);
    float behind = clamp(dot(l, -view) * 0.6 + 0.4, 0.0, 1.0);
    color += sunColor.rgb * rim * behind * sunColor.w * (0.4 + 0.6 * up);
    if ((Flags() & FLAG_FOLIAGE) != 0) {
        /* The sun behind a crown shines through its leaves, in their colour. */
        float through = pow(clamp(dot(l, -view), 0.0, 1.0), 4.0) * (1.0 - lit);
        color += color * sunColor.rgb * through * 0.9 * sunColor.w;
    }
    return color;
}

/* Distance from the camera along the view, packed in two 8-bit channels for the
   composite's ambient occlusion; 0 marks no surface. */
vec2 PackDistance(vec3 p, bool iso) {
    float dist = iso ? 65536.0 - (p.x + 0.8 * p.y + p.z) / 1.6248 : -p.z;
    float t = clamp(dist / 131072.0, 1.0 / 65025.0, 1.0 - 1.0 / 255.0);
    float hi = floor(t * 255.0) / 255.0;
    return vec2(hi, (t - hi) * 255.0);
}

#include "WATER.glsl"
#include "LAVA.glsl"

void main() {
    int mode = int(v_light.w);
    float id = drawId;
    gl_FragDepth = gl_FragCoord.z;
    o_light = vec4(0.0);
    o_shadow = vec4(0.0);
    /* Water contact is a height test, not a screen-space proximity test. The
       view-space point is projected onto the scene's world-up axis, which is
       valid for both rotated perspective scenery and isometric rooms. */
    o_height = dot(v_vpos.xyz, upDir.xyz);
    /* The broad sea's depth and visible pixels follow its raised mesh, but
       shoreline contact compares the fixed sea datum to fixed CodeJeu coast. */
    if ((Flags() & FLAG_WATER) != 0 && (Flags() & FLAG_WATER_TERRAIN) == 0) {
        o_height -= v_normal.w;
    }

    if (mode == MODE_SILHOUETTE) {
        /* A body flattened on the ground: the sun's shadow (r) or a light's (g,
           the share of that light it takes away here), fading with distance. */
        if (v_mat.w > 0.5 && dot(v_uv.zw, v_uv.zw) > 1.0) {
            discard;
        }
        float fade = 1.0 - smoothstep(0.3, 1.0, length(v_vpos.xyz - v_normal.xyz) / v_normal.w);
        o_color = vec4(0.0);
        o_id = vec4(0.0);
        if (v_mat.z <= 0.0) {
            o_shadow = vec4(sunDir.w * v_mat.y * fade, 0.0, 0.0, 0.0);
        } else {
            vec3 d = v_light.xyz - v_vpos.xyz;
            float f = clamp(1.0 - dot(d, d) / (v_mat.x * v_mat.x), 0.0, 1.0);
            float mine = f * f * v_mat.z;
            float total = 0.0;
            int count = int(lightCount);
            for (int k = 0; k < count; k++) {
                vec3 e = lightPos[k].xyz - v_vpos.xyz;
                float fk = clamp(1.0 - dot(e, e) / (lightPos[k].w * lightPos[k].w), 0.0, 1.0);
                total += fk * fk * lightColor[k].w * dot(lightColor[k].rgb, vec3(0.3, 0.59, 0.11));
            }
            float share = total > 0.0 ? min(mine / total, 1.0) : 0.0;
            o_shadow = vec4(0.0, share * v_mat.y * fade, 0.0, 0.0);
        }
        if (o_shadow.r < 0.004 && o_shadow.g < 0.004) {
            discard;
        }
        return;
    }

    if (mode == MODE_SHADOW) {
        o_color = vec4(0.0);
        o_id = vec4(0.0);
        o_shadow = vec4(ShadowOf(), 0.0, 0.0);
        if (o_shadow.r < 0.004 && o_shadow.g < 0.004) {
            discard;
        }
        return;
    }

    if (mode == MODE_SPRITE) {
        /* An extra's sprite. Coverage comes from the atlas cell, so the shape is
           the sprite's own; the colour is left to the software frame, which has
           it filtered as every other 2D pixel is; and the depth is the quad's,
           which is the whole point -- it is what lets the terrain or a body hide
           a sprite the software painter would have drawn over them. */
        if (texelFetch(u_atlas, ivec2(floor(v_uv.xy)), 0).g < 0.5) {
            discard;
        }
        o_color = vec4(0.0); /* alpha 0: show the software frame here */
        o_id = vec4(id, 0.0, 0.0, 1.0);
        o_light = vec4(min(DynamicLight(v_vpos.xyz, vec3(0.0, 1.0, 0.0), false), vec3(2.0)) * 0.5, 0.0);
        o_shadow = vec4(0.0, 0.0, PackDistance(v_vpos.xyz, false));
        return;
    }

    if (mode == MODE_BRICK) {
        BrickCoverage();
        o_color = vec4(0.0); /* alpha 0: show the software frame here */
        o_id = vec4(id, 0.0, 0.0, 1.0);
        vec3 surface;
        gl_FragDepth = BrickDepth(surface);
        o_light = vec4(min(DynamicLight(surface, vec3(0.0, 1.0, 0.0), false), vec3(2.0)) * 0.5, 0.0);
        o_shadow = vec4(0.0, 0.0, PackDistance(surface, true));
        return;
    }

    if (mode == MODE_PASS) {
        o_color = vec4(0.0);
        o_id = vec4(0.0);
        return;
    }

    /* Cabin panes and lamp glass are blended over their backgrounds. */
    bool boatGlass = (Flags() & FLAG_CITADEL_BOAT_GLASS) != 0;
    bool lampGlass = (Flags() & FLAG_EMISSIVE) != 0 && (mode == MODE_SOLID || mode == MODE_SHADED || mode == MODE_DISC);
    bool glass = boatGlass || lampGlass;
    if (glass != (glassPass > 0.5)) {
        discard;
    }
    if (boatGlass) {
        vec3 n = normalize(v_normal.xyz);
        vec3 view = normalize(-v_vpos.xyz);
        float rim = pow(1.0 - abs(dot(n, view)), 3.0);
        vec2 pane = clamp(v_uv.xy, vec2(0.0), vec2(1.0));
        float glint = exp(-pow((pane.x - 0.24 - pane.y * 0.12) / 0.045, 2.0));
        vec3 tint = vec3(0.32, 0.54, 0.60) + vec3(0.24, 0.26, 0.25) * glint + vec3(0.14) * rim;
        o_color = vec4(min(tint, vec3(0.85)), clamp(0.16 + 0.34 * rim + 0.10 * glint, 0.14, 0.60));
        o_id = vec4(id, 0.0, 0.0, 1.0);
        o_light = vec4(0.0);
        o_shadow = vec4(0.0);
        o_height = 0.0;
        return;
    }
    if (glass) {
        if (mode == MODE_DISC && dot(v_uv.zw, v_uv.zw) > 1.0) {
            discard;
        }
        vec3 n;
        vec3 view;
        if (mode == MODE_DISC) {
            vec2 d = v_uv.zw;
            n = vec3(d.x, -d.y, sqrt(max(1.0 - dot(d, d), 0.0)));
            view = vec3(0.0, 0.0, 1.0);
        } else {
            n = normalize(v_normal.xyz);
            view = (Flags() & FLAG_ISO) != 0 ? normalize(vec3(1.0, 0.8, 1.0)) : normalize(-v_vpos.xyz);
        }
        float facing = abs(dot(n, view));
        /* Fresnel: clear where the glass faces the eye, tinted and reflective at the rim. */
        float fresnel = pow(1.0 - facing, 2.0);
        /* The bulb inside shows through the middle of the globe. */
        float bulb = pow(facing, 3.0);
        vec3 tint = mix(Pal(Logical(int(v_mat.x) | 15)), vec3(1.0, 0.78, 0.35), 0.75);
        vec3 l = normalize(v_light.xyz);
        float highlight = pow(max(dot(normalize(l + view), n), 0.0), 40.0);
        vec3 c = tint * (0.5 + 0.5 * fresnel) + vec3(1.0, 0.82, 0.45) * bulb * 0.9 +
                 vec3(1.0, 0.95, 0.85) * highlight * 0.35 * specular;
        float alpha = clamp(0.25 + 0.5 * fresnel + 0.55 * bulb + highlight * 0.3, 0.0, 0.92);
        o_color = vec4(min(c, vec3(0.97)), alpha);
        o_id = vec4(id, 0.0, 0.0, 1.0);
        o_light = vec4(0.0, 0.0, 0.0, 0.6);
        return;
    }

    vec3 color;
    float emissive = 0.0; // written to o_light.a: the composite's glow halo
    vec2 fireLocal;
    float spec = 0.0;
    int base = int(v_mat.x);
    bool water = (Flags() & FLAG_WATER) != 0 && waterInfo.y > 0.5;
    bool shoreTerrain = (Flags() & FLAG_WATER_TERRAIN) != 0;
    /* Authored coastal polygons climb onto beaches and rocks. Their height is
       carried separately from the broad sea's swell attenuation. */
    float shoreWater = shoreTerrain ? 1.0 - smoothstep(32.0, 160.0, v_waterCoast) : 1.0;
    bool sky = (Flags() & FLAG_SKY) != 0;
    bool lava = (Flags() & FLAG_LAVA) != 0 && fireInfo.w > 0.5;
    float lavaHeight = 0.0;
    float lavaFlow = 0.0;
    float lavaFine = 0.0;
    vec2 lavaSlope = vec2(0.0);
    vec2 lavaWarp = vec2(0.0);
    vec2 lavaUvOffset = vec2(0.0);
    if (lava) {
        LavaField(v_uv.zw * 512.0, waterInfo.x, lavaHeight, lavaSlope,
                  lavaWarp, lavaFlow, lavaFine);
        /* The authored page remains recognizable while its texels creep with
           the same field as the project material. */
        lavaUvOffset = lavaWarp * 0.22;
    }
    vec3 surfaceNormal = v_normal.xyz;
    if (lava) {
        mat3 axes = mat3(waterAxisX.xyz, waterAxisY.xyz, waterAxisZ.xyz);
        vec3 movingNormal = normalize(axes * normalize(vec3(-lavaSlope.x, 1.0, -lavaSlope.y)));
        surfaceNormal = normalize(mix(normalize(surfaceNormal), movingNormal, 0.56));
    }

    if ((Flags() & FLAG_CITADEL_BOAT_INTERIOR) != 0) {
        vec2 pane = clamp(v_uv.xy, vec2(0.0), vec2(1.0));
        vec3 cabin = mix(vec3(0.035, 0.049, 0.056), vec3(0.13, 0.10, 0.075), 1.0 - pane.y);
        float seat = (1.0 - smoothstep(0.32, 0.35, pane.y)) * smoothstep(0.08, 0.16, pane.x) *
                     (1.0 - smoothstep(0.84, 0.92, pane.x));
        cabin = mix(cabin, vec3(0.34, 0.21, 0.12), seat);
        float seatBack = (smoothstep(0.31, 0.35, pane.y) - smoothstep(0.67, 0.71, pane.y)) *
                         smoothstep(0.18, 0.24, pane.x) * (1.0 - smoothstep(0.55, 0.61, pane.x));
        cabin = mix(cabin, vec3(0.25, 0.16, 0.10), seatBack);
        float rail = (1.0 - smoothstep(0.014, 0.032, abs(pane.x - 0.72))) * smoothstep(0.22, 0.34, pane.y);
        cabin = mix(cabin, vec3(0.50, 0.36, 0.18), rail * 0.78);
        color = cabin;
    } else if ((Flags() & FLAG_CITADEL_VEHICLE) != 0) {
        float shade = (mode == MODE_SHADED || v_normal.w > 0.01) ? ShadeValue(spec) : 15.0;
        vec3 authored = mode == MODE_SHADED ? Ramp(base, shade) : Pal(Logical(base));
        vec3 paint = texture(u_vehicleMetal, vec3(v_uv.xy, v_mat.w)).rgb;
        if (int(v_mat.w + 0.5) == 6) {
            /* Clean satin-metal grain; the palette still owns paint colour. */
            float value = dot(paint, vec3(0.2126, 0.7152, 0.0722));
            float grain = clamp(1.0 + (value / 0.748 - 1.0) * 1.6, 0.86, 1.14);
            color = authored * grain;
            color += spec * specular * 0.17 * authored;
        } else {
            color = mix(authored, paint * (0.78 + 0.22 * shade / 15.0), 0.76);
            color += spec * specular * 0.10 * paint;
        }
    } else if (water && waterInfo.w >= 0.0) {
        color = WaterColor(surfaceNormal, shoreTerrain ? 0.0 : v_waterCoast);
        if (shoreTerrain && shoreWater < 1.0) {
            bool shaded = mode == MODE_TEXSHADED;
            float shade = shaded ? ShadeValue(spec) : 0.0;
            vec3 bank = Textured(shade, shaded, vec2(0.0)).rgb;
            if (terrainMaterial > 0.5 && textureDetail > 1.5) {
                vec3 sand = TerrainAlbedo(Pal(Logical(107)), 107);
                bank = mix(bank, sand, 1.0 - smoothstep(280.0, 480.0, v_waterCoast));
            }
            color = mix(bank, color, shoreWater);
        }
    } else if (water) {
        /* CodeJeu 12/15 is the retail shoreline animation. Its page receives
           250 ms updates in the software path, unlike SkySeaTexture's layout. */
        surfaceNormal = normalize(v_normal.xyz);
        color = Textured(0.0, false, vec2(0.0)).rgb;
    } else if (mode == MODE_RGB) {
        vec3 n = normalize(v_normal.xyz);
        if (!gl_FrontFacing) n = -n;
        surfaceNormal = n;
        vec3 view = normalize(-v_vpos.xyz);
        vec3 light = normalize(v_light.xyz);
        float diffuse = max(dot(n, light), 0.0);
        float rough = clamp(v_uv.z, 0.08, 1.0);
        float metal = clamp(v_uv.w, 0.0, 1.0);
        float highlight = pow(max(dot(n, normalize(light + view)), 0.0), mix(100.0, 8.0, rough));
        color = v_mat.xyz * (0.48 + 0.52 * diffuse);
        color += mix(vec3(0.15), v_mat.xyz, metal) * highlight * (1.0 - rough * 0.65) * specular;
    } else if (mode == MODE_SOLID) {
        color = Pal(Logical(base));
        if ((Flags() & FLAG_EMISSIVE) != 0) {
            emissive = 1.0;
        }
    } else if (mode == MODE_SHADED) {
        float shade = ShadeValue(spec);
        color = Ramp(base, shade);
        color += spec * specular * 0.35 * Pal(Logical(base | 15));
        if ((Flags() & FLAG_EMISSIVE) != 0) {
            /* A lamp's glass, lit from inside: the ramp's brightest entry. */
            color = mix(Pal(Logical(base | 15)), vec3(1.0, 0.8, 0.38), 0.5);
            emissive = 0.6;
        }
    } else if ((mode == MODE_TEX || mode == MODE_TEXSHADED) && fireInfo.x > 0.5 && FireLocal(fireLocal)) {
        /* Animated fire texture: flames drawn here, glowing, not shaded. */
        vec4 fire = ProceduralFire(fireLocal);
        if (fire.a < 0.5 && (Flags() & FLAG_CHROMAKEY) != 0) {
            discard;
        }
        /* The flame itself is drawn above by the composite: this is its glowing bed. */
        color = fire.rgb * fire.a;
        emissive = fire.a * 0.8;
    } else if (mode == MODE_TEX) {
        color = Textured(0.0, false, lavaUvOffset).rgb;
    } else if (mode == MODE_TEXSHADED) {
        float shade = ShadeValue(spec);
        color = Textured(shade, true, lavaUvOffset).rgb;
        color += spec * specular * 0.25 * color;
    } else if (mode == MODE_CLUT) {
        /* Gouraud table fill: the CLUT row is the shade, the column the colour. */
        float shade = ShadeValue(spec);
        int row = int(floor(shade));
        int rowA = int(clutRow) + min(row, 15);
        int rowB = int(clutRow) + min(row + 1, 15);
        color = mix(Pal(Lut(rowA, base)), Pal(Lut(rowB, base)), fract(shade));
        if (terrainMaterial > 0.5 && textureDetail > 1.5 &&
            (Flags() & FLAG_TERRAIN) != 0 && (base == 27 || base == 107)) {
            color = TerrainAlbedo(color, base);
        }
    } else if (mode == MODE_ORB) {
        /* A glowing glass ball: lit from the upper left, a hot core, a coloured
           rim, and a sharp highlight. */
        vec2 d = v_uv.zw;
        float r2 = dot(d, d);
        if (r2 > 1.0) {
            discard;
        }
        vec3 n = vec3(d.x, -d.y, sqrt(1.0 - r2));
        vec3 l = normalize(vec3(-0.45, 0.6, 0.66));
        vec3 h = normalize(l + vec3(0.0, 0.0, 1.0));
        vec3 tint = v_mat.yzw;
        float lambert = max(dot(n, l), 0.0);
        float highlight = pow(max(dot(n, h), 0.0), 48.0);
        float rim = pow(1.0 - n.z, 2.0);
        float core = pow(n.z, 5.0);
        color = tint * (0.35 + 0.65 * lambert);
        color += mix(tint, vec3(1.0), 0.55) * core * 0.6;
        color += tint * rim * 0.9;
        color += vec3(1.0) * highlight * (0.35 + 0.6 * specular);
        o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
        o_id = vec4(id, 0.0, 0.0, 1.0);
        return;
    } else {
        float r2 = dot(v_uv.zw, v_uv.zw);
        if (r2 > 1.0) {
            discard;
        }
        color = Pal(Logical(base));
        if ((Flags() & FLAG_EMISSIVE) != 0) {
            emissive = 1.0;
        }
    }

    if ((Flags() & FLAG_CITADEL_OBJECT) != 0 && textureDetail > 1.5) {
        int layer = int(v_mat.w + 0.5);
        vec3 albedo = texture(u_vehicleMetal, vec3(v_uv.xy, (layer == 2 || layer == 5) ? 6.0 : v_mat.w)).rgb;
        if (layer == 2 || layer == 5) {
            /* Clean satin metal on bins, railings and lamp frames; palette owns the hue. */
            float value = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
            float grain = clamp(1.0 + (value / 0.748 - 1.0) * 1.6, 0.86, 1.14);
            color *= grain;
            color += spec * specular * 0.17 * color;
        } else {
            float mean = layer == 4 ? 0.36 : (layer == 3 ? 0.48 : 0.54);
            float authoredLight = dot(color, vec3(0.2126, 0.7152, 0.0722));
            vec3 shadedMaterial = albedo * clamp(authoredLight / mean, 0.32, 1.48);
            /* The palms already have painted frond detail: add grain lightly. */
            bool authoredFoliage = layer == 4 && (mode == MODE_TEX || mode == MODE_TEXSHADED);
            color = mix(color, shadedMaterial, authoredFoliage ? 0.28 : (layer == 4 ? 0.58 : 0.70));
        }
    }

    if (emissive > 0.0 && (Flags() & FLAG_EMISSIVE) != 0) {
        /* A lit globe: a warm yellow glass with a paler core, kept below white. */
        float centre = mode == MODE_DISC ? 1.0 - dot(v_uv.zw, v_uv.zw) : 0.5;
        color = mix(max(color, vec3(0.95, 0.72, 0.32)), vec3(1.0, 0.9, 0.6), centre * 0.45);
        color = min(color, vec3(0.97));
    }

    if (lava) {
        /* Two differently scaled flows stop the authored texture from sliding
           as one rigid sheet. Island-space coordinates keep both continuous
           across cube boundaries. */
        mat2 turn = mat2(0.8660254, -0.5, 0.5, 0.8660254);
        vec2 lavaTexUv = v_uv.zw / 8.0 + lavaWarp;
        vec2 lavaTexUv2 = turn * v_uv.zw / 12.5 - lavaWarp * 0.72 + vec2(0.31, 0.17);
        vec3 albedoA = texture(u_lava, lavaTexUv).rgb;
        vec3 albedoB = texture(u_lava, lavaTexUv2).rgb;
        vec3 albedo = mix(albedoA, albedoB, 0.26 + lavaFine * 0.12) * vec3(1.60, 1.35, 1.10);
        color = mix(color, albedo, 0.72);
        float molten = smoothstep(0.38, 0.83, albedo.r) * (0.78 + lavaFlow * 0.22);
        float crust = smoothstep(0.48, 0.77, 1.0 - lavaFlow + (0.5 - lavaFine) * 0.16);
        float vein = (1.0 - smoothstep(0.008, 0.045, abs(lavaFlow - 0.58))) *
                     smoothstep(0.40, 0.72, lavaFine);
        float pulse = 0.92 + 0.08 * sin(lavaHeight * 0.12 +
                                        waterInfo.x * 14.0 * (6.28318530718 / 256.0));

        color = mix(color * vec3(1.02, 0.98, 0.94), color * vec3(0.68, 0.59, 0.56), crust * 0.22);
        color += (vec3(0.16, 0.035, 0.003) * vein + vec3(0.22, 0.045, 0.003) * molten) * pulse;
        emissive = max(emissive, max(vein * 0.15, molten * 0.30));
    }

    bool litBody = (mode == MODE_SHADED || mode == MODE_TEXSHADED || mode == MODE_CLUT || mode == MODE_RGB) &&
                   (Flags() & FLAG_BAKED) == 0 && emissive <= 0.0;
    if (litBody && sunDir.w > 0.0) {
        color = GlobalLight(color, surfaceNormal);
    }

    if (fogEnd > fogStart) {
        float f = clamp((-v_vpos.z - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        color = mix(color, Pal(int(fogColor)), f);
    }

    o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
    bool visibleWater = water && shoreWater >= 0.5;
    bool baked = (Flags() & FLAG_BAKED) != 0 && !visibleWater;
    /* R8 alpha markers let the composite find water and ground contacts
       without mistaking an actor standing beside the sea for a seabed. */
    float material = clamp(emissive, 0.0, lava ? 0.5 : 1.0);
    if (visibleWater)
        material = 1.0 / 255.0;
    else if (sky)
        material = 2.0 / 255.0;
    else if ((Flags() & FLAG_GRASS) != 0)
        material = 4.0 / 255.0;
    else if (!lava && (Flags() & FLAG_TERRAIN) != 0 && terrainMaterial > 0.5 && base == 107)
        material = 6.0 / 255.0;
    else if (!lava && ((Flags() & FLAG_TERRAIN) != 0 || (water && shoreTerrain)))
        material = 5.0 / 255.0;
    o_light = vec4(min(DynamicLight(v_vpos.xyz, surfaceNormal, !baked), vec3(2.0)) * 0.5, material);
    if (!sky) {
        o_shadow = vec4(0.0, 0.0, PackDistance(v_vpos.xyz, (Flags() & FLAG_ISO) != 0));
    }
    o_id = vec4(id, 0.0, 0.0, 1.0);
}
