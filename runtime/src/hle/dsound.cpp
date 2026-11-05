// DirectSound replaced at the API level (addresses in config/overrides.txt).
//
// The XDK library drives the APU's voice processor and DSPs, none of which
// is emulated. Instead the entry points the game and Bink call are host
// functions here: buffers and streams become voices of a software mixer
// whose output goes to the host (audio_out.cpp). Sample data stays in guest
// memory and is read live, so Bink can keep filling its ring buffer.
#include <math.h>

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "audio.h"
#include "kernel.h"

constexpr u32 STREAM_VTABLE = 0x00263928;  // XDK vtable; its entries are overridden below
constexpr float PI = 3.14159265f;
constexpr s32 DEFAULT_HEADROOM = 600;  // hundredths of a dB, as DirectSound sets it

constexpr u32 S_OK_ = 0, E_OUTOFMEMORY_ = 0x8007000E, E_FAIL_ = 0x80004005;
constexpr u32 DSERR_STREAM_FULL = 0x88780032;
constexpr u32 XMEDIAPACKET_PENDING = 0x8000000A, XMEDIAPACKET_FLUSHED = 0x80004004;
constexpr u32 DSBCAPS_CTRL3D = 0x10;
constexpr u32 DSBCAPS_NO_HEADROOM = 0x82000 | DSBCAPS_CTRL3D;  // mix-in and 3D voices default to 0
constexpr u32 DSBPLAY_LOOPING = 1, DSBPLAY_FROMSTART = 2;
constexpr u32 DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4;
constexpr u32 DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2;
constexpr u32 XMO_STATUSF_ACCEPT_INPUT_DATA = 1;
constexpr u32 XMO_STREAMF_STREAM_INFO = 0x15;  // fixed sample size, async input, whole samples
constexpr u32 STREAM_LOOKAHEAD = 0x1000;
constexpr u32 DS3DMODE_HEADRELATIVE = 1, DS3DMODE_DISABLE = 2;
constexpr u16 WAVE_PCM = 1, WAVE_XBOX_ADPCM = 0x69;
constexpr u32 ADPCM_FRAMES_PER_BLOCK = 64, ADPCM_BLOCK_BYTES = 36;
enum { MIXBIN_FRONT_LEFT, MIXBIN_FRONT_RIGHT, MIXBIN_FRONT_CENTER, MIXBIN_LFE, MIXBIN_BACK_LEFT, MIXBIN_BACK_RIGHT };

static u32 arg(Ctx* c, int i) { return rd32(c->esp + 4 + 4 * i); }
static float farg(Ctx* c, int i) {
    float f;
    u32 v = arg(c, i);
    memcpy(&f, &v, 4);
    return f;
}
// stdcall return: result in eax, pop return address and arguments.
static void ret(Ctx* c, u32 value, u32 nargs) {
    c->eax = value;
    c->esp += 4 + 4 * nargs;
}

struct Vec {
    float x, y, z;
};
static Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float length(Vec a) { return sqrtf(dot(a, a)); }
static Vec normalized(Vec a) {
    float l = length(a);
    return l > 1e-6f ? Vec{a.x / l, a.y / l, a.z / l} : Vec{0, 0, 0};
}

struct Format {
    u16 tag = WAVE_PCM, channels = 1;
    u32 rate = 48000;
    u16 block = 2, bits = 16;
};

struct Packet {
    u32 buffer, size, completed_ptr, status_ptr;
    u64 end_frame;  // stream frame index just past this packet's audio
};

struct Voice {
    u32 handle = 0, refs = 1;
    bool stream = false;
    Format fmt;
    u32 owned_data = 0;  // buffer memory DirectSound allocated itself

    // Buffer playback.
    u32 data = 0, bytes = 0;
    bool playing = false, looping = false;
    double pos = 0;  // in source frames

    u32 freq = 0;  // 0 = format rate
    s32 volume = 0, headroom = DEFAULT_HEADROOM;
    bool has_bins = false;
    float bin_l = 1, bin_r = 1;

