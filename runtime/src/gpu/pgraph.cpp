// PGRAPH: methods of the graphics objects (Kelvin 3D class 0x97 and the 2D
// helpers) as they come out of the FIFO, turned into Vulkan work.
#include <condition_variable>
#include <map>
#include <unordered_map>

#include "kelvin.h"

static bool g_trace = getenv("NL_TRACE_PGRAPH") != nullptr;
static bool g_trace_methods = getenv("NL_TRACE_METHODS") != nullptr;

static Kelvin g_k;
static std::map<u32, u32> g_unknown_class;
static bool g_vk_ready;

static u32 obj_class(u32 inst) {
    if (inst == ~0u) return 0;
    u32 w;
    memcpy(&w, ramin_ptr(inst), 4);
    return w & 0xFF;
}

// DMA object lookups go through RAMHT; cache them per handle.
static std::unordered_map<u32, u32> g_dma_cache;
static u32 dma_handle_base(u32 h) {
    auto it = g_dma_cache.find(h);
    if (it != g_dma_cache.end()) return it->second;
    u32 b = dma_object(ramht_lookup(h)).base;
    g_dma_cache[h] = b;
    return b;
}
static u32 dma_base(u32 ctx_method) { return dma_handle_base(g_k.r(ctx_method)); }

// Guest address of `offset` within the DMA object bound by `ctx_method`.
static u32 dma_addr(u32 ctx_method, u32 offset) { return gpu_addr(dma_base(ctx_method) + offset); }

static void semaphore_release(u32 value) {
    u32 a = dma_addr(NV097_SET_CONTEXT_DMA_SEMAPHORE, g_k.r(NV097_SET_SEMAPHORE_OFFSET));
    if (g_trace) log_msg("[pgraph] semaphore %08X <- %08X", a, value);
    wr32(a, value);
}

// ------------------------------------------------------- software methods
static u32 g_subch;
static std::mutex g_trap_m;
static std::condition_variable g_trap_cv;

static void pgraph_trap(u32 method, u32 v) {
    if (g_trace) log_msg("[pgraph] software method %08X", v);
    {
        std::lock_guard<std::recursive_mutex> lk(g_nv_lock);
        nv_reg(NV_PGRAPH_TRAPPED_ADDR) = (g_subch << 16) | method;  // channel 0
        nv_reg(NV_PGRAPH_TRAPPED_DATA_LOW) = v;
        nv_reg(NV_PGRAPH_NSOURCE) = 1;  // NOTIFICATION
        nv_reg(NV_PGRAPH_INTR) |= NV_PGRAPH_INTR_ERROR;
    }
    nv2a_update_irq();
    std::unique_lock<std::mutex> lk(g_trap_m);
    // No g_nv_lock here: the acknowledging write holds it while it calls
    // pgraph_intr_cleared, which takes g_trap_m. A plain read is atomic.
    g_trap_cv.wait(lk, [] { return !(*(volatile u32*)&nv_reg(NV_PGRAPH_INTR) & NV_PGRAPH_INTR_ERROR); });
}

// The interrupt handler acknowledged a trap: let the FIFO continue.
void pgraph_intr_cleared() {
    { std::lock_guard<std::mutex> lk(g_trap_m); }
    g_trap_cv.notify_all();
}

// --------------------------------------------------------- render targets
static RenderTargets current_targets() {
    RenderTargets rt;
    u32 fmt = g_k.r(NV097_SET_SURFACE_FORMAT), pitch = g_k.r(NV097_SET_SURFACE_PITCH);
    switch (fmt & 15) {
    case 1: case 2: rt.color = ColorFmt::X1R5G5B5; break;
    case 3: rt.color = ColorFmt::R5G6B5; break;
    case 4: case 5: case 6: case 7: rt.color = ColorFmt::X8R8G8B8; break;
    case 8: rt.color = ColorFmt::A8R8G8B8; break;
    case 9: rt.color = ColorFmt::B8; break;
    case 10: rt.color = ColorFmt::G8B8; break;
    }
    switch (fmt >> 4 & 15) {
    case 1: rt.zeta = ZetaFmt::Z16; break;
    case 2: rt.zeta = ZetaFmt::Z24S8; break;
    }
    rt.color_pitch = pitch & 0xFFFF;
    rt.zeta_pitch = pitch >> 16;
    u32 ch = g_k.r(NV097_SET_SURFACE_CLIP_HORIZONTAL), cv = g_k.r(NV097_SET_SURFACE_CLIP_VERTICAL);
    rt.width = (ch & 0xFFFF) + (ch >> 16);
    rt.height = (cv & 0xFFFF) + (cv >> 16);
    if (rt.color != ColorFmt::None && rt.color_pitch)
        rt.color_addr = dma_addr(NV097_SET_CONTEXT_DMA_COLOR, g_k.r(NV097_SET_SURFACE_COLOR_OFFSET));
    if (rt.zeta != ZetaFmt::None && rt.zeta_pitch && g_k.r(NV097_SET_SURFACE_ZETA_OFFSET))
        rt.zeta_addr = dma_addr(NV097_SET_CONTEXT_DMA_ZETA, g_k.r(NV097_SET_SURFACE_ZETA_OFFSET));
    return rt;
}

static void bind_targets() {
    vkb_set_targets(current_targets());
    u32 ch = g_k.r(NV097_SET_SURFACE_CLIP_HORIZONTAL), cv = g_k.r(NV097_SET_SURFACE_CLIP_VERTICAL);
    vkb_set_scissor(ch & 0xFFFF, cv & 0xFFFF, ch >> 16, cv >> 16);
}

