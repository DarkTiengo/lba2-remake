#include <SYSTEM/GPURENDER.H>

#include <3D/CAMERA.H>
#include <3D/PROJ.H>
#include <SVGA/GPUOBJ.H>
#include <SVGA/GPUBVH.H>
#include <SVGA/GPUSKY.H>
#include <SVGA/GPUSTORM.H>
#include <SVGA/GPUWATER.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/LOG.H>

#include <SDL3/SDL.h>
#include <math.h>
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

bool GpuRender_TakeFrame(void) {
    return false;
}

void GpuRender_SetFrameFiltered(bool /*filtered*/) {
}

bool GpuRender_BeginComposite(void * /*sdlRenderer*/) {
    return false;
}

void GpuRender_EndComposite(void * /*sdlRenderer*/) {
}

#else

#include "SHADERS/COMPOSITE.frag.spv.h"
#include "SHADERS/COMPOSITE_HALF.frag.spv.h"
#include "SHADERS/FULLSCREEN.vert.spv.h"
#include "SHADERS/GPUOBJ.frag.spv.h"
#include "SHADERS/GPUOBJ.vert.spv.h"
#include "SHADERS/GPUTERRAIN.vert.spv.h"
#include "lava_png_embedded.h"

// --- Private state -----------------------------------------------------------
static SDL_Renderer *s_renderer = NULL;
static SDL_GPUDevice *s_device = NULL;

static SDL_GPUShader *s_objVert = NULL;
static SDL_GPUShader *s_terrainVert = NULL; ///< the smooth terrain, grown from its records and height maps
static SDL_GPUShader *s_objFrag = NULL;
static SDL_GPUShader *s_compositeFrag = NULL;
static SDL_GPUShader *s_fullscreenVert = NULL;
static SDL_GPUShader *s_halfFrag = NULL;
static SDL_GPUGraphicsPipeline *s_pipelineHalf = NULL; ///< shadows and occlusion at half resolution
static SDL_GPUTexture *s_rtHalf = NULL;
static SDL_GPUTexture *s_rtHalfNext = NULL; ///< the half pass's output, copied into s_rtHalf after it
static bool s_rtHistory = false;           ///< s_rtHalf holds the last present's accumulation
static float s_prevCapMat[9];              ///< the scene camera of the last present's half pass
static float s_prevCapCam[3];
static U32 s_presentNumber = 0;
static U32 s_halfW = 0, s_halfH = 0;
static bool s_halfReady = false; ///< this present's half pass ran: the composite reads it
static float s_params[1024];     ///< the composite's last uniforms, for the half pass
static U32 s_paramsSize = 0;
static SDL_GPUGraphicsPipeline *s_pipeline = NULL;
static SDL_GPUGraphicsPipeline *s_pipelineTerrain = NULL; ///< s_pipeline with GPUTERRAIN.vert and no vertex input
static SDL_GPUGraphicsPipeline *s_pipelinePainter = NULL; ///< depth write, no depth test
static SDL_GPUGraphicsPipeline *s_pipelineMesh = NULL;    ///< s_pipeline culling back faces: a whole model's polygons
static SDL_GPUGraphicsPipeline *s_pipelineGlass = NULL;   ///< blended over the scene, depth test without write
static SDL_GPUSampler *s_nearest = NULL;
static SDL_GPUSampler *s_linear = NULL;
static bool s_supersample = false;
static bool s_upscale = false;     ///< the frame is shown larger than its pixels
static bool s_tagsCleared = false; ///< the tag texture holds known contents

/* Frame-sized: follow Log. */
static SDL_GPUTexture *s_objColor = NULL;
static SDL_GPUTexture *s_objId = NULL;
static SDL_GPUTexture *s_objLight = NULL; ///< dynamic light reaching each scene surface
static SDL_GPUBuffer *s_heightBuffer = NULL; ///< GpuObjHeightMaps, for GPUTERRAIN.vert
static bool s_heightsSent = false;           ///< the buffer holds every slot written so far
static SDL_GPUBuffer *s_landBuffer = NULL;   ///< GpuObjLandRecords, sent with the height maps
static SDL_GPUBuffer *s_shoreBuffer = NULL;  ///< island-wide coastal attenuation field
static U32 s_shoreSentRevision = ~0u;
static SDL_GPUBuffer *s_meshBuffer = NULL;   ///< GpuObjMeshVerts: the decor's models
static U32 s_meshSent = 0;                   ///< mesh vertices the buffer holds
static U32 s_meshSentEpoch = 0;

static SDL_GPUBuffer *s_bvhNodes = NULL;  ///< ray-tracing hierarchy (GPUBVH.H)
static SDL_GPUBuffer *s_bvhTris = NULL;
static bool s_rayScene = false; ///< this present's hierarchy is built: the composite traces shadow rays
/* The lights the composite traces shadow rays toward: the nearest to the camera. */
#define RT_MAX_LIGHTS 8
/* The object vertex shaders' draw uniform (PLACE.glsl), in floats. Every push
   sends all of it: a shorter one leaves the rest of the block holding whatever
   the ring had there, a tree's sway or a model's light included. */
#define DRAW_VERTEX_UNIFORM_FLOATS 72
static float s_rtLights[RT_MAX_LIGHTS][4]; ///< scene position, radius
static float s_rtWeights[RT_MAX_LIGHTS];   ///< intensity times luma, as the shaders weigh lights
static U32 s_nbRtLights = 0;
static SDL_GPUTexture *s_objShadow = NULL;               ///< soft shadow darkness over each surface
static SDL_GPUTexture *s_objHeight = NULL;               ///< scene-space height for water contact
static SDL_GPUGraphicsPipeline *s_pipelineShadow = NULL; ///< shadows: darkness target only, darkest wins
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
static SDL_GPUTexture *s_frameIndex = NULL; ///< the software frame's palette indices (R8), when taken
static bool s_frameTaken = false;           ///< the coming composite reads s_frameIndex
static bool s_frameIndexed = false;         ///< the composite being prepared reads it
static bool s_frameFiltered = false;
static SDL_GPUTexture *s_lut = NULL;
static SDL_GPUTexture *s_pages = NULL;
static SDL_GPUTexture *s_atlas = NULL;
static SDL_GPUTexture *s_lavaAlbedo = NULL;
static SDL_GPUSampler *s_lavaSampler = NULL;
static U8 *s_lavaPixels = NULL;
static U32 s_lavaWidth = 0;
static U32 s_lavaHeight = 0;
static bool s_lavaUploaded = false;
static bool s_lavaReady = false;

static SDL_GPUBuffer *s_vertexBuffer = NULL;
static U32 s_vertexCapacity = 0;
static SDL_GPUBuffer *s_indexBuffer = NULL;
static U32 s_indexCapacity = 0;
static U32 s_mirroredIndices = 0;
/* The vertex buffer mirrors GpuObjVerts: each draw lies at its own First.
   Draws are only appended, so a present copies only the vertices written
   since the last one, unless they moved (GpuObj_VertexEpoch). */
static U32 s_mirroredVerts = 0;
static U32 s_statUploaded = 0; ///< vertices copied to the mirror, for LBA2_GPU_STATS
static U32 s_mirroredEpoch = 0;
static const SDL_GPUBuffer *s_mirroredBuffer = NULL;
/* The vertices travel through their own transfer buffer, mapped once a present:
   the shared one is mapped for every page and would be cycled, at its full size,
   as often. It is kept small and used a slice at a time, because the map cycles
   it every present and the driver then carries a copy of whatever it is sized
   for. Sizing it for the whole mirror meant carrying tens of megabytes that only
   a repack ever sends at once; a slice at a time bounds that. Frame time did not
   measurably change either way (three runs each, 26.6-30.5 ms against
   27.2-28.1 ms on a machine whose orbiting frame varies by a fifth), so this is
   memory rather than speed. */
#define GPURENDER_VERTEX_SLICE (4u << 20)
static SDL_GPUTransferBuffer *s_vertexTransfer = NULL;
static U32 s_vertexTransferCapacity = 0;
/* The soft shadows' vertices, rebuilt every frame, in a buffer of their own. */
static SDL_GPUBuffer *s_shadowBuffer = NULL;
static U32 s_shadowCapacity = 0;
static SDL_GPUTransferBuffer *s_transfer = NULL;
static U32 s_transferCapacity = 0;

/* The tags go straight from the scan into the buffer they are uploaded from,
   laid out as the frame is (GpuObj_BuildPresent writes each pixel of its box). */
static SDL_GPUTransferBuffer *s_tagTransfer = NULL;
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
    float GlassPass;
    float Pad1, Pad2, Pad3;
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

static bool LoadLavaTexture(void) {
    SDL_IOStream *io = SDL_IOFromConstMem(kLavaPng, kLavaPngSize);
    if (io == NULL) {
        Log_Warn("[GPU] Unable to open embedded lava texture: %s", SDL_GetError());
        return false;
    }
    SDL_Surface *loaded = SDL_LoadPNG_IO(io, true);
    if (loaded == NULL) {
        Log_Warn("[GPU] Unable to decode embedded lava texture: %s", SDL_GetError());
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (rgba == NULL || rgba->w <= 0 || rgba->h <= 0 || rgba->pitch < rgba->w * 4) {
        Log_Warn("[GPU] Unable to convert embedded lava texture: %s", SDL_GetError());
        if (rgba != NULL)
            SDL_DestroySurface(rgba);
        return false;
    }

    const size_t rowBytes = (size_t)rgba->w * 4;
    const size_t totalBytes = rowBytes * (size_t)rgba->h;
    s_lavaPixels = (U8 *)malloc(totalBytes);
    if (s_lavaPixels == NULL) {
        SDL_DestroySurface(rgba);
        Log_Warn("[GPU] Unable to allocate embedded lava texture pixels");
        return false;
    }
    if (!SDL_LockSurface(rgba)) {
        free(s_lavaPixels);
        s_lavaPixels = NULL;
        SDL_DestroySurface(rgba);
        Log_Warn("[GPU] Unable to lock embedded lava texture: %s", SDL_GetError());
        return false;
    }
    for (S32 y = 0; y < rgba->h; y++) {
        memcpy(s_lavaPixels + (size_t)y * rowBytes, (const U8 *)rgba->pixels + (size_t)y * rgba->pitch,
               rowBytes);
    }
    SDL_UnlockSurface(rgba);
    s_lavaWidth = (U32)rgba->w;
    s_lavaHeight = (U32)rgba->h;
    SDL_DestroySurface(rgba);

    SDL_GPUTextureCreateInfo lavaInfo;
    SDL_zero(lavaInfo);
    lavaInfo.type = SDL_GPU_TEXTURETYPE_2D;
    lavaInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    lavaInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    lavaInfo.width = s_lavaWidth;
    lavaInfo.height = s_lavaHeight;
    lavaInfo.layer_count_or_depth = 1;
    lavaInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    U32 side = SDL_max(s_lavaWidth, s_lavaHeight);
    while (side > 0) {
        lavaInfo.num_levels++;
        side >>= 1;
    }
    s_lavaAlbedo = SDL_CreateGPUTexture(s_device, &lavaInfo);
    if (s_lavaAlbedo == NULL) {
        Log_Warn("[GPU] Unable to create lava texture: %s", SDL_GetError());
        free(s_lavaPixels);
        s_lavaPixels = NULL;
        return false;
    }
    SDL_GPUSamplerCreateInfo samplerInfo;
    SDL_zero(samplerInfo);
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    s_lavaSampler = SDL_CreateGPUSampler(s_device, &samplerInfo);
    if (s_lavaSampler == NULL) {
        Log_Warn("[GPU] Unable to create lava sampler: %s", SDL_GetError());
        ReleaseTexture(&s_lavaAlbedo);
        free(s_lavaPixels);
        s_lavaPixels = NULL;
        return false;
    }
    return true;
}

static SDL_GPUShader *CreateShader(const unsigned char *code, size_t size, SDL_GPUShaderStage stage,
                                   Uint32 samplers, Uint32 uniformBuffers, Uint32 storageBuffers = 0) {
    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code = code;
    info.code_size = size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniformBuffers;
    info.num_storage_buffers = storageBuffers;
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
    ReleaseTexture(&s_objShadow);
    ReleaseTexture(&s_objHeight);
    ReleaseTexture(&s_rtHalf);
    ReleaseTexture(&s_rtHalfNext);
    ReleaseTexture(&s_depth);
    ReleaseTexture(&s_tags);
    ReleaseTexture(&s_frameIndex);
    if (s_tagTransfer) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_tagTransfer);
        s_tagTransfer = NULL;
    }
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