    bool is3d = false;
    u32 mode3d = 0;
    Vec p3d{0, 0, 0}, v3d{0, 0, 0};
    float min_dist = 1, max_dist = 1e9f;

    float last_l = 0, last_r = 0;  // gains of the previous block, for ramping

    // ADPCM: the block decoded last (buffers).
    s64 cached_block = -1;
    std::vector<float> block;

    // Streams: decoded frames not played yet, and the packets behind them.
    u32 max_packets = 0;
    std::deque<Packet> waiting;   // attached, not decoded yet
    std::deque<Packet> decoded;   // decoded, playing
    std::deque<float> fifo;       // interleaved
    u64 fifo_base = 0;            // stream frame index of fifo[0]
    u64 fifo_end = 0;             // stream frame index past the fifo
};

static std::mutex g_m;  // voices and listener; the mixer holds it per block
static std::unordered_map<u32, Voice*> g_voices;
static std::vector<u32> g_free_handles;
static u32 g_ds_handle;

static struct {
    Vec pos{0, 0, 0}, vel{0, 0, 0}, front{0, 0, 1}, top{0, 1, 0};
    float distance = 1, rolloff = 1, doppler = 1;
} g_listener;

static Voice* voice(u32 handle) {
    auto it = g_voices.find(handle);
    return it != g_voices.end() ? it->second : nullptr;
}

static Voice* new_voice() {
    u32 h;
    if (!g_free_handles.empty()) {
        h = g_free_handles.back();
        g_free_handles.pop_back();
    } else {
        h = kernel_alloc(16);
    }
    Voice* v = new Voice;
    v->handle = h;
    g_voices[h] = v;
    return v;
}

static Format read_format(u32 p) {
    Format f;
    if (!p) return f;
    f.tag = rd16(p + 0);
    f.channels = rd16(p + 2);
    f.rate = rd32(p + 4);
    f.block = rd16(p + 12);
    f.bits = rd16(p + 14);
    if (!f.channels) f.channels = 1;
    if (f.tag != WAVE_PCM && f.tag != WAVE_XBOX_ADPCM) log_msg("[dsound] unsupported format tag %04X", f.tag);
    if (!f.block) f.block = f.tag == WAVE_XBOX_ADPCM ? (u16)(ADPCM_BLOCK_BYTES * f.channels) : (u16)(f.channels * f.bits / 8);
    return f;
}

static u32 byte_to_frame(const Format& f, u32 bytes) {
    return f.tag == WAVE_XBOX_ADPCM ? bytes / f.block * ADPCM_FRAMES_PER_BLOCK : bytes / f.block;
}
static u32 frame_to_byte(const Format& f, u32 frame) {
    return f.tag == WAVE_XBOX_ADPCM ? frame / ADPCM_FRAMES_PER_BLOCK * f.block : frame * f.block;
}

// ---------------------------------------------------------------- decoding
static const s16 kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const s8 kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// One Xbox ADPCM block: per channel a 4-byte header (predictor, step index)
// and then 4-byte groups of nibbles, channels interleaved. The header sample
// is frame 0; the block's last code predicts the next block's header, so 64
// frames are kept. `out` receives interleaved frames.
static void decode_adpcm_block(u32 addr, u32 channels, float* out) {
    for (u32 ch = 0; ch < channels; ch++) {
        s32 pred = (s16)rd16(addr + 4 * ch);
        s32 index = rd8(addr + 4 * ch + 2);
        if (index > 88) index = 88;
        out[ch] = pred / 32768.0f;
        u32 data = addr + 4 * channels + ch * 4;
        for (u32 i = 1; i < ADPCM_FRAMES_PER_BLOCK; i++) {
            u32 n = i - 1;
            u32 byte = rd8(data + (n / 8) * 4 * channels + (n / 2) % 4);
            u32 code = n & 1 ? byte >> 4 : byte & 15;
            s32 step = kImaStep[index];
            s32 diff = step >> 3;
            if (code & 1) diff += step >> 2;
            if (code & 2) diff += step >> 1;
            if (code & 4) diff += step;
            pred += code & 8 ? -diff : diff;
            pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
            index += kImaIndex[code];
            index = index < 0 ? 0 : index > 88 ? 88 : index;
            out[i * channels + ch] = pred / 32768.0f;
        }
    }
}

