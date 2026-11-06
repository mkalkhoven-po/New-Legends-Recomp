// GLSL generation from Kelvin state: the transform program (NV2A vertex
// program microcode) or a fixed-function vertex stage, and the texture
// shader + register combiners as a fragment shader.
#include <stdarg.h>

#include <set>

#include "kelvin.h"

// Report a feature the generator only approximates, once per kind.
static void warn_once(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) log_msg("[shader] %s", what);
}

static void add(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
}

static const char* kUniforms = R"(
layout(std140, set = 0, binding = 0) uniform U {
    vec4 c[192];
    vec4 inlineAttr[16];
    vec4 surface;
    vec4 factor0[8];
    vec4 factor1[8];
    vec4 fogColor;
    vec4 misc;
    vec4 texScale[4];
    vec4 bumpMat[4];
    vec4 composite[4];
    vec4 texMatrix[16];
    vec4 modelView[4];
    vec4 invModelView[4];
    vec4 texgenPlane[16];
    vec4 sceneAmbient;
    vec4 materialEmission;
    vec4 eyePosition;
    vec4 fogParams;
    vec4 eyeVec;
    vec4 lights[72];
} u;
)";

// Interpolants between the stages.
static void varyings(std::string& s, const Kelvin& k, const char* dir) {
    const char* flat = k.r(NV097_SET_SHADE_MODE) == 0x1D00 ? "flat " : "";
    add(s, "layout(location = 0) %s%s vec4 vD0;\n", flat, dir);
    add(s, "layout(location = 1) %s%s vec4 vD1;\n", flat, dir);
    add(s, "layout(location = 2) %s%s vec4 vB0;\n", flat, dir);
    add(s, "layout(location = 3) %s%s vec4 vB1;\n", flat, dir);
    add(s, "layout(location = 4) %s vec4 vFog;\n", dir);
    for (int i = 0; i < 4; i++) add(s, "layout(location = %d) %s vec4 vT%d;\n", 5 + i, dir, i);
}

// ------------------------------------------------------- vertex program
static u32 fld(const u32* ins, int dw, int pos, int size) { return (ins[dw] >> pos) & ((1u << size) - 1); }

static const char* kSwz = "xyzw";
static const char* kOutRegs[] = {"oPos", "oUnk1", "oUnk2", "oD0", "oD1", "oFog", "oPts", "oB0",
                                 "oB1",  "oT0",   "oT1",   "oT2", "oT3", "oUnk13", "oUnk14", "oUnk15"};

static std::string vp_input(const u32* ins, int which, bool rel, const char* cname) {
    // which: 0 = A, 1 = B, 2 = C
    u32 mux, reg, neg, sx, sy, sz, sw;
    if (which == 0) {
        mux = fld(ins, 2, 26, 2), reg = fld(ins, 2, 28, 4), neg = fld(ins, 1, 8, 1);
        sx = fld(ins, 1, 6, 2), sy = fld(ins, 1, 4, 2), sz = fld(ins, 1, 2, 2), sw = fld(ins, 1, 0, 2);
    } else if (which == 1) {
        mux = fld(ins, 2, 11, 2), reg = fld(ins, 2, 13, 4), neg = fld(ins, 2, 25, 1);
        sx = fld(ins, 2, 23, 2), sy = fld(ins, 2, 21, 2), sz = fld(ins, 2, 19, 2), sw = fld(ins, 2, 17, 2);
    } else {
        mux = fld(ins, 3, 28, 2), reg = fld(ins, 2, 0, 2) << 2 | fld(ins, 3, 30, 2), neg = fld(ins, 2, 10, 1);
        sx = fld(ins, 2, 8, 2), sy = fld(ins, 2, 6, 2), sz = fld(ins, 2, 4, 2), sw = fld(ins, 2, 2, 2);
    }
    std::string base;
    char buf[64];
    switch (mux) {
    case 1:  // temporary
        if (reg == 12) base = "oPos";
        else snprintf(buf, sizeof buf, "R[%u]", reg), base = buf;
        break;
    case 2:  // vertex attribute
        snprintf(buf, sizeof buf, "v%u", fld(ins, 1, 9, 4));
        base = buf;
        break;
    case 3:  // constant
        if (rel) snprintf(buf, sizeof buf, "%s[clamp(A0 + %u, 0, 191)]", cname, fld(ins, 1, 13, 8));
        else snprintf(buf, sizeof buf, "%s[%u]", cname, fld(ins, 1, 13, 8));
        base = buf;
        break;
    default: base = "vec4(0.0)"; break;
    }
    std::string out = neg ? "(-" + base : base;
    out += ".";
    out += kSwz[sx];
    out += kSwz[sy];
    out += kSwz[sz];
    out += kSwz[sw];
    if (neg) out += ")";
    return out;
}

