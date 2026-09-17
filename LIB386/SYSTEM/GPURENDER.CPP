#include <SYSTEM/GPURENDER.H>

#include <SVGA/GPUOBJ.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/LOG.H>

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

/* The GPU renderer and custom render states arrived in SDL 3.4. Older SDL
   builds keep the software look: the layer is simply never available. */
#if !SDL_VERSION_ATLEAST(3, 4, 0)

bool GpuRender_Requested(void) {
    return false;
}

void *GpuRender_CreateRenderer(void * /*sdlWindow*/) {
    return NULL;
}

void GpuRender_Shutdown(void) {
}

bool GpuRender_BeginComposite(void * /*sdlRenderer*/) {
    return false;
}

void GpuRender_EndComposite(void * /*sdlRenderer*/) {
}

#else

#include "SHADERS/COMPOSITE.frag.spv.h"
#include "SHADERS/GPUOBJ.frag.spv.h"
#include "SHADERS/GPUOBJ.vert.spv.h"

// --- Private state -----------------------------------------------------------
static SDL_Renderer *s_renderer = NULL;
static SDL_GPUDevice *s_device = NULL;

static SDL_GPUShader *s_objVert = NULL;
static SDL_GPUShader *s_objFrag = NULL;
static SDL_GPUShader *s_compositeFrag = NULL;
static SDL_GPUGraphicsPipeline *s_pipeline = NULL;
static SDL_GPUGraphicsPipeline *s_pipelinePainter = NULL; ///< depth write, no depth test
static SDL_GPUSampler *s_nearest = NULL;
static SDL_GPUSampler *s_linear = NULL;
static bool s_supersample = false;
static bool s_upscale = false;     ///< the frame is shown larger than its pixels
static bool s_tagsCleared = false; ///< the tag texture holds known contents

/* Frame-sized: follow Log. */
static SDL_GPUTexture *s_objColor = NULL;
static SDL_GPUTexture *s_objId = NULL;
static SDL_GPUTexture *s_objLight = NULL; ///< dynamic light reaching each scene surface
static SDL_GPUTexture *s_depth = NULL;
static SDL_GPUTexture *s_tags = NULL;
static U32 s_frameW = 0; ///< software frame (tags)
static U32 s_frameH = 0;
static U32 s_targetW = 0; ///< GPU targets: the frame's size on screen
static U32 s_targetH = 0;

/* Largest GPU target side; beyond 4K the gain is not worth the fill rate. */
#define GPU_TARGET_MAX_W 3840
#define GPU_TARGET_MAX_H 2160
static SDL_GPURenderState *s_composite = NULL;

/* Fixed-size lookup data. */
static SDL_GPUTexture *s_palette = NULL;
static SDL_GPUTexture *s_lut = NULL;
static SDL_GPUTexture *s_pages = NULL;
static SDL_GPUTexture *s_atlas = NULL;

static SDL_GPUBuffer *s_vertexBuffer = NULL;
static U32 s_vertexCapacity = 0;
static SDL_GPUTransferBuffer *s_transfer = NULL;
static U32 s_transferCapacity = 0;

static float *s_tagScratch = NULL;
static float *s_tagUpload = NULL;
static S32 s_tagBox[4] = {0, 0, 0, 0}; ///< region of s_tags that may hold ids

#define LUT_ROWS 512

/* Per-draw lookup data resolved for one present. */
typedef struct {
    float Id;
    float LutRow;
    float ClutRow;
    float Page;
    float FogStart;
    float FogEnd;
    float FogColor;
    float Pad;
} T_DRAW_UNIFORMS;

static U8 s_lutData[LUT_ROWS * 256];
static U32 s_lutRows = 0;
static U8 *s_pagePtrs[8];
static U32 s_nbPages = 0;
static SDL_GPUTextureFormat s_depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;

// --- Private functions -------------------------------------------------------
static void ReleaseTexture(SDL_GPUTexture **texture) {
    if (*texture) {
        SDL_ReleaseGPUTexture(s_device, *texture);
        *texture = NULL;
    }
}

