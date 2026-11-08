// The co-op name tags drawn into the game's frame right before its HUD, so the HUD and the game's own menus cover
// them, and hidden by the scene's depth behind walls and hills (TOTK_COOP_TAGS_INFRAME=0: the overlay draws them over
// everything, as before). ui/coop_tags.cpp lays them out with Dear ImGui (vk_tags_set); this file draws them with
// shaders/coop_tag_vs.vert and coop_tag_fs.frag.
//
// Where the HUD starts (TOTK_DUMP of frame 3600 at Lookout Landing, 1.4.3, 4096x2304 scene, 3840x2160 output,
// 2026-11-07): after the bloom chain (passes narrower than a quarter of the scene), the tonemap draws into a scene-sized
// R11G11B10F target, one pass draws it back over the light target, and one full-screen draw scales that into an
// output-sized R11G11B10F target with no depth (the HUD target). The HUD's own small targets come next (minimap masks,
// 144x144 and smaller), then six passes draw the HUD into the HUD target, and a last pass copies the HUD target into
// the RGBA8 sRGB window texture the game presents. With a scene as large as the output the scene's last draw goes into
// the HUD target the same way (vk_dlaa.c, "where DLAA runs in TotK's frame": the game's FXAA draws back over it). So:
// - the HUD target is learned from the copy: a pass into an 8-bit sRGB target right after a pass into a float target of
//   its size (not a plain RGBA8 one: the game copies other float pictures into those mid-frame). That copy decodes nothing, so the HUD target holds linear colour, and the tags' sRGB colours are decoded.
// - after the bloom chain, a pass at least half the HUD target's width into other targets arms the frame; the tags go
//   in front of the second draw into the HUD target after it: the first is the scene's own (the scaling, or the FXAA
//   draw back), the second the HUD's first.
// - the camera: the scene camera block (constant bank c4 of the scene's draws, 0x940 bytes; vk_dlaa.c mv_pass), whose
//   vec4 rows 3-6 are the view-projection, row-major, OpenGL clip depth; the far plane is its c4[14].y
//   (vk_contact.c). Each tag's anchor (above the player's head) is projected with it, so the tag sits where the frame
//   drew the player.
// - the linear depth: the G-buffer's R32F target (view distance / far; vk_contact.c is_gbuffer), still whole at the
//   HUD (the game's own dispatch after the bloom reads it). The vertex shader reads it around the anchor.
// If no HUD start is found for 30 frames (another game version, a frame the dump did not show), vk_tags_live() turns 0
// and the overlay draws the tags itself.
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop_tag_fs_spv.h"
#include "coop_tag_vs_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "vk_tags.h"

#define MAX_VTX 8192
#define MAX_IDX 16384
#define CAM_BLOCK 0x940u

static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

// ---- from the render thread
static PlatMutex lock = PLAT_MUTEX_INIT;
static struct {
    VkTagVert v[MAX_VTX];
    uint16_t idx[MAX_IDX];
    VkTagItem it[VK_TAGS_MAX];
    uint32_t nv, ni, gen;
    int n;
    float win_h;
    uint64_t at;   // frame_now() when set: the overlay stops setting them when it has nothing to draw
} S;                                                        // lock: the newest tags
static struct { uint8_t *px; int w, h; uint32_t gen; } F;   // lock: the newest font atlas
static uint64_t point_frame;                                // the frame the HUD start was last found in, +1 (atomic)

static int infra_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_COOP_TAGS_INFRAME");
        on = !(e && !strcmp(e, "0"));
    }
    return on;
}

int vk_tags_live(void) {
    uint64_t p = __atomic_load_n(&point_frame, __ATOMIC_RELAXED);
    return infra_on() && p && frame_now() + 1 - p < 30;
}

void vk_tags_set(const VkTagVert *v, uint32_t nv, const uint16_t *idx, uint32_t ni, const VkTagItem *items, int n,
                 float win_h, uint32_t font_gen) {
    if (nv > MAX_VTX || ni > MAX_IDX) n = 0;
    if (n > VK_TAGS_MAX) n = VK_TAGS_MAX;
    plat_mutex_lock(&lock);
    S.n = n > 0 ? n : 0;
    S.at = frame_now();
    if (S.n) {
        memcpy(S.v, v, nv * sizeof *v), memcpy(S.idx, idx, ni * sizeof *idx), memcpy(S.it, items, (size_t)n * sizeof *items);
        S.nv = nv, S.ni = ni, S.win_h = win_h, S.gen = font_gen;
    }
    plat_mutex_unlock(&lock);
}

