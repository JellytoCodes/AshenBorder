#include "Animation/AnimationClip.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace
{
    constexpr uint32_t kMaximumNodes = 4096;
    constexpr size_t kMaximumKeys = 1000000;

    bool Finite(Engine::Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool Normalize(Engine::Quaternion& value)
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || !std::isfinite(value.w))
        {
            return false;
        }
        const double x = value.x;
        const double y = value.y;
        const double z = value.z;
        const double w = value.w;
        const double length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length == 0.0)
        {
            return false;
        }
        value = {static_cast<float>(x / length), static_cast<float>(y / length),
            static_cast<float>(z / length), static_cast<float>(w / length)};
        return true;
    }

    bool Affine(const Engine::Matrix& matrix)
    {
        for (const auto& row : matrix.m)
        {
            for (const float value : row)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
        }
        const double determinant =
            static_cast<double>(matrix._11) * (static_cast<double>(matrix._22) * matrix._33 - static_cast<double>(matrix._23) * matrix._32) -
            static_cast<double>(matrix._12) * (static_cast<double>(matrix._21) * matrix._33 - static_cast<double>(matrix._23) * matrix._31) +
            static_cast<double>(matrix._13) * (static_cast<double>(matrix._21) * matrix._32 - static_cast<double>(matrix._22) * matrix._31);
        return matrix._14 == 0.0f && matrix._24 == 0.0f && matrix._34 == 0.0f && matrix._44 == 1.0f &&
            determinant != 0.0 && std::isfinite(determinant);
    }

    Engine::Matrix Compose(Engine::Vector3 translation, Engine::Quaternion rotation,
        Engine::Vector3 scale)
    {
        Engine::Matrix matrix;
        DirectX::XMStoreFloat4x4(&matrix,
            DirectX::XMMatrixScaling(scale.x, scale.y, scale.z) *
            DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&rotation)) *
            DirectX::XMMatrixTranslation(translation.x, translation.y, translation.z));
        return matrix;
    }

    bool BindTRS(const Engine::Matrix& bind, Engine::Vector3& translation,
        Engine::Quaternion& rotation, Engine::Vector3& scale)
    {
        DirectX::XMVECTOR t;
        DirectX::XMVECTOR r;
        DirectX::XMVECTOR s;
        if (!DirectX::XMMatrixDecompose(&s, &r, &t, DirectX::XMLoadFloat4x4(&bind)))
        {
            return false;
        }
        DirectX::XMStoreFloat3(&translation, t);
        DirectX::XMStoreFloat4(&rotation, r);
        DirectX::XMStoreFloat3(&scale, s);
        if (!Finite(translation) || !Finite(scale) || !Normalize(rotation))
        {
            return false;
        }
        const Engine::Matrix rebuilt = Compose(translation, rotation, scale);
        if (!Affine(rebuilt))
        {
            return false;
        }
        for (size_t row = 0; row < 3; ++row)
        {
            double magnitude = 0.0;
            for (size_t column = 0; column < 3; ++column)
            {
                magnitude = std::max(magnitude, std::abs(static_cast<double>(bind.m[row][column])));
            }
            for (size_t column = 0; column < 3; ++column)
            {
                if (std::abs(static_cast<double>(rebuilt.m[row][column]) - bind.m[row][column]) > magnitude * 0.00001)
                {
                    return false;
                }
            }
        }
        return rebuilt._41 == bind._41 && rebuilt._42 == bind._42 && rebuilt._43 == bind._43;
    }

    template <typename Key>
    bool Times(std::span<const Key> keys, double duration)
    {
        double previous = -1.0;
        for (const Key& key : keys)
        {
            if (!std::isfinite(key.timeSeconds) || key.timeSeconds < 0.0 ||
                key.timeSeconds > duration || key.timeSeconds <= previous)
            {
                return false;
            }
            previous = key.timeSeconds;
        }
        return true;
    }

    Engine::Vector3 SampleVector(const std::vector<Engine::VectorKey>& keys,
        double time, Engine::Vector3 bind)
    {
        if (keys.empty())
        {
            return bind;
        }
        if (time <= keys.front().timeSeconds)
        {
            return keys.front().value;
        }
        if (time >= keys.back().timeSeconds)
        {
            return keys.back().value;
        }
        const auto next = std::upper_bound(keys.begin(), keys.end(), time,
            [](double value, const Engine::VectorKey& key)
            {
                return value < key.timeSeconds;
            });
        const auto& before = *(next - 1);
        const double fraction = (time - before.timeSeconds) / (next->timeSeconds - before.timeSeconds);
        const double complement = 1.0 - fraction;
        return {static_cast<float>(complement * before.value.x + fraction * next->value.x),
            static_cast<float>(complement * before.value.y + fraction * next->value.y),
            static_cast<float>(complement * before.value.z + fraction * next->value.z)};
    }

    Engine::Quaternion SampleRotation(const std::vector<Engine::RotationKey>& keys,
        double time, Engine::Quaternion bind)
    {
        if (keys.empty())
        {
            return bind;
        }
        if (time <= keys.front().timeSeconds)
        {
            return keys.front().value;
        }
        if (time >= keys.back().timeSeconds)
        {
            return keys.back().value;
        }
        const auto next = std::upper_bound(keys.begin(), keys.end(), time,
            [](double value, const Engine::RotationKey& key)
            {
                return value < key.timeSeconds;
            });
        const auto& before = *(next - 1);
        const float fraction = static_cast<float>((time - before.timeSeconds) /
            (next->timeSeconds - before.timeSeconds));
        Engine::Quaternion result;
        DirectX::XMStoreFloat4(&result, DirectX::XMQuaternionSlerp(
            DirectX::XMLoadFloat4(&before.value), DirectX::XMLoadFloat4(&next->value), fraction));
        Normalize(result);
        return result;
    }
}