static SDL_GPUTexture *CreateTexture(SDL_GPUTextureType type, SDL_GPUTextureFormat format,
                                     SDL_GPUTextureUsageFlags usage, U32 w, U32 h, U32 layers) {
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type = type;
    info.format = format;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = layers;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(s_device, &info);
    if (texture == NULL) {
        Log_Warn("[GPU] Unable to create %ux%u texture: %s", w, h, SDL_GetError());
    }
    return texture;
}

static SDL_GPUShader *CreateShader(const unsigned char *code, size_t size, SDL_GPUShaderStage stage,
                                   Uint32 samplers, Uint32 uniformBuffers) {
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = code;
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniformBuffers;
    SDL_GPUShader *shader = SDL_CreateGPUShader(s_device, &info);
    if (shader == NULL) {
        Log_Warn("[GPU] Unable to create shader: %s", SDL_GetError());
    }
    return shader;
}

static void ReleaseFrameResources(void) {
    if (s_composite) {
        SDL_DestroyGPURenderState(s_composite);
        s_composite = NULL;
    }
    ReleaseTexture(&s_objColor);
    ReleaseTexture(&s_objId);
    ReleaseTexture(&s_objLight);
    ReleaseTexture(&s_depth);
    ReleaseTexture(&s_tags);
    free(s_tagScratch);
    s_tagScratch = NULL;
    free(s_tagUpload);
    s_tagUpload = NULL;
    memset(s_tagBox, 0, sizeof(s_tagBox));
    s_frameW = 0;
    s_frameH = 0;
    s_targetW = 0;
    s_targetH = 0;
}

/* The GPU targets match the frame's size on screen, so bodies render at the
   window's resolution whatever the software frame is. Never below the frame. */
static void TargetSize(U32 w, U32 h, U32 *outW, U32 *outH) {
    SDL_FRect rect;
    U32 tw = w;
    U32 th = h;
    U32 onScreenW = w;
    U32 onScreenH = h;
    if (SDL_GetRenderLogicalPresentationRect(s_renderer, &rect) && rect.w >= 1.0f && rect.h >= 1.0f) {
        tw = (U32)(rect.w + 0.5f);
        th = (U32)(rect.h + 0.5f);
        onScreenW = tw;
        onScreenH = th;
    }
    if (GpuObjTargetHintW > tw || GpuObjTargetHintH > th) {
        tw = SDL_max(tw, GpuObjTargetHintW);
        th = SDL_max(th, GpuObjTargetHintH);
    }
    if (tw > GPU_TARGET_MAX_W || th > GPU_TARGET_MAX_H) {
        /* Keep the aspect while capping. */
        float scale = SDL_min((float)GPU_TARGET_MAX_W / (float)tw, (float)GPU_TARGET_MAX_H / (float)th);
        tw = (U32)((float)tw * scale);
        th = (U32)((float)th * scale);
    }
    if (tw < w || th < h) {
        tw = w;
        th = h;
    }
    s_supersample = tw > onScreenW + 1 || th > onScreenH + 1;
    s_upscale = onScreenW > w + 1 || onScreenH > h + 1;
    *outW = tw;
    *outH = th;
}

/* Composite switches: the player's GPU effects can change at any present. */
static void PushCompositeParams(void) {
    static const char *debug = SDL_getenv("LBA2_GPU_DEBUG");
    float params[4];
    params[0] = (debug != NULL) ? (float)SDL_atoi(debug) : 0.0f;
    params[1] = s_supersample ? 1.0f : 0.0f;
    params[2] = (s_upscale && GpuObjPixelFilter) ? 1.0f : 0.0f;
    params[3] = (s_upscale && GpuObjDeband) ? 1.0f : 0.0f;
    SDL_SetGPURenderStateFragmentUniforms(s_composite, 0, params, sizeof(params));
}