static float pcm_sample(u32 addr, u32 bits) {
    return bits == 8 ? ((int)rd8(addr) - 128) / 128.0f : (s16)rd16(addr) / 32768.0f;
}

// Frame `i` of a buffer voice into out[0..channels).
static void buffer_frame(Voice* v, u32 i, float* out) {
    const Format& f = v->fmt;
    if (f.tag == WAVE_XBOX_ADPCM) {
        s64 blk = i / ADPCM_FRAMES_PER_BLOCK;
        if (blk != v->cached_block) {
            v->block.resize(ADPCM_FRAMES_PER_BLOCK * f.channels);
            decode_adpcm_block(v->data + (u32)blk * f.block, f.channels, v->block.data());
            v->cached_block = blk;
        }
        for (u32 c = 0; c < f.channels; c++) out[c] = v->block[(i % ADPCM_FRAMES_PER_BLOCK) * f.channels + c];
    } else {
        u32 a = v->data + i * f.block;
        for (u32 c = 0; c < f.channels; c++) out[c] = pcm_sample(a + c * (f.bits / 8), f.bits);
    }
}

static void complete_packet(const Packet& p, u32 status, u32 bytes) {
    if (p.completed_ptr) wr32(p.completed_ptr, bytes);
    if (p.status_ptr) wr32(p.status_ptr, status);
}

// Move the next attached packet's audio into the fifo.
static bool stream_decode_next(Voice* v) {
    if (v->waiting.empty()) return false;
    Packet p = v->waiting.front();
    v->waiting.pop_front();
    const Format& f = v->fmt;
    u32 frames = byte_to_frame(f, p.size);
    if (f.tag == WAVE_XBOX_ADPCM) {
        std::vector<float> tmp(ADPCM_FRAMES_PER_BLOCK * f.channels);
        for (u32 b = 0; b < p.size / f.block; b++) {
            decode_adpcm_block(p.buffer + b * f.block, f.channels, tmp.data());
            v->fifo.insert(v->fifo.end(), tmp.begin(), tmp.end());
        }
    } else {
        for (u32 i = 0; i < frames; i++)
            for (u32 c = 0; c < f.channels; c++)
                v->fifo.push_back(pcm_sample(p.buffer + i * f.block + c * (f.bits / 8), f.bits));
    }
    v->fifo_end += frames;
    p.end_frame = v->fifo_end;
    v->decoded.push_back(p);
    return true;
}

// ------------------------------------------------------------------- mixer
static void voice_gains(Voice* v, float& l, float& r, double& pitch) {
    float gain = powf(10.0f, (float)(v->volume - v->headroom) / 2000.0f);
    l = v->bin_l;
    r = v->bin_r;
    pitch = 1.0;
    if (!v->is3d || v->mode3d == DS3DMODE_DISABLE) {
        l *= gain;
        r *= gain;
        return;
    }
    Vec rel = v->mode3d == DS3DMODE_HEADRELATIVE ? v->p3d : v->p3d - g_listener.pos;
    float d = length(rel);
    float dc = d < v->min_dist ? v->min_dist : d > v->max_dist ? v->max_dist : d;
    float denom = v->min_dist + g_listener.rolloff * (dc - v->min_dist);
    gain *= denom > 1e-6f ? v->min_dist / denom : 1.0f;
    if (v->fmt.channels == 1) {
        // Constant-power pan from the listener's right axis.
        Vec right = normalized(cross(g_listener.top, g_listener.front));
        float x = d > 1e-3f ? dot(rel, right) / d : 0;
        float angle = (x + 1) * PI / 4;
        l = cosf(angle) * 1.414f;
        r = sinf(angle) * 1.414f;
    }
    l *= gain;
    r *= gain;
    // Doppler: speed of sound in world units from the distance factor.
    if (g_listener.doppler > 0 && d > 1e-3f && v->mode3d != DS3DMODE_HEADRELATIVE) {
        float c = 343.0f / (g_listener.distance > 1e-6f ? g_listener.distance : 1.0f);
        Vec dir = {rel.x / d, rel.y / d, rel.z / d};
        float vl = dot(g_listener.vel, dir) * g_listener.doppler, vs = dot(v->v3d, dir) * g_listener.doppler;
        vl = fminf(vl, c * 0.5f);
        vs = fminf(vs, c * 0.5f);
        pitch = (c - vl) / (c - vs);
        pitch = pitch < 0.25 ? 0.25 : pitch > 4.0 ? 4.0 : pitch;
    }
}

