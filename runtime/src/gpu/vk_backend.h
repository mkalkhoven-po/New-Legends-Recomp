// Vulkan backend: the host window, device and the operations PGRAPH needs
// (render targets, clears, draws, presentation). Everything except the
// window message loop runs on the GPU (FIFO) thread.
#pragma once
#include <string>
#include <vector>

#include "gpu.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// Render target formats, resolved from NV097_SET_SURFACE_FORMAT.
enum class ColorFmt { None, R5G6B5, X1R5G5B5, X8R8G8B8, A8R8G8B8, B8, G8B8 };
enum class ZetaFmt { None, Z16, Z24S8 };

struct RenderTargets {
    u32 color_addr = 0, zeta_addr = 0;  // guest addresses (0 = none)
    ColorFmt color = ColorFmt::None;
    ZetaFmt zeta = ZetaFmt::None;
    u32 width = 0, height = 0;
    u32 color_pitch = 0, zeta_pitch = 0;
};

// One texture stage as the shaders see it.
struct TextureDesc {
    u32 addr = 0, palette_addr = 0, palette_len = 0;
    u32 format = 0;  // NV097_SET_TEXTURE_FORMAT_COLOR_*
    u32 width = 0, height = 0, depth = 1, levels = 1, pitch = 0;
    bool cube = false, linear = false;
    u32 address = 0, filter = 0, control0 = 0, border = 0;
};

// Fixed-function state that becomes part of the Vulkan pipeline.
struct PipelineState {
    u32 topology;  // VkPrimitiveTopology
    u8 blend_enable, blend_src, blend_dst, blend_op;           // VkBlendFactor / VkBlendOp
    u8 color_mask;                                             // VkColorComponentFlags
    u8 depth_test, depth_write, depth_op;                      // VkCompareOp
    u8 stencil_test, st_fail, st_zfail, st_pass, st_op;        // VkStencilOp / VkCompareOp
    u8 st_ref, st_read_mask, st_write_mask;
    u8 cull_mode, front_face, polygon_mode;
    u8 pad[3];
};

struct VertexAttrib {
    VkFormat format;  // VK_FORMAT_UNDEFINED = not fetched
    u32 offset;       // within the vertex
};
struct VertexLayout {
    VertexAttrib attr[16];
    u32 stride;
};

struct DrawCall {
    const std::string *vs, *fs;  // GLSL (cached strings, see vertex_shader)
    PipelineState ps;
    VertexLayout layout;
    const void* vertices;  // vertex data, `count` vertices of layout.stride bytes
    u32 vertex_bytes;
    std::vector<u32> indices;
    const void* uniforms;
    u32 uniform_bytes;
    TextureDesc tex[4];
    u32 tex_mask;  // stages with a texture bound
    u32 blend_color;
    float depth_bias_factor, depth_bias_units;
};

void vkb_init();
void vkb_set_targets(const RenderTargets& rt);
void vkb_set_scissor(int x, int y, int w, int h);
// flags: NV097_CLEAR_SURFACE bits (Z 0x1, S 0x2, R/G/B/A 0x10..0x80).
void vkb_clear(u32 flags, u32 color_argb, float depth, u32 stencil, int x0, int y0, int x1, int y1);
void vkb_draw(const DrawCall& d);
// End of frame: show the render target at `color_addr`.
void vkb_present(u32 color_addr);
// A rectangle between surfaces. Done: handled on render targets. Memory: not
// render targets, the caller copies guest memory. SourceIsTarget: the pixels
// are only in a render target and the destination is plain memory.
enum class BlitResult { Done, Memory, SourceIsTarget };
BlitResult vkb_blit(u32 src, u32 dst, u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h, u32 src_pitch, u32 dst_pitch, u32 bpp);
u32 vkb_frame_no();
u32 vkb_shader_id(const std::string& src, bool fragment);
