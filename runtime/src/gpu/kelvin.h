// Kelvin (NV2A 3D class 0x97) state shared by PGRAPH and the shader
// generator.
#pragma once
#include <string>

#include "vk_backend.h"

// Method numbers (NV097_*).
enum : u32 {
    NV097_NO_OPERATION = 0x0100,
    NV097_WAIT_FOR_IDLE = 0x0110,
    NV097_SET_FLIP_READ = 0x0120,
    NV097_SET_FLIP_WRITE = 0x0124,
    NV097_SET_FLIP_MODULO = 0x0128,
    NV097_FLIP_INCREMENT_WRITE = 0x012C,
    NV097_FLIP_STALL = 0x0130,
    NV097_SET_CONTEXT_DMA_NOTIFIES = 0x0180,
    NV097_SET_CONTEXT_DMA_A = 0x0184,
    NV097_SET_CONTEXT_DMA_B = 0x0188,
    NV097_SET_CONTEXT_DMA_STATE = 0x0190,
    NV097_SET_CONTEXT_DMA_COLOR = 0x0194,
    NV097_SET_CONTEXT_DMA_ZETA = 0x0198,
    NV097_SET_CONTEXT_DMA_VERTEX_A = 0x019C,
    NV097_SET_CONTEXT_DMA_VERTEX_B = 0x01A0,
    NV097_SET_CONTEXT_DMA_SEMAPHORE = 0x01A4,
    NV097_SET_CONTEXT_DMA_REPORT = 0x01A8,
    NV097_SET_SURFACE_CLIP_HORIZONTAL = 0x0200,
    NV097_SET_SURFACE_CLIP_VERTICAL = 0x0204,
    NV097_SET_SURFACE_FORMAT = 0x0208,
    NV097_SET_SURFACE_PITCH = 0x020C,
    NV097_SET_SURFACE_COLOR_OFFSET = 0x0210,
    NV097_SET_SURFACE_ZETA_OFFSET = 0x0214,
    NV097_SET_COMBINER_ALPHA_ICW = 0x0260,
    NV097_SET_COMBINER_SPECULAR_FOG_CW0 = 0x0288,
    NV097_SET_COMBINER_SPECULAR_FOG_CW1 = 0x028C,
    NV097_SET_CONTROL0 = 0x0290,
    NV097_SET_LIGHT_CONTROL = 0x0294,
    NV097_SET_FOG_MODE = 0x029C,
    NV097_SET_FOG_GEN_MODE = 0x02A0,
    NV097_SET_FOG_ENABLE = 0x02A4,
    NV097_SET_FOG_COLOR = 0x02A8,
    NV097_SET_WINDOW_CLIP_TYPE = 0x02B4,
    NV097_SET_WINDOW_CLIP_HORIZONTAL = 0x02C0,
    NV097_SET_WINDOW_CLIP_VERTICAL = 0x02E0,
    NV097_SET_ALPHA_TEST_ENABLE = 0x0300,
    NV097_SET_BLEND_ENABLE = 0x0304,
    NV097_SET_CULL_FACE_ENABLE = 0x0308,
    NV097_SET_DEPTH_TEST_ENABLE = 0x030C,
    NV097_SET_DITHER_ENABLE = 0x0310,
    NV097_SET_LIGHTING_ENABLE = 0x0314,
    NV097_SET_STENCIL_TEST_ENABLE = 0x032C,
    NV097_SET_POLY_OFFSET_FILL_ENABLE = 0x0338,
    NV097_SET_ALPHA_FUNC = 0x033C,
    NV097_SET_ALPHA_REF = 0x0340,
    NV097_SET_BLEND_FUNC_SFACTOR = 0x0344,
    NV097_SET_BLEND_FUNC_DFACTOR = 0x0348,
    NV097_SET_BLEND_COLOR = 0x034C,
    NV097_SET_BLEND_EQUATION = 0x0350,
    NV097_SET_DEPTH_FUNC = 0x0354,
    NV097_SET_COLOR_MASK = 0x0358,
    NV097_SET_DEPTH_MASK = 0x035C,
    NV097_SET_STENCIL_MASK = 0x0360,
    NV097_SET_STENCIL_FUNC = 0x0364,
    NV097_SET_STENCIL_FUNC_REF = 0x0368,
    NV097_SET_STENCIL_FUNC_MASK = 0x036C,
    NV097_SET_STENCIL_OP_FAIL = 0x0370,
    NV097_SET_STENCIL_OP_ZFAIL = 0x0374,
    NV097_SET_STENCIL_OP_ZPASS = 0x0378,
    NV097_SET_SHADE_MODE = 0x037C,
    NV097_SET_POLYGON_OFFSET_SCALE_FACTOR = 0x0384,
    NV097_SET_POLYGON_OFFSET_BIAS = 0x0388,
    NV097_SET_FRONT_POLYGON_MODE = 0x038C,
    NV097_SET_CLIP_MIN = 0x0394,
    NV097_SET_CLIP_MAX = 0x0398,
    NV097_SET_CULL_FACE = 0x039C,
    NV097_SET_FRONT_FACE = 0x03A0,
    NV097_SET_COLOR_MATERIAL = 0x0298,
    NV097_SET_NORMALIZATION_ENABLE = 0x03A4,
    NV097_SET_MATERIAL_EMISSION = 0x03A8,
    NV097_SET_MATERIAL_ALPHA = 0x03B4,
    NV097_SET_SPECULAR_ENABLE = 0x03B8,
    NV097_SET_LIGHT_ENABLE_MASK = 0x03BC,
    NV097_SET_TEXGEN_S = 0x03C0,  // + 0x10 per stage, + 4 per coordinate
    NV097_SET_TEXTURE_MATRIX_ENABLE = 0x0420,
    NV097_SET_MODEL_VIEW_MATRIX = 0x0480,
    NV097_SET_INVERSE_MODEL_VIEW_MATRIX = 0x0580,
    NV097_SET_TEXGEN_PLANE = 0x0840,  // + 0x40 per stage, + 0x10 per coordinate
    NV097_SET_SPECULAR_PARAMS = 0x09E0,
    NV097_SET_SCENE_AMBIENT_COLOR = 0x0A10,
    NV097_SET_EYE_POSITION = 0x0A50,
    NV097_SET_LIGHT = 0x1000,  // + 0x80 per light
    NV097_SET_COMPOSITE_MATRIX = 0x0680,
    NV097_SET_TEXTURE_MATRIX = 0x06C0,
    NV097_SET_FOG_PARAMS = 0x09C0,
    NV097_SET_SKIN_MODE = 0x0328,
    NV097_SET_VIEWPORT_OFFSET = 0x0A20,
    NV097_SET_COMBINER_FACTOR0 = 0x0A60,
    NV097_SET_COMBINER_FACTOR1 = 0x0A80,
    NV097_SET_COMBINER_ALPHA_OCW = 0x0AA0,
    NV097_SET_COMBINER_COLOR_ICW = 0x0AC0,
    NV097_SET_VIEWPORT_SCALE = 0x0AF0,
    NV097_SET_TRANSFORM_PROGRAM = 0x0B00,
    NV097_SET_TRANSFORM_CONSTANT = 0x0B80,
    NV097_SET_VERTEX3F = 0x1500,
    NV097_SET_VERTEX4F = 0x1518,
    NV097_SET_VERTEX_DATA_ARRAY_OFFSET = 0x1720,
    NV097_SET_VERTEX_DATA_ARRAY_FORMAT = 0x1760,
    NV097_SET_TWO_SIDE_LIGHT_EN = 0x17C4,
    NV097_GET_REPORT = 0x17D0,
    NV097_SET_BEGIN_END = 0x17FC,
    NV097_ARRAY_ELEMENT16 = 0x1800,
    NV097_ARRAY_ELEMENT32 = 0x1808,
    NV097_DRAW_ARRAYS = 0x1810,
    NV097_INLINE_ARRAY = 0x1818,
    NV097_SET_EYE_VECTOR = 0x181C,
    NV097_SET_VERTEX_DATA2F_M = 0x1880,
    NV097_SET_VERTEX_DATA2S = 0x1900,
    NV097_SET_VERTEX_DATA4UB = 0x1940,
    NV097_SET_VERTEX_DATA4S_M = 0x1980,
    NV097_SET_VERTEX_DATA4F_M = 0x1A00,
    NV097_SET_TEXTURE_OFFSET = 0x1B00,  // + 0x40 per stage
    NV097_SET_TEXTURE_FORMAT = 0x1B04,
    NV097_SET_TEXTURE_ADDRESS = 0x1B08,
    NV097_SET_TEXTURE_CONTROL0 = 0x1B0C,
    NV097_SET_TEXTURE_CONTROL1 = 0x1B10,
    NV097_SET_TEXTURE_FILTER = 0x1B14,
    NV097_SET_TEXTURE_IMAGE_RECT = 0x1B1C,
    NV097_SET_TEXTURE_PALETTE = 0x1B20,
    NV097_SET_TEXTURE_BORDER_COLOR = 0x1B24,
    NV097_SET_TEXTURE_SET_BUMP_ENV_MAT = 0x1B28,
    NV097_SET_SEMAPHORE_OFFSET = 0x1D6C,
    NV097_BACK_END_WRITE_SEMAPHORE_RELEASE = 0x1D70,
    NV097_TEXTURE_READ_SEMAPHORE_RELEASE = 0x1D74,
    NV097_SET_ZSTENCIL_CLEAR_VALUE = 0x1D8C,
    NV097_SET_COLOR_CLEAR_VALUE = 0x1D90,
    NV097_CLEAR_SURFACE = 0x1D94,
    NV097_SET_CLEAR_RECT_HORIZONTAL = 0x1D98,
    NV097_SET_CLEAR_RECT_VERTICAL = 0x1D9C,
    NV097_SET_COMBINER_COLOR_OCW = 0x1E40,
    NV097_SET_COMBINER_CONTROL = 0x1E60,
    NV097_SET_SHADER_STAGE_PROGRAM = 0x1E70,
    NV097_SET_DOT_RGBMAPPING = 0x1E74,
    NV097_SET_SHADER_OTHER_STAGE_INPUT = 0x1E78,
    NV097_SET_TRANSFORM_EXECUTION_MODE = 0x1E94,
    NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN = 0x1E98,
    NV097_SET_TRANSFORM_PROGRAM_LOAD = 0x1E9C,
    NV097_SET_TRANSFORM_PROGRAM_START = 0x1EA0,
    NV097_SET_TRANSFORM_CONSTANT_LOAD = 0x1EA4,
};

