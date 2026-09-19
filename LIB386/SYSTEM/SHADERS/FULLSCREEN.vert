#version 450
/* A triangle covering the whole target, for the GPU renderer's own full-screen
   passes: the colour and uv the SDL composite's vertices give its shader. */

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_color = vec4(1.0);
    v_uv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
