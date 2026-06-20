// NeuronAudio.cpp — single compile-unit stub for the NeuronAudio static library.
//
// All audio classes are compiled via their own .cpp files listed in
// NeuronAudio.vcxproj; this file documents what is in the library and validates
// that the headers compile together.
//
// Client-only (UWP). ERHeadless/NeuronClient do NOT link this library — event
// sounds are client-side feedback off replicated sim state (no audio on the
// wire, no determinism requirement). XAudio2 (2.9) + X3DAudio, Windows SDK only.
//
// Audio classes (files flat in the project; VS Filters group them):
//   engine/   AudioEngine        — front door: init, clip registry, playback
//   mixer/    Mixer              — mastering voice + four submix buses
//   spatial/  Spatializer        — X3DAudio listener/emitter (3D from day one)
//             SpatialMath        — device-free distance/Doppler model (tested)
//   wav/      WavReader/WavClip   — RIFF/WAVE PCM-16 (wraps NeuronTools WavParse)
//   (handles) VoicePool/VoiceId   — generation-checked pooled source voices

#include "pch.h"

#include "AudioEngine.h"
#include "Mixer.h"
#include "SpatialMath.h"
#include "Spatializer.h"
#include "VoiceHandle.h"
#include "VoicePool.h"
#include "WavReader.h"