uint32_t vk_tags_font(const uint8_t *rgba, int w, int h) {
    uint8_t *c = rgba && w > 0 && h > 0 ? malloc((size_t)w * h * 4) : NULL;
    if (c) memcpy(c, rgba, (size_t)w * h * 4);
    plat_mutex_lock(&lock);
    free(F.px);
    F.px = c, F.w = c ? w : 0, F.h = c ? h : 0;
    uint32_t g = ++F.gen;
    plat_mutex_unlock(&lock);
    return g;
}

// ---- the replay
static struct {
    int tried, ok, logged, warned;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkSampler lin, near;
    VkShaderModule vs, fs;
    struct { VkFormat fmt; VkPipeline p; } pipes[4];
    int npipes;
    VkImage font;
    VmaAllocation font_mem;
    VkImageView font_view;
    uint32_t font_gen;
    // learned across frames
    const VkTexImage *hud;                // the HUD target, its view format and size; hud_linear: copied into sRGB
    VkFormat hud_fmt;
    uint32_t hud_w, hud_h;
    int hud_linear;
    // this frame
    const VkTexImage *prev;               // the last pass's first colour target, its format and size
    VkFormat prev_fmt;
    uint32_t prev_w, prev_h;
    const VkTexImage *depth, *zbuf;       // the G-buffer's linear depth and its D32 depth
    uint32_t gbuf_w;
    uint64_t cam_vs, cam_fs;              // the scene camera blocks of the vertex and fragment stages
    int phase, armed, count, done, inside;   // phase 1: G-buffer seen, 2: the bloom began
} T;

static int init(VkCtx *c) {
    T.tried = 1;
    T.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 48};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &T.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkSamplerCreateInfo sn = si;
    sn.magFilter = sn.minFilter = VK_FILTER_NEAREST;
    VkShaderModuleCreateInfo mv = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof coop_tag_vs_spv,
                                   .pCode = coop_tag_vs_spv};
    VkShaderModuleCreateInfo mf = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof coop_tag_fs_spv,
                                   .pCode = coop_tag_fs_spv};
    if (!T.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &T.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &T.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &T.lin) != VK_SUCCESS ||
        vkCreateSampler(c->device, &sn, NULL, &T.near) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mv, NULL, &T.vs) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mf, NULL, &T.fs) != VK_SUCCESS) {
        rt_log("vk: co-op name tags in the frame off: their pipeline could not be set up");
        return 0;
    }
    return T.ok = 1;
}

// The pipeline for a HUD target of format fmt: blended like Dear ImGui, no depth.
static VkPipeline pipeline(VkCtx *c, VkFormat fmt) {
    for (int i = 0; i < T.npipes; i++)
        if (T.pipes[i].fmt == fmt) return T.pipes[i].p;
    if (T.npipes == 4) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = T.vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = T.fs, .pName = "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(VkTagVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                               {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
                                               {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    // the counts are dynamic too: the recorder (vk_rec.h) has packets for vkCmdSetViewportWithCount and
    // vkCmdSetScissorWithCount, not for vkCmdSetViewport and vkCmdSetScissor
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          (fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 0 : VK_COLOR_COMPONENT_A_BIT)};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = T.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    T.pipes[T.npipes].fmt = fmt, T.pipes[T.npipes++].p = p;
    return p;
}

typedef struct { VkBuffer b; VmaAllocation a; VkImage img; VmaAllocation img_mem; VkImageView view; } Gone;
static void gone(void *arg) {   // completion thread: what the finished submission used
    Gone *g = arg;
    VkCtx *c = vk_ctx();
    if (g->b) vmaDestroyBuffer(c->vma, g->b, g->a);
    if (g->view) vkDestroyImageView(c->device, g->view, NULL);
    if (g->img) vmaDestroyImage(c->vma, g->img, g->img_mem);
    free(g);
}

