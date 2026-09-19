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

layout(set = 3, binding = 0) uniform Draw {
    float drawId;
    float lutRow;   // fog remap row, -1 when none
    float clutRow;  // first row of the 16-row Gouraud CLUT
    float page;     // texture page layer
    float fogStart; // view depth where distance fog begins
    float fogEnd;   // and where it is total; <= fogStart disables it
    float fogColor; // palette index fog fades to
    float specular; // specular strength, 0 turns highlights off
    float glassPass; // 1: the blended pass drawing only lamp glass; 0: everything else
    float drawPad0;
    float drawPad1;
    float drawPad2;
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
    vec4 fireInfo;     // count, page slot, time in seconds, unused
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
const int MODE_SHADOW = 9;
const int MODE_SILHOUETTE = 10;

/* Must match ISO_DEPTH_RANGE in AFF_GPU.CPP: bricks and iso bodies share depth. */
const float ISO_DEPTH_RANGE = 524288.0;

const int FLAG_CHROMAKEY = 1;
const int FLAG_ISO = 2;
const int FLAG_BAKED = 4;
const int FLAG_EMISSIVE = 8;
const int FLAG_WATER = 16;
const int FLAG_SKY = 32;
const int FLAG_WATER_TERRAIN = 64;

vec3 Pal(int i) {
    return texelFetch(u_palette, ivec2(i & 255, 0), 0).rgb;
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

int Texel(int u, int v) {
    int offset = int(v_mat.y);
    int mask = int(v_mat.z);
    int index = (offset + ((((v & 255) << 8) | (u & 255)) & mask)) & 65535;
    return int(texelFetch(u_pages, ivec3(index & 255, index >> 8, int(page)), 0).r * 255.0 + 0.5);
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

vec4 Textured(float shade, bool shaded) {
    int clut = int(clutRow);
    vec2 t = v_uv.xy - 0.5;
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

    /* Lamp glass is drawn in its own blended pass, over what lies behind it. */
    bool glass = (Flags() & FLAG_EMISSIVE) != 0 && (mode == MODE_SOLID || mode == MODE_SHADED || mode == MODE_DISC);
    if (glass != (glassPass > 0.5)) {
        discard;
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
    bool sky = (Flags() & FLAG_SKY) != 0;
    vec3 surfaceNormal = v_normal.xyz;

    if (water && (Flags() & FLAG_WATER_TERRAIN) == 0) {
        color = WaterColor(surfaceNormal);
    } else if (water) {
        /* CodeJeu 12/15 is the retail shoreline animation. Its page receives
           250 ms updates in the software path, unlike SkySeaTexture's layout. */
        surfaceNormal = normalize(v_normal.xyz);
        color = Textured(0.0, false).rgb;
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
        color = Textured(0.0, false).rgb;
    } else if (mode == MODE_TEXSHADED) {
        float shade = ShadeValue(spec);
        color = Textured(shade, true).rgb;
        color += spec * specular * 0.25 * color;
    } else if (mode == MODE_CLUT) {
        /* Gouraud table fill: the CLUT row is the shade, the column the colour. */
        float shade = ShadeValue(spec);
        int row = int(floor(shade));
        int rowA = int(clutRow) + min(row, 15);
        int rowB = int(clutRow) + min(row + 1, 15);
        color = mix(Pal(Lut(rowA, base)), Pal(Lut(rowB, base)), fract(shade));
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

    if (emissive > 0.0 && (Flags() & FLAG_EMISSIVE) != 0) {
        /* A lit globe: a warm yellow glass with a paler core, kept below white. */
        float centre = mode == MODE_DISC ? 1.0 - dot(v_uv.zw, v_uv.zw) : 0.5;
        color = mix(max(color, vec3(0.95, 0.72, 0.32)), vec3(1.0, 0.9, 0.6), centre * 0.45);
        color = min(color, vec3(0.97));
    }

    bool litBody = (mode == MODE_SHADED || mode == MODE_TEXSHADED || mode == MODE_CLUT) &&
                   (Flags() & FLAG_BAKED) == 0 && emissive <= 0.0;
    if (litBody && sunDir.w > 0.0) {
        color = GlobalLight(color, surfaceNormal);
    }

    if (fogEnd > fogStart) {
        float f = clamp((-v_vpos.z - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        color = mix(color, Pal(int(fogColor)), f);
    }

    o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
    bool baked = (Flags() & FLAG_BAKED) != 0 && !water;
    /* One- and two-step R8 alpha markers let the composite find water edges
       without another full-resolution render target. */
    float material = water ? (1.0 / 255.0) : (sky ? (2.0 / 255.0) : clamp(emissive, 0.0, 1.0));
    o_light = vec4(min(DynamicLight(v_vpos.xyz, surfaceNormal, !baked), vec3(2.0)) * 0.5, material);
    if (!sky) {
        o_shadow = vec4(0.0, 0.0, PackDistance(v_vpos.xyz, (Flags() & FLAG_ISO) != 0));
    }
    o_id = vec4(id, 0.0, 0.0, 1.0);
}