static void mix_voice(Voice* v, float* out, u32 frames) {
    const Format& f = v->fmt;
    float gl, gr;
    double pitch;
    voice_gains(v, gl, gr, pitch);
    double step = (double)(v->freq ? v->freq : f.rate) / AUDIO_RATE * pitch;
    float a[8] = {0}, b[8] = {0};
    u32 total = v->stream ? 0 : byte_to_frame(f, v->bytes);
    if (!v->stream && !total) {
        v->playing = false;
        return;
    }
    for (u32 n = 0; n < frames; n++) {
        u64 i = (u64)v->pos;
        float t = (float)(v->pos - (double)i);
        if (v->stream) {
            // Keep two frames available for interpolation.
            while (i + 2 > v->fifo_end && stream_decode_next(v)) {
            }
            if (i + 2 > v->fifo_end) {
                // Starved: silence until the next packet. Everything queued
                // counts as played, or the last packet would never complete.
                v->pos = (double)v->fifo_end;
                break;
            }
            size_t off = (size_t)(i - v->fifo_base) * f.channels;
            for (u32 c = 0; c < f.channels; c++) {
                a[c] = v->fifo[off + c];
                b[c] = v->fifo[off + f.channels + c];
            }
        } else {
            if (i >= total) {
                if (v->looping) {
                    v->pos = fmod(v->pos, (double)total);
                    i = (u64)v->pos;
                } else {
                    v->playing = false;
                    v->pos = 0;
                    break;
                }
            }
            buffer_frame(v, (u32)i, a);
            u64 j = i + 1;
            if (j < total || v->looping) buffer_frame(v, (u32)(j % total), b);
            else memcpy(b, a, sizeof b);
        }
        float s0 = a[0] + (b[0] - a[0]) * t;
        float s1 = f.channels > 1 ? a[1] + (b[1] - a[1]) * t : s0;
        float fade = (float)n / frames;
        out[2 * n] += s0 * (v->last_l + (gl - v->last_l) * fade);
        out[2 * n + 1] += s1 * (v->last_r + (gr - v->last_r) * fade);
        v->pos += step;
    }
    v->last_l = gl;
    v->last_r = gr;
    if (v->stream) {
        u64 played = (u64)v->pos;
        while (!v->decoded.empty() && v->decoded.front().end_frame <= played) {
            complete_packet(v->decoded.front(), S_OK_, v->decoded.front().size);
            v->decoded.pop_front();
        }
        while (v->fifo_base + 8 <= played && v->fifo_base < v->fifo_end) {
            u32 n = (u32)fmin((double)(played - v->fifo_base), 4096.0);
            v->fifo.erase(v->fifo.begin(), v->fifo.begin() + n * f.channels);
            v->fifo_base += n;
        }
    }
}

void dsound_mix(float* out, u32 frames) {
    memset(out, 0, frames * 2 * sizeof(float));
    std::lock_guard<std::mutex> lk(g_m);
    for (auto& e : g_voices) {
        Voice* v = e.second;
        if (v->stream || v->playing) mix_voice(v, out, frames);
    }
    // Soft limit instead of clipping when many voices add up.
    for (u32 i = 0; i < frames * 2; i++) {
        float x = out[i], ax = fabsf(x);
        if (ax > 0.8f) out[i] = (x < 0 ? -1.0f : 1.0f) * (0.8f + 0.2f * tanhf((ax - 0.8f) / 0.2f));
    }
}