static void clear_surface(u32 flags) {
    bind_targets();
    u32 h = g_k.r(NV097_SET_CLEAR_RECT_HORIZONTAL), v = g_k.r(NV097_SET_CLEAR_RECT_VERTICAL);
    u32 zs = g_k.r(NV097_SET_ZSTENCIL_CLEAR_VALUE);
    float depth;
    u32 stencil = 0;
    if (((g_k.r(NV097_SET_SURFACE_FORMAT) >> 4) & 15) == 1) {
        depth = (zs & 0xFFFF) / 65535.0f;
    } else {
        depth = (zs >> 8) / 16777215.0f;
        stencil = zs & 0xFF;
    }
    if (g_trace) log_msg("[pgraph] clear %08X color %08X", flags, g_k.r(NV097_SET_COLOR_CLEAR_VALUE));
    vkb_clear(flags, g_k.r(NV097_SET_COLOR_CLEAR_VALUE), depth, stencil, h & 0xFFFF, v & 0xFFFF, h >> 16, v >> 16);
}

// ------------------------------------------------------------------- draws
static u32 g_prim;
enum DrawKind { DK_NONE, DK_ELEMENTS, DK_INLINE, DK_IMMEDIATE };
static DrawKind g_kind;
static std::vector<u32> g_elements;     // vertex indices (arrays and elements)
static std::vector<u32> g_inline;       // INLINE_ARRAY words
static std::vector<float> g_immediate;  // 16 x vec4 per vertex
static std::vector<u8> g_vbuf;

struct AttrFmt {
    u32 type, size, stride;
};
static AttrFmt attr_fmt(u32 i) {
    u32 v = g_k.r(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * i);
    return {v & 15, v >> 4 & 15, v >> 8};
}

// Bytes one attribute occupies in guest vertex data.
static u32 attr_bytes(const AttrFmt& f) {
    switch (f.type) {
    case 0: case 4: return f.size;      // UB_D3D / UB_OGL
    case 1: case 5: return 2 * f.size;  // S1 / S32K
    case 2: return 4 * f.size;          // F
    case 6: return 4;                   // CMP
    }
    return 0;
}