static std::string mask_str(u32 m) {
    std::string s;
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s;
}

static void vp_write(std::string& s, const std::string& dst, u32 mask, const char* val) {
    if (!mask) return;
    std::string m = mask_str(mask);
    add(s, "    %s.%s = (%s).%s;\n", dst.c_str(), m.c_str(), val, m.c_str());
}

static void translate_program(std::string& s, const Kelvin& k) {
    bool writes_consts = false;
    u32 start = k.r(NV097_SET_TRANSFORM_PROGRAM_START);
    for (u32 pc = start; pc < 136; pc++) {
        if (fld(k.program[pc], 3, 3 + 8, 1) == 0 && fld(k.program[pc], 3, 12, 4)) writes_consts = true;
        if (fld(k.program[pc], 3, 0, 1)) break;
    }
    const char* cname = "u.c";
    if (writes_consts) {
        s += "    vec4 C[192] = u.c;\n";
        cname = "C";
    }
    for (u32 pc = start; pc < 136; pc++) {
        const u32* ins = k.program[pc];
        u32 ilu = fld(ins, 1, 25, 3), mac = fld(ins, 1, 21, 4);
        bool rel = fld(ins, 3, 1, 1);
        std::string a = vp_input(ins, 0, rel, cname), b = vp_input(ins, 1, rel, cname), c = vp_input(ins, 2, rel, cname);
        add(s, "    // %03u: %08X %08X %08X\n", pc, ins[1], ins[2], ins[3]);
        s += "    {\n";
        bool has_mac = mac != 0, has_ilu = ilu != 0;
        if (has_mac) {
            std::string e;
            switch (mac) {
            case 1: e = a; break;
            case 2: e = a + " * " + b; break;
            case 3: e = a + " + " + c; break;
            case 4: e = a + " * " + b + " + " + c; break;
            case 5: e = "vec4(dot(" + a + ".xyz, " + b + ".xyz))"; break;
            case 6: e = "vec4(dot(" + a + ".xyz, " + b + ".xyz) + " + b + ".w)"; break;
            case 7: e = "vec4(dot(" + a + ", " + b + "))"; break;
            case 8: e = "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)"; break;
            case 9: e = "min(" + a + ", " + b + ")"; break;
            case 10: e = "max(" + a + ", " + b + ")"; break;
            case 11: e = "vec4(lessThan(" + a + ", " + b + "))"; break;
            case 12: e = "vec4(greaterThanEqual(" + a + ", " + b + "))"; break;
            case 13: e = a; break;  // ARL
            default: e = "vec4(0.0)"; break;
            }
            add(s, "        vec4 mac = %s;\n", e.c_str());
        }
        if (has_ilu) {
            std::string e;
            switch (ilu) {
            case 1: e = c; break;
            case 2: e = "vec4(1.0 / " + c + ".x)"; break;
            case 3: e = "vec4(rcc(" + c + ".x))"; break;
            case 4: e = "vec4(inversesqrt(abs(" + c + ".x)))"; break;
            case 5: e = "vexp(" + c + ".x)"; break;
            case 6: e = "vlog(" + c + ".x)"; break;
            case 7: e = "vlit(" + c + ")"; break;
            }
            add(s, "        vec4 ilu = %s;\n", e.c_str());
        }
        u32 mac_mask = fld(ins, 3, 24, 4), out_r = fld(ins, 3, 20, 4), ilu_mask = fld(ins, 3, 16, 4);
        u32 o_mask = fld(ins, 3, 12, 4), orb = fld(ins, 3, 11, 1), oaddr = fld(ins, 3, 3, 8), omux = fld(ins, 3, 2, 1);
        if (mac == 13) {
            s += "        A0 = int(floor(mac.x + 0.001));\n";
        } else if (has_mac && mac_mask) {
            vp_write(s, out_r == 12 ? std::string("oPos") : "R[" + std::to_string(out_r) + "]", mac_mask, "mac");
        }
        if (has_ilu && ilu_mask) {
            // An ILU op paired with a MAC op can only write R1.
            u32 r = has_mac ? 1 : out_r;
            vp_write(s, r == 12 ? std::string("oPos") : "R[" + std::to_string(r) + "]", ilu_mask, "ilu");
        }
        if (o_mask && (omux ? has_ilu : has_mac)) {
            const char* v = omux ? "ilu" : "mac";
            if (orb) vp_write(s, kOutRegs[oaddr & 15], o_mask, v);
            else if (writes_consts) vp_write(s, "C[" + std::to_string(oaddr) + "]", o_mask, v);
        }
        s += "    }\n";
        if (fld(ins, 3, 0, 1)) break;
    }
}