// ---------------------------------------------------------------- settings
static u32 release(Voice* v) {
    if (!v) return 0;
    if (--v->refs) return v->refs;
    g_voices.erase(v->handle);
    for (auto& q : {v->waiting, v->decoded})
        for (const Packet& p : q) complete_packet(p, XMEDIAPACKET_FLUSHED, 0);
    if (v->owned_data) kernel_contig_free(v->owned_data);
    g_free_handles.push_back(v->handle);
    delete v;
    return 0;
}

// DSMIXBINS: count, pointer to {bin, volume} pairs.
static void set_mixbins(Voice* v, u32 bins) {
    if (!v || !bins) return;
    u32 n = rd32(bins), pairs = rd32(bins + 4);
    if (n > 8 || !pairs) return;
    float l = 0, r = 0;
    for (u32 i = 0; i < n; i++) {
        u32 bin = rd32(pairs + 8 * i);
        float g = powf(10.0f, (float)(s32)rd32(pairs + 8 * i + 4) / 2000.0f);
        switch (bin) {
        case MIXBIN_FRONT_LEFT: case MIXBIN_BACK_LEFT: l += g; break;
        case MIXBIN_FRONT_RIGHT: case MIXBIN_BACK_RIGHT: r += g; break;
        case MIXBIN_FRONT_CENTER: l += g * 0.707f; r += g * 0.707f; break;
        default: break;  // LFE and effect sends
        }
    }
    v->has_bins = true;
    v->bin_l = fminf(l, 1.0f);
    v->bin_r = fminf(r, 1.0f);
}

// DS3DBUFFER: size, position, velocity, cone angles and orientation, cone
// volume, min and max distance, mode.
static void set_3d_params(Voice* v, u32 p) {
    if (!v || !p) return;
    float vals[6];
    memcpy(vals, gptr(p + 4), sizeof vals);
    v->p3d = {vals[0], vals[1], vals[2]};
    v->v3d = {vals[3], vals[4], vals[5]};
    v->min_dist = rdf32(p + 0x34);
    v->max_dist = rdf32(p + 0x38);
    v->mode3d = rd32(p + 0x3C);
}

static bool g_trace = getenv("NL_TRACE_DSOUND") != nullptr;

static u32 create_buffer(u32 desc, u32 out) {
    std::lock_guard<std::mutex> lk(g_m);
    u32 flags = rd32(desc + 4), bytes = rd32(desc + 8), fmt = rd32(desc + 12), bins = rd32(desc + 16);
    Voice* v = new_voice();
    v->fmt = read_format(fmt);
    v->is3d = (flags & DSBCAPS_CTRL3D) != 0;
    if (flags & DSBCAPS_NO_HEADROOM) v->headroom = 0;
    if (bytes) {
        v->owned_data = kernel_contig_alloc(bytes);
        if (!v->owned_data) {
            release(v);
            wr32(out, 0);
            return E_OUTOFMEMORY_;
        }
        v->data = v->owned_data;
        v->bytes = bytes;
    }
    set_mixbins(v, bins);
    if (g_trace)
        log_msg("[dsound] buffer %08X flags %X bytes %X format %04X %uch %u Hz %u bits block %u", v->handle, flags, bytes,
                v->fmt.tag, v->fmt.channels, v->fmt.rate, v->fmt.bits, v->fmt.block);
    wr32(out, v->handle);
    return S_OK_;
}