static VkFormat attr_vkformat(const AttrFmt& f) {
    static const VkFormat fl[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                  VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat ub[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat s1[] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM,
                                  VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat sk[] = {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                                  VK_FORMAT_R16G16B16A16_SSCALED};
    u32 n = (f.size - 1) & 3;
    switch (f.type) {
    case 0: return f.size == 4 ? VK_FORMAT_B8G8R8A8_UNORM : ub[n];
    case 4: return ub[n];
    case 1: return s1[n];
    case 5: return sk[n];
    case 2: return fl[n];
    case 6: return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

static VkPrimitiveTopology topology(u32 prim) {
    switch (prim) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: case 4: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 7: case 10: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// Quads and line loops have no Vulkan topology: rewrite the index list.
static void fix_indices(u32 prim, std::vector<u32>& idx) {
    if (prim == 8) {
        std::vector<u32> t;
        t.reserve(idx.size() / 4 * 6);
        for (size_t i = 0; i + 3 < idx.size(); i += 4) {
            u32 a = idx[i], b = idx[i + 1], c = idx[i + 2], d = idx[i + 3];
            t.insert(t.end(), {a, b, c, a, c, d});
        }
        idx.swap(t);
    } else if (prim == 3 && !idx.empty()) {
        idx.push_back(idx[0]);
    }
}

static VkBlendFactor blend_factor(u32 v) {
    switch (v) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

static VkBlendOp blend_op(u32 v) {
    switch (v) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    return VK_BLEND_OP_ADD;
}

static VkStencilOp stencil_op(u32 v) {
    switch (v) {
    case 0: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

static PipelineState pipeline_state(u32 prim) {
    PipelineState p{};
    p.topology = topology(prim);
    p.blend_enable = g_k.r(NV097_SET_BLEND_ENABLE) != 0;
    p.blend_src = blend_factor(g_k.r(NV097_SET_BLEND_FUNC_SFACTOR));
    p.blend_dst = blend_factor(g_k.r(NV097_SET_BLEND_FUNC_DFACTOR));
    p.blend_op = blend_op(g_k.r(NV097_SET_BLEND_EQUATION));
    u32 cm = g_k.r(NV097_SET_COLOR_MASK);
    p.color_mask = (cm >> 16 & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) | (cm >> 8 & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                   (cm & 1 ? VK_COLOR_COMPONENT_B_BIT : 0) | (cm >> 24 & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    p.depth_test = g_k.r(NV097_SET_DEPTH_TEST_ENABLE) != 0;
    p.depth_write = p.depth_test && g_k.r(NV097_SET_DEPTH_MASK) != 0;
    p.depth_op = g_k.r(NV097_SET_DEPTH_FUNC) & 7;
    p.stencil_test = g_k.r(NV097_SET_STENCIL_TEST_ENABLE) != 0;
    p.st_fail = stencil_op(g_k.r(NV097_SET_STENCIL_OP_FAIL));
    p.st_zfail = stencil_op(g_k.r(NV097_SET_STENCIL_OP_ZFAIL));
    p.st_pass = stencil_op(g_k.r(NV097_SET_STENCIL_OP_ZPASS));
    p.st_op = g_k.r(NV097_SET_STENCIL_FUNC) & 7;
    p.st_ref = (u8)g_k.r(NV097_SET_STENCIL_FUNC_REF);
    p.st_read_mask = (u8)g_k.r(NV097_SET_STENCIL_FUNC_MASK);
    p.st_write_mask = (u8)g_k.r(NV097_SET_STENCIL_MASK);
    if (g_k.r(NV097_SET_CULL_FACE_ENABLE)) {
        switch (g_k.r(NV097_SET_CULL_FACE)) {
        case 0x404: p.cull_mode = VK_CULL_MODE_FRONT_BIT; break;
        case 0x405: p.cull_mode = VK_CULL_MODE_BACK_BIT; break;
        default: p.cull_mode = VK_CULL_MODE_FRONT_AND_BACK; break;
        }
    }
    p.front_face = g_k.r(NV097_SET_FRONT_FACE) == 0x900 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    switch (g_k.r(NV097_SET_FRONT_POLYGON_MODE)) {
    case 0x1B00: p.polygon_mode = VK_POLYGON_MODE_POINT; break;
    case 0x1B01: p.polygon_mode = VK_POLYGON_MODE_LINE; break;
    default: p.polygon_mode = VK_POLYGON_MODE_FILL; break;
    }
    return p;
}

static void argb_to_vec4(u32 v, float* out) {
    out[0] = ((v >> 16) & 0xFF) / 255.f;
    out[1] = ((v >> 8) & 0xFF) / 255.f;
    out[2] = (v & 0xFF) / 255.f;
    out[3] = (v >> 24) / 255.f;
}

static bool is_linear_format(u32 f) {
    static const u8 linear[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
                                0x20, 0x35, 0x37, 0x3D, 0x3E, 0x3F, 0x40, 0x41};
    for (u8 l : linear)
        if (f == l) return true;
    return false;
}

static void texture_desc(u32 i, TextureDesc& t) {
    u32 base = 0x40 * i;
    u32 fmt = g_k.r(NV097_SET_TEXTURE_FORMAT + base);
    u32 dma = (fmt & 3) == 2 ? NV097_SET_CONTEXT_DMA_B : NV097_SET_CONTEXT_DMA_A;
    t.addr = dma_addr(dma, g_k.r(NV097_SET_TEXTURE_OFFSET + base));
    t.format = fmt >> 8 & 0xFF;
    t.cube = fmt >> 2 & 1;
    t.levels = fmt >> 16 & 15;
    if (!t.levels) t.levels = 1;
    t.width = 1u << (fmt >> 20 & 15);
    t.height = 1u << (fmt >> 24 & 15);
    t.depth = 1u << (fmt >> 28 & 15);
    t.linear = is_linear_format(t.format);
    if (t.linear) {
        // Linear (LU_IMAGE) formats take their size from IMAGE_RECT.
        u32 rect = g_k.r(NV097_SET_TEXTURE_IMAGE_RECT + base);
        t.width = rect >> 16;
        t.height = rect & 0xFFFF;
        t.levels = 1;
    }
    t.pitch = g_k.r(NV097_SET_TEXTURE_CONTROL1 + base) >> 16;
    u32 pal = g_k.r(NV097_SET_TEXTURE_PALETTE + base);
    u32 pdma = (pal & 3) == 2 ? NV097_SET_CONTEXT_DMA_B : NV097_SET_CONTEXT_DMA_A;
    t.palette_addr = t.format == 0x0B ? dma_addr(pdma, pal & ~0x3Fu) : 0;
    t.palette_len = 256u >> (pal >> 2 & 3);
    t.address = g_k.r(NV097_SET_TEXTURE_ADDRESS + base);
    t.filter = g_k.r(NV097_SET_TEXTURE_FILTER + base);
    t.control0 = g_k.r(NV097_SET_TEXTURE_CONTROL0 + base);
    t.border = g_k.r(NV097_SET_TEXTURE_BORDER_COLOR + base);
}

// Build the interleaved vertex buffer and index list for the draw.
static bool assemble_vertices(DrawKind kind, DrawCall& d, VsInputs& in) {
    AttrFmt fm[16];
    u32 stride = 0;
    for (u32 i = 0; i < 16; i++) {
        d.layout.attr[i] = {VK_FORMAT_UNDEFINED, 0};
        fm[i] = attr_fmt(i);
        if (kind == DK_IMMEDIATE) {
            d.layout.attr[i] = {VK_FORMAT_R32G32B32A32_SFLOAT, 16 * i};
            in.present |= 1 << i;
            continue;
        }
        u32 b = fm[i].size ? attr_bytes(fm[i]) : 0;
        if (!b) continue;
        d.layout.attr[i] = {attr_vkformat(fm[i]), stride};
        in.present |= 1 << i;
        if (fm[i].type == 6) in.packed |= 1 << i;
        stride += (b + 3) & ~3u;  // keep attributes 4-byte aligned
    }
    if (kind == DK_IMMEDIATE) stride = 256;
    d.layout.stride = stride;

    if (kind == DK_IMMEDIATE) {
        u32 n = (u32)(g_immediate.size() / 64);
        g_vbuf.assign((u8*)g_immediate.data(), (u8*)(g_immediate.data() + g_immediate.size()));
        for (u32 i = 0; i < n; i++) d.indices.push_back(i);
        g_immediate.clear();
    } else if (kind == DK_INLINE) {
        // Inline vertices: the enabled attributes back to back, unpadded.
        u32 vsize = 0;
        for (u32 i = 0; i < 16; i++)
            if (in.present >> i & 1) vsize += attr_bytes(fm[i]);
        u32 n = vsize ? (u32)(g_inline.size() * 4 / vsize) : 0;
        g_vbuf.assign((size_t)n * stride, 0);
        const u8* src = (const u8*)g_inline.data();
        for (u32 v = 0; v < n; v++) {
            for (u32 i = 0; i < 16; i++) {
                if (!(in.present >> i & 1)) continue;
                u32 b = attr_bytes(fm[i]);
                memcpy(&g_vbuf[(size_t)v * stride + d.layout.attr[i].offset], src, b);
                src += b;
            }
            d.indices.push_back(v);
        }
        g_inline.clear();
    } else {
        if (g_elements.empty()) return false;
        u32 lo = UINT32_MAX, hi = 0;
        for (u32 x : g_elements) {
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        // Sparse index sets get compacted instead of copying the range.
        std::vector<u32> verts;
        if ((u64)(hi - lo + 1) > (u64)g_elements.size() * 4 + 1024) {
            std::unordered_map<u32, u32> remap;
            for (u32 x : g_elements) {
                auto it = remap.find(x);
                if (it == remap.end()) {
                    it = remap.emplace(x, (u32)verts.size()).first;
                    verts.push_back(x);
                }
                d.indices.push_back(it->second);
            }
        } else {
            for (u32 x = lo; x <= hi; x++) verts.push_back(x);
            for (u32 x : g_elements) d.indices.push_back(x - lo);
        }
        g_elements.clear();
        g_vbuf.assign(verts.size() * stride, 0);
        for (u32 i = 0; i < 16; i++) {
            if (!(in.present >> i & 1)) continue;
            u32 off = g_k.r(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * i);
            u32 dma = off >> 31 ? NV097_SET_CONTEXT_DMA_VERTEX_B : NV097_SET_CONTEXT_DMA_VERTEX_A;
            u32 base = dma_addr(dma, off & 0x7FFFFFFF);
            u32 b = attr_bytes(fm[i]), o = d.layout.attr[i].offset;
            for (size_t v = 0; v < verts.size(); v++)
                memcpy(&g_vbuf[v * stride + o], gptr(base + verts[v] * fm[i].stride), b);
        }
    }
    d.vertices = g_vbuf.data();
    d.vertex_bytes = (u32)g_vbuf.size();
    return !d.indices.empty();
}

// The material's specular power, recovered from what D3D sends. The hardware
// evaluates a polynomial; D3D gets its first coefficient from a table in its
// data, indexed by 3 * log2(power), and for powers below 1 from
// -exp(-ln(64) / power). Going back through those gives the power itself.
constexpr u32 D3D_SPECULAR_TABLE = 0x001BC828;
constexpr u32 SPECULAR_TABLE_SIZE = 32;
static float specular_power(float a) {
    static float cached_a = 1, cached_power;
    if (a == cached_a) return cached_power;
    float power = 1;
    if (a > -1e-6f) {
        power = 1;
    } else if (a <= rdf32(D3D_SPECULAR_TABLE + 4 * (SPECULAR_TABLE_SIZE - 1))) {
        power = powf(2.0f, (SPECULAR_TABLE_SIZE - 1) / 3.0f);
    } else {
        // The table falls monotonically from 0 to -1.
        for (u32 i = 0; i + 1 < SPECULAR_TABLE_SIZE; i++) {
            float t0 = rdf32(D3D_SPECULAR_TABLE + 4 * i), t1 = rdf32(D3D_SPECULAR_TABLE + 4 * (i + 1));
            if (a <= t0 && a > t1) {
                power = powf(2.0f, (i + (t0 - a) / (t0 - t1)) / 3.0f);
                break;
            }
        }
    }
    cached_a = a;
    cached_power = power;
    return power;
}

static void fill_uniforms(const DrawCall& d, const RenderTargets& rt, Uniforms& u) {
    memcpy(u.c, g_k.constants, sizeof u.c);
    memcpy(u.inline_attr, g_k.inline_attr, sizeof u.inline_attr);
    u.surface[0] = (float)(rt.width ? rt.width : 640);
    u.surface[1] = (float)(rt.height ? rt.height : 480);
    u.surface[2] = rt.zeta == ZetaFmt::Z16 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
    for (u32 i = 0; i < 8; i++) {
        argb_to_vec4(g_k.r(NV097_SET_COMBINER_FACTOR0 + 4 * i), u.factor0[i]);
        argb_to_vec4(g_k.r(NV097_SET_COMBINER_FACTOR1 + 4 * i), u.factor1[i]);
    }
    // Unlike the other colours, this method takes its channels as ABGR.
    u32 fog = g_k.r(NV097_SET_FOG_COLOR);
    argb_to_vec4((fog & 0xFF00FF00u) | (fog >> 16 & 0xFF) | (fog & 0xFF) << 16, u.fog_color);
    u.misc[0] = (float)(g_k.r(NV097_SET_ALPHA_REF) & 0xFF);
    u.misc[1] = specular_power(g_k.f(NV097_SET_SPECULAR_PARAMS));
    for (u32 i = 0; i < 4; i++) {
        bool lin = (d.tex_mask >> i & 1) && d.tex[i].linear;
        u.tex_scale[i][0] = lin && d.tex[i].width ? 1.0f / d.tex[i].width : 1.0f;
        u.tex_scale[i][1] = lin && d.tex[i].height ? 1.0f / d.tex[i].height : 1.0f;
        for (u32 j = 0; j < 4; j++) u.bump_mat[i][j] = g_k.f(NV097_SET_TEXTURE_SET_BUMP_ENV_MAT + 0x40 * i + 4 * j);
        for (u32 j = 0; j < 16; j++) u.tex_matrix[i][j / 4][j % 4] = g_k.f(NV097_SET_TEXTURE_MATRIX + 0x40 * i + 4 * j);
    }
    for (u32 j = 0; j < 16; j++) {
        u.composite[j / 4][j % 4] = g_k.f(NV097_SET_COMPOSITE_MATRIX + 4 * j);
        u.model_view[j / 4][j % 4] = g_k.f(NV097_SET_MODEL_VIEW_MATRIX + 4 * j);
        u.inv_model_view[j / 4][j % 4] = g_k.f(NV097_SET_INVERSE_MODEL_VIEW_MATRIX + 4 * j);
    }
    for (u32 i = 0; i < 4; i++)
        for (u32 j = 0; j < 16; j++) u.texgen_plane[i][j / 4][j % 4] = g_k.f(NV097_SET_TEXGEN_PLANE + 0x40 * i + 4 * j);
    auto vec = [](float* dst, u32 method, u32 n) {
        for (u32 i = 0; i < n; i++) dst[i] = g_k.f(method + 4 * i);
    };
    vec(u.scene_ambient, NV097_SET_SCENE_AMBIENT_COLOR, 3);
    vec(u.material_emission, NV097_SET_MATERIAL_EMISSION, 3);
    u.material_emission[3] = g_k.f(NV097_SET_MATERIAL_ALPHA);
    vec(u.eye_position, NV097_SET_EYE_POSITION, 4);
    vec(u.fog_params, NV097_SET_FOG_PARAMS, 3);
    vec(u.eye_vec, NV097_SET_EYE_VECTOR, 3);
    for (u32 i = 0; i < 8; i++) {
        Uniforms::Light& l = u.lights[i];
        u32 b = NV097_SET_LIGHT + 0x80 * i;
        vec(l.ambient, b + 0x00, 3);
        vec(l.diffuse, b + 0x0C, 3);
        vec(l.specular, b + 0x18, 3);
        l.local_pos[3] = g_k.f(b + 0x24);  // range
        vec(l.half_vec, b + 0x28, 3);
        vec(l.inf_dir, b + 0x34, 3);
        vec(l.spot_falloff, b + 0x40, 3);
        vec(l.spot_dir, b + 0x4C, 4);
        vec(l.local_pos, b + 0x5C, 3);
        vec(l.attenuation, b + 0x68, 3);
    }
}

static void flush_draw() {
    u32 prim = g_prim;
    g_prim = 0;
    DrawKind kind = g_kind;
    g_kind = DK_NONE;
    if (!g_vk_ready || kind == DK_NONE) return;

    DrawCall d;
    VsInputs in{};
    if (!assemble_vertices(kind, d, in)) return;
    fix_indices(prim, d.indices);

    // Textures of the stages whose texture shader samples one.
    u32 prog = g_k.r(NV097_SET_SHADER_STAGE_PROGRAM);
    d.tex_mask = 0;
    for (u32 i = 0; i < 4; i++) {
        u32 mode = prog >> (5 * i) & 31;
        bool enabled = g_k.r(NV097_SET_TEXTURE_CONTROL0 + 0x40 * i) >> 30 & 1;
        if (enabled && mode && mode != 4 && mode != 5 && mode != 17) {
            texture_desc(i, d.tex[i]);
            d.tex_mask |= 1 << i;
        }
    }

    static Uniforms u;
    RenderTargets rt = current_targets();
    fill_uniforms(d, rt, u);
    d.uniforms = &u;
    d.uniform_bytes = sizeof u;
    d.ps = pipeline_state(prim);
    d.blend_color = g_k.r(NV097_SET_BLEND_COLOR);
    d.depth_bias_factor = d.depth_bias_units = 0;
    if (g_k.r(NV097_SET_POLY_OFFSET_FILL_ENABLE)) {
        d.depth_bias_factor = g_k.f(NV097_SET_POLYGON_OFFSET_SCALE_FACTOR);
        d.depth_bias_units = g_k.f(NV097_SET_POLYGON_OFFSET_BIAS);
    }
    d.vs = gen_vertex_shader(g_k, in);
    d.fs = gen_fragment_shader(g_k, d.tex, d.tex_mask);
    static int trace_frame = getenv("NL_TRACE_DRAWS") ? atoi(getenv("NL_TRACE_DRAWS")) : -1;
    if ((int)vkb_frame_no() == trace_frame)
        log_msg("[draw] vs %04u fs %04u cull %u/%X/%X z %u/%u blend %u/%X/%X cmask %X prim %u n %zu mode %s light %u mask %X skin %u texgen %08X attrs %04X tex %X fmt %02X/%02X shader %08X comb %u",
                vkb_shader_id(d.vs, false), vkb_shader_id(d.fs, true), g_k.r(NV097_SET_CULL_FACE_ENABLE),
                g_k.r(NV097_SET_CULL_FACE), g_k.r(NV097_SET_FRONT_FACE), g_k.r(NV097_SET_DEPTH_TEST_ENABLE),
                g_k.r(NV097_SET_DEPTH_MASK), g_k.r(NV097_SET_BLEND_ENABLE), g_k.r(NV097_SET_BLEND_FUNC_SFACTOR),
                g_k.r(NV097_SET_BLEND_FUNC_DFACTOR), g_k.r(NV097_SET_COLOR_MASK), prim, d.indices.size(), (g_k.r(NV097_SET_TRANSFORM_EXECUTION_MODE) & 3) == 2 ? "prog" : "ff",
                g_k.r(NV097_SET_LIGHTING_ENABLE), g_k.r(0x3BC), g_k.r(0x328), g_k.r(0x3C0), in.present, d.tex_mask,
                d.tex[0].format, d.tex[1].format, g_k.r(NV097_SET_SHADER_STAGE_PROGRAM),
                g_k.r(NV097_SET_COMBINER_CONTROL) & 0xFF);
    // NL_ONLY_VS=text: draw only what uses a vertex shader containing the
    // text (debugging, e.g. "A0 + 96" for skinned meshes).
    // NL_DEBUG_VS=text: dump constants and the first vertex of a few draws
    // whose vertex shader contains the text.
    static const char* dbg_vs = getenv("NL_DEBUG_VS");
    static int dbg_left = 3;
    if (dbg_vs && dbg_left > 0 && vkb_frame_no() > 2000 && d.vs.find(dbg_vs) != std::string::npos) {
        dbg_left--;
        log_msg("[dbgvs] frame %u prim %u verts %u stride %u", vkb_frame_no(), prim, d.vertex_bytes / (d.layout.stride ? d.layout.stride : 1), d.layout.stride);
        for (u32 i = 0; i < 16; i++)
            if (in.present >> i & 1) {
                AttrFmt f = attr_fmt(i);
                const u8* p = g_vbuf.data() + d.layout.attr[i].offset;
                log_msg("  v%u type %u size %u: %02X %02X %02X %02X  %08X %08X %08X %08X", i, f.type, f.size, p[0], p[1], p[2], p[3],
                        *(u32*)p, *(u32*)(p + 4), *(u32*)(p + 8), *(u32*)(p + 12));
            }
        for (u32 c : {11u, 17u, 18u, 19u, 20u, 28u, 29u, 30u, 31u, 32u, 33u, 34u, 35u, 58u, 59u, 96u, 97u, 98u, 99u, 100u, 101u})
            log_msg("  c[%u] = %g %g %g %g", c, g_k.constants[c][0], g_k.constants[c][1], g_k.constants[c][2], g_k.constants[c][3]);
    }
    static const char* only_vs = getenv("NL_ONLY_VS");
    if (only_vs && d.vs.find(only_vs) == std::string::npos) return;
    static const char* only_fs = getenv("NL_ONLY_FS");
    if (only_fs && d.fs.find(only_fs) == std::string::npos) return;
    bind_targets();
    vkb_draw(d);
}

// Immediate-mode vertex: emitted when the position attribute is written.
static void emit_immediate() {
    if (!g_prim) return;
    g_kind = DK_IMMEDIATE;
    g_immediate.insert(g_immediate.end(), &g_k.inline_attr[0][0], &g_k.inline_attr[0][0] + 64);
}

static void set_attr(u32 a, float x, float y, float z, float w) {
    float* v = g_k.inline_attr[a & 15];
    v[0] = x, v[1] = y, v[2] = z, v[3] = w;
}

static float as_float(u32 v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// Fixed-function state that lives in transform constant memory, where
// vertex programs read it (D3D's shader epilogue uses the viewport ones).
constexpr u32 XFCTX_EYEP = 0x38, XFCTX_FOG = 0x39, XFCTX_VPSCL = 0x3A, XFCTX_VPOFF = 0x3B;
static void alias_constant(u32 m, u32 v) {
    struct {
        u32 method, count, slot;
    } static const map[] = {
        {NV097_SET_VIEWPORT_SCALE, 4, XFCTX_VPSCL},
        {NV097_SET_VIEWPORT_OFFSET, 4, XFCTX_VPOFF},
        {NV097_SET_EYE_POSITION, 4, XFCTX_EYEP},
        {NV097_SET_FOG_PARAMS, 3, XFCTX_FOG},
    };
    for (auto& e : map)
        if (m >= e.method && m < e.method + 4 * e.count) g_k.constants[e.slot][(m - e.method) / 4] = as_float(v);
}

// Methods that load arrays (program, constants, immediate attributes).
static bool array_method(u32 m, u32 v) {
    if (m >= NV097_SET_TRANSFORM_PROGRAM && m < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
        u32 slot = (m - NV097_SET_TRANSFORM_PROGRAM) / 4;
        if (g_k.program_load < 136) g_k.program[g_k.program_load][slot % 4] = v;
        if (slot % 4 == 3) g_k.program_load++;
    } else if (m >= NV097_SET_TRANSFORM_CONSTANT && m < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
        u32 slot = (m - NV097_SET_TRANSFORM_CONSTANT) / 4;
        if (g_k.constant_load < 192) g_k.constants[g_k.constant_load][slot % 4] = as_float(v);
        if (slot % 4 == 3) g_k.constant_load++;
    } else if (m >= NV097_SET_VERTEX_DATA4F_M && m < NV097_SET_VERTEX_DATA4F_M + 0x100) {
        u32 a = (m - NV097_SET_VERTEX_DATA4F_M) / 16, c = (m / 4) & 3;
        g_k.inline_attr[a][c] = as_float(v);
        if (a == 0 && c == 3) emit_immediate();
    } else if (m >= NV097_SET_VERTEX_DATA2F_M && m < NV097_SET_VERTEX_DATA2F_M + 0x80) {
        u32 a = (m - NV097_SET_VERTEX_DATA2F_M) / 8, c = (m / 4) & 1;
        g_k.inline_attr[a][c] = as_float(v);
        if (c == 1) g_k.inline_attr[a][2] = 0, g_k.inline_attr[a][3] = 1;
        if (a == 0 && c == 1) emit_immediate();
    } else if (m >= NV097_SET_VERTEX_DATA4UB && m < NV097_SET_VERTEX_DATA4UB + 0x40) {
        u32 a = (m - NV097_SET_VERTEX_DATA4UB) / 4;
        set_attr(a, (v & 0xFF) / 255.f, (v >> 8 & 0xFF) / 255.f, (v >> 16 & 0xFF) / 255.f, (v >> 24) / 255.f);
        if (a == 0) emit_immediate();
    } else if (m >= NV097_SET_VERTEX_DATA2S && m < NV097_SET_VERTEX_DATA2S + 0x40) {
        u32 a = (m - NV097_SET_VERTEX_DATA2S) / 4;
        set_attr(a, (float)(s16)v, (float)(s16)(v >> 16), 0, 1);
        if (a == 0) emit_immediate();
    } else if (m >= NV097_SET_VERTEX_DATA4S_M && m < NV097_SET_VERTEX_DATA4S_M + 0x80) {
        u32 a = (m - NV097_SET_VERTEX_DATA4S_M) / 8, c = (m / 4) & 1;
        g_k.inline_attr[a][2 * c] = (float)(s16)v;
        g_k.inline_attr[a][2 * c + 1] = (float)(s16)(v >> 16);
        if (a == 0 && c == 1) emit_immediate();
    } else if (m >= NV097_SET_VERTEX3F && m < NV097_SET_VERTEX3F + 12) {
        g_k.inline_attr[0][(m - NV097_SET_VERTEX3F) / 4] = as_float(v);
        g_k.inline_attr[0][3] = 1;
        if (m == NV097_SET_VERTEX3F + 8) emit_immediate();
    } else if (m >= NV097_SET_VERTEX4F && m < NV097_SET_VERTEX4F + 16) {
        g_k.inline_attr[0][(m - NV097_SET_VERTEX4F) / 4] = as_float(v);
        if (m == NV097_SET_VERTEX4F + 12) emit_immediate();
    } else {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Kelvin
static void kelvin_method(u32 m, u32 v) {
    g_k.regs[m / 4] = v;
    if (array_method(m, v)) return;
    alias_constant(m, v);
    switch (m) {
    case NV097_NO_OPERATION:
        // D3D uses a NOP with a parameter as a software method: it traps
        // to the driver's interrupt handler, and the FIFO waits for it.
        if (v) pgraph_trap(m, v);
        break;
    case NV097_SET_FLIP_READ: g_k.flip_read = v; break;
    case NV097_SET_FLIP_WRITE: g_k.flip_write = v; break;
    case NV097_SET_FLIP_MODULO: g_k.flip_modulo = v; break;
    case NV097_FLIP_INCREMENT_WRITE:
        g_k.flip_write = g_k.flip_modulo ? (g_k.flip_write + 1) % g_k.flip_modulo : g_k.flip_write + 1;
        // The frame in the current colour surface is complete: show it.
        if (g_vk_ready) vkb_present(current_targets().color_addr);
        break;
    case NV097_FLIP_STALL: video_wait_vblank(); break;
    case NV097_SET_CONTEXT_DMA_A:
    case NV097_SET_CONTEXT_DMA_B:
    case NV097_SET_CONTEXT_DMA_COLOR:
    case NV097_SET_CONTEXT_DMA_ZETA:
    case NV097_SET_CONTEXT_DMA_VERTEX_A:
    case NV097_SET_CONTEXT_DMA_VERTEX_B:
    case NV097_SET_CONTEXT_DMA_SEMAPHORE:
    case NV097_SET_CONTEXT_DMA_REPORT:
        g_dma_cache.erase(v);  // the object behind a handle may have been rewritten
        break;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD: g_k.program_load = v; break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD: g_k.constant_load = v; break;
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
    case NV097_TEXTURE_READ_SEMAPHORE_RELEASE:
        semaphore_release(v);
        break;
    case NV097_GET_REPORT: {
        u32 a = dma_addr(NV097_SET_CONTEXT_DMA_REPORT, v & 0xFFFFFF);
        wr64(a, 0);      // timestamp
        wr32(a + 8, 0);  // zpass pixel count
        wr32(a + 12, 0);
        break;
    }
    case NV097_CLEAR_SURFACE:
        if (g_vk_ready) clear_surface(v);
        break;
    case NV097_SET_BEGIN_END:
        if (v) {
            g_prim = v;
            g_kind = DK_NONE;
            g_elements.clear();
            g_inline.clear();
            g_immediate.clear();
        } else {
            flush_draw();
        }
        break;
    case NV097_ARRAY_ELEMENT16:
        g_kind = DK_ELEMENTS;
        g_elements.push_back(v & 0xFFFF);
        g_elements.push_back(v >> 16);
        break;
    case NV097_ARRAY_ELEMENT32:
        g_kind = DK_ELEMENTS;
        g_elements.push_back(v);
        break;
    case NV097_DRAW_ARRAYS: {
        g_kind = DK_ELEMENTS;
        u32 start = v & 0xFFFFFF, count = (v >> 24) + 1;
        for (u32 i = 0; i < count; i++) g_elements.push_back(start + i);
        break;
    }
    case NV097_INLINE_ARRAY:
        g_kind = DK_INLINE;
        g_inline.push_back(v);
        break;
    }
}

// ------------------------------------------------------------- 2D objects
constexpr u32 CLASS_CLIP = 0x19, CLASS_BETA1 = 0x12, CLASS_BETA4 = 0x72;
constexpr u32 CLASS_M2MF = 0x39, CLASS_SURFACE_2D = 0x62, CLASS_IMAGE_BLIT = 0x9F;
static u32 g_2d[3][0x400 / 4];  // method values of the M2MF, surface and blit objects
enum { OBJ_M2MF, OBJ_SURFACE, OBJ_BLIT };

static void count_once(const char* what) {
    static std::map<std::string, u32> seen;
    if (!seen[what]++) log_msg("[pgraph] %s", what);
}

static u32 bytes_per_pixel_2d(u32 format) {
    switch (format) {
    case 1: return 1;     // Y8
    case 4: case 5: return 2;  // 16-bit formats
    default: return 4;
    }
}

// NV062 surface: 0x184 source DMA, 0x188 destination DMA, 0x300 format,
// 0x304 pitches, 0x308/0x30C offsets. NV09F blit: 0x300 source point, 0x304
// destination point, 0x308 size (starts the blit).
static void image_blit(u32 size) {
    const u32* sf = g_2d[OBJ_SURFACE];
    const u32* bl = g_2d[OBJ_BLIT];
    u32 w = size & 0xFFFF, h = size >> 16;
    u32 sx = bl[0x300 / 4] & 0xFFFF, sy = bl[0x300 / 4] >> 16, dx = bl[0x304 / 4] & 0xFFFF, dy = bl[0x304 / 4] >> 16;
    u32 src = gpu_addr(dma_handle_base(sf[0x184 / 4]) + sf[0x308 / 4]);
    u32 dst = gpu_addr(dma_handle_base(sf[0x188 / 4]) + sf[0x30C / 4]);
    u32 pitch_s = sf[0x304 / 4] & 0xFFFF, pitch_d = sf[0x304 / 4] >> 16, bpp = bytes_per_pixel_2d(sf[0x300 / 4]);
    switch (vkb_blit(src, dst, sx, sy, dx, dy, w, h, pitch_s, pitch_d, bpp)) {
    case BlitResult::Done: return;
    case BlitResult::SourceIsTarget: count_once("blit from render target to memory not supported"); return;
    case BlitResult::Memory: break;
    }
    for (u32 y = 0; y < h; y++)
        memmove(gptr(dst + (dy + y) * pitch_d + dx * bpp), gptr(src + (sy + y) * pitch_s + sx * bpp), (size_t)w * bpp);
}

// NV039: 0x184/0x188 DMA in/out, 0x30C/0x310 offsets, 0x314/0x318 pitches,
// 0x31C line length, 0x320 line count, 0x328 starts the copy.
static void m2mf_copy() {
    const u32* m = g_2d[OBJ_M2MF];
    u32 in = gpu_addr(dma_handle_base(m[0x184 / 4]) + m[0x30C / 4]);
    u32 out = gpu_addr(dma_handle_base(m[0x188 / 4]) + m[0x310 / 4]);
    for (u32 i = 0; i < m[0x320 / 4]; i++)
        memmove(gptr(out + i * m[0x318 / 4]), gptr(in + i * m[0x314 / 4]), m[0x31C / 4]);
}

static void object_2d_method(u32 cls, u32 m, u32 v) {
    int obj = cls == CLASS_M2MF ? OBJ_M2MF : cls == CLASS_SURFACE_2D ? OBJ_SURFACE : OBJ_BLIT;
    if (m < 0x400) g_2d[obj][m / 4] = v;
    if (cls == CLASS_IMAGE_BLIT && m == 0x308) image_blit(v);
    if (cls == CLASS_M2MF && m == 0x328) m2mf_copy();
}

void pgraph_method(u32 subch, u32 inst, u32 method, u32 value) {
    u32 cls = obj_class(inst);
    g_subch = subch;
    if (g_trace_methods) log_msg("[mthd] %u:%02X %04X %08X", subch, cls, method, value);
    if (cls == 0x97) return kelvin_method(method, value);
    if (cls == CLASS_SURFACE_2D || cls == CLASS_IMAGE_BLIT || cls == CLASS_M2MF) return object_2d_method(cls, method, value);
    if (cls == CLASS_CLIP || cls == CLASS_BETA1 || cls == CLASS_BETA4) return;  // blit state with no effect here
    if (g_unknown_class[cls]++ < 4)
        log_msg("[pgraph] class %02X (subchannel %u) method %04X <- %08X ignored", cls, subch, method, value);
}

void pgraph_reg_write(u32 off, u32 v) { nv_reg(off) = v; }

// Vulkan is created on the FIFO thread, which owns it from then on.
void pgraph_init_backend() {
    // Attribute defaults: (0,0,0,1), white diffuse.
    for (auto& a : g_k.inline_attr) a[0] = a[1] = a[2] = 0, a[3] = 1;
    for (int i = 0; i < 4; i++) g_k.inline_attr[3][i] = 1;
    vkb_init();
    g_vk_ready = true;
}