// ------------------------------------------------- fixed-function vertex
// Matrices are applied D3D style, row vector times matrix, with matrix
// column j being the method's floats 4j..4j+3: out.j = dot(v, M[j]).
static void row_transform(std::string& s, const char* dst, const char* v, const char* m, int base) {
    add(s, "    %s = vec4(dot(%s, %s[%d]), dot(%s, %s[%d]), dot(%s, %s[%d]), dot(%s, %s[%d]));\n", dst, v, m, base, v, m,
        base + 1, v, m, base + 2, v, m, base + 3);
}

// Colour material sources (NV097_SET_COLOR_MATERIAL, 2 bits each).
static const char* material_src(u32 v) { return v == 1 ? "v3" : v == 2 ? "v4" : nullptr; }

static void fixed_function(std::string& s, const Kelvin& k) {
    if (k.r(NV097_SET_SKIN_MODE)) warn_once("fixed-function skinning not supported");
    s += "    vec4 tPos, cPos;\n";
    row_transform(s, "tPos", "v0", "u.modelView", 0);
    row_transform(s, "cPos", "v0", "u.composite", 0);
    // The composite matrix includes the viewport: divide to screen space.
    s += "    oPos = vec4(cPos.xyz / cPos.w, cPos.w);\n";
    s += "    vec3 eyeP = tPos.xyz / tPos.w;\n";
    s += "    vec3 N = normalize(vec3(dot(vec4(v2.xyz, 0.0), u.invModelView[0]), dot(vec4(v2.xyz, 0.0), u.invModelView[1]), "
         "dot(vec4(v2.xyz, 0.0), u.invModelView[2])));\n";

    if (k.r(NV097_SET_LIGHTING_ENABLE)) {
        u32 cm = k.r(NV097_SET_COLOR_MATERIAL);
        u32 emission_src = cm & 3, ambient_src = cm >> 2 & 3, diffuse_src = cm >> 4 & 3, specular_src = cm >> 6 & 3;
        const char* alpha = diffuse_src == 1 ? "v3.a" : diffuse_src == 2 ? "v4.a" : "u.materialEmission.a";
        if (const char* a = material_src(ambient_src)) add(s, "    oD0 = vec4(%s.rgb * u.materialEmission.rgb, %s);\n", a, alpha);
        else add(s, "    oD0 = vec4(u.sceneAmbient.rgb, %s);\n", alpha);
        if (const char* e = material_src(emission_src)) add(s, "    oD0.rgb += %s.rgb;\n", e);
        s += "    vec3 lAmb = vec3(0.0), lDif = vec3(0.0), lSpec = vec3(0.0);\n";
        u32 mask = k.r(NV097_SET_LIGHT_ENABLE_MASK);
        for (int i = 0; i < 8; i++) {
            u32 mode = mask >> (2 * i) & 3;
            if (!mode) continue;
            if (mode == 3) warn_once("spot lights approximated");
            int b = 9 * i;
            add(s, "    { // light %d (%s)\n", i, mode == 1 ? "infinite" : mode == 2 ? "local" : "spot");
            if (mode == 1) {
                add(s, "        vec3 L = u.lights[%d].xyz, H = u.lights[%d].xyz; float att = 1.0;\n", b + 5, b + 6);
            } else {
                add(s, "        vec3 L = u.lights[%d].xyz - eyeP; float d = length(L); L /= d;\n", b + 3);
                add(s, "        vec3 a = u.lights[%d].xyz; float att = 1.0 / max(a.x + a.y * d + a.z * d * d, 1e-6);\n", b + 4);
                add(s, "        if (d > u.lights[%d].w) att = 0.0;\n", b + 3);
                s += "        vec3 H = normalize(L + u.eyePosition.xyz / max(u.eyePosition.w, 1e-6));\n";
                if (mode == 3) {
                    add(s, "        float ca = dot(-L, u.lights[%d].xyz) + u.lights[%d].w;\n", b + 8, b + 8);
                    add(s, "        vec3 f = u.lights[%d].xyz; att *= clamp(f.x + f.y * ca + f.z * ca * ca, 0.0, 1.0);\n", b + 7);
                }
            }
            s += "        float nl = max(dot(N, L), 0.0), nh = max(dot(N, H), 0.0);\n";
            s += "        float pf = nl > 0.0 ? pow(nh, u.misc.y) : 0.0;\n";
            add(s, "        lAmb += u.lights[%d].rgb * att; lDif += u.lights[%d].rgb * nl * att; lSpec += u.lights[%d].rgb * pf * att;\n",
                b, b + 1, b + 2);
            s += "    }\n";
        }
        s += "    oD0.rgb += lAmb;\n";
        if (const char* d = material_src(diffuse_src)) add(s, "    oD0.rgb += %s.rgb * lDif;\n", d);
        else s += "    oD0.rgb += lDif;\n";
        s += "    oD1 = vec4(0.0, 0.0, 0.0, v4.a);\n";
        if (k.r(NV097_SET_SPECULAR_ENABLE)) {
            if (const char* sp = material_src(specular_src)) add(s, "    oD1.rgb += %s.rgb * lSpec;\n", sp);
            else s += "    oD1.rgb += lSpec;\n";
        }
        s += "    oB0 = oD0; oB1 = oD1;\n";
    } else {
        s += "    oD0 = v3; oD1 = v4; oB0 = v7; oB1 = v8;\n";
    }

    // Texture coordinates: generation, then the texture matrix.
    s += "    vec3 Rf = reflect(normalize(eyeP), N);\n";
    for (int i = 0; i < 4; i++) {
        add(s, "    { vec4 t = v%d;\n", 9 + i);
        for (int j = 0; j < 4; j++) {
            u32 mode = k.r(NV097_SET_TEXGEN_S + 0x10 * i + 4 * j);
            char c = "xyzw"[j];
            int plane = 4 * i + j;
            switch (mode) {
            case 0: break;
            case 0x2400: add(s, "      t.%c = dot(tPos, u.texgenPlane[%d]);\n", c, plane); break;
            case 0x2401: add(s, "      t.%c = dot(v0, u.texgenPlane[%d]);\n", c, plane); break;
            case 0x2402:
                if (j < 2) {
                    s += "      { float m = 2.0 * sqrt(Rf.x * Rf.x + Rf.y * Rf.y + (Rf.z + 1.0) * (Rf.z + 1.0));\n";
                    add(s, "        t.%c = Rf.%c / m + 0.5; }\n", c, c);
                }
                break;
            case 0x8511: if (j < 3) add(s, "      t.%c = N.%c;\n", c, c); break;
            case 0x8512: if (j < 3) add(s, "      t.%c = Rf.%c;\n", c, c); break;
            default: {
                static u32 warned;
                if (!(warned & 1)) log_msg("[shader] texgen mode %04X not supported", mode);
                warned |= 1;
                break;
            }
            }
        }
        if (k.r(NV097_SET_TEXTURE_MATRIX_ENABLE + 4 * i)) {
            char dst[8];
            snprintf(dst, sizeof dst, "oT%d", i);
            row_transform(s, dst, "t", "u.texMatrix", 4 * i);
        } else {
            add(s, "      oT%d = t;\n", i);
        }
        s += "    }\n";
    }

    // Fog distance by the generation mode.
    switch (k.r(NV097_SET_FOG_GEN_MODE)) {
    case 0: s += "    float fogDist = v4.a;\n"; break;          // specular alpha
    case 1: s += "    float fogDist = length(eyeP);\n"; break;  // radial
    case 2: s += "    float fogDist = eyeP.z;\n"; break;        // planar
    case 3: s += "    float fogDist = abs(eyeP.z);\n"; break;
    default: s += "    float fogDist = v5.x;\n"; break;         // fog coordinate
    }
}