static u32 create_stream(u32 desc, u32 out) {
    std::lock_guard<std::mutex> lk(g_m);
    u32 flags = rd32(desc + 0), packets = rd32(desc + 4), fmt = rd32(desc + 8), bins = rd32(desc + 20);
    if (rd32(desc + 12)) log_msg("[dsound] stream completion callbacks are not supported");
    Voice* v = new_voice();
    v->stream = true;
    v->fmt = read_format(fmt);
    v->is3d = (flags & DSBCAPS_CTRL3D) != 0;
    if (flags & DSBCAPS_NO_HEADROOM) v->headroom = 0;
    v->max_packets = packets ? packets : 1;
    wr32(v->handle, STREAM_VTABLE);  // the game calls Process & co through it
    set_mixbins(v, bins);
    if (g_trace)
        log_msg("[dsound] stream %08X flags %X packets %u format %04X %uch %u Hz %u bits block %u", v->handle, flags, packets,
                v->fmt.tag, v->fmt.channels, v->fmt.rate, v->fmt.bits, v->fmt.block);
    wr32(out, v->handle);
    return S_OK_;
}

// ---------------------------------------------------------------- globals
// HRESULT DirectSoundCreate(LPGUID, LPDIRECTSOUND* ppDS, LPUNKNOWN)
void hle_DirectSoundCreate(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ds_handle) {
        g_ds_handle = kernel_alloc(16);
        audio_out_start();
    }
    wr32(arg(c, 1), g_ds_handle);
    ret(c, S_OK_, 3);
}
void hle_DirectSoundCreateBuffer(Ctx* c) { ret(c, create_buffer(arg(c, 0), arg(c, 1)), 2); }
void hle_DirectSoundCreateStream(Ctx* c) { ret(c, create_stream(arg(c, 0), arg(c, 1)), 2); }
void hle_DirectSoundDoWork(Ctx* c) { ret(c, 0, 0); }
void hle_DirectSoundUseHRTF(Ctx* c) { ret(c, 0, 0); }

// ---------------------------------------------------------------- listener
void hle_IDirectSound_Release(Ctx* c) { ret(c, 1, 1); }  // one shared object, lives forever
void hle_IDirectSound_CreateSoundBuffer(Ctx* c) { ret(c, create_buffer(arg(c, 1), arg(c, 2)), 4); }
// (this, image, size, location, ppDesc): no DSPs to load the image into.
void hle_IDirectSound_DownloadEffectsImage(Ctx* c) {
    if (arg(c, 4)) wr32(arg(c, 4), 0);
    ret(c, S_OK_, 5);
}
void hle_IDirectSound_CommitDeferredSettings(Ctx* c) { ret(c, S_OK_, 1); }  // settings apply at once
void hle_IDirectSound_SetI3DL2Listener(Ctx* c) { ret(c, S_OK_, 3); }        // no reverb

void hle_IDirectSound_SetDistanceFactor(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.distance = farg(c, 1);
    ret(c, S_OK_, 3);
}
void hle_IDirectSound_SetRolloffFactor(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.rolloff = farg(c, 1);
    ret(c, S_OK_, 3);
}
void hle_IDirectSound_SetDopplerFactor(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.doppler = farg(c, 1);
    ret(c, S_OK_, 3);
}
void hle_IDirectSound_SetPosition(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.pos = {farg(c, 1), farg(c, 2), farg(c, 3)};
    ret(c, S_OK_, 5);
}
void hle_IDirectSound_SetVelocity(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.vel = {farg(c, 1), farg(c, 2), farg(c, 3)};
    ret(c, S_OK_, 5);
}
void hle_IDirectSound_SetOrientation(Ctx* c) {
    std::lock_guard<std::mutex> lk(g_m);
    g_listener.front = normalized({farg(c, 1), farg(c, 2), farg(c, 3)});
    g_listener.top = normalized({farg(c, 4), farg(c, 5), farg(c, 6)});
    ret(c, S_OK_, 8);
}

// ------------------------------------------------- shared buffer and stream
#define VOICE_ARG0()                          \
    std::lock_guard<std::mutex> lk(g_m);      \
    Voice* v = voice(arg(c, 0))

