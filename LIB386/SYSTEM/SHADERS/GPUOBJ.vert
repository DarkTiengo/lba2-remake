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
    /* A draw kept from an earlier frame is placed where its own camera left it,
       so it is moved into this frame's camera here: rows of the difference
       between the two placements, each with its translation in w. Zero rows
       (the default) mean the draw was captured with the camera drawing it and
       its clip position is used as it stands. */
    vec4 xformRow0;
    vec4 xformRow1;
    vec4 xformRow2;
    vec4 proj;  // XCentre, YCentre, FRatioX, FRatioX * FRatioY
    vec4 frame; // 2 / width, 2 / height, near clip, the scene's depth range
};

const int FLAG_GRASS = 512;

void main() {
    vec4 clip = a_clip;
    vec4 vpos = a_vpos;
    vec4 normal = a_normal;
    if (xformRow0.xyz != vec3(0.0)) {
        /* The same placement the capture makes, from the same formulas
           (AffGpu_ViewVertex), on a point moved into this frame's camera. */
        vec3 p = vec3(dot(xformRow0.xyz, a_vpos.xyz) + xformRow0.w,
                      dot(xformRow1.xyz, a_vpos.xyz) + xformRow1.w,
                      dot(xformRow2.xyz, a_vpos.xyz) + xformRow2.w);
        float depth = -p.z;
        clip.x = (proj.x * frame.x - 1.0) * depth + p.x * proj.z * frame.x;
        clip.y = (1.0 - proj.y * frame.y) * depth - p.y * proj.w * frame.y;
        clip.z = (depth - frame.z) / frame.w * 0.999 * depth;
        clip.w = depth;
        vpos.xyz = p;
        normal.xyz = vec3(dot(xformRow0.xyz, a_normal.xyz), dot(xformRow1.xyz, a_normal.xyz),
                          dot(xformRow2.xyz, a_normal.xyz));
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
    v_light = a_light;
    v_vpos = vpos;
    v_mat = a_mat;
    v_uv = a_uv;
    v_slice = vec2(sliceNear, sliceSize);
}
