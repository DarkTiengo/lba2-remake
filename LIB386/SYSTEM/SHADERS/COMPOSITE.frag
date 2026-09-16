#version 450
/* Presents the software frame, swapping in the GPU colour wherever the pixel
   still shows what a GPU-rendered body wrote (tag == id written by the GPU). */

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 o_color;

layout(set = 2, binding = 0) uniform sampler2D u_frame;
layout(set = 2, binding = 1) uniform sampler2D u_objColor;
layout(set = 2, binding = 2) uniform sampler2D u_objId; // R32F draw ids
layout(set = 2, binding = 3) uniform sampler2D u_tags;  // R32F draw ids per pixel

layout(set = 3, binding = 0) uniform Params {
    float debugTint;   // > 0: tint GPU pixels green (LBA2_GPU_DEBUG)
    float supersample; // > 0: the GPU targets are larger than the screen area
};

void main() {
    vec4 frame = texture(u_frame, v_uv) * v_color;

    /* Tags are per software-frame pixel; the GPU targets may be larger (the
       window's resolution), so each is addressed in its own texels. */
    ivec2 tagSize = textureSize(u_tags, 0);
    ivec2 tp = clamp(ivec2(v_uv * vec2(tagSize)), ivec2(0), tagSize - ivec2(1));
    ivec2 objSize = textureSize(u_objId, 0);
    ivec2 p = clamp(ivec2(v_uv * vec2(objSize)), ivec2(0), objSize - ivec2(1));
    float tag = texelFetch(u_tags, tp, 0).r;
    float id = texelFetch(u_objId, p, 0).r;

    if (tag > 0.0 && abs(tag - id) < 0.5) {
        /* Colour is cleared to alpha 0 where no body is drawn, so a filtered
           read divided by its alpha averages only body texels at the edge. */
        vec3 gpu;
        if (supersample > 0.0) {
            vec4 c = textureLod(u_objColor, v_uv, 0.0);
            gpu = c.a > 0.0 ? c.rgb / c.a : texelFetch(u_objColor, p, 0).rgb;
        } else {
            gpu = texelFetch(u_objColor, p, 0).rgb;
        }
        gpu = mix(gpu, vec3(0.0, 1.0, 0.0), debugTint * 0.5);
        o_color = vec4(gpu, frame.a);
    } else {
        o_color = frame;
    }
}