void hle_ds_SetVolume(Ctx* c) {
    VOICE_ARG0();
    if (v) v->volume = (s32)arg(c, 1);
    ret(c, S_OK_, 2);
}
void hle_ds_SetHeadroom(Ctx* c) {
    VOICE_ARG0();
    if (v) v->headroom = (s32)arg(c, 1);
    ret(c, S_OK_, 2);
}
void hle_ds_SetFrequency(Ctx* c) {
    VOICE_ARG0();
    if (v) v->freq = arg(c, 1);
    ret(c, S_OK_, 2);
}
void hle_ds_SetMixBins(Ctx* c) {
    VOICE_ARG0();
    set_mixbins(v, arg(c, 1));
    ret(c, S_OK_, 2);
}
// (this, DSFILTERDESC*): the per-voice hardware filter is not modelled.
void hle_ds_SetFilter(Ctx* c) { ret(c, S_OK_, 2); }
// (this, mix-in buffer): submixes play straight to the output.
void hle_ds_SetOutputBuffer(Ctx* c) { ret(c, S_OK_, 2); }
void hle_ds_SetAllParameters(Ctx* c) {
    VOICE_ARG0();
    set_3d_params(v, arg(c, 1));
    ret(c, S_OK_, 3);
}
void hle_ds_SetMaxDistance(Ctx* c) {
    VOICE_ARG0();
    if (v) v->max_dist = farg(c, 1);
    ret(c, S_OK_, 3);
}
void hle_ds_SetMinDistance(Ctx* c) {
    VOICE_ARG0();
    if (v) v->min_dist = farg(c, 1);
    ret(c, S_OK_, 3);
}
void hle_ds_SetPosition(Ctx* c) {
    VOICE_ARG0();
    if (v) v->p3d = {farg(c, 1), farg(c, 2), farg(c, 3)};
    ret(c, S_OK_, 5);
}
void hle_ds_SetVelocity(Ctx* c) {
    VOICE_ARG0();
    if (v) v->v3d = {farg(c, 1), farg(c, 2), farg(c, 3)};
    ret(c, S_OK_, 5);
}
void hle_ds_Release(Ctx* c) {
    VOICE_ARG0();
    ret(c, release(v), 1);
}

// ------------------------------------------------------------------ buffers
void hle_buf_SetBufferData(Ctx* c) {
    VOICE_ARG0();
    if (v) {
        v->data = arg(c, 1);
        v->bytes = arg(c, 2);
        v->pos = 0;
        v->cached_block = -1;
    }
    ret(c, S_OK_, 3);
}
// (this, reserved, reserved, flags)
void hle_buf_Play(Ctx* c) {
    VOICE_ARG0();
    u32 flags = arg(c, 3);
    if (v) {
        if (g_trace)
            log_msg("[dsound] play %08X flags %X data %08X bytes %X vol %d room %d freq %u 3d %d", v->handle, flags, v->data,
                    v->bytes, v->volume, v->headroom, v->freq, v->is3d);
        if (flags & DSBPLAY_FROMSTART) v->pos = 0;
        v->looping = (flags & DSBPLAY_LOOPING) != 0;
        v->playing = true;
        v->last_l = v->last_r = 0;
    }
    ret(c, S_OK_, 4);
}
void hle_buf_Stop(Ctx* c) {
    VOICE_ARG0();
    if (v) v->playing = false;
    ret(c, S_OK_, 1);
}
void hle_buf_GetStatus(Ctx* c) {
    VOICE_ARG0();
    u32 s = 0;
    if (v && v->playing) s = DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0);
    wr32(arg(c, 1), s);
    ret(c, S_OK_, 2);
}
void hle_buf_SetCurrentPosition(Ctx* c) {
    VOICE_ARG0();
    if (v) {
        v->pos = byte_to_frame(v->fmt, arg(c, 1));
        v->cached_block = -1;
    }
    ret(c, S_OK_, 2);
}
// The write cursor sits a mix block ahead of the play cursor.
static u32 write_cursor(Voice* v) {
    u32 ahead = frame_to_byte(v->fmt, ADPCM_FRAMES_PER_BLOCK * 2);
    u32 w = frame_to_byte(v->fmt, (u32)v->pos) + ahead;
    return v->bytes ? w % v->bytes : 0;
}
void hle_buf_GetCurrentPosition(Ctx* c) {
    VOICE_ARG0();
    u32 play_ptr = arg(c, 1), write_ptr = arg(c, 2);
    if (play_ptr) wr32(play_ptr, v ? frame_to_byte(v->fmt, (u32)v->pos) : 0);
    if (write_ptr) wr32(write_ptr, v ? write_cursor(v) : 0);
    ret(c, S_OK_, 3);
}
// (this, offset, bytes, &p1, &n1, &p2, &n2, flags): pointers into the data,
// wrapping around the end. Unlock has nothing to do (the guest stub stays).
void hle_buf_Lock(Ctx* c) {
    VOICE_ARG0();
    u32 offset = arg(c, 1), bytes = arg(c, 2), p1 = arg(c, 3), n1 = arg(c, 4), p2 = arg(c, 5), n2 = arg(c, 6);
    u32 flags = arg(c, 7);
    if (!v || !v->bytes) {
        ret(c, E_FAIL_, 8);
        return;
    }
    if (flags & DSBLOCK_FROMWRITECURSOR) offset = write_cursor(v);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = v->bytes;
    offset %= v->bytes;
    if (bytes > v->bytes) bytes = v->bytes;
    u32 first = v->bytes - offset < bytes ? v->bytes - offset : bytes;
    wr32(p1, v->data + offset);
    wr32(n1, first);
    if (p2) wr32(p2, first < bytes ? v->data : 0);
    if (n2) wr32(n2, bytes - first);
    ret(c, S_OK_, 8);
}