/* Flame sprites the composite draws this present: boxes in frame uv. */
#define MAX_FLAME_SPRITES 16
static float s_flames[MAX_FLAME_SPRITES * 4];
static U32 s_nbFlames = 0;

/* One sprite per burning body; boxes that touch are merged (a fire made of many
   small bodies, a dragon's breath, is one flame). */
static void CollectFlames(const T_GPUOBJ_DRAW *const *draws, U32 count, U32 frameW, U32 frameH) {
    s_nbFlames = 0;
    if (!GpuObjFire || frameW == 0 || frameH == 0) {
        return;
    }
    for (U32 i = 0; i < count; i++) {
        const T_GPUOBJ_DRAW *draw = draws[i];
        if (!draw->HasFire || !draw->FireRises) {
            continue;
        }
        float box[4] = {draw->FireRect[0] / (float)frameW, draw->FireRect[1] / (float)frameH,
                        draw->FireRect[2] / (float)frameW, draw->FireRect[3] / (float)frameH};
        if (box[2] <= box[0] || box[3] <= box[1]) {
            continue;
        }
        S32 merged = FALSE;
        for (U32 k = 0; k < s_nbFlames; k++) {
            float *f = s_flames + k * 4;
            /* Only boxes that really overlap: two braziers one above the other on
               screen stay two flames. */
            const float ox = SDL_min(f[2], box[2]) - SDL_max(f[0], box[0]);
            const float oy = SDL_min(f[3], box[3]) - SDL_max(f[1], box[1]);
            const float smaller = SDL_min((f[2] - f[0]) * (f[3] - f[1]), (box[2] - box[0]) * (box[3] - box[1]));
            if (ox > 0.0f && oy > 0.0f && ox * oy >= smaller * 0.3f) {
                f[0] = SDL_min(f[0], box[0]);
                f[1] = SDL_min(f[1], box[1]);
                f[2] = SDL_max(f[2], box[2]);
                f[3] = SDL_max(f[3], box[3]);
                merged = TRUE;
                break;
            }
        }
        if (!merged && s_nbFlames < MAX_FLAME_SPRITES) {
            SDL_memcpy(s_flames + s_nbFlames * 4, box, sizeof(box));
            s_nbFlames++;
        }
    }
}

/* Composite switches: the player's GPU effects can change at any present. */
/* How bright the scene's palette is: 1 in daylight, 0 at night. */
static float Daylight(void) {
    const U32 *lut = GetVideoPaletteLUT();
    float luma = 0.0f;
    for (int i = 0; i < 256; i++) {
        luma += 0.3f * (float)((lut[i] >> 16) & 255) + 0.59f * (float)((lut[i] >> 8) & 255) +
                0.11f * (float)(lut[i] & 255);
    }
    luma /= 256.0f * 255.0f;
    const float day = (luma - 0.12f) / (0.32f - 0.12f);
    return day < 0.0f ? 0.0f : (day > 1.0f ? 1.0f : day);
}

static void PushCompositeParams(void) {
    static const char *debug = SDL_getenv("LBA2_GPU_DEBUG");
    float params[12 + GPUWATER_MAX_IMPACTS * 4 + MAX_FLAME_SPRITES * 4 + 12 + 12 + 4 + RT_MAX_LIGHTS * 4 + 8 +
                 GPUSTORM_UNIFORM_FLOATS + 32 + 16];
    SDL_zeroa(params);
    params[8] = (float)SDL_fmod((double)SDL_GetTicks() / 1000.0, 1000.0);
    params[9] = (float)s_nbFlames;
    params[10] = GpuWater_GetTime();
    params[11] = GpuWaterEnabled ? 1.0f : 0.0f;
    params[6] = GpuWater_GetWeather();
    SDL_memcpy(params + 12, s_flames, sizeof(s_flames));
    GpuWater_GetCompositeImpacts(params + 12 + MAX_FLAME_SPRITES * 4);
    params[0] = (debug != NULL) ? (float)SDL_atoi(debug) : 0.0f;
    params[1] = s_supersample ? 1.0f : 0.0f;
    params[2] = (s_upscale && GpuObjPixelFilter) ? 1.0f : 0.0f;
    params[3] = (s_upscale && GpuObjDeband) ? 1.0f : 0.0f;
    params[4] = (GpuObjFire || GpuObjGlow) ? 1.0f : 0.0f;
    params[7] = s_halfReady ? 1.0f : 0.0f;
    float *sun = params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4;
    /* Shoreline depth gating is geometric and must remain active when optional
       sunlight/AO grading is disabled. */
    sun[9] = GpuObjFocal * (float)s_targetW / (float)ModeDesiredX;
    sun[10] = 320.0f;
    sun[11] = TypeProj == TYPE_3D ? 1.0f : 0.0f;
    if (GpuObjSunlight) {
        const float day = Daylight();
        const float exterior = GpuObjSunExterior ? 1.0f : 0.0f;
        params[5] = 1.0f;
        /* Sunlit: warm; shaded: the sky's blue. Moonlit: silver over deep blue. */
        sun[0] = 1.0f + 0.08f * day;
        sun[1] = 1.0f + 0.02f * day;
        sun[2] = 0.9f + 0.1f * (1.0f - day);
        sun[3] = 0.35f + 0.15f * exterior;
        sun[4] = 0.9f - 0.08f * (1.0f - day);
        sun[5] = 0.96f;
        sun[6] = 1.08f + 0.06f * (1.0f - day);
        sun[7] = 0.16f * exterior * day + 0.05f;
        /* Rooms: their furniture is bricks, whose depth is the brick's box, so
           occlusion would come out blocky; the art already shades them. */
        sun[8] = 0.55f * exterior;
        sun[11] = exterior;
    }
    /* Sun rays through the frame's bodies: the sun, the projection that turns a
       pixel and its distance back into a scene point, the target's scale. */
    if (s_rayScene) {
        float *rt = params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4 + 12;
        const float sunUp = GpuObjSunDir[0] * GpuObjUpDir[0] + GpuObjSunDir[1] * GpuObjUpDir[1] +
                            GpuObjSunDir[2] * GpuObjUpDir[2];
        rt[0] = GpuObjSunDir[0];
        rt[1] = GpuObjSunDir[1];
        rt[2] = GpuObjSunDir[2];
        rt[3] = (GpuObjSunlight && sunUp > 0.05f) ? 0.2f + 0.4f * Daylight() : 0.0f;
        rt[4] = GpuObjProj[0];
        rt[5] = GpuObjProj[1];
        rt[6] = GpuObjProj[2];
        rt[7] = GpuObjProj[3];
        rt[8] = (float)s_targetW / (float)ModeDesiredX;
        rt[9] = (float)s_targetH / (float)ModeDesiredY;
        rt[10] = 30.0f;
        rt[11] = 40000.0f;
        /* Lights: count and the clearance kept around each (its lamp's frame). */
        rt[12] = GpuObjLightsEnabled ? (float)s_nbRtLights : 0.0f;
        rt[13] = 450.0f;
        memcpy(rt + 16, s_rtLights, sizeof(float) * 4 * RT_MAX_LIGHTS);
        memcpy(rt + 16 + RT_MAX_LIGHTS * 4, s_rtWeights, sizeof(float) * RT_MAX_LIGHTS);
    }
    /* Rain, lightning bolts and their flash: the storm needs the projection
       even when no ray is traced. */
    if (!s_rayScene && (GpuStorm_Active() || GpuSky_FogIndex() >= 0) && GpuObjSunExterior) {
        float *rt = params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4 + 12;
        rt[4] = GpuObjProj[0];
        rt[5] = GpuObjProj[1];
        rt[6] = GpuObjProj[2];
        rt[7] = GpuObjProj[3];
        rt[8] = (float)s_targetW / (float)ModeDesiredX;
        rt[9] = (float)s_targetH / (float)ModeDesiredY;
    }
    GpuStorm_GetUniforms(params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4 + 12 + 12 + 4 +
                             RT_MAX_LIGHTS * 4 + 8,
                         (float)ModeDesiredX, (float)ModeDesiredY);
    /* The sky: the fog colour at the horizon (w on), up (w daylight), the
       world's X (w cloud cover) and Z (w storm) axes, toward the sun (w
       seconds). */
    {
        float *sky = params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4 + 12 + 12 + 4 +
                     RT_MAX_LIGHTS * 4 + 8 + GPUSTORM_UNIFORM_FLOATS;
        const S32 fog = GpuSky_FogIndex();
        /* Up is the rays' too (a grass blade takes the ground's normal). */
        memcpy(sky + 4, GpuObjUpDir, sizeof(float) * 3);
        if (fog >= 0 && GpuObjSunExterior) {
            const U32 c = GetVideoPaletteLUT()[fog];
            const float storm = GpuSky_Storm();
            sky[0] = (float)((c >> 16) & 255) / 255.0f;
            sky[1] = (float)((c >> 8) & 255) / 255.0f;
            sky[2] = (float)(c & 255) / 255.0f;
            sky[3] = GpuSky_CloudsBelow() ? 2.0f : 1.0f;
            memcpy(sky + 4, GpuObjUpDir, sizeof(float) * 3);
            sky[7] = Daylight();
            memcpy(sky + 8, GpuObjWorldX, sizeof(float) * 3);
            sky[11] = GpuSky_Cover() + (0.95f - GpuSky_Cover()) * storm;
            memcpy(sky + 12, GpuObjWorldZ, sizeof(float) * 3);
            sky[15] = storm;
            memcpy(sky + 16, GpuObjSunDir, sizeof(float) * 3);
            sky[19] = GpuWater_GetTime();
            GpuSky_GetFog(sky + 20, sky + 21);
            sky[22] = GpuObjSceneryClaimed ? 1.0f : 0.0f;
            GpuSky_GetCamera(sky + 24);
            GpuSky_GetPlanet(sky + 28);
        }
    }
    /* The half pass's accumulation: this present's scene space into the last
       one's (a scene point is M * world - camera, M the capture's rotation),
       and whether the last one's result is there to be reused. */
    {
        float *acc = params + 12 + MAX_FLAME_SPRITES * 4 + GPUWATER_MAX_IMPACTS * 4 + 12 + 12 + 4 + RT_MAX_LIGHTS * 4 +
                     8 + GPUSTORM_UNIFORM_FLOATS + 32;
        const float *a = s_prevCapMat, *b = GpuObjCapMat;
        float r[9];
        for (S32 i = 0; i < 3; i++) {
            for (S32 j = 0; j < 3; j++) {
                /* The last rotation times this one's transpose (its inverse). */
                r[i * 3 + j] = a[i * 3] * b[j * 3] + a[i * 3 + 1] * b[j * 3 + 1] + a[i * 3 + 2] * b[j * 3 + 2];
            }
        }
        for (S32 i = 0; i < 3; i++) {
            acc[i * 4 + 0] = r[i * 3];
            acc[i * 4 + 1] = r[i * 3 + 1];
            acc[i * 4 + 2] = r[i * 3 + 2];
            acc[i * 4 + 3] = r[i * 3] * GpuObjCapCam[0] + r[i * 3 + 1] * GpuObjCapCam[1] + r[i * 3 + 2] * GpuObjCapCam[2] -
                             s_prevCapCam[i];
        }
        acc[12] = s_rtHistory && GpuObjCapValid ? 1.0f : 0.0f;
        acc[13] = (float)(s_presentNumber % 1024u);
        acc[14] = s_frameIndexed ? 1.0f : 0.0f;
        acc[15] = s_frameFiltered ? 1.0f : 0.0f;
    }
    SDL_COMPILE_TIME_ASSERT(half_params, sizeof(params) <= sizeof(s_params));
    memcpy(s_params, params, sizeof(params));
    s_paramsSize = (U32)sizeof(params);
    if (!SDL_SetGPURenderStateFragmentUniforms(s_composite, 0, params, sizeof(params))) {
        Log_Warn("[GPU] Unable to update composite uniforms: %s", SDL_GetError());
    }
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
    s_objShadow = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, targetUsage, tw, th, 1);
    s_objHeight = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, targetUsage, tw, th, 1);
    s_rtHalf = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, targetUsage, (tw + 1) / 2,
                             (th + 1) / 2, 1);
    s_rtHalfNext = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, targetUsage,
                                 (tw + 1) / 2, (th + 1) / 2, 1);
    s_rtHistory = false;
    s_halfW = (tw + 1) / 2;
    s_halfH = (th + 1) / 2;
    s_depth = CreateTexture(SDL_GPU_TEXTURETYPE_2D, s_depthFormat, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, tw, th, 1);
    s_tags = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1);
    s_frameIndex = CreateTexture(SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREFORMAT_R8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, w, h, 1);
    SDL_GPUTransferBufferCreateInfo tagInfo;
    SDL_zero(tagInfo);
    tagInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tagInfo.size = w * h * (Uint32)sizeof(float);
    s_tagTransfer = SDL_CreateGPUTransferBuffer(s_device, &tagInfo);
    if (!s_objColor || !s_objId || !s_objLight || !s_objShadow || !s_objHeight || !s_depth || !s_tags ||
        !s_frameIndex || !s_tagTransfer) {
        ReleaseFrameResources();
        return false;
    }

    SDL_GPUTextureSamplerBinding bindings[9];
    bindings[0].texture = s_objColor;
    bindings[0].sampler = s_linear;
    bindings[1].texture = s_objId;
    bindings[1].sampler = s_nearest;
    bindings[2].texture = s_tags;
    bindings[2].sampler = s_nearest;
    bindings[3].texture = s_objLight;
    bindings[3].sampler = s_nearest;
    bindings[4].texture = s_objShadow;
    bindings[4].sampler = s_linear;
    bindings[5].texture = s_objHeight;
    bindings[5].sampler = s_nearest;
    bindings[6].texture = s_rtHalf;
    bindings[6].sampler = s_nearest;
    bindings[7].texture = s_frameIndex;
    bindings[7].sampler = s_nearest;
    bindings[8].texture = s_palette;
    bindings[8].sampler = s_nearest;

    SDL_GPURenderStateCreateInfo stateInfo;
    SDL_zero(stateInfo);
    stateInfo.fragment_shader = s_compositeFrag;
    stateInfo.num_sampler_bindings = 9;
    stateInfo.sampler_bindings = bindings;
    SDL_GPUBuffer *storage[2] = {s_bvhNodes, s_bvhTris};
    stateInfo.num_storage_buffers = 2;
    stateInfo.storage_buffers = storage;
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
    /* The new buffer holds nothing of the mirror, and may well come back at the
       old one's address, so the pointer cannot be what says so. */
    s_mirroredBuffer = NULL;
    s_mirroredVerts = 0;
    U32 capacity = s_vertexCapacity ? s_vertexCapacity : (1u << 20);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ; /* GPUTERRAIN.vert reads records */
    info.size = capacity;
    s_vertexBuffer = SDL_CreateGPUBuffer(s_device, &info);
    s_vertexCapacity = s_vertexBuffer ? capacity : 0;
    return s_vertexBuffer != NULL;
}