// Fog factor from the distance (both transform modes), into oFog.
static void fog_factor(std::string& s, const Kelvin& k) {
    if (!k.r(NV097_SET_FOG_ENABLE)) return;
    switch (k.r(NV097_SET_FOG_MODE)) {
    case 0x800: case 0x802:  // EXP
        s += "    oFog = vec4(u.fogParams.x + exp2(fogDist * u.fogParams.y * 16.0) - 1.5);\n";
        break;
    case 0x801: case 0x803:  // EXP2
        s += "    oFog = vec4(u.fogParams.x + exp2(-fogDist * fogDist * u.fogParams.y * u.fogParams.y * 32.0) - 1.5);\n";
        break;
    default:  // LINEAR
        s += "    oFog = vec4(u.fogParams.x + fogDist * u.fogParams.y - 1.0);\n";
        break;
    }
}

std::string gen_vertex_shader(const Kelvin& k, VsInputs in) {
    std::string s = "#version 450\n";
    s += kUniforms;
    for (int i = 0; i < 16; i++)
        if (in.present >> i & 1) add(s, "layout(location = %d) in %s inV%d;\n", i, in.packed >> i & 1 ? "uint" : "vec4", i);
    varyings(s, k, "out");
    s += R"(
float rcc(float x) {
    float r = 1.0 / x;
    float m = clamp(abs(r), 5.42101e-20, 1.84467e+19);
    return r < 0.0 ? -m : m;
}
vec4 vexp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }
vec4 vlog(float x) {
    float a = abs(x);
    if (a == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);
    float e = floor(log2(a));
    return vec4(e, a / exp2(e), log2(a), 1.0);
}
vec4 vlit(vec4 s) {
    float d = max(s.x, 0.0), sp = 0.0;
    if (s.x > 0.0) sp = pow(max(s.y, 0.0), clamp(s.w, -128.0, 128.0));
    return vec4(1.0, d, sp, 1.0);
}
vec4 unpackCmp(uint p) {
    int x = int(p << 21) >> 21, y = int(p << 10) >> 21, z = int(p) >> 22;
    return vec4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);
}
void main() {
)";
    for (int i = 0; i < 16; i++) {
        if (!(in.present >> i & 1)) add(s, "    vec4 v%d = u.inlineAttr[%d];\n", i, i);
        else if (in.packed >> i & 1) add(s, "    vec4 v%d = unpackCmp(inV%d);\n", i, i);
        else add(s, "    vec4 v%d = inV%d;\n", i, i);
    }
    s += R"(    vec4 R[12];
    for (int i = 0; i < 12; i++) R[i] = vec4(0.0);
    int A0 = 0;
    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0), oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 oB0 = vec4(0.0, 0.0, 0.0, 1.0), oB1 = vec4(0.0, 0.0, 0.0, 1.0), oFog = vec4(1.0), oPts = vec4(1.0);
    vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = oT0, oT2 = oT0, oT3 = oT0;
    vec4 oUnk1, oUnk2, oUnk13, oUnk14, oUnk15;
)";
    bool program = (k.r(NV097_SET_TRANSFORM_EXECUTION_MODE) & 3) == 2;
    if (program) {
        translate_program(s, k);
        s += "    float fogDist = oFog.x;\n";
    } else {
        fixed_function(s, k);
    }
    fog_factor(s, k);
    // Screen space (what the NV2A rasterizer takes) to Vulkan clip space;
    // w is kept for perspective-correct interpolation.
    s += R"(
    float w = oPos.w;
    if (isinf(w) || isnan(w) || w == 0.0) w = 1.0;
    w = sign(w) * clamp(abs(w), 5.42101e-20, 1.84467e+19);
    vec2 ndc = oPos.xy / u.surface.xy * 2.0 - 1.0;
    gl_Position = vec4(ndc * w, oPos.z * u.surface.z * w, w);
    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);
    vB0 = clamp(oB0, 0.0, 1.0); vB1 = clamp(oB1, 0.0, 1.0);
    vFog = oFog;
    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3;
}
)";
    return s;
}

