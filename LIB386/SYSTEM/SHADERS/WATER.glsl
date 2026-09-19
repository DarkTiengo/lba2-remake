/* Exterior sea material. World-space phase keeps adjacent tiles and camera
   movement continuous. The palette remains the source of every colour so
   fades, lightning and scene colour changes reach both water and reflection. */
layout(set = 3, binding = 3) uniform Water {
    vec4 waterInfo; // game time (seconds), enabled, rain strength, padding
    vec4 waterAxisX;
    vec4 waterAxisY;
    vec4 waterAxisZ;
    vec4 waterImpacts[8]; // island x/z, start time, strength
};

vec3 WaterTexel(ivec2 p, int offset) {
    /* Match Texel(): the sea and sky share the 256x256 page, and the sky
       starts at a linear offset of 128 bytes rather than at x = 128. */
    int index = (offset + ((((p.y & 255) << 8) | (p.x & 255)))) & 65535;
    int texel = int(texelFetch(u_pages, ivec3(index & 255, index >> 8, int(page)), 0).r * 255.0 + 0.5);
    return Pal(Logical(texel));
}

vec3 WaterTexture(vec2 uv, int offset) {
    vec2 p = uv - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = fract(p);
    return mix(mix(WaterTexel(i, offset), WaterTexel(i + ivec2(1, 0), offset), f.x),
               mix(WaterTexel(i + ivec2(0, 1), offset), WaterTexel(i + ivec2(1, 1), offset), f.x), f.y);
}

/* The indexed page contains deliberate pixel accents. A tiny world-space
   average keeps those accents as soft glints when the sea is viewed obliquely
   instead of producing isolated white squares. */
vec3 WaterTextureSoft(vec2 uv, int offset) {
    vec2 d = vec2(0.10, 0.0);
    return WaterTexture(uv, offset) * 0.60 +
           (WaterTexture(uv + d, offset) + WaterTexture(uv - d, offset)) * 0.20;
}

float WaterHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec2 WaterDetailNormal(vec2 p, out float detail) {
    /* Directional sine components avoid the square cadence of a sampled or
       cell-noise normal while retaining a technical, asset-free detail field. */
    vec2 q = p + vec2(0.37, -0.18) * waterInfo.x * 0.08;
    vec2 d0 = normalize(vec2(0.83, 0.56));
    vec2 d1 = normalize(vec2(-0.62, 0.78));
    vec2 d2 = normalize(vec2(0.27, -0.96));
    float a = dot(q, d0) * 3.1 + waterInfo.x * 0.51;
    float b = dot(q, d1) * 4.7 - waterInfo.x * 0.73 + 1.4;
    float c = dot(q, d2) * 7.3 + waterInfo.x * 0.94 - 2.1;
    vec2 slope = d0 * cos(a) * 0.010 + d1 * cos(b) * 0.007 + d2 * cos(c) * 0.004;
    detail = 0.5 + 0.5 * (sin(a) * 0.55 + sin(b) * 0.30 + sin(c) * 0.15);
    return slope;
}

/* Rain drops have a jittered centre, independent phase and independent speed.
   This avoids a synchronized checkerboard while retaining a bounded shader
   cost and world anchoring. */
