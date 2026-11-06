// NV2A texture decoding: swizzled (Morton order) and linear layouts, the
// 16/32-bit colour formats, luminance/alpha, P8 palettes and DXT1/3/5.
#include "texture.h"

#include <map>

enum Kind { K_NONE, K_SWZ, K_LIN, K_DXT };
struct Fmt {
    Kind kind;
    u32 bpp;  // bits per pixel (DXT: per pixel of the 4x4 block, 4 or 8)
    u32 (*pixel)(const u8*);
};

static u32 x5(u32 v) { return (v << 3) | (v >> 2); }
static u32 x6(u32 v) { return (v << 2) | (v >> 4); }
static u32 x4(u32 v) { return v * 17; }
static u32 rgb(u32 a, u32 r, u32 g, u32 b) { return a << 24 | r << 16 | g << 8 | b; }
static u16 u16at(const u8* p) { return (u16)(p[0] | p[1] << 8); }
static u32 u32at(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }

static u32 p_a1r5g5b5(const u8* p) { u32 v = u16at(p); return rgb(v >> 15 ? 255 : 0, x5(v >> 10 & 31), x5(v >> 5 & 31), x5(v & 31)); }
static u32 p_x1r5g5b5(const u8* p) { return p_a1r5g5b5(p) | 0xFF000000u; }
static u32 p_a4r4g4b4(const u8* p) { u32 v = u16at(p); return rgb(x4(v >> 12), x4(v >> 8 & 15), x4(v >> 4 & 15), x4(v & 15)); }
static u32 p_r5g6b5(const u8* p) { u32 v = u16at(p); return rgb(255, x5(v >> 11), x6(v >> 5 & 63), x5(v & 31)); }
static u32 p_r6g5b5(const u8* p) { u32 v = u16at(p); return rgb(255, x6(v >> 10), x5(v >> 5 & 31), x5(v & 31)); }
static u32 p_a8r8g8b8(const u8* p) { return u32at(p); }
static u32 p_x8r8g8b8(const u8* p) { return u32at(p) | 0xFF000000u; }
static u32 p_a8b8g8r8(const u8* p) { u32 v = u32at(p); return (v & 0xFF00FF00u) | (v >> 16 & 0xFF) | (v & 0xFF) << 16; }
static u32 p_b8g8r8a8(const u8* p) { return rgb(p[0], p[1], p[2], p[3]); }
static u32 p_r8g8b8a8(const u8* p) { return rgb(p[0], p[3], p[2], p[1]); }
static u32 p_y8(const u8* p) { return rgb(255, p[0], p[0], p[0]); }
static u32 p_ay8(const u8* p) { return rgb(p[0], p[0], p[0], p[0]); }
static u32 p_a8(const u8* p) { return rgb(p[0], 255, 255, 255); }
static u32 p_a8y8(const u8* p) { return rgb(p[0], p[1], p[1], p[1]); }
static u32 p_g8b8(const u8* p) { return rgb(255, 0, p[1], p[0]); }
static u32 p_r8b8(const u8* p) { return rgb(255, p[1], 0, p[0]); }
static u32 p_y16(const u8* p) { return rgb(255, p[1], p[1], p[1]); }
static u32 p_r5g5b5a1(const u8* p) { u32 v = u16at(p); return rgb(v & 1 ? 255 : 0, x5(v >> 11), x5(v >> 6 & 31), x5(v >> 1 & 31)); }
static u32 p_r4g4b4a4(const u8* p) { u32 v = u16at(p); return rgb(x4(v & 15), x4(v >> 12), x4(v >> 8 & 15), x4(v >> 4 & 15)); }

