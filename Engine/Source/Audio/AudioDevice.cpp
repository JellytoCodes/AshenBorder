#include "Audio/AudioDevice.h"

#include "Audio/AudioClip.h"
#include "Core/Log.h"

#include <objbase.h>

#include <cmath>
#include <exception>
#include <utility>

namespace Engine
{
    AudioDevice::~AudioDevice()
    {
        Shutdown();
    }

    void AudioDevice::UseSilent(std::string_view reason, long code)
    {
        if (_master)
        {
            _master->DestroyVoice();
            _master = nullptr;
        }
        _engine.Reset();
        _silent = true;
        _silentReason = reason;
        Log::Error(reason, code);
        Log::Write("Audio is using silent fallback; no audible output is claimed.");
    }

    bool AudioDevice::Initialize(uint32_t maxEffectVoices, bool forceSilent)
    {
        if (_initialized || maxEffectVoices == 0 || maxEffectVoices > 64)
        {
            Log::Write("Audio initialization requires an unused device and 1..64 effect voices.");
            return false;
        }
        try
        {
            _effects.resize(maxEffectVoices);
            _silentReason.reserve(128);
        }
        catch (const std::exception&)
        {
            _effects.clear();
            Log::Write("Audio voice storage allocation failed.");
            return false;
        }
        _initialized = true;
        if (forceSilent)
        {
            _silent = true;
            _silentReason = "Forced silent mode";
            Log::Write("Audio initialized in forced silent mode; hardware playback was not attempted.");
            return true;
        }
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        _ownsCom = SUCCEEDED(comResult);
        if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        {
            UseSilent("Audio COM initialization failed", comResult);
            return true;
        }
        HRESULT result = XAudio2Create(_engine.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result))
        {
            UseSilent("XAudio2Create failed", result);
            return true;
        }
        result = _engine->CreateMasteringVoice(&_master);
        if (FAILED(result))
        {
            UseSilent("XAudio2 mastering voice unavailable", result);
            return true;
        }
        Log::Write("XAudio2 hardware output initialized; successful submission is not a listening test.");
        return true;
    }

    bool AudioDevice::PrepareVoice(std::shared_ptr<const AudioClip> clip, bool loop, float volume, Voice& out)
    {
        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = clip->Channels();
        format.nSamplesPerSec = clip->SampleRate();
        format.wBitsPerSample = clip->BitsPerSample();
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        IXAudio2SourceVoice* source = nullptr;
        HRESULT result = _engine->CreateSourceVoice(&source, &format);
        if (FAILED(result))
        {
            Log::Error("Create audio source voice", result);
            return false;
        }
        Voice candidate{source, std::move(clip)};
        XAUDIO2_BUFFER buffer = {};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(candidate.clip->Samples().size());
        buffer.pAudioData = candidate.clip->Samples().data();
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
        result = source->SetVolume(volume);
        if (SUCCEEDED(result))
        {
            result = source->SubmitSourceBuffer(&buffer);
        }
        if (SUCCEEDED(result))
        {
            // StopEngine pauses processing globally, while preserving submitted
            // queues and source-voice Start state for later StartEngine.
            result = source->Start();
        }
        if (FAILED(result))
        {
            Log::Error("Submit/start PCM audio", result);
            Destroy(candidate);
            return false;
        }
        out = std::move(candidate);
        return true;
    }

    bool AudioDevice::PlayMusic(std::shared_ptr<const AudioClip> clip, bool loop, float volume)
    {
        if (!_initialized || !clip || !std::isfinite(volume) || volume < 0.0f || volume > 1.0f)
        {
            return false;
        }
        if (_silent)
        {
            return true;
        }
        Voice next;
        if (!PrepareVoice(std::move(clip), loop, volume, next))
        {
            return false;
        }
        Destroy(_music);
        _music = std::move(next);
        return true;
    }

    bool AudioDevice::PlayEffect(std::shared_ptr<const AudioClip> clip, float volume)
    {
        if (!_initialized || !clip || !std::isfinite(volume) || volume < 0.0f || volume > 1.0f)
        {
            return false;
        }
        if (_silent)
        {
            return true;
        }
        Update();
        for (Voice& voice : _effects)
        {
            if (!voice.source)
            {
                return PrepareVoice(std::move(clip), false, volume, voice);
            }
        }
        // Bounded polyphony: discard this request, never steal music/another
        // source or grow the caller-selected effect voice budget.
        return false;
    }

    void AudioDevice::Destroy(Voice& voice) noexcept
    {
        if (voice.source)
        {
            // DestroyVoice waits for audio processing to stop referencing PCM.
            voice.source->DestroyVoice();
            voice.source = nullptr;
        }
        voice.clip.reset();
    }

    void AudioDevice::Update()
    {
        const auto completed = [](Voice& voice)
        {
            if (voice.source)
            {
                XAUDIO2_VOICE_STATE state = {};
                voice.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (state.BuffersQueued == 0)
                {
                    Destroy(voice);
                }
            }
        };
        completed(_music);
        for (Voice& voice : _effects)
        {
            completed(voice);
        }
    }

    void AudioDevice::StopEffects() noexcept
    {
        for (Voice& voice : _effects)
        {
            Destroy(voice);
        }
    }

    void AudioDevice::StopAll() noexcept
    {
        StopEffects();
        Destroy(_music);
    }

    bool AudioDevice::SetMuted(bool muted)
    {
        if (!_initialized)
        {
            return false;
        }
        if (_master)
        {
            const HRESULT result = _master->SetVolume(muted ? 0.0f : 1.0f);
            if (FAILED(result))
            {
                Log::Error("Set master audio mute", result);
                return false;
            }
        }
        _muted = muted;
        return true;
    }

    bool AudioDevice::SetPaused(bool paused)
    {
        if (!_initialized)
        {
            return false;
        }
        if (paused == _paused)
        {
            return true;
        }
        if (_engine)
        {
            if (paused)
            {
                _engine->StopEngine();
            }
            else
            {
                const HRESULT result = _engine->StartEngine();
                if (FAILED(result))
                {
                    Log::Error("Resume XAudio2", result);
                    return false;
                }
            }
        }
        _paused = paused;
        return true;
    }

    void AudioDevice::Shutdown() noexcept
    {
        StopAll();
        if (_master)
        {
            _master->DestroyVoice();
            _master = nullptr;
        }
        _engine.Reset();
        if (_ownsCom)
        {
            CoUninitialize();
            _ownsCom = false;
        }
        _effects.clear();
        _silentReason.clear();
        _initialized = false;
        _silent = false;
        _muted = false;
        _paused = false;
    }

    bool AudioDevice::IsInitialized() const noexcept
    {
        return _initialized;
    }

    bool AudioDevice::IsSilent() const noexcept
    {
        return _silent;
    }

    bool AudioDevice::IsMuted() const noexcept
    {
        return _muted;
    }

    bool AudioDevice::IsPaused() const noexcept
    {
        return _paused;
    }

    uint32_t AudioDevice::ActiveEffectCount() const noexcept
    {
        uint32_t count = 0;
        for (const Voice& voice : _effects)
        {
            if (voice.source)
            {
                XAUDIO2_VOICE_STATE state = {};
                voice.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (state.BuffersQueued != 0)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    std::string_view AudioDevice::SilentReason() const noexcept
    {
        return _silentReason;
    }
}