// ------------------------------------------------------------------ streams
// Entries of the XMediaObject vtable at STREAM_VTABLE.
void hle_stream_AddRef(Ctx* c) {
    VOICE_ARG0();
    if (v) v->refs++;
    ret(c, v ? v->refs : 0, 1);
}
void hle_stream_Release(Ctx* c) {
    VOICE_ARG0();
    ret(c, release(v), 1);
}
// XMEDIAINFO: flags, input size, output size, max lookahead.
void hle_stream_GetInfo(Ctx* c) {
    VOICE_ARG0();
    u32 info = arg(c, 1);
    wr32(info + 0, XMO_STREAMF_STREAM_INFO);
    wr32(info + 4, v ? v->fmt.block : 4);
    wr32(info + 8, 0);
    wr32(info + 12, STREAM_LOOKAHEAD);
    ret(c, S_OK_, 2);
}
static bool stream_has_room(Voice* v) { return v->waiting.size() + v->decoded.size() < v->max_packets; }
void hle_stream_GetStatus(Ctx* c) {
    VOICE_ARG0();
    wr32(arg(c, 1), v && stream_has_room(v) ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0);
    ret(c, S_OK_, 2);
}
// (this, input packet, output packet). XMEDIAPACKET: buffer, size,
// completed size pointer, status pointer, event/context, timestamp.
void hle_stream_Process(Ctx* c) {
    VOICE_ARG0();
    u32 in = arg(c, 1);
    if (!v || !in || !stream_has_room(v)) {
        ret(c, DSERR_STREAM_FULL, 3);
        return;
    }
    Packet p{rd32(in + 0), rd32(in + 4), rd32(in + 8), rd32(in + 12), 0};
    if (rd32(in + 16)) log_msg("[dsound] stream packet completion events are not supported");
    if (p.completed_ptr) wr32(p.completed_ptr, 0);
    if (p.status_ptr) wr32(p.status_ptr, XMEDIAPACKET_PENDING);
    v->waiting.push_back(p);
    ret(c, S_OK_, 3);
}
void hle_stream_Discontinuity(Ctx* c) { ret(c, S_OK_, 1); }  // starving is silent already
void hle_stream_Flush(Ctx* c) {
    VOICE_ARG0();
    if (v) {
        for (auto* q : {&v->decoded, &v->waiting}) {
            for (const Packet& p : *q) complete_packet(p, XMEDIAPACKET_FLUSHED, 0);
            q->clear();
        }
        v->fifo.clear();
        v->fifo_base = v->fifo_end = 0;
        v->pos = 0;
    }
    ret(c, S_OK_, 1);
}