static bool EnsureFrameResources(U32 w, U32 h) {
    U32 tw, th;
    TargetSize(w, h, &tw, &th);
    if (s_composite != NULL && s_frameW == w && s_frameH == h && s_targetW == tw && s_targetH == th) {
        PushCompositeParams();
        return true;
    }
    ReleaseFrameResources();

    const SDL_GPUTextureUsageFlags targetUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    s_objColor = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, targetUsage, tw, th, 1);
    s_objId = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, targetUsage, tw, th, 1);
    s_objLight = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, targetUsage, tw, th, 1);
    s_depth = CreateTexture(SDL_GPU_TEXTURETYPE_2D, s_depthFormat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, tw, th, 1);
    s_tags = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1);
    s_tagScratch = (float *)malloc((size_t)w * h * sizeof(float));
    s_tagUpload = (float *)malloc((size_t)w * h * sizeof(float));
    if (!s_objColor || !s_objId || !s_objLight || !s_depth || !s_tags || !s_tagScratch || !s_tagUpload) {
        ReleaseFrameResources();
        return false;
    }

    SDL_GPUTextureSamplerBinding bindings[4];
    bindings[0].texture = s_objColor;
    bindings[0].sampler = s_linear;
    bindings[1].texture = s_objId;
    bindings[1].sampler = s_nearest;
    bindings[2].texture = s_tags;
    bindings[2].sampler = s_nearest;
    bindings[3].texture = s_objLight;
    bindings[3].sampler = s_nearest;

    SDL_GPURenderStateCreateInfo stateInfo;
    SDL_zero(stateInfo);
    stateInfo.fragment_shader = s_compositeFrag;
    stateInfo.num_sampler_bindings = 4;
    stateInfo.sampler_bindings = bindings;
    s_composite = SDL_CreateGPURenderState(s_renderer, &stateInfo);
    if (s_composite == NULL) {
        Log_Warn("[GPU] Unable to create composite state: %s", SDL_GetError());
        ReleaseFrameResources();
        return false;
    }

    PushCompositeParams();

    s_frameW = w;
    s_frameH = h;
    s_targetW = tw;
    s_targetH = th;
    s_tagsCleared = false;
    return true;
}

static bool EnsureTransfer(U32 size) {
    if (size <= s_transferCapacity) {
        return true;
    }
    if (s_transfer) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_transfer);
        s_transfer = NULL;
    }
    U32 capacity = s_transferCapacity ? s_transferCapacity : (1u << 20);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = capacity;
    s_transfer = SDL_CreateGPUTransferBuffer(s_device, &info);
    s_transferCapacity = s_transfer ? capacity : 0;
    return s_transfer != NULL;
}

static bool EnsureVertexBuffer(U32 size) {
    if (size <= s_vertexCapacity) {
        return true;
    }
    if (s_vertexBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_vertexBuffer);
        s_vertexBuffer = NULL;
    }
    U32 capacity = s_vertexCapacity ? s_vertexCapacity : (1u << 20);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    info.size = capacity;
    s_vertexBuffer = SDL_CreateGPUBuffer(s_device, &info);
    s_vertexCapacity = s_vertexBuffer ? capacity : 0;
    return s_vertexBuffer != NULL;
}