struct Kelvin {
    u32 regs[0x2000 / 4];  // last value written to each method
    u32 program[136][4];   // transform program microcode
    float constants[192][4];
    float inline_attr[16][4];  // current value of each vertex attribute
    u32 program_load, constant_load;
    u32 program_version;  // bumped on every write to `program`
    u32 flip_read, flip_write, flip_modulo;

    u32 r(u32 method) const { return regs[method / 4]; }
    float f(u32 method) const {
        float v;
        memcpy(&v, &regs[method / 4], 4);
        return v;
    }
};

// Uniform block shared by the generated vertex and fragment shaders
// (std140; keep in sync with the GLSL in shaders.cpp).
struct Uniforms {
    float c[192][4];
    float inline_attr[16][4];
    float surface[4];  // width, height, 1/zmax, unused
    float factor0[8][4];
    float factor1[8][4];
    float fog_color[4];
    float misc[4];  // alpha ref, ...
    float tex_scale[4][4];
    float bump_mat[4][4];
    float composite[4][4];
    float tex_matrix[4][4][4];
    // Fixed-function transform and lighting.
    float model_view[4][4];
    float inv_model_view[4][4];
    float texgen_plane[4][4][4];  // stage, S/T/R/Q
    float scene_ambient[4];       // rgb
    float material_emission[4];   // rgb, a = material alpha
    float eye_position[4];
    float fog_params[4];
    float eye_vec[4];
    struct Light {
        float ambient[4], diffuse[4], specular[4];
        float local_pos[4];    // xyz, w = range
        float attenuation[4];  // constant, linear, quadratic
        float inf_dir[4], half_vec[4];
        float spot_falloff[4], spot_dir[4];
    } lights[8];
};

// Which vertex attributes the draw supplies and how (CMP packed normals
// arrive as raw words and are unpacked in the shader).
struct VsInputs {
    u16 present;  // fetched from the vertex buffer
    u16 packed;   // NV097 CMP format
};

// GLSL for the current state. Cached: the same state gives the same string
// object, which lives for the rest of the run.
const std::string* vertex_shader(const Kelvin& k, VsInputs in);
const std::string* fragment_shader(const Kelvin& k, const TextureDesc* tex, u32 tex_mask);