static bool EnsureIndexBuffer(U32 size) {
    if (size <= s_indexCapacity) {
        return true;
    }
    if (s_indexBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_indexBuffer);
        s_indexBuffer = NULL;
    }
    U32 capacity = s_indexCapacity ? s_indexCapacity : (1u << 18);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    info.size = capacity;
    s_indexBuffer = SDL_CreateGPUBuffer(s_device, &info);
    s_indexCapacity = s_indexBuffer ? capacity : 0;
    s_mirroredIndices = 0;
    return s_indexBuffer != NULL;
}

static bool EnsureVertexTransfer(U32 size) {
    if (size <= s_vertexTransferCapacity) {
        return true;
    }
    if (s_vertexTransfer) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_vertexTransfer);
        s_vertexTransfer = NULL;
    }
    U32 capacity = s_vertexTransferCapacity ? s_vertexTransferCapacity : (1u << 20);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUTransferBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = capacity;
    s_vertexTransfer = SDL_CreateGPUTransferBuffer(s_device, &info);
    s_vertexTransferCapacity = s_vertexTransfer ? capacity : 0;
    return s_vertexTransfer != NULL;
}

static bool EnsureShadowBuffer(U32 size) {
    if (size <= s_shadowCapacity) {
        return true;
    }
    if (s_shadowBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_shadowBuffer);
        s_shadowBuffer = NULL;
    }
    U32 capacity = s_shadowCapacity ? s_shadowCapacity : (1u << 16);
    while (capacity < size) {
        capacity *= 2;
    }
    SDL_GPUBufferCreateInfo info;
    SDL_zero(info);
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    info.size = capacity;
    s_shadowBuffer = SDL_CreateGPUBuffer(s_device, &info);
    s_shadowCapacity = s_shadowBuffer ? capacity : 0;
    return s_shadowBuffer != NULL;
}

