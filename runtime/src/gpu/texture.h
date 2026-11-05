// NV2A texture formats -> BGRA8 pixels for upload.
#pragma once
#include <vector>

#include "vk_backend.h"

struct DecodedTexture {
    struct Sub {
        u32 level, layer, w, h;
        std::vector<u32> pixels;  // 0xAARRGGBB
    };
    u32 w = 0, h = 0, levels = 0;
    std::vector<Sub> subs;
};

// Bytes of guest memory the texture occupies (0 = unsupported format).
u32 texture_size(const TextureDesc& d);
bool decode_texture(const TextureDesc& d, DecodedTexture& out);