vec2 RainRipples(vec2 p, out float glint) {
    /* p is island-space units / 512. A 5.5-cell field keeps a rain ring in
       the 30--45 scene-unit range instead of making one ring half a person. */
    vec2 grid = p * 5.5;
    /* Derivatives of a floor/cell lookup are undefined at the cell edge. Take
       the pixel footprint from the continuous field before introducing those
       discontinuities, then clamp it so perspective cannot turn a drop into a
       broad square or a one-pixel alias. */
    vec2 footprint = max(abs(dFdx(grid)), abs(dFdy(grid)));
    float footprintSize = max(footprint.x, footprint.y);
    const float ringWidth = 0.045;
    float resolvable = 1.0 - smoothstep(0.16, 0.42, footprintSize);
    vec2 base = floor(grid);
    vec2 slope = vec2(0.0);
    glint = 0.0;
    /* Include neighbours so a drop remains a complete ring as it crosses a
       cell boundary; the fixed 3x3 loop is cheap and has no visible lattice. */
    for (int ix = -1; ix <= 1; ix++) {
        for (int iy = -1; iy <= 1; iy++) {
            vec2 cell = base + vec2(float(ix), float(iy));
            vec2 centre = vec2(WaterHash(cell + vec2(8.3, 2.1)),
                               WaterHash(cell + vec2(3.7, 9.4))) * 0.72 + 0.14;
            vec2 delta = grid - cell - centre;
            float distance = length(delta);
            float seed = WaterHash(cell + vec2(17.2, 31.8));
            /* A sparse, per-cell event field is less regular than a drop in
               every tile. Re-jitter the centre and phase at each event so the
               bounded grid does not settle into a repeating circular lattice. */
            float speed = 1.4 + seed * 1.9;
            float eventTime = waterInfo.x * speed + seed * 17.0;
            float eventCycle = floor(eventTime);
            float eventSeed = WaterHash(cell + vec2(91.4, 42.6) + eventCycle);
            if (eventSeed > 0.38) {
                continue;
            }
            centre += (vec2(WaterHash(cell + vec2(63.1, 27.8) + eventCycle),
                            WaterHash(cell + vec2(14.6, 78.2) + eventCycle)) - 0.5) * 0.16;
            delta = grid - cell - centre;
            distance = length(delta);
            float age = fract(eventTime + eventSeed * 0.24);
            float radius = age * (0.35 + seed * 0.10);
            float ring = exp(-pow((distance - radius) / ringWidth, 2.0));
            ring *= smoothstep(0.02, 0.12, age) * (1.0 - smoothstep(0.76, 1.0, age));
            ring *= resolvable * 0.35;
            slope += delta / max(distance, 0.04) * ring;
            glint = max(glint, ring * 0.35);
        }
    }
    return slope * 0.028;
}

vec2 WaterWave(vec2 p, vec2 direction, float frequency, float speed, float slope) {
    float phase = dot(p, direction) * frequency + waterInfo.x * speed * (6.28318530718 / 256.0);
    /* Fine capillary waves disappear into the average instead of crawling as
       stripes when a distant water polygon covers only a few pixels. */
    float attenuation = 1.0 - smoothstep(0.55, 2.0, fwidth(phase));
    return direction * cos(phase) * slope * attenuation;
}

float WaterImpactAge(float start) {
    return mod(waterInfo.x - start + 256.0, 256.0);
}

vec2 WaterImpactWaves(vec2 p, out float foam) {
    vec2 slope = vec2(0.0);
    foam = 0.0;
    for (int k = 0; k < 8; k++) {
        vec4 impact = waterImpacts[k];
        if (impact.w <= 0.0) {
            continue;
        }
        float age = WaterImpactAge(impact.z);
        if (age >= 2.7) {
            continue;
        }
        vec2 delta = p - impact.xy;
        float distance = length(delta);
        float radius = age * (2.9 + impact.w);
        float width = 0.72 + age * 0.28;
        float front = exp(-pow((distance - radius) / width, 2.0));
        /* A broad, damped crest reads as one disturbance; a high carrier
           frequency turns the impact into a stack of razor-thin contour rings. */
        float wake = cos((distance - radius) * 2.1) * front * 0.55;
        float fade = 1.0 - smoothstep(1.3, 2.7, age);
        slope += delta / max(distance, 0.05) * wake * impact.w * fade * 0.14;
        foam = max(foam, front * fade * clamp(impact.w * 0.44, 0.0, 1.0));
    }
    return slope;
}