static bool CreatePipeline(void) {
    s_objVert = CreateShader(GPUOBJ_vert_spv, GPUOBJ_vert_spv_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    s_objFrag = CreateShader(GPUOBJ_frag_spv, GPUOBJ_frag_spv_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 2);
    /* The renderer binds the frame texture at slot 0, the state's three after it. */
    s_compositeFrag = CreateShader(COMPOSITE_frag_spv, COMPOSITE_frag_spv_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 1);
    if (!s_objVert || !s_objFrag || !s_compositeFrag) {
        return false;
    }

    const SDL_GPUTextureFormat depthCandidates[] = {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM,
                                                    SDL_GPU_TEXTUREFORMAT_D16_UNORM};
    for (size_t i = 0; i < SDL_arraysize(depthCandidates); i++) {
        if (SDL_GPUTextureSupportsFormat(s_device, depthCandidates[i], SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            s_depthFormat = depthCandidates[i];
            break;
        }
    }
    if (s_depthFormat == SDL_GPU_TEXTUREFORMAT_INVALID ||
        !SDL_GPUTextureSupportsFormat(s_device, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        Log_Warn("[GPU] Required texture formats are not supported by this device");
        return false;
    }

    SDL_GPUVertexBufferDescription vbuf;
    SDL_zero(vbuf);
    vbuf.slot = 0;
    vbuf.pitch = sizeof(T_GPUOBJ_VERTEX);
    vbuf.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[6];
    for (Uint32 i = 0; i < 6; i++) {
        attributes[i].location = i;
        attributes[i].buffer_slot = 0;
        attributes[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[i].offset = i * sizeof(float) * 4;
    }

    SDL_GPUColorTargetDescription targets[3];
    SDL_zero(targets);
    targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    targets[2].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader = s_objVert;
    info.fragment_shader = s_objFrag;
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 6;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    /* The engine already culled back faces before a primitive reached the capture. */
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.enable_depth_clip = true;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = true;
    info.target_info.color_target_descriptions = targets;
    info.target_info.num_color_targets = 3;
    info.target_info.depth_stencil_format = s_depthFormat;
    info.target_info.has_depth_stencil_target = true;

    s_pipeline = SDL_CreateGPUGraphicsPipeline(s_device, &info);
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    s_pipelinePainter = SDL_CreateGPUGraphicsPipeline(s_device, &info);
    if (s_pipeline == NULL || s_pipelinePainter == NULL) {
        Log_Warn("[GPU] Unable to create object pipeline: %s", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo samplerInfo;
    SDL_zero(samplerInfo);
    samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    s_nearest = SDL_CreateGPUSampler(s_device, &samplerInfo);
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    s_linear = SDL_CreateGPUSampler(s_device, &samplerInfo);

    const SDL_GPUTextureUsageFlags sampled = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    s_palette = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, sampled, 256, 1, 1);
    s_lut = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, sampled, 256, LUT_ROWS, 1);
    s_pages = CreateTexture(SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREFORMAT_R8_UNORM, sampled, 256, 256,
                            SDL_arraysize(s_pagePtrs));

    s_atlas = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, sampled, GPUOBJ_ATLAS_SIZE,
                            GPUOBJ_ATLAS_SIZE, 1);
    GpuObjAtlasDirty = TRUE;

    return s_nearest && s_linear && s_palette && s_lut && s_pages && s_atlas;
}

static void ReleasePipeline(void) {
    if (s_device == NULL) {
        return;
    }
    ReleaseFrameResources();
    ReleaseTexture(&s_palette);
    ReleaseTexture(&s_lut);
    ReleaseTexture(&s_pages);
    ReleaseTexture(&s_atlas);
    if (s_vertexBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_vertexBuffer);
        s_vertexBuffer = NULL;
    }
    s_vertexCapacity = 0;
    if (s_transfer) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_transfer);
        s_transfer = NULL;
    }
    s_transferCapacity = 0;
    if (s_nearest) {
        SDL_ReleaseGPUSampler(s_device, s_nearest);
        s_nearest = NULL;
    }
    if (s_linear) {
        SDL_ReleaseGPUSampler(s_device, s_linear);
        s_linear = NULL;
    }
    if (s_pipeline) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipeline);
        s_pipeline = NULL;
    }
    if (s_pipelinePainter) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelinePainter);
        s_pipelinePainter = NULL;
    }
    if (s_objVert) {
        SDL_ReleaseGPUShader(s_device, s_objVert);
        s_objVert = NULL;
    }
    if (s_objFrag) {
        SDL_ReleaseGPUShader(s_device, s_objFrag);
        s_objFrag = NULL;
    }
    if (s_compositeFrag) {
        SDL_ReleaseGPUShader(s_device, s_compositeFrag);
        s_compositeFrag = NULL;
    }
}

/* Uploads size bytes at the start of the transfer buffer into a texture region. */
static void UploadTexture(SDL_GPUCopyPass *pass, SDL_GPUTexture *texture, U32 layer, U32 x, U32 y, U32 w, U32 h,
                          const void *data, U32 size) {
    void *mapped = SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
    if (mapped == NULL) {
        return;
    }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(s_device, s_transfer);

    SDL_GPUTextureTransferInfo source;
    SDL_zero(source);
    source.transfer_buffer = s_transfer;

    SDL_GPUTextureRegion region;
    SDL_zero(region);
    region.texture = texture;
    region.layer = layer;
    region.x = x;
    region.y = y;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_UploadToGPUTexture(pass, &source, &region, false);
}

/* Row of rows*256 bytes of lookup data for this present, deduplicated; -1 when full. */
static S32 LutRows(const U8 *data, U32 rows) {
    for (U32 r = 0; r + rows <= s_lutRows; r++) {
        if (memcmp(s_lutData + r * 256, data, rows * 256) == 0) {
            return (S32)r;
        }
    }
    if (s_lutRows + rows > LUT_ROWS) {
        return -1;
    }
    memcpy(s_lutData + s_lutRows * 256, data, rows * 256);
    S32 row = (S32)s_lutRows;
    s_lutRows += rows;
    return row;
}

