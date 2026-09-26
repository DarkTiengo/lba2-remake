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
    vec4 waves; // x seconds in the sea's 256-second cycle, y storm, z 1 when the swell is on
    vec4 capRow0; // the rotation the draw was captured with, a row each:
    vec4 capRow1; // a world direction d is (dot(row0, d), dot(row1, d), dot(row2, d))
    vec4 capRow2; // in the draw's own view space (vpos.z the negated depth)
};

const int FLAG_GRASS = 512;
const int FLAG_WAVES = 1024;

/* The sea's broad swell at world (x, z): the same five components as
   GpuWater_SampleSurface (SVGA/GPUWATER.CPP) and WATER.glsl. Returns the
   height; the world normal in n. */
float Swell(vec2 world, out vec3 n) {
    const vec2 direction[5] = vec2[5](vec2(0.859292, 0.511483), vec2(-0.430315, 0.902679), vec2(0.180818, -0.983517),
                                      vec2(0.972996, 0.230817), vec2(0.721105, -0.692826));
    const float frequency[5] = float[5](0.24, 0.38, 0.72, 1.28, 0.18);
    const float speed[5] = float[5](11.0, -16.0, 25.0, -37.0, 15.0);
    const float amplitude[5] = float[5](106.6667, 45.8106, 14.2223, 4.0, 113.7778);
    vec2 p = world / 512.0;
    float h = 0.0;
    vec2 d = vec2(0.0);
    for (int i = 0; i < 5; i++) {
        float scale = i == 4 ? waves.y : 1.0;
        float phase = dot(p, direction[i]) * frequency[i] + waves.x * speed[i] * (6.28318530718 / 256.0);
        float rise = amplitude[i] * scale;
        h += rise * sin(phase);
        d += direction[i] * (rise * cos(phase) * frequency[i] / 512.0);
    }
    n = vec3(-d.x, 1.0, -d.y) / sqrt(dot(d, d) + 1.0);
    return h;
}

/* AffGpu_ViewVertex's placement of a view-space point. */
vec4 Place(vec3 p) {
    float depth = -p.z;
    return vec4((proj.x * frame.x - 1.0) * depth + p.x * proj.z * frame.x,
                (1.0 - proj.y * frame.y) * depth - p.y * proj.w * frame.y,
                (depth - frame.z) / frame.w * 0.999 * depth, depth);
}

void main() {
    vec4 clip = a_clip;
    vec4 vpos = a_vpos;
    vec4 normal = a_normal;
    bool placed = false;
    if ((int(a_vpos.w + 0.5) & FLAG_WAVES) != 0 && waves.z > 0.5) {
        /* The sea: raised along the world's up, in the space it was captured in. */
        vec3 n;
        float h = Swell(a_uv.zw * 512.0, n);
        vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
        vpos.xyz += h * up;
        normal = vec4(dot(capRow0.xyz, n), dot(capRow1.xyz, n), dot(capRow2.xyz, n), h);
        placed = true;
    }
    if (xformRow0.xyz != vec3(0.0)) {
        /* The same placement the capture makes, from the same formulas
           (AffGpu_ViewVertex), on a point moved into this frame's camera. */
        vec3 p = vec3(dot(xformRow0.xyz, vpos.xyz) + xformRow0.w,
                      dot(xformRow1.xyz, vpos.xyz) + xformRow1.w,
                      dot(xformRow2.xyz, vpos.xyz) + xformRow2.w);
        normal.xyz = vec3(dot(xformRow0.xyz, normal.xyz), dot(xformRow1.xyz, normal.xyz),
                          dot(xformRow2.xyz, normal.xyz));
        vpos.xyz = p;
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
    v_light = a_light;
    v_vpos = vpos;
    v_mat = a_mat;
    v_uv = a_uv;
    v_slice = vec2(sliceNear, sliceSize);
}
