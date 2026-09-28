/* Slow, viscous motion shared by the lava vertex and fragment paths. World is
   in scene units; the 256-second clock loops with every component in phase. */
void LavaField(vec2 world, float clock, out float height, out vec2 slope,
               out vec2 warp, out float flow, out float fine) {
    const vec2 d0 = vec2(0.819232, 0.573462);
    const vec2 d1 = vec2(-0.503871, 0.863779);
    const vec2 d2 = vec2(0.219512, -0.975610);
    const vec2 d3 = vec2(-0.940887, 0.338719);
    const float cycle = 6.28318530718 / 256.0;
    vec2 p = world / 512.0;
    float a = dot(p, d0) * 0.28 + clock * 3.0 * cycle;
    float b = dot(p, d1) * 0.51 - clock * 5.0 * cycle + 1.7;
    float c = dot(p, d2) * 0.94 + clock * 7.0 * cycle - 0.8;
    float d = dot(p, d3) * 1.70 - clock * 9.0 * cycle + 2.3;

    height = sin(a) * 24.0 + sin(b) * 10.0 + sin(c) * 4.0 + sin(d) * 1.5;
    slope = d0 * (cos(a) * 24.0 * 0.28 / 512.0) +
            d1 * (cos(b) * 10.0 * 0.51 / 512.0) +
            d2 * (cos(c) * 4.0 * 0.94 / 512.0) +
            d3 * (cos(d) * 1.5 * 1.70 / 512.0);
    warp = vec2(sin(a + c) * 0.042 + sin(d) * 0.018,
                cos(b - c) * 0.038 + cos(a + d) * 0.016);
    flow = clamp(0.5 + sin(a) * 0.25 + sin(b) * 0.16 + sin(c) * 0.09, 0.0, 1.0);
    fine = clamp(0.5 + sin(c + d) * 0.30 + cos(b * 2.0 - d) * 0.20, 0.0, 1.0);
}