static bool CreatePipeline(void) {
    s_objVert = CreateShader(GPUOBJ_vert_spv, GPUOBJ_vert_spv_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    s_terrainVert = CreateShader(GPUTERRAIN_vert_spv, GPUTERRAIN_vert_spv_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 4);
    s_objFrag = CreateShader(GPUOBJ_frag_spv, GPUOBJ_frag_spv_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 4);
    /* The renderer binds the frame texture at slot 0, the state's three after it. */
    s_compositeFrag = CreateShader(COMPOSITE_frag_spv, COMPOSITE_frag_spv_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 1, 2);
    /* The half-resolution pass: the composite's source built with HALF_PASS,
       bound as the composite is (a stand-in texture at the frame's slot). */
    s_fullscreenVert = CreateShader(FULLSCREEN_vert_spv, FULLSCREEN_vert_spv_len, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    s_halfFrag = CreateShader(COMPOSITE_HALF_frag_spv, COMPOSITE_HALF_frag_spv_len, SDL_GPU_SHADERSTAGE_FRAGMENT, 11, 1, 2);
    if (!s_objVert || !s_terrainVert || !s_objFrag || !s_compositeFrag || !s_fullscreenVert || !s_halfFrag) {
        return false;
    }
    {
        SDL_GPUColorTargetDescription half;
        SDL_zero(half);
        half.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo halfInfo;
        SDL_zero(halfInfo);
        halfInfo.vertex_shader = s_fullscreenVert;
        halfInfo.fragment_shader = s_halfFrag;
        halfInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        halfInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        halfInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        halfInfo.target_info.color_target_descriptions = &half;
        halfInfo.target_info.num_color_targets = 1;
        s_pipelineHalf = SDL_CreateGPUGraphicsPipeline(s_device, &halfInfo);
        if (s_pipelineHalf == NULL) {
            Log_Warn("[GPU] Unable to create the half-resolution pass: %s", SDL_GetError());
            return false;
        }
    }
    /* The ray-tracing hierarchy, at its largest: filled each frame it is used. */
    SDL_GPUBufferCreateInfo bvhInfo;
    SDL_zero(bvhInfo);
    bvhInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bvhInfo.size = GPUBVH_MAX_NODES * GPUBVH_NODE_FLOATS * sizeof(float);
    s_bvhNodes = SDL_CreateGPUBuffer(s_device, &bvhInfo);
    bvhInfo.size = GPUBVH_MAX_TRIS * GPUBVH_TRI_FLOATS * sizeof(float);
    s_bvhTris = SDL_CreateGPUBuffer(s_device, &bvhInfo);
    bvhInfo.size = sizeof(GpuObjHeightMaps);
    s_heightBuffer = SDL_CreateGPUBuffer(s_device, &bvhInfo);
    bvhInfo.size = sizeof(GpuObjLandRecords);
    s_landBuffer = SDL_CreateGPUBuffer(s_device, &bvhInfo);
    bvhInfo.size = GPUWATER_SHORE_SIDE * GPUWATER_SHORE_SIDE * sizeof(float);
    s_shoreBuffer = SDL_CreateGPUBuffer(s_device, &bvhInfo);
    s_shoreSentRevision = ~0u;
    s_heightsSent = false;
    SDL_GPUBufferCreateInfo meshInfo;
    SDL_zero(meshInfo);
    meshInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    meshInfo.size = GPUOBJ_MESH_MAX_VERTS * (Uint32)sizeof(T_GPUOBJ_VERTEX);
    s_meshBuffer = SDL_CreateGPUBuffer(s_device, &meshInfo);
    s_meshSent = 0;
    if (!s_heightBuffer || !s_landBuffer || !s_shoreBuffer || !s_meshBuffer) {
        Log_Warn("[GPU] Unable to create terrain buffers: %s", SDL_GetError());
        return false;
    }
    if (!s_bvhNodes || !s_bvhTris) {
        Log_Warn("[GPU] Unable to create the ray-tracing buffers: %s", SDL_GetError());
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

    SDL_GPUColorTargetDescription targets[5];
    SDL_zero(targets);
    targets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    targets[1].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    targets[2].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    targets[3].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    targets[4].format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;

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
    info.target_info.num_color_targets = 5;
    info.target_info.depth_stencil_format = s_depthFormat;
    info.target_info.has_depth_stencil_target = true;

    s_pipeline = SDL_CreateGPUGraphicsPipeline(s_device, &info);
    {
        /* A kept model is drawn whole: the GPU culls its back faces, which the
           engine does for a captured body (TestVisible keeps the polygons
           turning counter-clockwise on the screen). */
        SDL_GPUGraphicsPipelineCreateInfo mesh = info;
        mesh.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        mesh.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        s_pipelineMesh = SDL_CreateGPUGraphicsPipeline(s_device, &mesh);
    }
    {
        /* The smooth terrain: the same targets and depth, its vertices made in
           the shader from storage buffers. */
        SDL_GPUGraphicsPipelineCreateInfo terrain = info;
        terrain.vertex_shader = s_terrainVert;
        SDL_zero(terrain.vertex_input_state);
        s_pipelineTerrain = SDL_CreateGPUGraphicsPipeline(s_device, &terrain);
    }
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    s_pipelinePainter = SDL_CreateGPUGraphicsPipeline(s_device, &info);

    /* Lamp glass: colour blended over what is behind it, the id replaced, the
       light target keeping its colour and taking the glass's glow in alpha. */
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.enable_depth_write = false;
    targets[0].blend_state.enable_blend = true;
    targets[0].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    targets[0].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    targets[0].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    targets[0].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[0].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    targets[0].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    targets[2].blend_state.enable_blend = true;
    targets[2].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targets[2].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[2].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    targets[2].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[2].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targets[2].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    /* The glass keeps the shadows under it. */
    targets[3].blend_state = targets[2].blend_state;
    targets[3].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targets[3].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[4].blend_state.enable_blend = true;
    targets[4].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targets[4].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[4].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    targets[4].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    targets[4].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[4].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    s_pipelineGlass = SDL_CreateGPUGraphicsPipeline(s_device, &info);

    /* Shadows: only the darkness target changes, overlapping shadows keep the darkest. */
    SDL_GPUColorTargetBlendState keep;
    SDL_zero(keep);
    keep.enable_blend = true;
    keep.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    keep.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    keep.color_blend_op = SDL_GPU_BLENDOP_ADD;
    keep.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    keep.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    keep.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    targets[0].blend_state = keep;
    targets[1].blend_state = keep;
    targets[2].blend_state = keep;
    targets[3].blend_state.enable_blend = true;
    targets[3].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[3].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[3].blend_state.color_blend_op = SDL_GPU_BLENDOP_MAX;
    targets[3].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[3].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    targets[3].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
    targets[4].blend_state = keep;
    s_pipelineShadow = SDL_CreateGPUGraphicsPipeline(s_device, &info);
    if (s_pipeline == NULL || s_pipelineTerrain == NULL || s_pipelinePainter == NULL || s_pipelineMesh == NULL || s_pipelineGlass == NULL ||
        s_pipelineShadow == NULL) {
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

    if (s_nearest && s_linear && s_palette && s_lut && s_pages && s_atlas)
        LoadLavaTexture();

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
    ReleaseTexture(&s_lavaAlbedo);
    if (s_lavaSampler) {
        SDL_ReleaseGPUSampler(s_device, s_lavaSampler);
        s_lavaSampler = NULL;
    }
    free(s_lavaPixels);
    s_lavaPixels = NULL;
    s_lavaWidth = s_lavaHeight = 0;
    s_lavaUploaded = false;
    s_lavaReady = false;
    if (s_vertexBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_vertexBuffer);
        s_vertexBuffer = NULL;
    }
    if (s_indexBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_indexBuffer);
        s_indexBuffer = NULL;
    }
    s_indexCapacity = 0;
    s_mirroredIndices = 0;
    if (s_shadowBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_shadowBuffer);
        s_shadowBuffer = NULL;
    }
    s_shadowCapacity = 0;
    s_mirroredBuffer = NULL;
    if (s_vertexTransfer) {
        SDL_ReleaseGPUTransferBuffer(s_device, s_vertexTransfer);
        s_vertexTransfer = NULL;
    }
    s_vertexTransferCapacity = 0;
    if (s_bvhNodes) {
        SDL_ReleaseGPUBuffer(s_device, s_bvhNodes);
        s_bvhNodes = NULL;
    }
    if (s_landBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_landBuffer);
        s_landBuffer = NULL;
    }
    if (s_shoreBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_shoreBuffer);
        s_shoreBuffer = NULL;
    }
    s_shoreSentRevision = ~0u;
    if (s_meshBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_meshBuffer);
        s_meshBuffer = NULL;
    }
    s_meshSent = 0;
    if (s_heightBuffer) {
        SDL_ReleaseGPUBuffer(s_device, s_heightBuffer);
        s_heightBuffer = NULL;
    }
    if (s_bvhTris) {
        SDL_ReleaseGPUBuffer(s_device, s_bvhTris);
        s_bvhTris = NULL;
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
    if (s_pipelineHalf) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelineHalf);
        s_pipelineHalf = NULL;
    }
    if (s_fullscreenVert) {
        SDL_ReleaseGPUShader(s_device, s_fullscreenVert);
        s_fullscreenVert = NULL;
    }
    if (s_halfFrag) {
        SDL_ReleaseGPUShader(s_device, s_halfFrag);
        s_halfFrag = NULL;
    }
    if (s_pipelineShadow) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelineShadow);
        s_pipelineShadow = NULL;
    }
    if (s_pipelineTerrain) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelineTerrain);
        s_pipelineTerrain = NULL;
    }
    if (s_terrainVert) {
        SDL_ReleaseGPUShader(s_device, s_terrainVert);
        s_terrainVert = NULL;
    }
    if (s_pipelineGlass) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelineGlass);
        s_pipelineGlass = NULL;
    }
    if (s_pipelinePainter) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelinePainter);
        s_pipelinePainter = NULL;
    }
    if (s_pipelineMesh) {
        SDL_ReleaseGPUGraphicsPipeline(s_device, s_pipelineMesh);
        s_pipelineMesh = NULL;
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

/* A land draw's triangles for the shadow rays: the records' flat triangles,
   placed as GPUTERRAIN.vert places their corners. The records are the whole
   cube and the hierarchy's room is the frame's, so only those that can shade
   something in view: in view themselves, or with the shadow they cast (the
   triangle carried away from the sun, as far as a ray reaches) crossing it.
   A caster just off screen -- the hill behind the camera, the tree beside it
   -- shades what is on screen, and choosing by view alone made those shadows
   come and go as the camera turned. No water, and none the GPU does not draw. */
static U32 LandRayTris(const T_GPUOBJ_DRAW *draw, float *tris, U32 n) {
    static const S32 kCX[4] = {0, 0, 1, 1};
    static const S32 kCZ[4] = {0, 1, 1, 0};
    static const U8 kCorners[2][2][3] = {{{0, 1, 2}, {2, 3, 0}}, {{3, 0, 1}, {1, 2, 3}}};
    const S32 slot = draw->LandSlot;
    const S32 count = GpuObjLandCount[slot];
    const U32 *rec = GpuObjLandRecords + (size_t)slot * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS;
    const float *heights = GpuObjHeightMaps + (size_t)slot * GPUOBJ_HEIGHTMAP_SIDE * GPUOBJ_HEIGHTMAP_SIDE;
    const float *m = draw->CapMat;
    const float *o = draw->LandOrigin;
    /* The view's bounds as x / depth and y / depth, where the frame's edges are
       from the projection's centre, a margin wider. */
    const float margin = 48.0f;
    const float fx = GpuObjProj[2], fy = GpuObjProj[2] * GpuObjProj[3];
    const float xLo = (-margin - GpuObjProj[0]) / fx, xHi = ((float)ModeDesiredX + margin - GpuObjProj[0]) / fx;
    /* FRatioY is negative: y grows down the frame as it shrinks in view space. */
    const float yA = (-margin - GpuObjProj[1]) / fy, yB = ((float)ModeDesiredY + margin - GpuObjProj[1]) / fy;
    const float yLo = yA < yB ? yA : yB, yHi = yA < yB ? yB : yA;
    /* Away from the sun, in view space (GpuObjSunDir points toward it). */
    const float away[3] = {-GpuObjSunDir[0], -GpuObjSunDir[1], -GpuObjSunDir[2]};
    const float reach = 40000.0f; /* the composite's ray length (rtInfo.w) */
    for (S32 k = 0; k < count && n < GPUBVH_MAX_TRIS; k++, rec += GPUOBJ_LAND_WORDS) {
        const U32 bits = rec[0];
        const S32 mode = (S32)((bits >> 17) & 15);
        if ((bits & (1u << 14)) != 0 || mode == GPUOBJ_MODE_PASS) {
            continue;
        }
        const S32 cell = (S32)(bits & 0xFFFu);
        const U8 *corners = kCorners[(bits & (1u << 13)) != 0][(bits & (1u << 12)) != 0];
        float p[9];
        float c[3] = {0.0f, 0.0f, 0.0f};
        for (S32 i = 0; i < 3; i++) {
            const S32 gx = (cell & 63) + kCX[corners[i]], gz = (cell >> 6) + kCZ[corners[i]];
            const float w[3] = {(float)(gx * 512), heights[gz * GPUOBJ_HEIGHTMAP_SIDE + gx], (float)(gz * 512)};
            float *q = p + i * 3;
            for (S32 r = 0; r < 3; r++) {
                q[r] = m[r * 3] * w[0] + m[r * 3 + 1] * w[1] + m[r * 3 + 2] * w[2] + o[r];
                c[r] += q[r] * (1.0f / 3.0f);
            }
        }
        /* The triangle, then its shadow's path, a sixth of the reach a step. A
           cell is 512 across: its centre is near enough to any of its points
           at the steps' spacing that the view's margin covers the difference. */
        S32 keep = FALSE;
        for (S32 step = 0; step <= 6 && !keep; step++) {
            const float t = reach * (float)step / 6.0f;
            const float x = c[0] + away[0] * t, y = c[1] + away[1] * t, depth = -(c[2] + away[2] * t);
            if (depth <= 1.0f || depth >= draw->LandFar) {
                continue;
            }
            keep = x >= xLo * depth && x <= xHi * depth && y >= yLo * depth && y <= yHi * depth;
            /* A triangle across the edge of the view: its centre may be just out. */
            if (!keep && step == 0) {
                for (S32 i = 0; i < 3 && !keep; i++) {
                    const float *q = p + i * 3;
                    const float d = -q[2];
                    keep = d > 1.0f && q[0] >= xLo * d && q[0] <= xHi * d && q[1] >= yLo * d && q[1] <= yHi * d;
                }
            }
        }
        if (!keep) {
            continue;
        }
        memcpy(tris + n * 9, p, sizeof(p));
        n++;
    }
    return n;
}

/* Uploads size bytes at the start of the transfer buffer into a texture region. */
/* The live bodies' and terrain's triangles into the hierarchy shadow rays are
   traced through: not the sky or sea, bricks, sphere impostors, or lamp glass
   (the light shines from inside it). */
static bool BuildRayScene(const T_GPUOBJ_DRAW *const *draws, U32 count) {
    static float tris[GPUBVH_MAX_TRIS * 9];
    U32 n = 0;
    /* Grass blades are too many and too thin to cast a traced shadow. */
    const S32 skipFlags = GPUOBJ_FLAG_WATER | GPUOBJ_FLAG_SKY | GPUOBJ_FLAG_EMISSIVE | GPUOBJ_FLAG_GRASS;
    for (U32 d = 0; d < count && n < GPUBVH_MAX_TRIS; d++) {
        /* Only the scene: a menu's bodies live in their own projection. */
        if (!draws[d]->Scene) {
            continue;
        }
        if (draws[d]->Procedural == GPUOBJ_PROC_LAND) {
            n = LandRayTris(draws[d], tris, n);
            continue;
        }
        if (draws[d]->Mesh) {
            /* A kept model: its triangles placed by the draw's rows. */
            const T_GPUOBJ_VERTEX *mv = GpuObjMeshVerts + draws[d]->First;
            const float *m = draws[d]->MeshRows;
            for (U32 i = 0; i + 2 < draws[d]->Count && n < GPUBVH_MAX_TRIS; i += 3) {
                float *o = tris + n * 9;
                for (int k = 0; k < 3; k++) {
                    const float *src = mv[i + k].vpos;
                    for (int r = 0; r < 3; r++) {
                        o[k * 3 + r] = m[r * 4] * src[0] + m[r * 4 + 1] * src[1] + m[r * 4 + 2] * src[2] + m[r * 4 + 3];
                    }
                }
                n++;
            }
            continue;
        }
        const T_GPUOBJ_VERTEX *v = GpuObjVerts + draws[d]->First;
        /* An indexed draw shares its points between triangles, so the triples
           are in the indices. Its detail triangles are the exception: their
           original is carried by the three corner vertices the block starts
           with, which is where the rays want it, so those are read straight
           from the vertices below and skipped here. */
        if (draws[d]->NbIndices > 0) {
            const U32 *ix = GpuObjIndices + draws[d]->FirstIndex;
            for (U32 i = 0; i + 2 < draws[d]->NbIndices && n < GPUBVH_MAX_TRIS; i += 3) {
                const T_GPUOBJ_VERTEX *a = &v[ix[i]];
                const S32 mode = (S32)a->light[3];
                const S32 flags = (S32)(a->vpos[3] + 0.5f);
                if (mode == GPUOBJ_MODE_PASS || mode == GPUOBJ_MODE_BRICK || mode == GPUOBJ_MODE_DISC ||
                    mode == GPUOBJ_MODE_ORB || (flags & skipFlags) != 0 || (flags & GPUOBJ_FLAG_DETAIL) != 0) {
                    continue;
                }
                float *o = tris + n * 9;
                for (int k = 0; k < 3; k++) {
                    const float *src = v[ix[i + k]].vpos;
                    o[k * 3 + 0] = src[0];
                    o[k * 3 + 1] = src[1];
                    o[k * 3 + 2] = src[2];
                }
                n++;
            }
            for (U32 i = 0; i + 2 < draws[d]->Count && n < GPUBVH_MAX_TRIS; i++) {
                const S32 flags = (S32)(v[i].vpos[3] + 0.5f);
                if (v[i].mat[3] < 0.5f || (flags & GPUOBJ_FLAG_DETAIL) == 0 || (flags & skipFlags) != 0) {
                    continue;
                }
                /* A block of small triangles: the three it starts with are the
                   original triangle's corners, which is the one the rays take. */
                float *o = tris + n * 9;
                for (int k = 0; k < 3; k++) {
                    o[k * 3 + 0] = v[i + k].vpos[0];
                    o[k * 3 + 1] = v[i + k].vpos[1];
                    o[k * 3 + 2] = v[i + k].vpos[2];
                }
                n++;
                i += 2;
            }
            continue;
        }
        for (U32 i = 0; i + 2 < draws[d]->Count && n < GPUBVH_MAX_TRIS; i += 3) {
            const S32 mode = (S32)v[i].light[3];
            const S32 flags = (S32)(v[i].vpos[3] + 0.5f);
            if (mode == GPUOBJ_MODE_PASS || mode == GPUOBJ_MODE_BRICK || mode == GPUOBJ_MODE_DISC ||
                mode == GPUOBJ_MODE_ORB || (flags & skipFlags) != 0) {
                continue;
            }
            /* Smooth terrain: its many small triangles lie on or above the
               original, which the rays keep, sixteen times fewer. They come
               indexed and are taken a block at a time above. */
            if ((flags & GPUOBJ_FLAG_DETAIL) != 0) {
                continue;
            }
            float *o = tris + n * 9;
            for (int k = 0; k < 3; k++) {
                const float *src = v[i + k].vpos;
                o[k * 3 + 0] = src[0];
                o[k * 3 + 1] = src[1];
                o[k * 3 + 2] = src[2];
            }
            n++;
        }
    }
    GpuBvh_Build(tris, n);
    return GpuBvhNbNodes > 0;
}

/* The hierarchy is built on a worker thread while the present copies the
   vertices and tags: both only read the frame's draws. */
typedef struct {
    const T_GPUOBJ_DRAW *const *Draws;
    U32 Count;
    bool Built;
} T_BVH_JOB;

static int SDLCALL BvhWorker(void *data) {
    T_BVH_JOB *job = (T_BVH_JOB *)data;
    job->Built = BuildRayScene(job->Draws, job->Count);
    return 0;
}

static void UploadBuffer(SDL_GPUCopyPass *pass, SDL_GPUBuffer *buffer, const void *data, U32 size) {
    void *mapped = SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
    if (mapped == NULL) {
        return;
    }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(s_device, s_transfer);

    SDL_GPUTransferBufferLocation source;
    SDL_zero(source);
    source.transfer_buffer = s_transfer;

    SDL_GPUBufferRegion region;
    SDL_zero(region);
    region.buffer = buffer;
    region.size = size;
    SDL_UploadToGPUBuffer(pass, &source, &region, true);
}

/* The same, into a buffer at an offset, for data that is appended to rather
   than rewritten (the indices). */
static void UploadBuffer2(SDL_GPUCopyPass *pass, SDL_GPUBuffer *buffer, const void *data, U32 offset, U32 size) {
    if (size == 0 || size > s_transferCapacity) {
        return; /* the caller sizes the transfer buffer before the pass opens */
    }
    void *mapped = SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
    if (mapped == NULL) {
        return;
    }
    memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(s_device, s_transfer);

    SDL_GPUTransferBufferLocation source;
    SDL_zero(source);
    source.transfer_buffer = s_transfer;

    SDL_GPUBufferRegion region;
    SDL_zero(region);
    region.buffer = buffer;
    region.offset = offset;
    region.size = size;
    SDL_UploadToGPUBuffer(pass, &source, &region, true);
}

static bool UploadTexture(SDL_GPUCopyPass *pass, SDL_GPUTexture *texture, U32 layer, U32 x, U32 y, U32 w, U32 h,
                          const void *data, U32 size) {
    void *mapped = SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
    if (mapped == NULL) {
        return false;
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
    return true;
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
    const U8 *seaPage = GpuWater_GetSeaPage();
    if (GpuWaterEnabled && seaPage != NULL)
        PageSlot((U8 *)seaPage);
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
        u.GlassPass = 0.0f;
        u.Pad1 = u.Pad2 = u.Pad3 = 0.0f;
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
        *vertexBytes += draw->Mesh ? 0 : draw->Count * sizeof(T_GPUOBJ_VERTEX);
        n++;
    }
    return n;
}

/* Copies into the mirror the vertices [from, to) of GpuObjVerts, and the frame's
   shadow vertices into their own buffer, a slice at a time. */
static void UploadVertices(SDL_GPUCopyPass *pass, U32 from, U32 to, U32 shadows) {
    const U32 vsize = (U32)sizeof(T_GPUOBJ_VERTEX);
    const U32 perSlice = GPURENDER_VERTEX_SLICE / vsize;
    SDL_GPUTransferBufferLocation source;
    SDL_zero(source);
    source.transfer_buffer = s_vertexTransfer;
    SDL_GPUBufferRegion region;
    SDL_zero(region);

    for (U32 at = from; at < to; at += perSlice) {
        const U32 count = (to - at > perSlice) ? perSlice : (to - at);
        U8 *mapped = (U8 *)SDL_MapGPUTransferBuffer(s_device, s_vertexTransfer, true);
        if (mapped == NULL) {
            return;
        }
        memcpy(mapped, GpuObjVerts + at, count * vsize);
        SDL_UnmapGPUTransferBuffer(s_device, s_vertexTransfer);
        source.offset = 0;
        region.buffer = s_vertexBuffer;
        region.offset = at * vsize;
        region.size = count * vsize;
        SDL_UploadToGPUBuffer(pass, &source, &region, false);
    }

    if (shadows > 0 && s_shadowBuffer != NULL) {
        const U32 bytes = shadows * vsize;
        U8 *mapped = (U8 *)SDL_MapGPUTransferBuffer(s_device, s_vertexTransfer, true);
        if (mapped == NULL) {
            return;
        }
        memcpy(mapped, GpuObjShadowVerts, bytes);
        SDL_UnmapGPUTransferBuffer(s_device, s_vertexTransfer);
        source.offset = 0;
        region.buffer = s_shadowBuffer;
        region.offset = 0;
        region.size = bytes;
        SDL_UploadToGPUBuffer(pass, &source, &region, false);
    }
}

/* The global light (sun or room light) for the shaders: direction toward it and
   sun-shadow strength, the world's up and sky-fill strength, the sun's colour and
   rim strength. A night palette dims the sun and cools it into moonlight. */
static void SetSunlight(float *sun) {
    if (!GpuObjSunlight) {
        return;
    }
    const float day = Daylight();
    const float exterior = GpuObjSunExterior ? 1.0f : 0.0f;

    sun[0] = GpuObjSunDir[0];
    sun[1] = GpuObjSunDir[1];
    sun[2] = GpuObjSunDir[2];
    /* Sun shadows: clear under a daylight sky, faint at night and in rooms. */
    sun[3] = exterior * (0.2f + 0.4f * day) + (1.0f - exterior) * 0.25f;
    sun[4] = GpuObjUpDir[0];
    sun[5] = GpuObjUpDir[1];
    sun[6] = GpuObjUpDir[2];
    sun[7] = 0.22f + 0.12f * exterior * day;
    /* Warm daylight, cool moonlight. */
    sun[8] = 1.0f - 0.35f * (1.0f - day) * exterior;
    sun[9] = 0.9f - 0.1f * (1.0f - day) * exterior;
    sun[10] = 0.75f + 0.15f * (1.0f - day) * exterior;
    sun[11] = 0.18f + 0.14f * exterior * day;
}

/* The composite's shadow rays and ambient occlusion into s_rtHalf, one texel
   for four of the targets' pixels, with the composite's own bindings and
   uniforms (the HALF_PASS build of its shader). */
static void RenderHalf(SDL_GPUCommandBuffer *cmd) {
    if (s_pipelineHalf == NULL || s_rtHalf == NULL || s_rtHalfNext == NULL || s_paramsSize == 0) {
        return;
    }
    SDL_GPUColorTargetInfo target;
    SDL_zero(target);
    target.texture = s_rtHalfNext;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
    if (pass == NULL) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, s_pipelineHalf);
    SDL_GPUTextureSamplerBinding bindings[11];
    SDL_GPUTexture *const textures[11] = {s_objColor, s_objColor,  s_objId,     s_tags,    s_objLight, s_objShadow,
                                          s_objHeight, s_objColor, s_frameIndex, s_palette, s_rtHalf};
    for (int i = 0; i < 11; i++) {
        bindings[i].texture = textures[i];
        bindings[i].sampler = (i == 0 || i == 1 || i == 10) ? s_linear : s_nearest;
    }
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 11);
    SDL_GPUBuffer *storage[2] = {s_bvhNodes, s_bvhTris};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, storage, 2);
    SDL_PushGPUFragmentUniformData(cmd, 0, s_params, s_paramsSize);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    /* The result becomes what the composite reads, and the next pass's history. */
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (copy != NULL) {
        SDL_GPUTextureLocation from;
        SDL_GPUTextureLocation to;
        SDL_zero(from);
        SDL_zero(to);
        from.texture = s_rtHalfNext;
        to.texture = s_rtHalf;
        /* Cycled: the last composite may still be reading it. */
        SDL_CopyGPUTextureToTexture(copy, &from, &to, s_halfW, s_halfH, 1, true);
        SDL_EndGPUCopyPass(copy);
    }
    s_halfReady = true;
    s_rtHistory = true;
    memcpy(s_prevCapMat, GpuObjCapMat, sizeof(s_prevCapMat));
    memcpy(s_prevCapCam, GpuObjCapCam, sizeof(s_prevCapCam));
}