// ------------------------------------------------------ register combiners
static std::string comb_reg(u32 reg, bool final) {
    switch (reg) {
    case 0: return "vec4(0.0)";
    case 1: return "C0";
    case 2: return "C1";
    case 3: return "fog";
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    case 14: return final ? "specR0" : "vec4(0.0)";
    case 15: return final ? "ef" : "vec4(0.0)";
    }
    return "vec4(0.0)";
}

// One combiner input byte: register (bits 0-3), alpha (4), mapping (5-7).
static std::string comb_input(u32 byte, bool alpha_portion, bool final) {
    std::string r = comb_reg(byte & 15, final);
    bool alpha = byte >> 4 & 1;
    std::string x = alpha_portion ? (alpha ? r + ".a" : r + ".b") : (alpha ? "vec3(" + r + ".a)" : r + ".rgb");
    const char* zero = alpha_portion ? "0.0" : "vec3(0.0)";
    const char* one = alpha_portion ? "1.0" : "vec3(1.0)";
    char buf[256];
    switch (byte >> 5 & 7) {
    case 0: snprintf(buf, sizeof buf, "max(%s, %s)", x.c_str(), zero); break;
    case 1: snprintf(buf, sizeof buf, "(%s - clamp(%s, %s, %s))", one, x.c_str(), zero, one); break;
    case 2: snprintf(buf, sizeof buf, "(2.0 * max(%s, %s) - 1.0)", x.c_str(), zero); break;
    case 3: snprintf(buf, sizeof buf, "(-2.0 * max(%s, %s) + 1.0)", x.c_str(), zero); break;
    case 4: snprintf(buf, sizeof buf, "(max(%s, %s) - 0.5)", x.c_str(), zero); break;
    case 5: snprintf(buf, sizeof buf, "(-max(%s, %s) + 0.5)", x.c_str(), zero); break;
    case 6: snprintf(buf, sizeof buf, "%s", x.c_str()); break;
    default: snprintf(buf, sizeof buf, "(-%s)", x.c_str()); break;
    }
    return buf;
}

