#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Engine
{
    // Immutable, fully resident RIFF/WAVE PCM: mono/stereo, 8/16-bit,
    // 8000..192000 Hz. Compressed and extensible WAVE are not accepted.
    class AudioClip
    {
    public:
        AudioClip(const AudioClip&) = delete;
        AudioClip& operator=(const AudioClip&) = delete;

        static std::shared_ptr<const AudioClip> LoadWav(
            const std::filesystem::path& file, std::string& error);

        uint32_t SampleRate() const noexcept;
        uint16_t Channels() const noexcept;
        uint16_t BitsPerSample() const noexcept;
        std::span<const uint8_t> Samples() const noexcept;
        double DurationSeconds() const noexcept;

    private:
        AudioClip() = default;

        uint32_t _sampleRate = 0;
        uint16_t _channels = 0;
        uint16_t _bitsPerSample = 0;
        std::vector<uint8_t> _samples;
    };
}
