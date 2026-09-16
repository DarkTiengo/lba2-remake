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

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_id;

layout(set = 2, binding = 0) uniform sampler2D u_palette;    // 256x1 RGBA
layout(set = 2, binding = 1) uniform sampler2D u_lut;        // 256xN R8: logical palettes, CLUT blocks
layout(set = 2, binding = 2) uniform sampler2DArray u_pages; // 256x256xN R8 texture pages

layout(set = 3, binding = 0) uniform Draw {
    float drawId;
    float lutRow;  // fog remap row, -1 when none
    float clutRow; // first row of the 16-row Gouraud CLUT
    float page;    // texture page layer
};

const int MODE_PASS = 0;
const int MODE_SOLID = 1;
const int MODE_SHADED = 2;
const int MODE_TEX = 3;
const int MODE_TEXSHADED = 4;
const int MODE_DISC = 5;

const int FLAG_CHROMAKEY = 1;
const int FLAG_ISO = 2;

vec3 Pal(int i) {
    return texelFetch(u_palette, ivec2(i & 255, 0), 0).rgb;
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
    vec3 n = normalize(v_normal.xyz);
    vec3 l = normalize(v_light.xyz);
    float ndl = dot(n, l);

    vec3 view;
    if ((int(v_vpos.w) & FLAG_ISO) != 0) {
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
    if ((int(v_vpos.w) & FLAG_CHROMAKEY) != 0 && texel == 0) {
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

void main() {
    int mode = int(v_light.w);
    float id = drawId;

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
        color += spec * 0.35 * Pal(Logical(base | 15));
    } else if (mode == MODE_TEX) {
        color = Textured(0.0, false).rgb;
    } else if (mode == MODE_TEXSHADED) {
        float shade = ShadeValue(spec);
        color = Textured(shade, true).rgb;
        color += spec * 0.25 * color;
    } else {
        if (dot(v_uv.zw, v_uv.zw) > 1.0) {
            discard;
        }
        color = Pal(Logical(base));
    }

    o_color = vec4(clamp(color, 0.0, 1.0), 1.0);
    o_id = vec4(id, 0.0, 0.0, 1.0);
}