static const char* comb_op(u32 op) {
    switch (op) {
    case 1: return "(%s - 0.5)";
    case 2: return "(%s * 2.0)";
    case 3: return "((%s - 0.5) * 2.0)";
    case 4: return "(%s * 4.0)";
    case 6: return "(%s * 0.5)";
    default: return "(%s)";
    }
}

static void comb_stage(std::string& s, u32 icw, u32 ocw, bool alpha, bool msb_mux) {
    std::string A = comb_input(icw >> 24 & 0xFF, alpha, false), B = comb_input(icw >> 16 & 0xFF, alpha, false);
    std::string C = comb_input(icw >> 8 & 0xFF, alpha, false), D = comb_input(icw & 0xFF, alpha, false);
    u32 cd_dst = ocw & 15, ab_dst = ocw >> 4 & 15, sum_dst = ocw >> 8 & 15;
    bool cd_dot = !alpha && (ocw >> 12 & 1), ab_dot = !alpha && (ocw >> 13 & 1), mux = ocw >> 14 & 1;
    u32 op = ocw >> 15 & 7;
    const char* T = alpha ? "float" : "vec3";
    std::string ab = ab_dot ? "vec3(dot(" + A + ", " + B + "))" : A + " * " + B;
    std::string cd = cd_dot ? "vec3(dot(" + C + ", " + D + "))" : C + " * " + D;
    add(s, "        %s ab = %s;\n", T, ab.c_str());
    add(s, "        %s cd = %s;\n", T, cd.c_str());
    if (mux)
        add(s, "        %s sum = %s ? cd : ab;\n", T, msb_mux ? "r0.a >= 0.5" : "(int(r0.a * 255.0 + 0.5) & 1) != 0");
    else
        add(s, "        %s sum = ab + cd;\n", T);
    const char* sw = alpha ? ".a" : ".rgb";
    char e[128];
    struct Out {
        u32 dst;
        const char* v;
        bool b2a;
    } outs[3] = {{ab_dst, "ab", !alpha && (ocw >> 19 & 1)}, {cd_dst, "cd", !alpha && (ocw >> 18 & 1)}, {sum_dst, "sum", false}};
    for (auto& o : outs) {
        if (!o.dst) continue;
        snprintf(e, sizeof e, comb_op(op), o.v);
        std::string dst = comb_reg(o.dst, false);
        if (dst.rfind("vec4", 0) == 0) continue;
        add(s, "        %s%s = clamp(%s, -1.0, 1.0);\n", dst.c_str(), sw, e);
        if (o.b2a) add(s, "        %s.a = %s.b;\n", dst.c_str(), dst.c_str());
    }
}