static S32 PageSlot(U8 *page) {
    for (U32 i = 0; i < s_nbPages; i++) {
        if (s_pagePtrs[i] == page) {
            return (S32)i;
        }
    }
    if (s_nbPages >= SDL_arraysize(s_pagePtrs)) {
        return -1;
    }
    s_pagePtrs[s_nbPages] = page;
    return (S32)s_nbPages++;
}

/* Resolves the lookup data of every live draw. A draw whose data does not
   fit is dropped; its pixels then keep the software colour. */
static U32 ResolveDraws(const T_GPUOBJ_DRAW *const *draws, U32 count, const T_GPUOBJ_DRAW **kept,
                        T_DRAW_UNIFORMS *uniforms, U32 *vertexBytes) {
    s_lutRows = 0;
    s_nbPages = 0;
    *vertexBytes = 0;
    U32 n = 0;
    for (U32 i = 0; i < count; i++) {
        const T_GPUOBJ_DRAW *draw = draws[i];
        T_DRAW_UNIFORMS u;
        u.Id = (float)(draw->Scene ? (draw->Id | GPUOBJ_SCENE_ID_BIT) : draw->Id);
        u.LutRow = -1.0f;
        u.ClutRow = 0.0f;
        u.Page = 0.0f;
        u.FogStart = (float)draw->FogStart;
        u.FogEnd = (float)draw->FogEnd;
        u.FogColor = (float)(draw->FogColor & 0xFF);
        u.Pad = GpuObjSpecular ? 1.0f : 0.0f; /* specular strength */
        if (draw->HasLogical) {
            S32 row = LutRows(draw->Logical, 1);
            if (row < 0) {
                continue;
            }
            u.LutRow = (float)row;
        }
        if (draw->Clut != NULL) {
            S32 row = LutRows(draw->Clut, 16);
            if (row < 0) {
                continue;
            }
            u.ClutRow = (float)row;
        }
        if (draw->Page != NULL) {
            S32 slot = PageSlot(draw->Page);
            if (slot < 0) {
                continue;
            }
            u.Page = (float)slot;
        }
        kept[n] = draw;
        uniforms[n] = u;
        *vertexBytes += draw->Count * sizeof(T_GPUOBJ_VERTEX);
        n++;
    }
    return n;
}

static void UploadVertices(SDL_GPUCopyPass *pass, const T_GPUOBJ_DRAW *const *draws, U32 count, U32 bytes) {
    U8 *mapped = (U8 *)SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
    if (mapped == NULL) {
        return;
    }
    U32 offset = 0;
    for (U32 i = 0; i < count; i++) {
        U32 size = draws[i]->Count * sizeof(T_GPUOBJ_VERTEX);
        memcpy(mapped + offset, GpuObjVerts + draws[i]->First, size);
        offset += size;
    }
    SDL_UnmapGPUTransferBuffer(s_device, s_transfer);

    SDL_GPUTransferBufferLocation source;
    SDL_zero(source);
    source.transfer_buffer = s_transfer;

    SDL_GPUBufferRegion region;
    SDL_zero(region);
    region.buffer = s_vertexBuffer;
    region.size = bytes;
    SDL_UploadToGPUBuffer(pass, &source, &region, false);
}

