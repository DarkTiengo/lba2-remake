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
   same formulas.

   A grass draw's records are tufts, one vertex each: the root on the ground
   as drawn, where GrassTri found grass. Each becomes three blades, nine
   vertices, spread, leant and sized by the same hashes GrassTri used, facing
   the camera and bent by the wind. */

layout(location = 0) out vec4 v_normal;
layout(location = 1) flat out vec4 v_light;
layout(location = 2) out vec4 v_vpos;
layout(location = 3) flat out vec4 v_mat;
layout(location = 4) out vec4 v_uv;
layout(location = 5) flat out vec2 v_slice;
layout(location = 6) out float v_waterCoast;

layout(std430, set = 0, binding = 0) readonly buffer Records {
    vec4 records4[]; // the vertex array: six vec4 a vertex
};
layout(std430, set = 0, binding = 1) readonly buffer Heights {
    float heights[]; // GPUOBJ_HEIGHTMAP_SIDE squared a map
};
layout(std430, set = 0, binding = 2) readonly buffer LandBuffer {
    uvec4 landRecords[]; // two a record, GPUOBJ_LAND_MAX records a slot: (LAND_* bits, shading, uv0, uv1), (uv2, -)
};
layout(std430, set = 0, binding = 3) readonly buffer ShoreBuffer {
    float shoreWeights[];
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

/* GrassHash: an integer hash to [0, 1). */
float GrassHash(uint a, uint b, uint c) {
    uint h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

/* A world offset in the draw's view space. */
vec3 World(vec3 d) {
    return vec3(dot(capRow0.xyz, d), dot(capRow1.xyz, d), dot(capRow2.xyz, d));
}

void Grass() {
    int tuft = int(records.x + 0.5) + gl_VertexIndex / 9;
    int blade = (gl_VertexIndex % 9) / 3;
    int corner = gl_VertexIndex % 3;
    vec4 normal = Field(tuft, 1);
    vec4 root = Field(tuft, 3);
    float grow = normal.x;
    uint seed = uint(normal.y + 0.5);
    uint k = seed & 15u;
    uint key = seed >> 4;
    uint cell = uint(normal.z + 0.5);
    uint xi = cell & 255u;
    uint zi = cell >> 8;
    uint j = uint(blade);
    float h0 = GrassHash(k * 3u + j, xi * 31u + key, zi * 17u);
    float h1 = GrassHash(zi * 5u + key, k * 3u + j, xi * 11u);
    float height = (55.0 + 60.0 * h0) * grow;
    float halfWidth = 9.0 + 5.0 * h1;
    float angle = (float(blade) + h1) * 2.0944;
    vec2 out2 = vec2(cos(angle), sin(angle));
    /* The camera's right, level. */
    vec2 right = normalize(vec2(capRow0.x, capRow0.z));
    const vec2 windDir = vec2(0.94, 0.34); /* one wind over the island */

    vec3 base = vec3(out2.x * 18.0 * h0, 0.0, out2.y * 18.0 * h0);
    vec3 d;
    float shade;
    if (corner == 0) {
        d = base + vec3(-right.x * halfWidth, -6.0, -right.y * halfWidth);
        shade = normal.w * 0.62;
    } else if (corner == 1) {
        d = base + vec3(right.x * halfWidth, -6.0, right.y * halfWidth);
        shade = normal.w * 0.62;
    } else {
        float lean = height * (0.18 + 0.22 * h1);
        d = base + vec3(out2.x * lean, height, out2.y * lean);
        shade = min(normal.w * 1.08 + 0.6, 15.0);
    }
    vec3 p = root.xyz + World(d);
    vec3 n = vec3(0.0, 0.0, 1.0);
    vec4 clip = Place(p);
    if (corner == 2) {
        /* The tip, bent: gusts travel over the field (the phase follows the
           ground), each blade flutters in them. As GPUOBJ.vert did, along the
           clip-space offset of a unit of wind. */
        vec3 sway = vec3(windDir.x * height * 0.3, -height * 0.05, windDir.y * height * 0.3);
        vec4 moved = Place(p + World(sway));
        float ph = Field(tuft, 4).w;
        float gust = 0.5 + 0.5 * sin(time * 1.1 - ph * 0.8);
        float flutter = sin(time * 4.3 + ph * 5.3);
        clip.xy += (moved.xy - clip.xy) * (wind * (0.2 + 0.8 * gust + 0.15 * flutter));
    }
    if (grow <= 0.0) {
        clip = vec4(0.0, 0.0, 0.0, 1.0);
    }
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = vec4(n, shade);
    v_light = Field(tuft, 2);
    v_vpos = vec4(p, root.w);
    v_mat = Field(tuft, 4);
    v_uv = vec4(Field(tuft, 5).xy, 0.0, 0.0);
    v_slice = vec2(sliceNear, sliceSize);
    v_waterCoast = 0.0;
}

/* A cube's land: one record a fill of a half-cell, as DrawFeuillePolyZBuf
   fills it. Flat far from the camera (three vertices a record), or cut into
   sixteen on the curve (forty-eight) when the cube reaches into the curve's
   range. The camera comes from the uniform: the rotation the draw was captured
   with and where the cube's origin lies in view space. */
const uint LAND_MAX = 16384u;
const uint LAND_HALF = 1u << 12;
const uint LAND_SENS = 1u << 13;
const uint LAND_WATER = 1u << 14;
const uint LAND_LAVA = 1u << 26;
const uint LAND_CHROMAKEY = 1u << 15;
const uint LAND_VERTEX_SHADE = 1u << 16;
const uint LAND_UV = 1u << 24;
const ivec2 kCorner[4] = ivec2[4](ivec2(0, 0), ivec2(0, 1), ivec2(1, 1), ivec2(1, 0));

void Land() {
    bool curved = landInfo.y > 0.5;
    int per = curved ? 48 : 3;
    int slot = int(landInfo.x + 0.5);
    uint record = (uint(slot) * LAND_MAX + uint(gl_VertexIndex / per)) * 2u;
    uvec4 r = landRecords[record];
    uint uvCorner[3] = uint[3](r.z, r.w, landRecords[record + 1u].x);
    int cell = int(r.x & 0xFFFu);
    ivec2 at = ivec2(cell & 63, cell >> 6);
    int halfIndex = (r.x & LAND_HALF) != 0u ? 1 : 0;
    ivec3 corners = (r.x & LAND_SENS) != 0u ? (halfIndex == 0 ? ivec3(3, 0, 1) : ivec3(1, 2, 3))
                                            : (halfIndex == 0 ? ivec3(0, 1, 2) : ivec3(2, 3, 0));
    g_map = slot * GRID_SIDE * GRID_SIDE;

    vec3 cp[3];
    vec2 cg[3];
    float ch[3];
    float far = land.w;
    bool beyond = false;
    for (int i = 0; i < 3; i++) {
        ivec2 g = at + kCorner[corners[i]];
        cg[i] = vec2(g);
        ch[i] = GridHeight(g.x, g.y);
        vec3 w = vec3(float(g.x) * 512.0, ch[i], float(g.y) * 512.0);
        cp[i] = vec3(dot(capRow0.xyz, w), dot(capRow1.xyz, w), dot(capRow2.xyz, w)) + land.xyz;
        /* A cell reaching past the far clip is not drawn, as the classic walk
           skips it. */
        beyond = beyond || -cp[i].z >= far;
    }

    vec3 b;
    if (curved) {
        ivec2 sub = kSub[gl_VertexIndex % 48];
        b = vec3(float(sub.x), float(sub.y), 0.0) / SUBDIV;
        b.z = 1.0 - b.x - b.y;
    } else {
        int k = gl_VertexIndex % 3;
        b = vec3(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);
    }
    vec3 p = b.x * cp[0] + b.y * cp[1] + b.z * cp[2];
    vec2 grid = b.x * cg[0] + b.y * cg[1] + b.z * cg[2];
    if (curved) {
        float plane = b.x * ch[0] + b.y * ch[1] + b.z * ch[2];
        uint flat3 = (r.x >> 21) & 7u;
        vec3 weight = vec3((flat3 & 1u) != 0u ? 0.0 : 1.0, (flat3 & 2u) != 0u ? 0.0 : 1.0, (flat3 & 4u) != 0u ? 0.0 : 1.0);
        vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
        p += Rise(grid.x, grid.y, plane, dot(b, weight), -p.z) * up;
    }

    float color = float(r.y & 0xFFu);
    float shade = float((r.y >> 8) & 0xFFu);
    if ((r.x & LAND_VERTEX_SHADE) != 0u) {
        vec3 light;
        for (int i = 0; i < 3; i++) {
            float raw = float((r.y >> (16 + 4 * i)) & 15u);
            light[i] = max(raw * 256.0 - 255.0, 0.0) / 256.0;
        }
        shade = dot(b, light);
    }
    vec2 uv = vec2(0.0);
    if ((r.x & LAND_UV) != 0u) {
        for (int i = 0; i < 3; i++) {
            uint packed = uvCorner[i];
            uv += b[i] * vec2(float(packed & 0xFFFFu), float(packed >> 16)) / 256.0;
        }
    }
    int flags = 4; /* GPUOBJ_FLAG_BAKED */
    if ((r.x & LAND_CHROMAKEY) != 0u) {
        flags |= 1;
    }
    vec2 phase = grid + landInfo.zw;
    if ((r.x & LAND_WATER) != 0u) {
        flags |= 16 | 64; /* GPUOBJ_FLAG_WATER | GPUOBJ_FLAG_WATER_TERRAIN */
    } else {
        flags |= 2048; /* GPUOBJ_FLAG_TERRAIN */
        if ((r.x & LAND_LAVA) != 0u) {
            flags |= 4096; /* GPUOBJ_FLAG_LAVA */
        }
    }

    vec3 n = vec3(0.0, 0.0, 1.0);
    Reframe(p, n);
    vec4 clip = beyond ? vec4(0.0, 0.0, 0.0, 1.0) : Place(p);
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = vec4(n, shade);
    v_light = vec4(0.0, 0.0, 0.0, float((r.x >> 17) & 15u));
    v_vpos = vec4(p, float(flags));
    v_mat = vec4(color, 0.0, 65535.0, 0.0);
    v_uv = vec4(uv, phase);
    v_slice = vec2(sliceNear, sliceSize);
    v_waterCoast = (r.x & LAND_WATER) != 0u ? dot(b, vec3(ch[0], ch[1], ch[2])) : 0.0;
}

/* The sea: a record a tile of the classic sea, its four corners as
   TERRAIN_GPU.CPP's WaterVertex places them, cut into 8 x 8 cells in its order,
   each point raised by the swell as GPUOBJ.vert raises a captured one. */
const int SEA_SUB = 8;
const ivec2 kSeaStep[6] = ivec2[6](ivec2(0, 0), ivec2(0, 1), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(1, 0));

float ShoreAt(ivec2 cell) {
    return shoreWeights[cell.y * 1025 + cell.x];
}

void ShoreSample(vec2 grid, out float weight, out vec2 gradient) {
    weight = 1.0;
    gradient = vec2(0.0);
    if (waves.w < 0.5 || grid.x < 0.0 || grid.y < 0.0 || grid.x >= 1024.0 || grid.y >= 1024.0) return;
    ivec2 cell = ivec2(grid);
    vec2 f = fract(grid);
    float a = ShoreAt(cell), b = ShoreAt(cell + ivec2(1, 0));
    float c = ShoreAt(cell + ivec2(0, 1)), d = ShoreAt(cell + ivec2(1, 1));
    weight = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    gradient = vec2(mix(b - a, d - c, f.y), mix(c - a, d - b, f.x)) / 512.0;
}

void Sea() {
    int per = SEA_SUB * SEA_SUB * 6;
    int first = int(records.x + 0.5) + (gl_VertexIndex / per) * 4;
    int inside = gl_VertexIndex % per;
    int cell = inside / 6;
    ivec2 step = kSeaStep[inside % 6];
    float u = float(cell % SEA_SUB + step.x) / float(SEA_SUB);
    float t = float(cell / SEA_SUB + step.y) / float(SEA_SUB);
    /* PlanePoint's weights of corners 0 (0, 0), 1 (0, 1), 2 (1, 1), 3 (1, 0). */
    vec4 w = vec4((1.0 - u) * (1.0 - t), (1.0 - u) * t, u * t, u * (1.0 - t));
    vec3 p = w.x * Field(first, 3).xyz + w.y * Field(first + 1, 3).xyz + w.z * Field(first + 2, 3).xyz +
             w.w * Field(first + 3, 3).xyz;
    vec4 uv = w.x * Field(first, 5) + w.y * Field(first + 1, 5) + w.z * Field(first + 2, 5) + w.w * Field(first + 3, 5);
    vec4 normal = Field(first, 1);
    int flags = int(floor(Field(first, 3).w + 0.5));
    bool lava = (flags & FLAG_LAVA) != 0;
    float shore;
    vec2 shoreGradient;
    ShoreSample(uv.zw, shore, shoreGradient);
    if (lava && records.z > 0.5) {
        vec3 n;
        float rawHeight = LavaSwell(uv.zw * 512.0, n);
        vec2 slope = -n.xz / max(n.y, 0.001) * shore + rawHeight * shoreGradient;
        float h = rawHeight * shore;
        vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
        p += h * up;
        normal = vec4(World(normalize(vec3(-slope.x, 1.0, -slope.y))), h);
    } else if (!lava && waves.z > 0.5) {
        vec3 n;
        float rawHeight = Swell(uv.zw * 512.0, n);
        vec2 slope = -n.xz / max(n.y, 0.001) * shore + rawHeight * shoreGradient;
        float h = rawHeight * shore;
        vec3 up = vec3(capRow0.y, capRow1.y, capRow2.y);
        p += h * up;
        normal = vec4(World(normalize(vec3(-slope.x, 1.0, -slope.y))), h);
    }
    vec3 n = normal.xyz;
    if (Reframe(p, n)) {
        normal.xyz = n;
    }
    vec4 clip = Place(p);
    gl_Position = vec4(clip.xy, sliceNear * clip.w + clip.z * sliceSize, clip.w);
    v_normal = normal;
    v_light = Field(first, 2);
    v_vpos = vec4(p, Field(first, 3).w);
    v_mat = Field(first, 4);
    v_uv = uv;
    v_slice = vec2(sliceNear, sliceSize);
    v_waterCoast = lava ? 0.0 : shore;
}

void main() {
    if (records.y > 3.5) {
        Sea();
        return;
    }
    if (records.y > 2.5) {
        Land();
        return;
    }
    if (records.y > 1.5) {
        Grass();
        return;
    }
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
    vec2 materialGrid = vec2(0.0);
    int base = int(Field(first, 4).x + 0.5);
    if ((int(vpos0.w + 0.5) & 2048) != 0 && int(Field(first, 2).w + 0.5) == 6 &&
        (base == 27 || base == 107)) {
        materialGrid = grid + landInfo.zw;
    }
    v_uv = vec4(b.x * uv0.xy + b.y * uv1.xy + b.z * uv2.xy, materialGrid);
    v_slice = vec2(sliceNear, sliceSize);
    v_waterCoast = 0.0;
}
