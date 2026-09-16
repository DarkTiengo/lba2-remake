#version 450
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
};

layout(set = 3, binding = 1) uniform Lights {
    vec4 lightPos[16];   // xyz in the scene's space, w radius
    vec4 lightColor[16]; // rgb, w intensity
    float lightCount;
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

const int MODE_PASS = 0;
const int MODE_SOLID = 1;
const int MODE_SHADED = 2;
const int MODE_TEX = 3;
const int MODE_TEXSHADED = 4;
const int MODE_DISC = 5;
const int MODE_CLUT = 6;
const int MODE_BRICK = 7;

/* Must match ISO_DEPTH_RANGE in AFF_GPU.CPP: bricks and iso bodies share depth. */
const float ISO_DEPTH_RANGE = 524288.0;

const int FLAG_CHROMAKEY = 1;
const int FLAG_ISO = 2;
const int FLAG_BAKED = 4;

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

void main() {
    int mode = int(v_light.w);
    float id = drawId;
    gl_FragDepth = gl_FragCoord.z;
    o_light = vec4(0.0);

    if (mode == MODE_BRICK) {
        BrickCoverage();
        o_color = vec4(0.0); /* alpha 0: show the software frame here */
        o_id = vec4(id, 0.0, 0.0, 1.0);
        vec3 surface;
        gl_FragDepth = BrickDepth(surface);
        o_light = vec4(min(DynamicLight(surface, vec3(0.0, 1.0, 0.0), false), vec3(2.0)) * 0.5, 1.0);
        return;
    }

    if (mode == MODE_PASS) {
        o_color = vec4(0.0);
        o_id = vec4(0.0);
        return;
    }

    vec3 color;
    float spec = 0.0;
    int base = int(v_mat.x);

    if (mode == MODE_SOLID) {
        color = Pal(Logical(base));
    } else if (mode == MODE_SHADED) {
        float shade = ShadeValue(spec);
        color = Ramp(base, shade);
        color += spec * specular * 0.35 * Pal(Logical(base | 15));
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
    } else {
        if (dot(v_uv.zw, v_uv.zw) > 1.0) {
            discard;
        }
        color = Pal(Logical(base));
    }

    if (fogEnd > fogStart) {
        float f = clamp((-v_vpos.z - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        color = mix(color, Pal(int(fogColor)), f);
    }

    o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
    bool baked = (Flags() & FLAG_BAKED) != 0;
    o_light = vec4(min(DynamicLight(v_vpos.xyz, v_normal.xyz, !baked), vec3(2.0)) * 0.5, 1.0);
    o_id = vec4(id, 0.0, 0.0, 1.0);
}
