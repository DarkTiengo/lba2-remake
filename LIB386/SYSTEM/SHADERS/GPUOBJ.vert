#version 450
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

layout(set = 1, binding = 0) uniform Draw {
    float sliceNear; // depth where this draw's slice starts
    float sliceSize; // depth span of one slice
    float time;      // seconds, for the grass in the wind
    float wind;      // the wind's strength (1 a breeze, more in a storm)
};

const int FLAG_GRASS = 512;

void main() {
    vec4 clip = a_clip;
    if ((int(a_vpos.w + 0.5) & FLAG_GRASS) != 0) {
        /* A grass blade's tip bends with the wind: gusts travel over the
           field (the phase follows the ground), each blade flutters in them. */
        float ph = a_mat.w;
        float gust = 0.5 + 0.5 * sin(time * 1.1 - ph * 0.8);
        float flutter = sin(time * 4.3 + ph * 5.3);
        clip.xy += a_uv.zw * (wind * (0.2 + 0.8 * gust + 0.15 * flutter));
    }
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = a_normal;
    v_light = a_light;
    v_vpos = a_vpos;
    v_mat = a_mat;
    v_uv = a_uv;
    v_slice = vec2(sliceNear, sliceSize);
}
