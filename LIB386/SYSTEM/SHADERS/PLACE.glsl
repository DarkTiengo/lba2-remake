/* The draw uniform and the placement both object vertex shaders share
   (GPUOBJ.vert, GPUTERRAIN.vert). */

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
    vec4 records; // GPUTERRAIN.vert: x the first vertex of the draw's triangle records
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

/* A view-space point and its normal moved into this frame's camera, when the
   draw was kept from an earlier one (xformRow0 is zero otherwise). */
bool Reframe(inout vec3 p, inout vec3 n) {
    if (xformRow0.xyz == vec3(0.0)) {
        return false;
    }
    p = vec3(dot(xformRow0.xyz, p) + xformRow0.w, dot(xformRow1.xyz, p) + xformRow1.w,
             dot(xformRow2.xyz, p) + xformRow2.w);
    n = vec3(dot(xformRow0.xyz, n), dot(xformRow1.xyz, n), dot(xformRow2.xyz, n));
    return true;
}