std::string gen_fragment_shader(const Kelvin& k, const TextureDesc* tex, u32 tex_mask) {
    std::string s = "#version 450\n";
    s += kUniforms;
    varyings(s, k, "in");
    for (int i = 0; i < 4; i++) {
        add(s, "layout(set = 0, binding = %d) uniform sampler2D tex%d;\n", 1 + i, i);
        add(s, "layout(set = 0, binding = %d) uniform samplerCube cube%d;\n", 5 + i, i);
    }
    s += "layout(location = 0) out vec4 fragColor;\n";
    s += "float sgn8(float x) { return fract(x + 0.5) * 2.0 - 1.0; }\n";
    s += "void main() {\n";
    s += "    vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n";
    s += "    vec4 vT[4] = vec4[4](vT0, vT1, vT2, vT3);\n";

    // Texture shader stages.
    u32 prog = k.r(NV097_SET_SHADER_STAGE_PROGRAM);
    u32 other = k.r(NV097_SET_SHADER_OTHER_STAGE_INPUT), rgbmap = k.r(NV097_SET_DOT_RGBMAPPING);
    s += "    float dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n";
    for (int i = 0; i < 4; i++) {
        u32 mode = prog >> (5 * i) & 31;
        if (mode == 0) continue;
        // Which earlier stage feeds this one, and how its colour maps to a vector.
        int in = i == 1 ? 0 : i == 2 ? (other >> 16 & 15) : i == 3 ? (other >> 20 & 15) : 0;
        char map[48];
        if (i > 0 && (rgbmap >> (4 * (i - 1)) & 15) == 0) snprintf(map, sizeof map, "t%d.rgb", in);
        else snprintf(map, sizeof map, "(t%d.rgb * 2.0 - 1.0)", in);
        auto sample_2d = [&](const char* uv) { add(s, "    t%d = texture(tex%d, (%s) * u.texScale[%d].xy);\n", i, i, uv, i); };
        char expr[128];
        bool input_signed = (tex_mask >> in & 1) && (tex[in].filter >> 28);
        switch (mode) {
        case 1:  // PROJECT2D
            snprintf(expr, sizeof expr, "vT%d.xy / vT%d.w", i, i);
            sample_2d(expr);
            break;
        case 2:  // PROJECT3D: volume textures are not supported, sample as 2D
            warn_once("volume textures sampled as 2D");
            snprintf(expr, sizeof expr, "vT%d.xy / vT%d.w", i, i);
            sample_2d(expr);
            break;
        case 3: add(s, "    t%d = texture(cube%d, vT%d.xyz);\n", i, i, i); break;
        case 4: add(s, "    t%d = clamp(vT%d, 0.0, 1.0);\n", i, i); break;
        case 5:  // CLIP_PLANE: kill the fragment where the selected coordinates are negative
            add(s, "    if (any(lessThan(vT%d, vec4(0.0)))) discard;\n", i);
            break;
        case 6:  // BUMPENVMAP: offset by an earlier stage's signed du/dv (blue, green)
        case 7:
            if (mode == 7) warn_once("bump map luminance ignored");
            add(s, "    { vec2 d = %s; vec2 uv = vT%d.xy / vT%d.w + vec2(dot(d, u.bumpMat[%d].xz), dot(d, u.bumpMat[%d].yw));\n",
                input_signed ? (std::string("t") + std::to_string(in) + ".bg").c_str()
                             : (std::string("(t") + std::to_string(in) + ".bg * 2.0 - 1.0)").c_str(),
                i, i, i, i);
            add(s, "      t%d = texture(tex%d, uv * u.texScale[%d].xy); }\n", i, i, i);
            break;
        case 9:  // DOT_ST: 2x2 matrix with the previous stage's dot product
            add(s, "    dot%d = dot(vT%d.xyz, %s);\n", i, i, map);
            snprintf(expr, sizeof expr, "vec2(dot%d, dot%d)", i - 1, i);
            sample_2d(expr);
            break;
        case 11:  // DOT_RFLCT_DIFF: normal from this stage's and its neighbours' dots
            add(s, "    dot%d = dot(vT%d.xyz, %s);\n", i, i, map);
            add(s, "    t%d = texture(cube%d, vec3(dot%d, dot%d, dot(vT%d.xyz, %s)));\n", i, i, i - 1, i, i + 1, map);
            break;
        case 12:  // DOT_RFLCT_SPEC: reflect the eye vector (texture coordinate w's)
        case 18:  // ..._CONST: reflect a constant eye vector
            add(s, "    dot%d = dot(vT%d.xyz, %s);\n", i, i, map);
            add(s, "    { vec3 n = vec3(dot%d, dot%d, dot%d);\n", i - 2, i - 1, i);
            if (mode == 12) add(s, "      vec3 e = vec3(vT%d.w, vT%d.w, vT%d.w);\n", i - 2, i - 1, i);
            else s += "      vec3 e = u.eyeVec.xyz;\n";
            add(s, "      t%d = texture(cube%d, 2.0 * n * dot(n, e) / dot(n, n) - e); }\n", i, i);
            break;
        case 14:  // DOT_STR_CUBE: 3x3 matrix into a cube map
            add(s, "    dot%d = dot(vT%d.xyz, %s);\n", i, i, map);
            add(s, "    t%d = texture(cube%d, vec3(dot%d, dot%d, dot%d));\n", i, i, i - 2, i - 1, i);
            break;
        case 15:  // DPNDNT_AR
            snprintf(expr, sizeof expr, "t%d.ar", in);
            sample_2d(expr);
            break;
        case 16:  // DPNDNT_GB
            snprintf(expr, sizeof expr, "t%d.gb", in);
            sample_2d(expr);
            break;
        case 17:  // DOTPRODUCT: only feeds later stages
            add(s, "    dot%d = dot(vT%d.xyz, %s);\n", i, i, map);
            break;
        default: {
            char msg[48];
            snprintf(msg, sizeof msg, "texture shader mode %u not supported", mode);
            warn_once(msg);
            snprintf(expr, sizeof expr, "vT%d.xy / vT%d.w", i, i);
            sample_2d(expr);
            break;
        }
        }
        if (tex_mask >> i & 1) {
            // Channels the texture stores as signed bytes.
            u32 sg = tex[i].filter >> 28;
            if (sg & 1) add(s, "    t%d.a = sgn8(t%d.a);\n", i, i);
            if (sg & 2) add(s, "    t%d.r = sgn8(t%d.r);\n", i, i);
            if (sg & 4) add(s, "    t%d.g = sgn8(t%d.g);\n", i, i);
            if (sg & 8) add(s, "    t%d.b = sgn8(t%d.b);\n", i, i);
            if (tex[i].control0 >> 2 & 1) add(s, "    if (t%d.a == 0.0) discard;\n", i);
        }
    }

    // Register combiners.
    u32 ctl = k.r(NV097_SET_COMBINER_CONTROL);
    u32 stages = ctl & 0xFF;
    if (stages > 8) stages = 8;
    bool msb_mux = ctl >> 8 & 1, each_f0 = ctl >> 12 & 1, each_f1 = ctl >> 16 & 1;
    s += "    vec4 v0 = vD0, v1 = vD1;\n";
    if (k.r(NV097_SET_TWO_SIDE_LIGHT_EN)) s += "    if (!gl_FrontFacing) { v0 = vB0; v1 = vB1; }\n";
    s += "    vec4 fog = vec4(u.fogColor.rgb, clamp(vFog.x, 0.0, 1.0));\n";
    s += "    vec4 r0 = vec4(0.0, 0.0, 0.0, t0.a), r1 = vec4(0.0);\n";
    s += "    vec4 C0, C1;\n";
    for (u32 st = 0; st < stages; st++) {
        add(s, "    // stage %u\n    C0 = u.factor0[%u]; C1 = u.factor1[%u];\n", st, each_f0 ? st : 0, each_f1 ? st : 0);
        s += "    {\n";
        comb_stage(s, k.r(NV097_SET_COMBINER_COLOR_ICW + 4 * st), k.r(NV097_SET_COMBINER_COLOR_OCW + 4 * st), false, msb_mux);
        s += "    }\n    {\n";
        comb_stage(s, k.r(NV097_SET_COMBINER_ALPHA_ICW + 4 * st), k.r(NV097_SET_COMBINER_ALPHA_OCW + 4 * st), true, msb_mux);
        s += "    }\n";
    }

    // Final combiner: rgb = A*B + (1-A)*C + D, alpha = G.
    u32 cw0 = k.r(NV097_SET_COMBINER_SPECULAR_FOG_CW0), cw1 = k.r(NV097_SET_COMBINER_SPECULAR_FOG_CW1);
    s += "    C0 = u.factor0[0]; C1 = u.factor1[0];\n";
    if (cw1 >> 7 & 1) s += "    vec4 specR0 = vec4(clamp(r0.rgb + v1.rgb, 0.0, 1.0), 0.0);\n";
    else s += "    vec4 specR0 = vec4(r0.rgb + v1.rgb, 0.0);\n";
    auto fin = [&](u32 byte, bool alpha) {
        // Final combiner inputs only map identity (0) or invert (1).
        std::string r = comb_reg(byte & 15, true);
        bool a = byte >> 4 & 1;
        std::string x = alpha ? r + ".a" : (a ? "vec3(" + r + ".a)" : r + ".rgb");
        if (byte >> 5 & 1) x = (alpha ? "(1.0 - clamp(" : "(vec3(1.0) - clamp(") + x + ", 0.0, 1.0))";
        else x = "clamp(" + x + ", 0.0, 1.0)";
        return x;
    };
    std::string E = fin(cw1 >> 24 & 0xFF, false), F = fin(cw1 >> 16 & 0xFF, false);
    add(s, "    vec4 ef = vec4(%s * %s, 0.0);\n", E.c_str(), F.c_str());
    std::string A = fin(cw0 >> 24 & 0xFF, false), B = fin(cw0 >> 16 & 0xFF, false);
    std::string Cc = fin(cw0 >> 8 & 0xFF, false), D = fin(cw0 & 0xFF, false), G = fin(cw1 >> 8 & 0xFF, true);
    add(s, "    vec3 A = %s;\n", A.c_str());
    add(s, "    vec4 outc = vec4(clamp(A * %s + (vec3(1.0) - A) * %s + %s, 0.0, 1.0), %s);\n", B.c_str(), Cc.c_str(),
        D.c_str(), G.c_str());

    // Alpha test.
    if (k.r(NV097_SET_ALPHA_TEST_ENABLE)) {
        static const char* cmp[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        u32 f = k.r(NV097_SET_ALPHA_FUNC) & 7;
        if (f == 0) s += "    discard;\n";
        else if (f != 7) add(s, "    if (!(int(outc.a * 255.0 + 0.5) %s int(u.misc.x))) discard;\n", cmp[f]);
    }
    s += "    fragColor = outc;\n}\n";
    return s;
}