static void RenderDraws(SDL_GPUCommandBuffer *cmd, const T_GPUOBJ_DRAW *const *draws, const T_DRAW_UNIFORMS *uniforms,
                        U32 count) {
    SDL_GPUColorTargetInfo colors[5];
    SDL_zero(colors);
    colors[0].texture = s_objColor;
    colors[1].texture = s_objId;
    colors[2].texture = s_objLight;
    colors[3].texture = s_objShadow;
    colors[4].texture = s_objHeight;
    for (int i = 0; i < 5; i++) {
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

    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, colors, 5, &depth);
    if (pass == NULL) {
        return;
    }
    SDL_GPUGraphicsPipeline *bound = s_pipeline;
    SDL_BindGPUGraphicsPipeline(pass, bound);

    SDL_GPUBufferBinding vbind;
    vbind.buffer = s_vertexBuffer;
    vbind.offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);

    SDL_GPUTextureSamplerBinding samplers[5];
    samplers[0].texture = s_palette;
    samplers[0].sampler = s_nearest;
    samplers[1].texture = s_lut;
    samplers[1].sampler = s_nearest;
    samplers[2].texture = s_pages;
    samplers[2].sampler = s_nearest;
    samplers[3].texture = s_atlas;
    samplers[3].sampler = s_nearest;
    samplers[4].texture = s_lavaAlbedo != NULL ? s_lavaAlbedo : s_palette;
    samplers[4].sampler = s_lavaSampler != NULL ? s_lavaSampler : s_nearest;
    SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);

    const double seconds = (double)SDL_GetTicks() / 1000.0;

    /* std140: 16 position/radius vec4, 16 colour/intensity vec4, count, then
       the global light: sun direction, up, sun colour. */
    float lights[GPUOBJ_MAX_LIGHTS * 8 + 16];
    SDL_zeroa(lights);
    U32 nbLights = GpuObjLightsEnabled ? GpuObjNbLights : 0;
    for (U32 l = 0; l < nbLights; l++) {
        memcpy(lights + l * 4, GpuObjLights[l].Pos, sizeof(float) * 4);
        memcpy(lights + GPUOBJ_MAX_LIGHTS * 4 + l * 4, GpuObjLights[l].Color, sizeof(float) * 4);
    }
    /* Firelight: every live body showing flames casts a warm flickering light. */
    if (GpuObjLightsEnabled && GpuObjFire) {
        for (U32 i = 0; i < count && nbLights < GPUOBJ_MAX_LIGHTS; i++) {
            const T_GPUOBJ_DRAW *draw = draws[i];
            if (!draw->HasFire) {
                continue;
            }
            /* A fire made of many small bodies (a dragon's breath) is one light. */
            S32 merged = FALSE;
            for (U32 l = GpuObjLightsEnabled ? GpuObjNbLights : 0; l < nbLights; l++) {
                const float dx = lights[l * 4] - draw->FirePos[0];
                const float dy = lights[l * 4 + 1] - draw->FirePos[1];
                const float dz = lights[l * 4 + 2] - draw->FirePos[2];
                if (dx * dx + dy * dy + dz * dz < 1200.0f * 1200.0f) {
                    merged = TRUE;
                    break;
                }
            }
            if (merged) {
                continue;
            }
            const float phase = draw->FirePos[0] * 0.013f + draw->FirePos[2] * 0.007f;
            const float flicker = 0.78f + 0.12f * (float)sin(seconds * 9.1 + phase) +
                                  0.07f * (float)sin(seconds * 23.7 + phase * 3.0) + 0.03f * (float)sin(seconds * 41.3);
            float *pos = lights + nbLights * 4;
            pos[0] = draw->FirePos[0];
            pos[1] = draw->FirePos[1];
            pos[2] = draw->FirePos[2];
            pos[3] = 2400.0f;
            float *colour = lights + GPUOBJ_MAX_LIGHTS * 4 + nbLights * 4;
            colour[0] = 1.0f;
            colour[1] = 0.55f;
            colour[2] = 0.2f;
            colour[3] = 0.6f * flicker;
            nbLights++;
        }
    }
    /* Lamp globes on models: a steady light in the globe's colour. They were
       taken in the order the draws came, until the lights ran out -- and the
       ring of cubes is drawn farthest first, in an order the camera changes --
       so with more lamps around than lights, which ones lit (and cast their
       shadows) changed as the camera turned. The nearest to the scene's focus
       now, the rest dropped: a choice only the one the camera follows moves. */
    if (GpuObjLightsEnabled && GpuObjGlow) {
        static float lamp[256][4];
        static float lampColour[256][3];
        U32 nbLamps = 0;
        for (U32 i = 0; i < count && nbLamps < SDL_arraysize(lamp); i++) {
            const T_GPUOBJ_DRAW *draw = draws[i];
            if (!draw->HasGlow) {
                continue;
            }
            S32 merged = FALSE;
            for (U32 l = 0; l < nbLamps; l++) {
                const float dx = lamp[l][0] - draw->GlowPos[0];
                const float dy = lamp[l][1] - draw->GlowPos[1];
                const float dz = lamp[l][2] - draw->GlowPos[2];
                if (dx * dx + dy * dy + dz * dz < 600.0f * 600.0f) {
                    merged = TRUE;
                    break;
                }
            }
            if (merged) {
                continue;
            }
            lamp[nbLamps][0] = draw->GlowPos[0];
            lamp[nbLamps][1] = draw->GlowPos[1];
            lamp[nbLamps][2] = draw->GlowPos[2];
            /* Exterior worlds are drawn at a far larger scale than rooms. */
            lamp[nbLamps][3] = draw->HasGlow == 2 ? 9000.0f : 2400.0f;
            memcpy(lampColour[nbLamps], draw->GlowColor, sizeof(lampColour[nbLamps]));
            nbLamps++;
        }
        const float *focus = GpuObjFocusValid ? GpuObjFocus : NULL;
        while (nbLamps > 0 && nbLights < GPUOBJ_MAX_LIGHTS) {
            U32 best = 0;
            float bestD = 1e30f;
            for (U32 l = 0; l < nbLamps; l++) {
                const float dx = lamp[l][0] - (focus ? focus[0] : 0.0f);
                const float dy = lamp[l][1] - (focus ? focus[1] : 0.0f);
                const float dz = lamp[l][2] - (focus ? focus[2] : 0.0f);
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestD) {
                    bestD = d;
                    best = l;
                }
            }
            float *pos = lights + nbLights * 4;
            memcpy(pos, lamp[best], sizeof(float) * 4);
            float *colour = lights + GPUOBJ_MAX_LIGHTS * 4 + nbLights * 4;
            /* A warm lamplight tinted by the glass. */
            colour[0] = 0.5f + 0.5f * lampColour[best][0];
            colour[1] = 0.39f + 0.39f * lampColour[best][1];
            colour[2] = 0.18f + 0.18f * lampColour[best][2];
            colour[3] = 0.9f;
            nbLights++;
            nbLamps--;
            memcpy(lamp[best], lamp[nbLamps], sizeof(lamp[best]));
            memcpy(lampColour[best], lampColour[nbLamps], sizeof(lampColour[best]));
        }
    }
    lights[GPUOBJ_MAX_LIGHTS * 8] = (float)nbLights;
    /* The composite's shadow rays aim at the lights nearest to the scene's
       focus (to the camera when there is none): nearest the camera, the eight
       changed as it orbited, and their shadows came and went with them. */
    s_nbRtLights = 0;
    const float *focus = GpuObjFocusValid ? GpuObjFocus : NULL;
    for (U32 l = 0; l < nbLights; l++) {
        const float *pos = lights + l * 4;
        const float *colour = lights + GPUOBJ_MAX_LIGHTS * 4 + l * 4;
        const float fx = pos[0] - (focus ? focus[0] : 0.0f), fy = pos[1] - (focus ? focus[1] : 0.0f),
                    fz = pos[2] - (focus ? focus[2] : 0.0f);
        const float d = fx * fx + fy * fy + fz * fz;
        U32 slot = s_nbRtLights;
        if (s_nbRtLights == RT_MAX_LIGHTS) {
            /* Replace the farthest kept, if this one is nearer. */
            U32 far = 0;
            float farD = -1.0f;
            for (U32 k = 0; k < RT_MAX_LIGHTS; k++) {
                const float kx = s_rtLights[k][0] - (focus ? focus[0] : 0.0f),
                            ky = s_rtLights[k][1] - (focus ? focus[1] : 0.0f),
                            kz = s_rtLights[k][2] - (focus ? focus[2] : 0.0f);
                const float dk = kx * kx + ky * ky + kz * kz;
                if (dk > farD) {
                    farD = dk;
                    far = k;
                }
            }
            if (d >= farD) {
                continue;
            }
            slot = far;
        } else {
            s_nbRtLights++;
        }
        memcpy(s_rtLights[slot], pos, sizeof(s_rtLights[slot]));
        s_rtWeights[slot] = colour[3] * (0.3f * colour[0] + 0.59f * colour[1] + 0.11f * colour[2]);
    }
    SetSunlight(lights + GPUOBJ_MAX_LIGHTS * 8 + 4);
    SDL_PushGPUFragmentUniformData(cmd, 1, lights, sizeof(lights));

    /* Fire regions of the object page, when that page is among this frame's. */
    float fire[20];
    SDL_zeroa(fire);
    if (GpuObjFire && GpuObjFirePage != NULL) {
        for (U32 i = 0; i < s_nbPages; i++) {
            if (s_pagePtrs[i] == GpuObjFirePage) {
                for (U32 k = 0; k < GpuObjNbFires; k++) {
                    fire[k * 4 + 0] = (float)GpuObjFireRects[k].X;
                    fire[k * 4 + 1] = (float)GpuObjFireRects[k].Y;
                    fire[k * 4 + 2] = (float)GpuObjFireRects[k].W;
                    fire[k * 4 + 3] = (float)GpuObjFireRects[k].H;
                }
                fire[16] = (float)GpuObjNbFires;
                fire[17] = (float)i;
                break;
            }
        }
    }
    fire[18] = (float)fmod(seconds, 1000.0);
    fire[19] = GpuObjLava && s_lavaReady ? 1.0f : 0.0f;
    SDL_PushGPUFragmentUniformData(cmd, 2, fire, sizeof(fire));
    float water[GPUWATER_UNIFORM_FLOATS];
    GpuWater_GetUniforms(water);
    water[3] = -1.0f;
    const U8 *seaPage = GpuWater_GetSeaPage();
    for (U32 p = 0; p < s_nbPages; p++) {
        if (s_pagePtrs[p] == seaPage && seaPage != NULL) {
            water[3] = (float)p;
            break;
        }
    }
    SDL_PushGPUFragmentUniformData(cmd, 3, water, sizeof(water));

    S32 slice = -1;
    U32 glassDraw[64];
    U32 glassFirst[64];
    float glassSlice[256];
    /* Grass sways in the wind, harder in a storm. */
    const float windTime = (float)fmod(seconds, 3600.0);
    const float wind = GpuStorm_Active() ? 1.8f : 1.0f;
    SDL_Rect glassScissor[64];
    U32 nbGlass = 0;
    for (U32 i = 0; i < count; i++) {
        const T_GPUOBJ_DRAW *draw = draws[i];

        /* Scene draws share the back half of the depth range and test real
           depth against each other. Every other draw is ordered as software
           drew it: a run of z-buffered bodies (drawn front to back, depth
           decides) shares a slice, any other body lands in front of everything
           before it; all of them in the front half, over the scene. */
        float sliceUniform[DRAW_VERTEX_UNIFORM_FLOATS] = {0.0f, 0.0f, windTime, wind};
        /* A draw kept from an earlier frame is placed again here, in the vertex
           shader, through the difference between the camera that captured it and
           the one this frame draws with: rows of the rotation with the
           translation in w, then the projection to rebuild its clip position
           with. Left at zero for a draw captured with the camera drawing it,
           whose clip position stands as the CPU computed it.

           LBA2_GPU_REPLACE=1 sends every retainable draw down this path with its
           own camera on both sides, which must land where it already is: the
           self-test for the arithmetic, and the one to run after touching it. */
        static const char *replaceAll = SDL_getenv("LBA2_GPU_REPLACE");
        const bool replace = draw->HasCapCamera && (draw->Retained || (replaceAll != NULL && draw->Retainable));
        if (replace) {
            const float *a = draw->Retained ? GpuObjCapMat : draw->CapMat;
            const float *nowCam = draw->Retained ? GpuObjCapCam : draw->CapCam;
            const float *b = draw->CapMat;
            for (S32 r = 0; r < 3; r++) {
                for (S32 c = 0; c < 3; c++) {
                    sliceUniform[4 + r * 4 + c] =
                        a[r * 3 + 0] * b[c * 3 + 0] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2];
                }
                sliceUniform[4 + r * 4 + 3] = sliceUniform[4 + r * 4 + 0] * draw->CapCam[0] +
                                              sliceUniform[4 + r * 4 + 1] * draw->CapCam[1] +
                                              sliceUniform[4 + r * 4 + 2] * draw->CapCam[2] - nowCam[r];
            }
        }
        /* The projection, for any draw the vertex shader places itself (a
           retained one, the sea's swell), and the sea's clock. */
        sliceUniform[16] = GpuObjProj[0];
        sliceUniform[17] = GpuObjProj[1];
        sliceUniform[18] = GpuObjProj[2];
        sliceUniform[19] = GpuObjProj[2] * GpuObjProj[3];
        sliceUniform[20] = 2.0f / (float)ModeDesiredX;
        sliceUniform[21] = 2.0f / (float)ModeDesiredY;
        sliceUniform[22] = (float)NearClip;
        sliceUniform[23] = GPUOBJ_SCENE_DEPTH_RANGE;
        sliceUniform[24] = GpuWater_GetTime();
        sliceUniform[25] = GpuWater_GetWeather();
        sliceUniform[26] = GpuWaterEnabled && draw->HasCapCamera ? 1.0f : 0.0f;
        U32 shoreRevision;
        S32 shoreSide;
        const float *shoreField = GpuWater_GetShoreField(&shoreSide, &shoreRevision);
        sliceUniform[27] = shoreField != NULL && shoreSide == GPUWATER_SHORE_SIDE &&
                                   shoreRevision == s_shoreSentRevision
                               ? 1.0f
                               : 0.0f;
        sliceUniform[40] = (float)draw->First;
        sliceUniform[41] = (float)draw->Procedural;
        sliceUniform[42] = GpuObjLava ? 1.0f : 0.0f;
        if (draw->Procedural == GPUOBJ_PROC_LAND) {
            sliceUniform[44] = draw->LandOrigin[0];
            sliceUniform[45] = draw->LandOrigin[1];
            sliceUniform[46] = draw->LandOrigin[2];
            sliceUniform[47] = draw->LandFar;
            sliceUniform[48] = (float)draw->LandSlot;
            sliceUniform[49] = draw->LandSmooth ? 1.0f : 0.0f;
            sliceUniform[50] = draw->LandPhase[0];
            sliceUniform[51] = draw->LandPhase[1];
        }
        if (draw->Mesh) {
            /* A model placed by its own rows, then moved as a kept draw is. */
            float rows[12];
            const float *x = sliceUniform + 4;
            for (S32 r = 0; r < 3; r++) {
                for (S32 c = 0; c < 4; c++) {
                    const float *m = draw->MeshRows;
                    rows[r * 4 + c] = replace ? x[r * 4] * m[c] + x[r * 4 + 1] * m[4 + c] + x[r * 4 + 2] * m[8 + c] +
                                                    (c == 3 ? x[r * 4 + 3] : 0.0f)
                                              : m[r * 4 + c];
                }
            }
            memcpy(sliceUniform + 4, rows, sizeof(rows));
            memcpy(sliceUniform + 52, draw->MeshLight, sizeof(draw->MeshLight));
        }
        if (draw->Tree) {
            memcpy(sliceUniform + 56, draw->TreeUniform, sizeof(draw->TreeUniform));
        }
        if (draw->HasCapCamera) {
            for (S32 r = 0; r < 3; r++) {
                for (S32 c = 0; c < 3; c++) {
                    sliceUniform[28 + r * 4 + c] = draw->CapMat[r * 3 + c];
                }
            }
        }
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
            SDL_GPUGraphicsPipeline *wanted =
                draw->Procedural ? s_pipelineTerrain
                                 : (draw->Mesh ? s_pipelineMesh : (draw->Painter ? s_pipelinePainter : s_pipeline));
            if (wanted != bound) {
                bound = wanted;
                SDL_BindGPUGraphicsPipeline(pass, bound);
                if (draw->Procedural) {
                    SDL_GPUBuffer *storage[4] = {s_vertexBuffer, s_heightBuffer, s_landBuffer, s_shoreBuffer};
                    SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 4);
                } else if (draw->Mesh) {
                    SDL_GPUBufferBinding mbind;
                    mbind.buffer = s_meshBuffer;
                    mbind.offset = 0;
                    SDL_BindGPUVertexBuffers(pass, 0, &mbind, 1);
                } else {
                    SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
                }
                SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
            }
            SDL_SetGPUScissor(pass, &scissor);
            SDL_PushGPUVertexUniformData(cmd, 0, sliceUniform, sizeof(sliceUniform));
            SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms[i], sizeof(T_DRAW_UNIFORMS));
            if (draw->Procedural) {
                U32 grown;
                if (draw->Procedural == GPUOBJ_PROC_LAND) {
                    const S32 records = GpuObjLandCount[draw->LandSlot];
                    grown = records > 0 ? (U32)records * (draw->LandSmooth ? GPUOBJ_PROC_TERRAIN_VERTS : 3u) : 0u;
                } else if (draw->Procedural == GPUOBJ_PROC_SEA) {
                    grown = draw->Count / 4 * GPUOBJ_PROC_SEA_VERTS;
                } else if (draw->Procedural == GPUOBJ_PROC_GRASS) {
                    grown = draw->Count * GPUOBJ_PROC_GRASS_VERTS;
                } else {
                    grown = draw->Count / 3 * GPUOBJ_PROC_TERRAIN_VERTS;
                }
                SDL_DrawGPUPrimitives(pass, grown, 1, 0, 0);
            } else if (draw->NbIndices > 0 && s_indexBuffer != NULL) {
                SDL_GPUBufferBinding ibind;
                SDL_zero(ibind);
                ibind.buffer = s_indexBuffer;
                ibind.offset = 0;
                SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitives(pass, draw->NbIndices, 1, draw->FirstIndex, (Sint32)draw->First, 0);
            } else {
                SDL_DrawGPUPrimitives(pass, draw->Count, 1, draw->First, 0);
            }
            if (draw->HasGlow && nbGlass < SDL_arraysize(glassDraw)) {
                glassDraw[nbGlass] = i;
                glassFirst[nbGlass] = draw->First;
                glassSlice[nbGlass * 4] = sliceUniform[0];
                glassSlice[nbGlass * 4 + 1] = sliceUniform[1];
                glassSlice[nbGlass * 4 + 2] = windTime;
                glassSlice[nbGlass * 4 + 3] = wind;
                glassScissor[nbGlass] = scissor;
                nbGlass++;
            }
        }
    }

    /* Soft shadows over the scene surfaces, occluded by what stands in front. */
    if (GpuObjNbShadowVerts > 0 && GpuObjShadows && s_shadowBuffer != NULL &&
        GpuObjNbShadowVerts * sizeof(T_GPUOBJ_VERTEX) <= s_shadowCapacity) {
        SDL_BindGPUGraphicsPipeline(pass, s_pipelineShadow);
        SDL_GPUBufferBinding sbind;
        sbind.buffer = s_shadowBuffer;
        sbind.offset = 0;
        SDL_BindGPUVertexBuffers(pass, 0, &sbind, 1);
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
        SDL_Rect full;
        full.x = 0;
        full.y = 0;
        full.w = (int)s_targetW;
        full.h = (int)s_targetH;
        SDL_SetGPUScissor(pass, &full);
        const float sceneSlice[DRAW_VERTEX_UNIFORM_FLOATS] = {0.5f, 0.5f, windTime, wind};
        SDL_PushGPUVertexUniformData(cmd, 0, sceneSlice, sizeof(sceneSlice));
        T_DRAW_UNIFORMS u;
        SDL_zero(u);
        u.LutRow = -1.0f;
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_DrawGPUPrimitives(pass, GpuObjNbShadowVerts, 1, 0, 0);
    }

    /* Second pass: lamp glass over everything drawn behind it. */
    if (nbGlass > 0) {
        SDL_BindGPUGraphicsPipeline(pass, s_pipelineGlass);
        SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
        SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
        for (U32 k = 0; k < nbGlass; k++) {
            const U32 i = glassDraw[k];
            T_DRAW_UNIFORMS u = uniforms[i];
            u.GlassPass = 1.0f;
            SDL_SetGPUScissor(pass, &glassScissor[k]);
            float glassUniform[DRAW_VERTEX_UNIFORM_FLOATS] = {glassSlice[k * 4], glassSlice[k * 4 + 1],
                                                              glassSlice[k * 4 + 2], glassSlice[k * 4 + 3]};
            SDL_PushGPUVertexUniformData(cmd, 0, glassUniform, sizeof(glassUniform));
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(T_DRAW_UNIFORMS));
            SDL_DrawGPUPrimitives(pass, draws[i]->Count, 1, glassFirst[k], 0);
        }
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
    Log_Info("GPU        objects rendered by %s on %s", SDL_GetGPUDeviceDriver(s_device),
             SDL_GetStringProperty(SDL_GetGPUDeviceProperties(s_device), SDL_PROP_GPU_DEVICE_NAME_STRING, "?"));
    return renderer;
}