static void RenderDraws(SDL_GPUCommandBuffer *cmd, const T_GPUOBJ_DRAW *const *draws, const T_DRAW_UNIFORMS *uniforms,
                        U32 count) {
    SDL_GPUColorTargetInfo colors[3];
    SDL_zero(colors);
    colors[0].texture = s_objColor;
    colors[1].texture = s_objId;
    colors[2].texture = s_objLight;
    for (int i = 0; i < 3; i++) {
        colors[i].load_op = SDL_GPU_LOADOP_CLEAR;
        colors[i].store_op = SDL_GPU_STOREOP_STORE;
        colors[i].cycle = true;
    }

    SDL_GPUDepthStencilTargetInfo depth;
    SDL_zero(depth);
    depth.texture = s_depth;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, colors, 3, &depth);
    if (pass == NULL) {
        return;
    }
    SDL_GPUGraphicsPipeline *bound = s_pipeline;
    SDL_BindGPUGraphicsPipeline(pass, bound);

    SDL_GPUBufferBinding vbind;
    vbind.buffer = s_vertexBuffer;
    vbind.offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);

    SDL_GPUTextureSamplerBinding samplers[4];
    samplers[0].texture = s_palette;
    samplers[0].sampler = s_nearest;
    samplers[1].texture = s_lut;
    samplers[1].sampler = s_nearest;
    samplers[2].texture = s_pages;
    samplers[2].sampler = s_nearest;
    samplers[3].texture = s_atlas;
    samplers[3].sampler = s_nearest;
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);

    /* std140: 16 position/radius vec4, 16 colour/intensity vec4, count. */
    float lights[GPUOBJ_MAX_LIGHTS * 8 + 4];
    SDL_zeroa(lights);
    const U32 nbLights = GpuObjLightsEnabled ? GpuObjNbLights : 0;
    for (U32 l = 0; l < nbLights; l++) {
        memcpy(lights + l * 4, GpuObjLights[l].Pos, sizeof(float) * 4);
        memcpy(lights + GPUOBJ_MAX_LIGHTS * 4 + l * 4, GpuObjLights[l].Color, sizeof(float) * 4);
    }
    lights[GPUOBJ_MAX_LIGHTS * 8] = (float)nbLights;
    SDL_PushGPUFragmentUniformData(cmd, 1, lights, sizeof(lights));

    U32 first = 0;
    S32 slice = -1;
    for (U32 i = 0; i < count; i++) {
        const T_GPUOBJ_DRAW *draw = draws[i];

        /* Scene draws share the back half of the depth range and test real
           depth against each other. Every other draw is ordered as software
           drew it: a run of z-buffered bodies (drawn front to back, depth
           decides) shares a slice, any other body lands in front of everything
           before it; all of them in the front half, over the scene. */
        float sliceUniform[2];
        if (draw->Scene) {
            sliceUniform[0] = 0.5f;
            sliceUniform[1] = 0.5f;
        } else {
            if (!(draw->ZBuffer && i > 0 && draws[i - 1]->ZBuffer && !draws[i - 1]->Scene)) {
                slice++;
            }
            sliceUniform[1] = 0.5f / GPUOBJ_SLICES;
            sliceUniform[0] = (GPUOBJ_SLICES - 1.0f - (float)slice) * sliceUniform[1];
            if (sliceUniform[0] < 0.0f) {
                sliceUniform[0] = 0.0f;
            }
        }

        /* The clip window is inclusive frame pixels; scale it to the target. */
        S32 x0 = draw->X0 < 0 ? 0 : draw->X0;
        S32 y0 = draw->Y0 < 0 ? 0 : draw->Y0;
        S32 x1 = draw->X1 >= (S32)s_frameW ? (S32)s_frameW - 1 : draw->X1;
        S32 y1 = draw->Y1 >= (S32)s_frameH ? (S32)s_frameH - 1 : draw->Y1;
        SDL_Rect scissor;
        scissor.x = (int)((S64)x0 * s_targetW / s_frameW);
        scissor.y = (int)((S64)y0 * s_targetH / s_frameH);
        scissor.w = (int)((S64)(x1 + 1) * s_targetW / s_frameW) - scissor.x;
        scissor.h = (int)((S64)(y1 + 1) * s_targetH / s_frameH) - scissor.y;
        if (scissor.w > 0 && scissor.h > 0) {
            SDL_GPUGraphicsPipeline *wanted = draw->Painter ? s_pipelinePainter : s_pipeline;
            if (wanted != bound) {
                bound = wanted;
                SDL_BindGPUGraphicsPipeline(pass, bound);
                SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
                SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
            }
            SDL_SetGPUScissor(pass, &scissor);
            SDL_PushGPUVertexUniformData(cmd, 0, sliceUniform, sizeof(sliceUniform));
            SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms[i], sizeof(T_DRAW_UNIFORMS));
            SDL_DrawGPUPrimitives(pass, draw->Count, 1, first, 0);
        }
        first += draw->Count;
    }

    SDL_EndGPURenderPass(pass);
}

