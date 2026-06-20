#pragma once
// Precompiled header for NeuronAudio (client-only lib; ERHeadless links none).
//
// API entry points used by this library (XAudio2 2.9 + X3DAudio, Windows SDK
// 10.0.26100; for a Windows reviewer to sanity-check the blind-written .cpp):
//   XAudio2Create · IXAudio2::CreateMasteringVoice/CreateSubmixVoice/
//   CreateSourceVoice/StartEngine/StopEngine · IXAudio2MasteringVoice::
//   GetVoiceDetails/GetChannelMask · IXAudio2Voice::SetVolume/DestroyVoice/
//   SetOutputVoices · IXAudio2SourceVoice::SubmitSourceBuffer/Start/Stop/
//   FlushSourceBuffers/GetState/SetFrequencyRatio/SetOutputMatrix ·
//   X3DAudioInitialize · X3DAudioCalculate.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <winrt/base.h>

#include <xaudio2.h>
#include <x3daudio.h>

#include <DirectXMath.h>
