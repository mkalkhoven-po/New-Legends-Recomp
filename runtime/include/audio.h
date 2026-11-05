// Host audio: the DirectSound replacement mixes, audio_out.cpp plays it.
#pragma once
#include "runtime.h"

constexpr u32 AUDIO_RATE = 48000;

// Fill `frames` of interleaved stereo float samples (hle/dsound.cpp).
void dsound_mix(float* out, u32 frames);

void audio_out_start();
// Stop producing sound (the process is being replaced on a quick reboot).
void audio_out_stop();