// --- Interface ---------------------------------------------------------------
bool GpuRender_Requested(void) {
    const char *env = SDL_getenv("LBA2_GPU");
    return env != NULL && SDL_atoi(env) != 0;
}

void *GpuRender_CreateRenderer(void *sdlWindow) {
    SDL_Renderer *renderer = SDL_CreateGPURenderer(NULL, (SDL_Window *)sdlWindow);
    if (renderer == NULL) {
        Log_Warn("[GPU] Unable to create GPU renderer: %s", SDL_GetError());
        return NULL;
    }

    s_renderer = renderer;
    s_device = SDL_GetGPURendererDevice(renderer);
    if (s_device == NULL || !(SDL_GetGPUShaderFormats(s_device) & SDL_GPU_SHADERFORMAT_SPIRV) || !CreatePipeline()) {
        Log_Warn("[GPU] GPU object renderer unavailable, using the software look");
        ReleasePipeline();
        SDL_DestroyRenderer(renderer);
        s_renderer = NULL;
        s_device = NULL;
        return NULL;
    }

    GpuObjAvailable = TRUE;
    Log_Info("GPU        objects rendered by %s", SDL_GetGPUDeviceDriver(s_device));
    return renderer;
}

void GpuRender_Shutdown(void) {
    GpuObjAvailable = FALSE;
    ReleasePipeline();
    s_renderer = NULL;
    s_device = NULL;
}