static const std::map<u32, Fmt> kFormats = {
    {0x00, {K_SWZ, 8, p_y8}},        {0x01, {K_SWZ, 8, p_ay8}},       {0x02, {K_SWZ, 16, p_a1r5g5b5}},
    {0x03, {K_SWZ, 16, p_x1r5g5b5}}, {0x04, {K_SWZ, 16, p_a4r4g4b4}}, {0x05, {K_SWZ, 16, p_r5g6b5}},
    {0x06, {K_SWZ, 32, p_a8r8g8b8}}, {0x07, {K_SWZ, 32, p_x8r8g8b8}}, {0x0B, {K_SWZ, 8, nullptr}},  // P8
    {0x0C, {K_DXT, 4, nullptr}},     {0x0E, {K_DXT, 8, nullptr}},     {0x0F, {K_DXT, 8, nullptr}},
    {0x10, {K_LIN, 16, p_a1r5g5b5}}, {0x11, {K_LIN, 16, p_r5g6b5}},   {0x12, {K_LIN, 32, p_a8r8g8b8}},
    {0x13, {K_LIN, 8, p_y8}},        {0x19, {K_SWZ, 8, p_a8}},        {0x1A, {K_SWZ, 16, p_a8y8}},
    {0x1B, {K_LIN, 8, p_ay8}},       {0x1C, {K_LIN, 16, p_x1r5g5b5}}, {0x1D, {K_LIN, 16, p_a4r4g4b4}},
    {0x1E, {K_LIN, 32, p_x8r8g8b8}}, {0x1F, {K_LIN, 8, p_a8}},        {0x20, {K_LIN, 16, p_a8y8}},
    {0x27, {K_SWZ, 16, p_r6g5b5}},   {0x28, {K_SWZ, 16, p_g8b8}},     {0x29, {K_SWZ, 16, p_r8b8}},
    {0x3A, {K_SWZ, 32, p_a8b8g8r8}}, {0x3B, {K_SWZ, 32, p_b8g8r8a8}}, {0x3C, {K_SWZ, 32, p_r8g8b8a8}},
    {0x3F, {K_LIN, 32, p_a8b8g8r8}}, {0x40, {K_LIN, 32, p_b8g8r8a8}}, {0x41, {K_LIN, 32, p_r8g8b8a8}},
    {0x14, {K_LIN, 8, p_y8}},        {0x16, {K_LIN, 16, p_r8b8}},     {0x17, {K_LIN, 16, p_g8b8}},
    {0x18, {K_LIN, 16, p_g8b8}},     {0x32, {K_SWZ, 16, p_y16}},      {0x35, {K_LIN, 16, p_y16}},
    {0x37, {K_LIN, 16, p_r6g5b5}},   {0x38, {K_SWZ, 16, p_r5g5b5a1}}, {0x39, {K_SWZ, 16, p_r4g4b4a4}},
    {0x3D, {K_LIN, 16, p_r5g5b5a1}}, {0x3E, {K_LIN, 16, p_r4g4b4a4}},
};

static const Fmt* fmt_of(u32 f) {
    auto it = kFormats.find(f);
    if (it != kFormats.end()) return &it->second;
    static std::map<u32, bool> warned;
    if (!warned[f]) {
        warned[f] = true;
        log_msg("[texture] unsupported format %02X", f);
    }
    return nullptr;
}

static u32 level_bytes(const Fmt& f, u32 w, u32 h) {
    if (f.kind == K_DXT) return ((w + 3) / 4) * ((h + 3) / 4) * (f.bpp == 4 ? 8 : 16);
    return w * h * f.bpp / 8;
}

