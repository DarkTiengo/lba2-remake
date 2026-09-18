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
    slope += WaterWave(p, normalize(vec2(0.18, -0.98)), 0.72, 25.0, 0.016);
    slope += WaterWave(p, normalize(vec2(0.97, 0.23)), 1.28, -37.0, 0.007);
    /* Wind roughens the swell and adds short-wave energy, but never overwhelms
       the base colour. */
    slope += WaterWave(p, normalize(vec2(0.72, -0.69)), 0.18, 15.0, 0.040 * storm);
    slope += WaterWave(p, normalize(vec2(-0.12, -0.99)), 0.46, -23.0, 0.021 * storm);
    float impactFoam;
    slope += WaterImpactWaves(p, impactFoam);
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
    vec2 waterUv = p * 0.13 + vec2(0.5) + slope * (0.35 + storm * 0.12);
    vec3 baseFine = WaterTexture(waterUv, 0);
    vec3 baseBroad = WaterTexture(p * 0.047 + vec2(0.17, 0.63) - slope * 0.18, 0);
    vec3 base = mix(baseFine, baseBroad, 0.36);
    vec3 color = mix(base * (0.90 + 0.10 * facing), sky, 0.08 + fresnel * 0.62);
    vec3 light = normalize(v_light.xyz);
    vec3 halfVector = normalize(view + light);
    float highlight = pow(max(dot(viewNormal, halfVector), 0.0), 72.0) *
                      max(dot(viewNormal, light), 0.0);
    /* A palette-derived glint cannot remain bright during a fade to black. */
    color += max(sky, base) * highlight * specular * (0.32 + 0.18 * storm);
    vec3 foamColor = min(max(sky, base) * 1.38, vec3(0.92));
    float surfaceFoam = impactFoam * (0.46 + 0.16 * storm);
    color = mix(color, foamColor, surfaceFoam);
    return color;
}