bool GpuRender_BeginComposite(void *sdlRenderer) {
    if (!GpuObjAvailable || !GpuObjEnabled || sdlRenderer != s_renderer || Log == NULL) {
        return false;
    }

    U32 w = ModeDesiredX;
    U32 h = ModeDesiredY;
    if (!EnsureFrameResources(w, h)) {
        return false;
    }

    Uint64 t0 = SDL_GetPerformanceCounter();
    const T_GPUOBJ_DRAW *const *live = NULL;
    S32 box[4];
    U32 liveCount = GpuObj_BuildPresent(s_tagScratch, &live, box);
    Uint64 t1 = SDL_GetPerformanceCounter();

    static const T_GPUOBJ_DRAW *kept[8192];
    static T_DRAW_UNIFORMS uniforms[8192];
    U32 vertexBytes = 0;
    U32 count = liveCount > 0 ? ResolveDraws(live, liveCount, kept, uniforms, &vertexBytes) : 0;
    if (count == 0) {
        vertexBytes = 0;
        memset(box, 0, sizeof(box));
    }

    /* With nothing to draw the composite is still worth it when the frame is
       upscaled: the pixel art filter runs there. */
    const bool boxEmpty = box[2] <= box[0] || box[3] <= box[1];
    const bool lastEmpty = s_tagBox[2] <= s_tagBox[0] || s_tagBox[3] <= s_tagBox[1];
    if (count == 0 && !s_upscale && lastEmpty && s_tagsCleared) {
        return false;
    }

    /* Upload the union of this box and the last one, so ids left from the
       previous present outside the new box are cleared. A texture of unknown
       contents takes the whole frame. */
    S32 up[4] = {0, 0, 0, 0};
    if (!s_tagsCleared) {
        up[2] = (S32)w;
        up[3] = (S32)h;
    } else if (boxEmpty) {
        memcpy(up, s_tagBox, sizeof(up));
    } else if (lastEmpty) {
        memcpy(up, box, sizeof(up));
    } else {
        up[0] = SDL_min(box[0], s_tagBox[0]);
        up[1] = SDL_min(box[1], s_tagBox[1]);
        up[2] = SDL_max(box[2], s_tagBox[2]);
        up[3] = SDL_max(box[3], s_tagBox[3]);
    }
    s_tagsCleared = true;
    U32 upW = (U32)(up[2] - up[0]);
    U32 upH = (U32)(up[3] - up[1]);
    for (S32 y = up[1]; y < up[3]; y++) {
        float *dst = s_tagUpload + (size_t)(y - up[1]) * upW;
        if (boxEmpty || y < box[1] || y >= box[3]) {
            memset(dst, 0, upW * sizeof(float));
            continue;
        }
        U32 left = (U32)(box[0] - up[0]);
        U32 inside = (U32)(box[2] - box[0]);
        memset(dst, 0, left * sizeof(float));
        memcpy(dst + left, s_tagScratch + (size_t)y * w + box[0], inside * sizeof(float));
        memset(dst + left + inside, 0, (upW - left - inside) * sizeof(float));
    }
    memcpy(s_tagBox, box, sizeof(s_tagBox));
    U32 tagBytes = upW * upH * sizeof(float);
    U32 need = (U32)SDL_arraysize(s_pagePtrs) * 65536;
    const U32 atlasBytes = (U32)GPUOBJ_ATLAS_SIZE * GPUOBJ_ATLAS_SIZE * 2;
    const bool atlasUpload = GpuObjAtlasDirty && GpuObjAtlas != NULL;
    if (atlasUpload && atlasBytes > need)
        need = atlasBytes;
    if (vertexBytes > need)
        need = vertexBytes;
    if (tagBytes > need)
        need = tagBytes;

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmd == NULL) {
        return false;
    }
    if (!EnsureTransfer(need) || (vertexBytes > 0 && !EnsureVertexBuffer(vertexBytes))) {
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    U8 palette[256 * 4];
    const U32 *lut = GetVideoPaletteLUT();
    for (int i = 0; i < 256; i++) {
        palette[i * 4 + 0] = (U8)(lut[i] >> 16);
        palette[i * 4 + 1] = (U8)(lut[i] >> 8);
        palette[i * 4 + 2] = (U8)lut[i];
        palette[i * 4 + 3] = 0xFF;
    }
    UploadTexture(copy, s_palette, 0, 0, 0, 256, 1, palette, sizeof(palette));
    if (s_lutRows > 0) {
        UploadTexture(copy, s_lut, 0, 0, 0, 256, s_lutRows, s_lutData, s_lutRows * 256);
    }
    for (U32 p = 0; p < s_nbPages; p++) {
        UploadTexture(copy, s_pages, p, 0, 0, 256, 256, s_pagePtrs[p], 65536);
    }
    if (atlasUpload) {
        UploadTexture(copy, s_atlas, 0, 0, 0, GPUOBJ_ATLAS_SIZE, GPUOBJ_ATLAS_SIZE, GpuObjAtlas, atlasBytes);
        GpuObjAtlasDirty = FALSE;
    }
    if (vertexBytes > 0) {
        UploadVertices(copy, kept, count, vertexBytes);
    }
    if (upW > 0 && upH > 0) {
        UploadTexture(copy, s_tags, 0, (U32)up[0], (U32)up[1], upW, upH, s_tagUpload, tagBytes);
    }
    SDL_EndGPUCopyPass(copy);

    Uint64 t2 = SDL_GetPerformanceCounter();
    if (count > 0) {
        RenderDraws(cmd, kept, uniforms, count);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    Uint64 t3 = SDL_GetPerformanceCounter();

    static const char *stats = SDL_getenv("LBA2_GPU_STATS");
    if (stats != NULL) {
        static double sumTags = 0, sumUpload = 0, sumRender = 0;
        static U32 frames = 0, sumDraws = 0, sumVerts = 0;
        double freq = (double)SDL_GetPerformanceFrequency() / 1000.0;
        sumTags += (double)(t1 - t0) / freq;
        sumUpload += (double)(t2 - t1) / freq;
        sumRender += (double)(t3 - t2) / freq;
        sumDraws += count;
        sumVerts += vertexBytes / sizeof(T_GPUOBJ_VERTEX);
        if (++frames == 300) {
            Log_Info("[GPU] per present: tags %.2f ms, resolve+upload %.2f ms, render+submit %.2f ms, %u draws, %u verts",
                     sumTags / frames, sumUpload / frames, sumRender / frames, sumDraws / frames, sumVerts / frames);
            sumTags = sumUpload = sumRender = 0;
            frames = sumDraws = sumVerts = 0;
        }
    }

    SDL_SetGPURenderState((SDL_Renderer *)sdlRenderer, s_composite);
    return true;
}

void GpuRender_EndComposite(void *sdlRenderer) {
    SDL_SetGPURenderState((SDL_Renderer *)sdlRenderer, NULL);
}

#endif // SDL_VERSION_ATLEAST(3, 4, 0)

// =============================================================================
#ifdef __cplusplus
}
#endif
