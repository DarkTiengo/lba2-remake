#version 450
#extension GL_GOOGLE_include_directive : require
/* The smooth terrain, grown on the GPU from the cube's height map.

   The capture writes one record a land triangle near the camera: its three
   corners as flat vertices (T_GPUOBJ_VERTEX, placed and shaded as the classic
   terrain lies), each carrying its grid position in uv.zw, its share of the
   curve in normal.x and the height map it belongs to in normal.y. The draw has
   no vertex input: every triangle becomes forty-eight vertices here, sixteen
   small triangles over a four-way subdivision, each point moved from the
   triangle's plane toward the Catmull-Rom curve through the heights around it.
   It is TERRAIN_GPU.CPP's SmoothTri, which used to do this on the CPU, to the
   same formulas. */

layout(location = 0) out vec4 v_normal;
layout(location = 1) flat out vec4 v_light;
layout(location = 2) out vec4 v_vpos;
layout(location = 3) flat out vec4 v_mat;
layout(location = 4) out vec4 v_uv;
layout(location = 5) flat out vec2 v_slice;

layout(std430, set = 0, binding = 0) readonly buffer Records {
    vec4 records4[]; // the vertex array: six vec4 a vertex
};
layout(std430, set = 0, binding = 1) readonly buffer Heights {
    float heights[]; // GPUOBJ_HEIGHTMAP_SIDE squared a map
};

#include "PLACE.glsl"

const int GRID_SIDE = 65;
const float SUBDIV = 4.0;
const float MAX_OFFSET = 220.0;
const float MAX_SINK = 0.0;
const float FADE_NEAR = 18000.0;
const float FADE_FAR = 26000.0;

/* The small triangles' corners, (i, j) of (i / 4, j / 4, rest) in the original's
   barycentric coordinates, in SmoothTri's order. */
const ivec2 kSub[48] = ivec2[48](
    ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 0), ivec2(1, 1), ivec2(0, 1), ivec2(0, 1), ivec2(1, 1),
    ivec2(0, 2), ivec2(1, 1), ivec2(1, 2), ivec2(0, 2), ivec2(0, 2), ivec2(1, 2), ivec2(0, 3), ivec2(1, 2),
    ivec2(1, 3), ivec2(0, 3), ivec2(0, 3), ivec2(1, 3), ivec2(0, 4), ivec2(1, 0), ivec2(2, 0), ivec2(1, 1),
    ivec2(2, 0), ivec2(2, 1), ivec2(1, 1), ivec2(1, 1), ivec2(2, 1), ivec2(1, 2), ivec2(2, 1), ivec2(2, 2),
    ivec2(1, 2), ivec2(1, 2), ivec2(2, 2), ivec2(1, 3), ivec2(2, 0), ivec2(3, 0), ivec2(2, 1), ivec2(3, 0),
    ivec2(3, 1), ivec2(2, 1), ivec2(2, 1), ivec2(3, 1), ivec2(2, 2), ivec2(3, 0), ivec2(4, 0), ivec2(3, 1));

int g_map;

float GridHeight(int x, int z) {
    x = clamp(x, 0, GRID_SIDE - 1);
    z = clamp(z, 0, GRID_SIDE - 1);
    return heights[g_map + z * GRID_SIDE + x];
}

float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

float CurveHeight(float x, float z) {
    int ix = int(x);
    int iz = int(z);
    float u = x - float(ix);
    float t = z - float(iz);
    float row[4];
    for (int k = 0; k < 4; k++) {
        int gz = iz - 1 + k;
        row[k] = CatmullRom(GridHeight(ix - 1, gz), GridHeight(ix, gz), GridHeight(ix + 1, gz), GridHeight(ix + 2, gz), u);
    }
    return CatmullRom(row[0], row[1], row[2], row[3], t);
}

/* SmoothOffsetAt: how far the point rises toward the curve. */
float Rise(float gx, float gz, float plane, float cornerWeight, float depth) {
    float w = clamp((FADE_FAR - depth) / (FADE_FAR - FADE_NEAR), 0.0, 1.0);
    float shore = clamp((plane - 64.0) / 320.0, 0.0, 1.0);
    float border = min(min(gx, gz), min(float(GRID_SIDE - 1) - gx, float(GRID_SIDE - 1) - gz));
    border = min(border, 1.0);
    float offset = clamp(CurveHeight(gx, gz) - plane, -MAX_SINK, MAX_OFFSET);
    return offset * w * shore * border * cornerWeight;
}

vec4 Field(int vertex, int field) {
    return records4[vertex * 6 + field];
}

void main() {
    int tri = gl_VertexIndex / 48;
    ivec2 sub = kSub[gl_VertexIndex % 48];
    vec3 b = vec3(float(sub.x), float(sub.y), 0.0) / SUBDIV;
    b.z = 1.0 - b.x - b.y;

    int first = int(records.x + 0.5) + tri * 3;
    vec4 normal0 = Field(first, 1), normal1 = Field(first + 1, 1), normal2 = Field(first + 2, 1);
    vec4 vpos0 = Field(first, 3), vpos1 = Field(first + 1, 3), vpos2 = Field(first + 2, 3);
    vec4 uv0 = Field(first, 5), uv1 = Field(first + 1, 5), uv2 = Field(first + 2, 5);

    g_map = int(normal0.y + 0.5) * GRID_SIDE * GRID_SIDE;
    /* The original triangle's plane: straight along its edges and the cell's
       diagonal, as the classic terrain lies. */
    float plane = b.x * GridHeight(int(uv0.z + 0.5), int(uv0.w + 0.5)) +
                 b.y * GridHeight(int(uv1.z + 0.5), int(uv1.w + 0.5)) +
                 b.z * GridHeight(int(uv2.z + 0.5), int(uv2.w + 0.5));
    vec2 grid = b.x * uv0.zw + b.y * uv1.zw + b.z * uv2.zw;
    vec3 p = b.x * vpos0.xyz + b.y * vpos1.xyz + b.z * vpos2.xyz;
    float cornerWeight = b.x * normal0.x + b.y * normal1.x + b.z * normal2.x;
    /* A unit of height in the draw's view space: the world's up. */
    vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
    p += Rise(grid.x, grid.y, plane, cornerWeight, -p.z) * up;

    vec3 n = vec3(0.0, 0.0, 1.0);
    Reframe(p, n);
    vec4 clip = Place(p);
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = vec4(n, b.x * normal0.w + b.y * normal1.w + b.z * normal2.w);
    v_light = Field(first, 2);
    v_vpos = vec4(p, vpos0.w);
    v_mat = Field(first, 4);
    v_uv = vec4(b.x * uv0.xy + b.y * uv1.xy + b.z * uv2.xy, 0.0, 0.0);
    v_slice = vec2(sliceNear, sliceSize);
}