void GpuRender_Shutdown(void) {
    GpuObjAvailable = FALSE;
    ReleasePipeline();
    s_renderer = NULL;
    s_device = NULL;
}

bool GpuRender_TakeFrame(void) {
    s_frameTaken = GpuObjAvailable && GpuObjEnabled && s_renderer != NULL && Log != NULL &&
                   EnsureFrameResources(ModeDesiredX, ModeDesiredY);
    return s_frameTaken;
}

void GpuRender_SetFrameFiltered(bool filtered) {
    s_frameFiltered = filtered;
}

bool GpuRender_BeginComposite(void *sdlRenderer) {
    s_frameIndexed = s_frameTaken;
    s_frameTaken = false;
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
    /* A fresh buffer each present (the GPU may still be reading the last one). */
    float *tagOut = (float *)SDL_MapGPUTransferBuffer(s_device, s_tagTransfer, true);
    if (tagOut == NULL) {
        return false;
    }
    U32 liveCount = GpuObj_BuildPresent(tagOut, &live, box);
    Uint64 t1 = SDL_GetPerformanceCounter();

    static const T_GPUOBJ_DRAW *kept[8192];
    static T_DRAW_UNIFORMS uniforms[8192];
    U32 vertexBytes = 0;
    U32 count = liveCount > 0 ? ResolveDraws(live, liveCount, kept, uniforms, &vertexBytes) : 0;
    /* A present without the half pass leaves nothing to accumulate onto. */
    if (!s_halfReady) {
        s_rtHistory = false;
    }
    s_halfReady = false; /* until this present's half pass has run */
    s_presentNumber++;
    /* Start the hierarchy now; it is waited for just before its upload. */
    static T_BVH_JOB bvhJob;
    SDL_Thread *bvhThread = NULL;
    s_rayScene = GpuObjRayScene && count > 0;
    if (s_rayScene) {
        bvhJob.Draws = kept;
        bvhJob.Count = count;
        bvhJob.Built = false;
        bvhThread = SDL_CreateThread(BvhWorker, "lba2_bvh", &bvhJob);
        if (bvhThread == NULL) {
            BvhWorker(&bvhJob);
        }
    }
    const U32 shadowVerts = count > 0 && GpuObjShadows ? GpuObjNbShadowVerts : 0;
    /* The mirror takes what was written since the last present. */
    const U32 nbVerts = GpuObj_VertexCount();
    const U32 nbIndices = GpuObj_IndexCount();
    U32 mirrorFrom = 0;
    if (s_mirroredBuffer == s_vertexBuffer && s_vertexBuffer != NULL && s_mirroredEpoch == GpuObj_VertexEpoch() &&
        s_mirroredVerts <= nbVerts) {
        mirrorFrom = s_mirroredVerts;
    }
    CollectFlames(kept, count, w, h);
    PushCompositeParams();
    if (count == 0) {
        vertexBytes = 0;
        memset(box, 0, sizeof(box));
    }

    /* With nothing to draw the composite is still worth it when the frame is
       upscaled: the pixel art filter runs there. */
    const bool boxEmpty = box[2] <= box[0] || box[3] <= box[1];
    const bool lastEmpty = s_tagBox[2] <= s_tagBox[0] || s_tagBox[3] <= s_tagBox[1];
    /* A frame taken as indices has no other way to the screen. */
    if (count == 0 && !s_upscale && lastEmpty && s_tagsCleared && !GpuStorm_Active() && !s_frameIndexed) {
        SDL_UnmapGPUTransferBuffer(s_device, s_tagTransfer);
        return false; /* no rays either: count is 0 */
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
    /* The scan wrote the box; the rest of the region goes up cleared. */
    for (S32 y = up[1]; y < up[3]; y++) {
        float *dst = tagOut + (size_t)y * w + up[0];
        if (boxEmpty || y < box[1] || y >= box[3]) {
            memset(dst, 0, upW * sizeof(float));
            continue;
        }
        U32 left = (U32)(box[0] - up[0]);
        U32 inside = (U32)(box[2] - box[0]);
        memset(dst, 0, left * sizeof(float));
        memset(dst + left + inside, 0, (upW - left - inside) * sizeof(float));
    }
    SDL_UnmapGPUTransferBuffer(s_device, s_tagTransfer);
    memcpy(s_tagBox, box, sizeof(s_tagBox));
    U32 need = (U32)SDL_arraysize(s_pagePtrs) * 65536;
    if (s_frameIndexed && w * h > need) {
        need = w * h;
    }
    const U32 atlasBytes = (U32)GPUOBJ_ATLAS_SIZE * GPUOBJ_ATLAS_SIZE * 2;
    const bool atlasUpload = GpuObjAtlasDirty && GpuObjAtlas != NULL;
    if (atlasUpload && atlasBytes > need)
        need = atlasBytes;
    const U32 lavaBytes = s_lavaWidth * s_lavaHeight * 4;
    if (s_lavaPixels != NULL && lavaBytes > need)
        need = lavaBytes;
    const U32 shadowBytes = shadowVerts * (U32)sizeof(T_GPUOBJ_VERTEX);
    /* The hierarchy's size is known only once it is built: room for the largest. */
    if (s_rayScene) {
        const U32 bvhMax = SDL_max((U32)(GPUBVH_MAX_NODES * GPUBVH_NODE_FLOATS * sizeof(float)),
                                   (U32)(GPUBVH_MAX_TRIS * GPUBVH_TRI_FLOATS * sizeof(float)));
        need = SDL_max(need, bvhMax);
    }
    /* So do the height maps, all of them when the buffer is new. */
    const S32 heightFrom = s_heightsSent ? GpuObjHeightMapsDirtyFrom : 0;
    const S32 heightTo = s_heightsSent ? GpuObjHeightMapsDirtyTo : GPUOBJ_MAX_HEIGHTMAPS;
    const U32 heightSlot = GPUOBJ_HEIGHTMAP_SIDE * GPUOBJ_HEIGHTMAP_SIDE * (U32)sizeof(float);
    const bool heightUpload = count > 0 && heightFrom >= 0 && heightTo > heightFrom;
    const U32 landSlot = GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS * (U32)sizeof(U32);
    if (heightUpload) {
        need = SDL_max(need, (U32)(heightTo - heightFrom) * SDL_max(heightSlot, landSlot));
    }
    S32 shoreSide;
    U32 shoreRevision;
    const float *shoreField = GpuWater_GetShoreField(&shoreSide, &shoreRevision);
    const bool shoreUpload = shoreField != NULL && shoreSide == GPUWATER_SHORE_SIDE &&
                             shoreRevision != s_shoreSentRevision;
    const U32 shoreBytes = GPUWATER_SHORE_SIDE * GPUWATER_SHORE_SIDE * (U32)sizeof(float);
    if (shoreUpload)
        need = SDL_max(need, shoreBytes);

    /* The indices ride in the same transfer buffer. */
    if (count > 0 && nbIndices > 0) {
        need = SDL_max(need, nbIndices * (U32)sizeof(U32));
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_device);
    if (cmd == NULL || !EnsureTransfer(need) ||
        (count > 0 && !EnsureVertexBuffer(nbVerts * (U32)sizeof(T_GPUOBJ_VERTEX))) ||
        (count > 0 && nbIndices > 0 && !EnsureIndexBuffer(nbIndices * (U32)sizeof(U32))) ||
        (count > 0 && !EnsureVertexTransfer(shadowBytes > GPURENDER_VERTEX_SLICE ? shadowBytes
                                                                                 : GPURENDER_VERTEX_SLICE)) ||
        (shadowVerts > 0 && !EnsureShadowBuffer(shadowBytes))) {
        if (bvhThread != NULL) {
            SDL_WaitThread(bvhThread, NULL);
        }
        if (cmd != NULL) {
            SDL_CancelGPUCommandBuffer(cmd);
        }
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
    if (s_frameIndexed) {
        UploadTexture(copy, s_frameIndex, 0, 0, 0, w, h, Log, w * h);
    }
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
    if (s_lavaPixels != NULL && s_lavaAlbedo != NULL) {
        s_lavaUploaded = UploadTexture(copy, s_lavaAlbedo, 0, 0, 0, s_lavaWidth, s_lavaHeight, s_lavaPixels,
                                       lavaBytes);
    }
    if (count > 0) {
        /* A new buffer holds nothing yet. */
        const U32 from = s_mirroredBuffer == s_vertexBuffer ? mirrorFrom : 0;
        UploadVertices(copy, from, nbVerts, shadowVerts);
        s_statUploaded += nbVerts - from;
        s_mirroredBuffer = s_vertexBuffer;
        s_mirroredEpoch = GpuObj_VertexEpoch();
        s_mirroredVerts = nbVerts;
        /* The indices behind them, the same way: appended since the last
           present unless the array moved under us. */
        if (nbIndices > 0 && s_indexBuffer != NULL) {
            const U32 fromIndex = (from == 0 || s_mirroredIndices > nbIndices) ? 0 : s_mirroredIndices;
            if (nbIndices > fromIndex) {
                UploadBuffer2(copy, s_indexBuffer, GpuObjIndices + fromIndex, fromIndex * (U32)sizeof(U32),
                              (nbIndices - fromIndex) * (U32)sizeof(U32));
            }
            s_mirroredIndices = nbIndices;
        }
    }
    if (heightUpload) {
        UploadBuffer2(copy, s_heightBuffer, GpuObjHeightMaps + (size_t)heightFrom * (heightSlot / sizeof(float)),
                      (U32)heightFrom * heightSlot, (U32)(heightTo - heightFrom) * heightSlot);
        UploadBuffer2(copy, s_landBuffer, GpuObjLandRecords + (size_t)heightFrom * (landSlot / sizeof(U32)),
                      (U32)heightFrom * landSlot, (U32)(heightTo - heightFrom) * landSlot);
        s_heightsSent = true;
        GpuObjHeightMapsDirtyFrom = -1;
        GpuObjHeightMapsDirtyTo = -1;
    }
    if (shoreUpload) {
        void *mapped = SDL_MapGPUTransferBuffer(s_device, s_transfer, true);
        if (mapped != NULL) {
            memcpy(mapped, shoreField, shoreBytes);
            SDL_UnmapGPUTransferBuffer(s_device, s_transfer);
            SDL_GPUTransferBufferLocation source;
            SDL_zero(source);
            source.transfer_buffer = s_transfer;
            SDL_GPUBufferRegion region;
            SDL_zero(region);
            region.buffer = s_shoreBuffer;
            region.size = shoreBytes;
            SDL_UploadToGPUBuffer(copy, &source, &region, true);
            s_shoreSentRevision = shoreRevision;
        }
    }
    if (count > 0 && s_meshBuffer != NULL) {
        /* The models written since the last present, all of them after a reset. */
        if (s_meshSentEpoch != GpuObjMeshEpoch || s_meshSent > GpuObjMeshCount) {
            s_meshSent = 0;
            s_meshSentEpoch = GpuObjMeshEpoch;
        }
        const U32 vsize = (U32)sizeof(T_GPUOBJ_VERTEX);
        const U32 chunk = s_transferCapacity / vsize;
        while (s_meshSent < GpuObjMeshCount && chunk > 0) {
            const U32 n = SDL_min(chunk, GpuObjMeshCount - s_meshSent);
            UploadBuffer2(copy, s_meshBuffer, GpuObjMeshVerts + s_meshSent, s_meshSent * vsize, n * vsize);
            s_meshSent += n;
        }
    }
    if (bvhThread != NULL) {
        SDL_WaitThread(bvhThread, NULL);
    }
    s_rayScene = s_rayScene && bvhJob.Built;
    if (s_rayScene) {
        UploadBuffer(copy, s_bvhNodes, GpuBvhNodes, GpuBvhNbNodes * GPUBVH_NODE_FLOATS * (U32)sizeof(float));
        UploadBuffer(copy, s_bvhTris, GpuBvhTris, GpuBvhNbTris * GPUBVH_TRI_FLOATS * (U32)sizeof(float));
    }
    if (upW > 0 && upH > 0) {
        SDL_GPUTextureTransferInfo source;
        SDL_zero(source);
        source.transfer_buffer = s_tagTransfer;
        source.offset = ((U32)up[1] * w + (U32)up[0]) * (U32)sizeof(float);
        source.pixels_per_row = w;
        source.rows_per_layer = upH;
        SDL_GPUTextureRegion region;
        SDL_zero(region);
        region.texture = s_tags;
        region.x = (U32)up[0];
        region.y = (U32)up[1];
        region.w = upW;
        region.h = upH;
        region.d = 1;
        SDL_UploadToGPUTexture(copy, &source, &region, false);
    }
    SDL_EndGPUCopyPass(copy);
    if (s_lavaUploaded)
        SDL_GenerateMipmapsForGPUTexture(cmd, s_lavaAlbedo);

    Uint64 t2 = SDL_GetPerformanceCounter();
    if (count > 0) {
        RenderDraws(cmd, kept, uniforms, count);
        /* Shadow rays and occlusion at half resolution, outdoors, where they
           cost the most; the composite then reads them. */
        if (s_rayScene || (GpuObjSunlight && GpuObjSunExterior)) {
            PushCompositeParams();
            RenderHalf(cmd);
        }
        PushCompositeParams();
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        s_shoreSentRevision = ~0u;
        s_lavaUploaded = false;
        return false;
    }
    if (s_lavaUploaded) {
        s_lavaReady = true;
        s_lavaUploaded = false;
        free(s_lavaPixels);
        s_lavaPixels = NULL;
    }
    Uint64 t3 = SDL_GetPerformanceCounter();

    if (GpuObjVertsExhausted > 0) {
        static bool said = false;
        if (!said) {
            said = true;
            Log_Info("[GPU] the vertex budget (%u) ran out: some draws keep their software pixels",
                     (unsigned)GPUOBJ_MAX_VERTS);
        }
    }

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
            Log_Info("[GPU] per present: tags %.2f ms, resolve+upload %.2f ms, render+submit %.2f ms, %u draws, %u verts, "
                     "%u uploaded",
                     sumTags / frames, sumUpload / frames, sumRender / frames, sumDraws / frames, sumVerts / frames,
                     s_statUploaded / frames);
            s_statUploaded = 0;
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