namespace Engine
{
    bool AnimationClip::Initialize(const ClipData& data, std::string& error)
    {
        error.clear();
        if (data.name.empty() || data.name.size() > 4096 || data.sourceTake.empty() ||
            data.sourceTake.size() > 4096 || data.nodeCount == 0 || data.nodeCount > kMaximumNodes ||
            !std::isfinite(data.durationSeconds) || data.durationSeconds <= 0.0 || data.durationSeconds > 1000000.0 ||
            data.channels.size() > data.nodeCount || data.skeletonSignature.size() != 64 ||
            !std::all_of(data.skeletonSignature.begin(), data.skeletonSignature.end(), [](char value)
            {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            }))
        {
            error = "Animation clip requires bounded names/nodes, a SHA-256 signature and positive finite seconds";
            return false;
        }
        std::array<bool, kMaximumNodes> used = {};
        size_t totalKeys = 0;
        for (const TRSChannel& channel : data.channels)
        {
            if (channel.nodeIndex >= data.nodeCount || used[channel.nodeIndex] ||
                (channel.translations.empty() && channel.scales.empty() && channel.rotations.empty()))
            {
                error = "Animation channels require distinct valid nodes and at least one track";
                return false;
            }
            used[channel.nodeIndex] = true;
            for (const size_t count : {channel.translations.size(), channel.scales.size(), channel.rotations.size()})
            {
                if (count > kMaximumKeys - totalKeys)
                {
                    error = "Animation clip exceeds one million total keys";
                    return false;
                }
                totalKeys += count;
            }
            if (!Times<VectorKey>(channel.translations, data.durationSeconds) ||
                !Times<VectorKey>(channel.scales, data.durationSeconds) ||
                !Times<RotationKey>(channel.rotations, data.durationSeconds))
            {
                error = "Animation key times must be finite, strictly increasing and within the clip duration";
                return false;
            }
            for (const VectorKey& key : channel.translations)
            {
                if (!Finite(key.value))
                {
                    error = "Animation translation key is nonfinite";
                    return false;
                }
            }
            for (const VectorKey& key : channel.scales)
            {
                if (!Finite(key.value) || key.value.x <= 0.0f || key.value.y <= 0.0f || key.value.z <= 0.0f)
                {
                    error = "Animation scale keys must have finite positive components";
                    return false;
                }
            }
            for (const RotationKey& key : channel.rotations)
            {
                Quaternion rotation = key.value;
                if (!Normalize(rotation))
                {
                    error = "Animation rotation key is nonfinite or zero";
                    return false;
                }
            }
        }
        try
        {
            ClipData prepared = data;
            for (TRSChannel& channel : prepared.channels)
            {
                for (RotationKey& key : channel.rotations)
                {
                    Normalize(key.value);
                }
            }
            _data = std::move(prepared);
            _initialized = true;
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Animation clip initialization failed: ") + exception.what();
            return false;
        }
    }

    bool AnimationClip::Sample(double timeSeconds, std::span<const NodeData> nodes,
        std::span<Matrix> out, std::string& error) const
    {
        error.clear();
        if (!_initialized || !std::isfinite(timeSeconds) || timeSeconds < 0.0 ||
            nodes.size() != _data.nodeCount || out.size() != nodes.size())
        {
            error = "Animation sampling requires initialized data, finite nonnegative time and matching node/output counts";
            return false;
        }
        try
        {
            std::vector<Matrix> prepared;
            prepared.reserve(nodes.size());
            for (const NodeData& node : nodes)
            {
                if (!Affine(node.bindLocal))
                {
                    error = "Animation bind transform is nonfinite, non-affine or singular";
                    return false;
                }
                prepared.push_back(node.bindLocal);
            }
            const double time = std::min(timeSeconds, _data.durationSeconds);
            for (const TRSChannel& channel : _data.channels)
            {
                Vector3 translation;
                Quaternion rotation;
                Vector3 scale;
                if (!BindTRS(nodes[channel.nodeIndex].bindLocal, translation, rotation, scale))
                {
                    error = "Animated bind transform cannot be reconstructed as TRS; animated shear is unsupported";
                    return false;
                }
                translation = SampleVector(channel.translations, time, translation);
                scale = SampleVector(channel.scales, time, scale);
                rotation = SampleRotation(channel.rotations, time, rotation);
                if (!Finite(translation) || !Finite(scale) || scale.x <= 0.0f ||
                    scale.y <= 0.0f || scale.z <= 0.0f || !Normalize(rotation))
                {
                    error = "Animation sample produced an invalid TRS or nonpositive scale";
                    return false;
                }
                prepared[channel.nodeIndex] = Compose(translation, rotation, scale);
                if (!Affine(prepared[channel.nodeIndex]))
                {
                    error = "Animation sampled transform is nonfinite or singular";
                    return false;
                }
            }
            std::copy(prepared.begin(), prepared.end(), out.begin());
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Animation sampling failed: ") + exception.what();
            return false;
        }
    }

    const ClipData& AnimationClip::Data() const noexcept
    {
        return _data;
    }
}
