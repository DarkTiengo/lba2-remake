#version 450
#extension GL_GOOGLE_include_directive : require
/* 3D body vertex: positions arrive already in clip space (the CPU projects with
   the engine's own formulas so silhouettes match the software rasterizer). The
   depth is local to the body; the draw's slice places it in front of every
   body drawn before it, which is the software painter's order. */

layout(location = 0) in vec4 a_clip;
layout(location = 1) in vec4 a_normal;
layout(location = 2) in vec4 a_light;
layout(location = 3) in vec4 a_vpos;
layout(location = 4) in vec4 a_mat;
layout(location = 5) in vec4 a_uv;

layout(location = 0) out vec4 v_normal;
layout(location = 1) flat out vec4 v_light;
layout(location = 2) out vec4 v_vpos;
layout(location = 3) flat out vec4 v_mat;
layout(location = 4) out vec4 v_uv;
layout(location = 5) flat out vec2 v_slice;
layout(location = 6) out float v_waterCoast;

#include "PLACE.glsl"

void main() {
    vec4 clip = a_clip;
    vec4 vpos = a_vpos;
    vec4 normal = a_normal;
    vec4 light = a_light;
    if (instance.w > 0.0) {
        /* A kept model: its normals carry their length, the light is the draw's. */
        normal.w *= instance.w;
        light.xyz = instance.xyz;
    }
    bool placed = false;
    vec4 uv = a_uv;
    if (tree0.w > 0.0) {
        /* A tree in the wind, moved in its own space before it is placed. */
        vec3 rel = a_vpos.xyz - tree1.xyz;
        bool leaf = (int(a_vpos.w + 0.5) & FLAG_FOLIAGE) != 0;
        vpos.xyz += TreeSway(rel, leaf);
        if (leaf && tree3.w > 0.0) {
            /* The crown is shaded as the round mass of leaves it stands for
               more than as its facets; the length is the shade's scale. */
            float len = length(normal.xyz);
            vec3 out_ = normalize(a_vpos.xyz - tree3.xyz);
            normal.xyz = normalize(mix(normal.xyz / max(len, 1e-6), out_, 0.55)) * len;
            /* Where the leaf lies on the tree, the same from any camera: the
               fragment shader's tufts. */
            vec3 side = cross(tree0.xyz, tree2.xyz);
            uv.zw = vec2(dot(rel, side) + dot(rel, tree2.xyz) * 0.7, dot(rel, tree0.xyz));
        }
        placed = true;
    } else if ((int(a_vpos.w + 0.5) & FLAG_FOLIAGE) != 0) {
        /* Not a tree in this draw (gfx_trees off): shaded as any body. */
        vpos.w = float(int(a_vpos.w + 0.5) & ~FLAG_FOLIAGE);
    }
    if ((int(a_vpos.w + 0.5) & FLAG_WAVES) != 0 && waves.z > 0.5) {
        /* The sea: raised along the world's up, in the space it was captured in. */
        vec3 n;
        float h = Swell(a_uv.zw * 512.0, n);
        vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
        vpos.xyz += h * up;
        normal = vec4(dot(capRow0.xyz, n), dot(capRow1.xyz, n), dot(capRow2.xyz, n), h);
        placed = true;
    }
    /* The same placement the capture makes, from the same formulas
       (AffGpu_ViewVertex), on a point moved into this frame's camera. */
    vec3 p = vpos.xyz;
    vec3 n = normal.xyz;
    if (Reframe(p, n)) {
        vpos.xyz = p;
        normal.xyz = n;
        placed = true;
    }
    if (placed) {
        clip = Place(vpos.xyz);
    }
    if ((int(a_vpos.w + 0.5) & FLAG_GRASS) != 0) {
        /* A grass blade's tip bends with the wind: gusts travel over the
           field (the phase follows the ground), each blade flutters in them. */
        float ph = a_mat.w;
        float gust = 0.5 + 0.5 * sin(time * 1.1 - ph * 0.8);
        float flutter = sin(time * 4.3 + ph * 5.3);
        clip.xy += a_uv.zw * (wind * (0.2 + 0.8 * gust + 0.15 * flutter));
    }
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = normal;
    v_light = light;
    v_vpos = vpos;
    v_mat = a_mat;
    v_uv = uv;
    v_slice = vec2(sliceNear, sliceSize);
    v_waterCoast = (int(a_vpos.w + 0.5) & FLAG_WAVES) != 0 ? 1.0 : 0.0;
}
