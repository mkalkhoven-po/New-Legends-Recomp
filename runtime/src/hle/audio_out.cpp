// Audio output: pulls the mixer from a host thread into a WASAPI shared-mode
// stream. Without a usable device the mixer still runs on a timer, because
// the game depends on voices and stream packets making progress.
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <vector>

#include "audio.h"

constexpr u32 NULL_BLOCK_FRAMES = AUDIO_RATE / 100;
constexpr REFERENCE_TIME BUFFER_100NS = 400000;  // 40 ms
constexpr DWORD DEVICE_RETRY_MS = 2000;

static std::atomic<bool> g_stop{false};
static FILE* g_dump;

// NL_AUDIO_DUMP=path: also write the mix as raw 48 kHz stereo s16.
static void dump(const float* in, u32 frames) {
    if (!g_dump) return;
    std::vector<s16> pcm(frames * 2);
    for (u32 i = 0; i < frames * 2; i++) {
        float x = in[i] * 32767.0f;
        pcm[i] = (s16)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
    fwrite(pcm.data(), 2, pcm.size(), g_dump);
    fflush(g_dump);
}

template <class T>
static void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Play until the device goes away. Returns false if it could not start.
static bool run_device() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool started = false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = AUDIO_RATE * 8;
    UINT32 size = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) goto done;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) goto done;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) goto done;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  BUFFER_100NS, 0, &wf, nullptr)))
        goto done;
    client->SetEventHandle(ev);
    client->GetBufferSize(&size);
    if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) goto done;
    if (FAILED(client->Start())) goto done;
    started = true;
    log_msg("audio: WASAPI stream started, %u frames buffered", size);
    while (!g_stop) {
        WaitForSingleObject(ev, 100);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        UINT32 n = size - padding;
        if (!n) continue;
        BYTE* buf;
        if (FAILED(render->GetBuffer(n, &buf))) break;
        dsound_mix((float*)buf, n);
        dump((float*)buf, n);
        if (FAILED(render->ReleaseBuffer(n, 0))) break;
    }
    client->Stop();
done:
    release(render);
    release(client);
    release(dev);
    release(en);
    CloseHandle(ev);
    return started;
}

// Keep the mixer running in real time with nowhere to send the sound.
static void run_null(DWORD ms) {
    static std::vector<float> block(NULL_BLOCK_FRAMES * 2);
    for (DWORD t = 0; t < ms && !g_stop; t += 10) {
        Sleep(10);
        dsound_mix(block.data(), NULL_BLOCK_FRAMES);
        dump(block.data(), NULL_BLOCK_FRAMES);
    }
}

static DWORD WINAPI audio_thread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (const char* path = getenv("NL_AUDIO_DUMP")) g_dump = fopen(path, "wb");
    while (!g_stop) {
        if (!run_device() && !g_stop) log_msg("audio: no output device, running silently");
        if (!g_stop) run_null(DEVICE_RETRY_MS);
    }
    return 0;
}

void audio_out_start() { CloseHandle(CreateThread(nullptr, 0, audio_thread, nullptr, 0, nullptr)); }
void audio_out_stop() { g_stop = true; }
