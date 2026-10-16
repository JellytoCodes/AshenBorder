#pragma once

#include <xaudio2.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    class AudioClip;

    // Single caller thread, including Initialize/Shutdown (COM ownership).
    // Source voices retain immutable PCM until DestroyVoice has completed.
    class AudioDevice
    {
    public:
        AudioDevice() = default;
        ~AudioDevice();
        AudioDevice(const AudioDevice&) = delete;
        AudioDevice& operator=(const AudioDevice&) = delete;
        AudioDevice(AudioDevice&&) = delete;
        AudioDevice& operator=(AudioDevice&&) = delete;

        bool Initialize(uint32_t maxEffectVoices = 16, bool forceSilent = false);
        bool PlayMusic(std::shared_ptr<const AudioClip> clip, bool loop = true, float volume = 1.0f);
        bool PlayEffect(std::shared_ptr<const AudioClip> clip, float volume = 1.0f);
        void Update();
        void StopEffects() noexcept;
        void StopAll() noexcept;
        bool SetMuted(bool muted);
        bool SetPaused(bool paused);
        void Shutdown() noexcept;

        bool IsInitialized() const noexcept;
        bool IsSilent() const noexcept;
        bool IsMuted() const noexcept;
        bool IsPaused() const noexcept;
        uint32_t ActiveEffectCount() const noexcept;
        std::string_view SilentReason() const noexcept;

    private:
        struct Voice
        {
            IXAudio2SourceVoice* source = nullptr;
            std::shared_ptr<const AudioClip> clip;
        };

        bool PrepareVoice(std::shared_ptr<const AudioClip> clip, bool loop, float volume, Voice& out);
        static void Destroy(Voice& voice) noexcept;
        void UseSilent(std::string_view reason, long code);

        Microsoft::WRL::ComPtr<IXAudio2> _engine;
        IXAudio2MasteringVoice* _master = nullptr;
        Voice _music;
        std::vector<Voice> _effects;
        std::string _silentReason;
        bool _initialized = false;
        bool _silent = false;
        bool _muted = false;
        bool _paused = false;
        bool _ownsCom = false;
    };
}
