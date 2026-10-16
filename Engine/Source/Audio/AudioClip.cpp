#include "Audio/AudioClip.h"

#include <cstring>
#include <exception>
#include <fstream>

namespace
{
    uint16_t Read16(const uint8_t* bytes) noexcept
    {
        return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
    }

    uint32_t Read32(const uint8_t* bytes) noexcept
    {
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    }
}

namespace Engine
{
    std::shared_ptr<const AudioClip> AudioClip::LoadWav(const std::filesystem::path& file, std::string& error)
    {
        try
        {
            std::ifstream stream(file, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                error = "Cannot open PCM WAV file.";
                return nullptr;
            }
            const auto length = stream.tellg();
            if (length < 44 || length > 64 * 1024 * 1024)
            {
                error = "WAV file must contain 44 bytes to 64 MiB.";
                return nullptr;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(length));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
                std::memcmp(bytes.data() + 8, "WAVE", 4) != 0 ||
                static_cast<uint64_t>(Read32(bytes.data() + 4)) + 8 != bytes.size())
            {
                error = "WAV RIFF/WAVE header or declared file length is invalid.";
                return nullptr;
            }

            uint32_t sampleRate = 0;
            uint32_t averageBytes = 0;
            uint16_t channels = 0;
            uint16_t bits = 0;
            uint16_t blockAlign = 0;
            bool hasFormat = false;
            bool hasData = false;
            size_t dataOffset = 0;
            size_t dataSize = 0;
            size_t offset = 12;
            while (offset < bytes.size())
            {
                if (bytes.size() - offset < 8)
                {
                    error = "WAV chunk header is truncated.";
                    return nullptr;
                }
                const uint8_t* header = bytes.data() + offset;
                const uint32_t chunkSize = Read32(header + 4);
                offset += 8;
                const uint64_t padded = static_cast<uint64_t>(chunkSize) + (chunkSize & 1u);
                if (padded > bytes.size() - offset)
                {
                    error = "WAV chunk data or odd-byte padding is truncated.";
                    return nullptr;
                }
                if (std::memcmp(header, "fmt ", 4) == 0)
                {
                    if (hasFormat || (chunkSize != 16 && chunkSize != 18))
                    {
                        error = "WAV requires one 16/18-byte PCM format chunk.";
                        return nullptr;
                    }
                    const uint8_t* format = bytes.data() + offset;
                    const uint16_t tag = Read16(format);
                    channels = Read16(format + 2);
                    sampleRate = Read32(format + 4);
                    averageBytes = Read32(format + 8);
                    blockAlign = Read16(format + 12);
                    bits = Read16(format + 14);
                    if (tag != 1 || channels < 1 || channels > 2 || sampleRate < 8000 ||
                        sampleRate > 192000 || (bits != 8 && bits != 16) ||
                        (chunkSize == 18 && Read16(format + 16) != 0))
                    {
                        error = "Only mono/stereo 8/16-bit PCM at 8000..192000 Hz is supported.";
                        return nullptr;
                    }
                    hasFormat = true;
                }
                else if (std::memcmp(header, "data", 4) == 0)
                {
                    if (hasData || chunkSize == 0)
                    {
                        error = "WAV requires one non-empty data chunk.";
                        return nullptr;
                    }
                    hasData = true;
                    dataOffset = offset;
                    dataSize = chunkSize;
                }
                offset += static_cast<size_t>(padded);
            }
            const uint32_t expectedAlign = static_cast<uint32_t>(channels) * bits / 8;
            if (!hasFormat || !hasData || expectedAlign == 0 || blockAlign != expectedAlign ||
                averageBytes != sampleRate * expectedAlign || dataSize % expectedAlign != 0)
            {
                error = "WAV format/data chunks, byte rate, or sample alignment are invalid.";
                return nullptr;
            }
            std::shared_ptr<AudioClip> clip(new AudioClip());
            clip->_sampleRate = sampleRate;
            clip->_channels = channels;
            clip->_bitsPerSample = bits;
            clip->_samples.assign(bytes.begin() + dataOffset, bytes.begin() + dataOffset + dataSize);
            error.clear();
            return clip;
        }
        catch (const std::exception& exception)
        {
            error = std::string("WAV load failed: ") + exception.what();
            return nullptr;
        }
    }

    uint32_t AudioClip::SampleRate() const noexcept
    {
        return _sampleRate;
    }

    uint16_t AudioClip::Channels() const noexcept
    {
        return _channels;
    }

    uint16_t AudioClip::BitsPerSample() const noexcept
    {
        return _bitsPerSample;
    }

    std::span<const uint8_t> AudioClip::Samples() const noexcept
    {
        return _samples;
    }

    double AudioClip::DurationSeconds() const noexcept
    {
        const uint32_t bytesPerFrame = static_cast<uint32_t>(_channels) * _bitsPerSample / 8;
        return bytesPerFrame == 0 || _sampleRate == 0 ? 0.0 :
            static_cast<double>(_samples.size()) / bytesPerFrame / _sampleRate;
    }
}