// A host-visible buffer for this submission, mapped at *map; freed when it completes.
static VkBuffer host_buffer(VkCtx *c, VkExec *ex, VkDeviceSize size, VkBufferUsageFlags usage, void **map) {
    Gone *g = calloc(1, sizeof *g);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info;
    if (!g || vmaCreateBuffer(c->vma, &bi, &ai, &g->b, &g->a, &info) != VK_SUCCESS || !info.pMappedData) {
        if (g && g->b) vmaDestroyBuffer(c->vma, g->b, g->a);
        free(g);
        return VK_NULL_HANDLE;
    }
    *map = info.pMappedData;
    vk_completion_add(ex->fence, gone, g);
    return g->b;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = to, .image = img,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &d);
}

// The font atlas of generation gen into T.font, through a staging buffer (outside a pass). 0: none.
static int font_up(VkCtx *c, VkExec *ex, uint32_t gen) {
    if (T.font && T.font_gen == gen) return 1;
    plat_mutex_lock(&lock);
    int w = F.w, h = F.h, ok = F.gen == gen && F.px;
    void *map = NULL;
    VkBuffer sb = ok ? host_buffer(c, ex, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &map) : VK_NULL_HANDLE;
    if (sb) memcpy(map, F.px, (size_t)w * h * 4);
    plat_mutex_unlock(&lock);
    if (!sb) return 0;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {(uint32_t)w, (uint32_t)h, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(c->vma, &ii, &ai, &img, &mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(c->device, &vi, NULL, &view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, img, mem);
        return 0;
    }
    image_barrier(ex->cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(ex->cb, sb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(ex->cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (T.font) {   // the old atlas goes when this submission is done (the ones before it used it)
        Gone *g = calloc(1, sizeof *g);
        if (g) g->img = T.font, g->img_mem = T.font_mem, g->view = T.font_view, vk_completion_add(ex->fence, gone, g);
    }
    T.font = img, T.font_mem = mem, T.font_view = view, T.font_gen = gen;
    return 1;
}

static int is_float(VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT; }
static int is_srgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

static int targets(const VkExec *ex, const VkTexImage *img) {
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++)
        if (ex->color[i].img == img) return 1;
    return 0;
}

void vk_tags_pass_begin(VkExec *ex) {
    if (T.inside || !infra_on()) return;
    // the G-buffer: an R32F linear depth with a D32F depth at one size (vk_contact.c is_gbuffer)
    if (!ex->pass_noatt && ex->pass_w >= 640 && ex->depth.format == VK_FORMAT_D32_SFLOAT && !ex->depth.level)
        for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
            const VkExecTarget *t = &ex->color[i];
            if (t->image && !t->level && t->format == VK_FORMAT_R32_SFLOAT && t->width == ex->pass_w &&
                t->height == ex->pass_h) {
                T.depth = t->img, T.zbuf = ex->depth.img, T.gbuf_w = ex->pass_w;
                if (!T.phase) T.phase = 1;
                break;
            }
        }
    if (T.phase == 1 && ex->pass_w * 4 < T.gbuf_w) T.phase = 2;   // the bloom chain
    const VkExecTarget *c0 = ex->pass_ncolor ? &ex->color[0] : NULL;
    if (!c0 || !c0->image || !c0->img) return;
    // the copy into the window texture: the pass before it drew into the HUD target. Only an sRGB target: the game also
    // copies float pictures into plain RGBA8 ones of other sizes in the middle of a frame (1636x920 and 2048x1152 at a
    // 3840x2160 output, 2026-11-07), and taking those for the HUD target flipped it from frame to frame.
    if (ex->pass_ncolor == 1 && is_srgb8(c0->format) && T.prev && T.prev != c0->img && is_float(T.prev_fmt) &&
        T.prev_w == c0->width && T.prev_h == c0->height) {
        if (T.hud != T.prev || T.hud_fmt != T.prev_fmt) {
            T.hud = T.prev, T.hud_fmt = T.prev_fmt, T.hud_w = T.prev_w, T.hud_h = T.prev_h, T.hud_linear = 1;
            static int logs;
            if (logs < 8) logs++, rt_log("vk: co-op name tags: the HUD target is %ux%u (format %d), copied into an sRGB "
                                         "window texture", T.hud_w, T.hud_h, (int)T.hud_fmt);
        }
    }
    if (T.phase == 2 && T.hud && !targets(ex, T.hud) && ex->pass_w * 2 >= T.hud_w) T.armed = 1, T.count = 0;
    T.prev = c0->img, T.prev_fmt = c0->format, T.prev_w = c0->width, T.prev_h = c0->height;
}

void vk_tags_frame(void) {
    T.prev = NULL, T.depth = NULL, T.cam_vs = T.cam_fs = 0;
    T.phase = T.armed = T.count = T.done = 0;
}

static void record(VkExec *ex);

void vk_tags_draw(VkExec *ex) {
    if (T.inside || !infra_on()) return;
    if (T.zbuf && ex->depth.img == T.zbuf) {   // a scene draw: its camera blocks
        const p_bind_buf *b = &ex->ubo[0][1];
        if (b->addr && b->size == CAM_BLOCK) T.cam_vs = b->addr;
        b = &ex->ubo[1][1];
        if (b->addr && b->size == CAM_BLOCK) T.cam_fs = b->addr;
    }
    if (!T.armed || T.done || !T.hud || ex->color[0].img != T.hud || ++T.count < 2) return;
    T.done = 1;
    __atomic_store_n(&point_frame, frame_now() + 1, __ATOMIC_RELAXED);
    record(ex);
}

static const float *block(uint64_t addr, uint64_t off, uint64_t n) {
    if (!addr) return NULL;
    const float *p = (const float *)vk_usnap_bytes(addr + off, n);
    return p ? p : (const float *)(uintptr_t)(addr + off);   // guest memory, as vk_contact.c reads it
}

// Every vkCmd* here goes through the recorder thread's wrappers (vk_rec.h): a command without one is made at once on
// this thread, ahead of the recorder's queue, into the command buffer the recorder is filling. vkCmdSetViewport,
// vkCmdSetScissor and vkCmdBindVertexBuffers did that (2026-11-07: a GPU fault and device lost in both games right
// after the first tags were drawn), so the *WithCount and *2 forms are used.
static void record(VkExec *ex) {
    VkCtx *c = vk_ctx();
    if (!c || (!T.tried && !init(c)) || !T.ok) return;
    static VkTagVert v[MAX_VTX];
    static uint16_t idx[MAX_IDX];
    static VkTagItem it[VK_TAGS_MAX];
    plat_mutex_lock(&lock);
    int n = frame_now() - S.at < 10 ? S.n : 0;
    uint32_t nv = S.nv, ni = S.ni, gen = S.gen;
    float win_h = S.win_h;
    if (n) memcpy(v, S.v, nv * sizeof *v), memcpy(idx, S.idx, ni * sizeof *idx), memcpy(it, S.it, (size_t)n * sizeof *it);
    plat_mutex_unlock(&lock);
    const float *vp = block(T.cam_vs, 48, 64), *far = block(T.cam_fs ? T.cam_fs : T.cam_vs, 14 * 16, 16);
    if (!n || !vp || !(win_h > 0)) return;
    // where each anchor is on the HUD target, and how deep
    float W = (float)T.hud_w, H = (float)T.hud_h, ax[VK_TAGS_MAX], ay[VK_TAGS_MAX], az[VK_TAGS_MAX];
    int show = 0;
    for (int i = 0; i < n; i++) {
        double p[4] = {it[i].world[0], it[i].world[1], it[i].world[2], 1}, q[4];
        for (int r = 0; r < 4; r++) q[r] = vp[r * 4] * p[0] + vp[r * 4 + 1] * p[1] + vp[r * 4 + 2] * p[2] + vp[r * 4 + 3];
        az[i] = (float)q[3];
        if (!(q[3] > 0.3)) continue;
        ax[i] = (float)((q[0] / q[3] * 0.5 + 0.5) * W), ay[i] = (float)((0.5 - q[1] / q[3] * 0.5) * H);
        if (ax[i] < -W * 0.25f || ax[i] > W * 1.25f || ay[i] < -H * 0.25f || ay[i] > H * 1.25f) az[i] = 0;
        else show++;
    }
    float zfar = far ? far[1] : 0;
    if (!T.logged) {
        T.logged = 1;
        rt_log("vk: co-op name tags drawn in the frame: HUD target %ux%u, far %g, %s", T.hud_w, T.hud_h, zfar,
               T.depth ? "hidden by the scene depth" : "no scene depth");
    }
    if (!show) return;
    int reopen;
    T.inside = 1;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    VkImageView hv = cb ? vk_exec_target_view(T.hud, T.hud_fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0)
                        : VK_NULL_HANDLE;
    VkImageView dv = T.depth ? vk_exec_target_view(T.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0)
                             : VK_NULL_HANDLE;
    VkPipeline pipe = hv ? pipeline(c, T.hud_fmt) : VK_NULL_HANDLE;
    void *map = NULL;
    VkDeviceSize ioff = ((VkDeviceSize)nv * sizeof *v + 3) & ~(VkDeviceSize)3;
    VkBuffer buf = pipe && font_up(c, ex, gen)
                       ? host_buffer(c, ex, ioff + ni * sizeof *idx, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &map)
                       : VK_NULL_HANDLE;
    if (buf) {
        memcpy(map, v, nv * sizeof *v), memcpy((uint8_t *)map + ioff, idx, ni * sizeof *idx);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = hv,
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {T.hud_w, T.hud_h}},
                              .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkViewport vpt = {0, 0, W, H, 0, 1};
        VkRect2D sc = {{0, 0}, {T.hud_w, T.hud_h}};
        vkCmdSetViewportWithCount(cb, 1, &vpt);
        vkCmdSetScissorWithCount(cb, 1, &sc);
        VkDescriptorImageInfo fi = {T.lin, T.font_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo di = dv ? (VkDescriptorImageInfo){T.near, dv, VK_IMAGE_LAYOUT_GENERAL} : fi;   // unread without depth
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &fi},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &di}};
        rec_push(T.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, T.layout, 0, 2, w);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers2(cb, 0, 1, &buf, &zero, NULL, NULL);
        vkCmdBindIndexBuffer(cb, buf, ioff, VK_INDEX_TYPE_UINT16);
        int occlude = dv && zfar > 1;
        for (int i = 0; i < n; i++) {
            if (!(az[i] > 0.3f) || it[i].first + it[i].count > ni) continue;
            float pc[12] = {ax[i], ay[i], 2.0f / W, 2.0f / H,
                            az[i], zfar, 0.004f, (float)occlude,
                            1.0f, (float)T.hud_linear, H / win_h, 0};
            vkCmdPushConstants(cb, T.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
            vkCmdDrawIndexed(cb, it[i].count, 1, it[i].first, 0, 0);
        }
        vkCmdEndRendering(cb);
    } else if (!T.warned) {
        T.warned = 1;
        rt_log("vk: co-op name tags in the frame: nothing drawn (%s)", !cb ? "not recording" : !hv ? "no view of the HUD target"
               : !pipe ? "no pipeline" : "no font atlas or buffer");
    }
    ex->recorded += 12;
    if (cb) vk_exec_inline_end(reopen);
    // the game's next draw binds its pipeline, state, descriptors and vertex buffers again
    vk_exec_draw_new_cb();
    ex->dirty |= VK_EXEC_DIRTY_ALL;
    T.inside = 0;
}

void vk_tags_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !T.tried) return;
    for (int i = 0; i < T.npipes; i++) vkDestroyPipeline(c->device, T.pipes[i].p, NULL);
    if (T.font_view) vkDestroyImageView(c->device, T.font_view, NULL);
    if (T.font) vmaDestroyImage(c->vma, T.font, T.font_mem);
    if (T.vs) vkDestroyShaderModule(c->device, T.vs, NULL);
    if (T.fs) vkDestroyShaderModule(c->device, T.fs, NULL);
    if (T.lin) vkDestroySampler(c->device, T.lin, NULL);
    if (T.near) vkDestroySampler(c->device, T.near, NULL);
    if (T.layout) vkDestroyPipelineLayout(c->device, T.layout, NULL);
    if (T.dsl) vkDestroyDescriptorSetLayout(c->device, T.dsl, NULL);
    memset(&T, 0, sizeof T);
}