vec3 WaterColor(out vec3 viewNormal) {
    vec2 p = v_uv.zw;
    float storm = clamp(waterInfo.z, 0.0, 1.0);
    /* A dominant swell and shorter cross-waves share the same world-anchored
       phase. Their restrained slopes describe a displaced surface, rather
       than a height-field of unrelated sine stripes. */
    vec2 slope = WaterWave(p, normalize(vec2(0.86, 0.51)), 0.24, 11.0, 0.050);
    slope += WaterWave(p, normalize(vec2(-0.43, 0.90)), 0.38, -16.0, 0.034);
    slope += WaterWave(p, normalize(vec2(0.18, -0.98)), 0.72, 25.0, 0.020);
    slope += WaterWave(p, normalize(vec2(0.97, 0.23)), 1.28, -37.0, 0.010);
    slope += WaterWave(p, normalize(vec2(0.61, 0.79)), 2.35, 43.0, 0.0045);
    slope += WaterWave(p, normalize(vec2(-0.91, 0.41)), 3.90, -59.0, 0.0025);
    float detail;
    slope += WaterDetailNormal(p, detail);
    /* Wind roughens the swell and adds short-wave energy, but never overwhelms
       the base colour. */
    slope += WaterWave(p, normalize(vec2(0.72, -0.69)), 0.18, 15.0, 0.040 * storm);
    slope += WaterWave(p, normalize(vec2(-0.12, -0.99)), 0.46, -23.0, 0.021 * storm);
    float impactFoam;
    slope += WaterImpactWaves(p, impactFoam);
    float rainGlint = 0.0;
    if (storm > 0.0) {
        slope += RainRipples(p, rainGlint) * storm;
    }
    vec3 normal = normalize(vec3(-slope.x, 1.0, -slope.y));
    mat3 axes = mat3(waterAxisX.xyz, waterAxisY.xyz, waterAxisZ.xyz);
    viewNormal = normalize(axes * normal);
    vec3 view = normalize(-v_vpos.xyz);
    vec3 worldView = transpose(axes) * view;
    float facing = clamp(dot(normal, worldView), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    vec3 reflected = reflect(-worldView, normal);
    vec2 skyUv = vec2(64.0) + reflected.xz / max(abs(reflected.y), 0.25) * 18.0;
    vec3 sky = WaterTexture(skyUv, 128);
    /* The polygon UVs restart at clipped sea-tile boundaries. Sampling the
       palette page from the world phase keeps the colour field continuous
       across those triangles and blends two scales to hide page repetition. */
    vec2 waterUv = p * 0.29 + vec2(0.5) + slope * (0.55 + storm * 0.16);
    vec3 baseFine = WaterTextureSoft(waterUv, 0);
    vec3 baseBroad = WaterTextureSoft(p * 0.105 + vec2(0.17, 0.63) - slope * 0.24, 0);
    vec3 base = mix(baseFine, baseBroad, 0.30);
    vec3 color = mix(base * (0.90 + 0.10 * facing), sky, 0.08 + fresnel * 0.62);
    /* Let the analytic waves show as restrained moving value changes even
       where neighbouring palette texels happen to share the same blue. */
    float ripple = 0.5 + 0.5 * sin(dot(p, normalize(vec2(-0.37, 0.93))) * 2.9 -
                                      waterInfo.x * 0.82);
    color = mix(color, color * (0.92 + ripple * 0.12), 0.42 + storm * 0.18);
    color *= 0.93 + detail * 0.14;
    vec3 light = normalize(v_light.xyz);
    vec3 halfVector = normalize(view + light);
    float highlight = pow(max(dot(viewNormal, halfVector), 0.0), 72.0) *
                      max(dot(viewNormal, light), 0.0);
    /* A palette-derived glint cannot remain bright during a fade to black. */
    color += max(sky, base) * highlight * specular * (0.32 + 0.18 * storm);
    vec3 foamColor = min(max(sky, base) * 1.38, vec3(0.92));
    float surfaceFoam = max(impactFoam * (0.46 + 0.16 * storm), rainGlint * storm * 0.22);
    color = mix(color, foamColor, surfaceFoam);
    return color;
}