static u32 face_bytes(const Fmt& f, const TextureDesc& d) {
    u32 total = 0, w = d.width, h = d.height;
    for (u32 l = 0; l < d.levels; l++) {
        total += level_bytes(f, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

u32 texture_size(const TextureDesc& d) {
    const Fmt* f = fmt_of(d.format);
    if (!f || !d.width || !d.height) return 0;
    if (f->kind == K_LIN) return d.pitch * d.height;
    u32 face = face_bytes(*f, d);
    return d.cube ? ((face + 127) & ~127u) * 6 : face;
}

// Morton-order offsets: bits of x and y interleave while both dimensions
// still have bits left, then the remaining dimension's bits follow.
static void swizzle_masks(u32 w, u32 h, u32& mx, u32& my) {
    mx = my = 0;
    u32 bit = 1, m = 1;
    bool more;
    do {
        more = false;
        if (bit < w) mx |= m, m <<= 1, more = true;
        if (bit < h) my |= m, m <<= 1, more = true;
        bit <<= 1;
    } while (more);
}

static u32 spread(u32 v, u32 mask) {  // _pdep_u32 without BMI2
    u32 r = 0;
    for (u32 b = 1; mask; b <<= 1) {
        u32 low = mask & (0 - mask);
        if (v & b) r |= low;
        mask &= mask - 1;
    }
    return r;
}

static void decode_dxt(const u8* src, u32 w, u32 h, u32 kind, std::vector<u32>& out) {
    out.assign(w * h, 0);
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = kind == 1 ? 8 : 16;
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8* b = src + (by * bw + bx) * bsize;
            const u8* cb = kind == 1 ? b : b + 8;
            u32 c0 = u16at(cb), c1 = u16at(cb + 2), bits = u32at(cb + 4);
            u32 pal[4];
            pal[0] = p_r5g6b5(cb);
            pal[1] = p_r5g6b5(cb + 2);
            auto mix = [](u32 a, u32 b, u32 wa, u32 wb, u32 d) {
                u32 r = 0;
                for (u32 s = 0; s < 24; s += 8) r |= ((((a >> s) & 0xFF) * wa + ((b >> s) & 0xFF) * wb) / d) << s;
                return r | 0xFF000000u;
            };
            if (c0 > c1 || kind != 1) {
                pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                pal[3] = mix(pal[0], pal[1], 1, 2, 3);
            } else {
                pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                pal[3] = 0;  // transparent black
            }
            u8 alpha[16];
            if (kind == 3) {  // DXT2/3: explicit 4-bit alpha
                for (u32 i = 0; i < 16; i++) alpha[i] = (u8)x4((b[i / 2] >> (4 * (i & 1))) & 15);
            } else if (kind == 5) {  // DXT4/5: interpolated alpha
                u32 a0 = b[0], a1 = b[1], ap[8];
                ap[0] = a0;
                ap[1] = a1;
                if (a0 > a1) {
                    for (u32 i = 1; i < 7; i++) ap[1 + i] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (u32 i = 1; i < 5; i++) ap[1 + i] = ((5 - i) * a0 + i * a1) / 5;
                    ap[6] = 0;
                    ap[7] = 255;
                }
                u64 abits = 0;
                for (u32 i = 0; i < 6; i++) abits |= (u64)b[2 + i] << (8 * i);
                for (u32 i = 0; i < 16; i++) alpha[i] = (u8)ap[(abits >> (3 * i)) & 7];
            }
            for (u32 i = 0; i < 16; i++) {
                u32 x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                if (x >= w || y >= h) continue;
                u32 c = pal[(bits >> (2 * i)) & 3];
                if (kind != 1) c = (c & 0xFFFFFFu) | (u32)alpha[i] << 24;
                out[y * w + x] = c;
            }
        }
}

bool decode_texture(const TextureDesc& d, DecodedTexture& out) {
    const Fmt* f = fmt_of(d.format);
    if (!f || !d.width || !d.height) return false;
    std::vector<u32> palette;
    if (d.format == 0x0B) {
        palette.resize(256, 0);
        for (u32 i = 0; i < d.palette_len && i < 256; i++) palette[i] = rd32(d.palette_addr + 4 * i);
    }
    out.w = d.width;
    out.h = d.height;
    out.levels = f->kind == K_LIN ? 1 : d.levels;
    u32 faces = d.cube ? 6 : 1;
    u32 face_stride = f->kind == K_LIN ? 0 : (face_bytes(*f, d) + 127) & ~127u;
    for (u32 face = 0; face < faces; face++) {
        const u8* src = gptr(d.addr + face * face_stride);
        u32 w = d.width, h = d.height;
        for (u32 l = 0; l < out.levels; l++) {
            DecodedTexture::Sub s{l, face, w, h, {}};
            if (f->kind == K_DXT) {
                decode_dxt(src, w, h, d.format == 0x0C ? 1 : d.format == 0x0E ? 3 : 5, s.pixels);
            } else {
                s.pixels.resize(w * h);
                u32 bpp = f->bpp / 8;
                if (f->kind == K_LIN) {
                    for (u32 y = 0; y < h; y++)
                        for (u32 x = 0; x < w; x++) s.pixels[y * w + x] = f->pixel(src + y * d.pitch + x * bpp);
                } else {
                    u32 mx, my;
                    swizzle_masks(w, h, mx, my);
                    std::vector<u32> sx(w);
                    for (u32 x = 0; x < w; x++) sx[x] = spread(x, mx);
                    for (u32 y = 0; y < h; y++) {
                        u32 oy = spread(y, my);
                        for (u32 x = 0; x < w; x++) {
                            const u8* p = src + (sx[x] | oy) * bpp;
                            s.pixels[y * w + x] = f->pixel ? f->pixel(p) : palette[*p];
                        }
                    }
                }
            }
            src += level_bytes(*f, w, h);
            out.subs.push_back(std::move(s));
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    return true;
}
