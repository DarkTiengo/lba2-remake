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
    /* Subpixel ripples fade out instead of shimmering at the horizon. */
    float attenuation = 1.0 - smoothstep(0.4, 2.5, fwidth(phase));
    return direction * cos(phase) * slope * attenuation;
}

float WaterHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

/* One short-lived ring per moving world-space cell suggests rain striking the
   surface without spawning hundreds of CPU particles. */
vec2 RainRipples(vec2 p, out float glint) {
    vec2 grid = p * 0.72;
    vec2 cell = floor(grid);
    vec2 centre = vec2(WaterHash(cell), WaterHash(cell + vec2(19.1, 7.7)));
    vec2 delta = fract(grid) - centre;
    float distance = length(delta);
    float age = fract(waterInfo.x * 0.92 + WaterHash(cell + 3.7));
    float radius = age * 0.62;
    float ring = exp(-pow((distance - radius) / 0.055, 2.0)) * (1.0 - age);
    glint = ring;
    return delta / max(distance, 0.025) * ring * 0.055;
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
        float width = 0.5 + age * 0.22;
        float front = exp(-pow((distance - radius) / width, 2.0));
        float wake = cos((distance - radius) * 5.2) * front;
        float fade = 1.0 - smoothstep(1.3, 2.7, age);
        slope += delta / max(distance, 0.05) * wake * impact.w * fade * 0.14;
        foam = max(foam, front * fade * clamp(impact.w * 0.58, 0.0, 1.0));
    }
    return slope;
}

vec3 WaterColor(out vec3 viewNormal) {
    vec2 p = v_uv.zw;
    float storm = clamp(waterInfo.z, 0.0, 1.0);
    vec2 slope = WaterWave(p, vec2(0.8, 0.6), 0.65, 37.0, 0.13);
    slope += WaterWave(p, vec2(-0.6, 0.8), 1.05, -53.0, 0.085);
    slope += WaterWave(p, vec2(0.28, -0.96), 2.3, 83.0, 0.035);
    slope += WaterWave(p, vec2(0.96, 0.28), 4.7, -127.0, 0.018);
    /* Rain scenes retain the original directions but gain long, faster swells
       and small impact rings, so the coast reads as rough rather than noisy. */
    slope += WaterWave(p, vec2(0.94, -0.34), 0.39, 61.0, 0.19 * storm);
    slope += WaterWave(p, vec2(-0.22, -0.98), 0.82, -89.0, 0.12 * storm);
    float rainGlint = 0.0;
    if (storm > 0.0) {
        slope += RainRipples(p, rainGlint) * storm;
    }
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
    vec3 base = WaterTexture(v_uv.xy + slope * (3.0 + storm * 1.4), 0);
    vec3 color = mix(base * (0.88 + 0.12 * facing), sky, 0.12 + fresnel * 0.58);
    vec3 light = normalize(v_light.xyz);
    vec3 halfVector = normalize(view + light);
    float highlight = pow(max(dot(viewNormal, halfVector), 0.0), 72.0) *
                      max(dot(viewNormal, light), 0.0);
    /* A palette-derived glint cannot remain bright during a fade to black. */
    color += max(sky, base) * highlight * specular * 0.7;
    vec3 foamColor = min(max(sky, base) * 1.38, vec3(0.92));
    float surfaceFoam = max(impactFoam * 0.62, rainGlint * storm * 0.24);
    color = mix(color, foamColor, surfaceFoam);
    return color;
}
