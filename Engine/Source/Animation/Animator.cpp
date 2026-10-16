#include "Animation/Animator.h"

#include "Animation/AnimationClip.h"
#include "Graphics/Model.h"

#include <cmath>
#include <exception>
#include <span>
#include <utility>

namespace
{
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

    Engine::Matrix Multiply(const Engine::Matrix& left, const Engine::Matrix& right)
    {
        Engine::Matrix result;
        DirectX::XMStoreFloat4x4(&result, DirectX::XMLoadFloat4x4(&left) * DirectX::XMLoadFloat4x4(&right));
        return result;
    }

    bool NodeOrder(std::span<const Engine::NodeData> nodes, std::vector<uint32_t>& out,
        std::string& error)
    {
        std::vector<bool> ready(nodes.size(), false);
        out.reserve(nodes.size());
        for (const Engine::NodeData& node : nodes)
        {
            if (node.parent < -1 || node.parent >= static_cast<int32_t>(nodes.size()) || !Affine(node.bindLocal))
            {
                error = "Animator model has invalid parent or bind transform";
                return false;
            }
        }
        while (out.size() < nodes.size())
        {
            const size_t previous = out.size();
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                const int32_t parent = nodes[index].parent;
                if (!ready[index] && (parent < 0 || ready[static_cast<size_t>(parent)]))
                {
                    ready[index] = true;
                    out.push_back(static_cast<uint32_t>(index));
                }
            }
            if (out.size() == previous)
            {
                error = "Animator model hierarchy contains a cycle";
                return false;
            }
        }
        return true;
    }

    bool BuildPose(const Engine::Model& model, const Engine::AnimationClip* clip, double time,
        std::span<const uint32_t> order, std::shared_ptr<const Engine::AnimationPose>& out,
        std::string& error)
    {
        const auto nodes = model.Nodes();
        std::vector<Engine::Matrix> local(nodes.size());
        if (clip)
        {
            if (!clip->Sample(time, nodes, local, error))
            {
                return false;
            }
        }
        else
        {
            for (size_t index = 0; index < nodes.size(); ++index)
            {
                local[index] = nodes[index].bindLocal;
            }
        }
        auto pose = std::make_shared<Engine::AnimationPose>();
        pose->skeletonSignature = model.SkeletonSignature();
        pose->nodeWorld.resize(nodes.size());
        for (const uint32_t index : order)
        {
            const int32_t parent = nodes[index].parent;
            pose->nodeWorld[index] = parent < 0 ? local[index]
                : Multiply(local[index], pose->nodeWorld[static_cast<size_t>(parent)]);
            if (!Affine(pose->nodeWorld[index]))
            {
                error = "Animator hierarchy produces a nonfinite or singular pose";
                return false;
            }
        }
        const auto meshes = model.Meshes();
        pose->meshPalettes.resize(meshes.size());
        for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
        {
            const Engine::MeshGpu& mesh = meshes[meshIndex];
            if (mesh.nodeIndex >= nodes.size())
            {
                error = "Animator mesh node is outside the model hierarchy";
                return false;
            }
            if (!mesh.skin)
            {
                continue;
            }
            const Engine::SkinBinding& skin = *mesh.skin;
            if (skin.jointNodeIndices.empty() || skin.jointNodeIndices.size() > Engine::kMaxSkinJoints ||
                skin.inverseBind.size() != skin.jointNodeIndices.size())
            {
                error = "Animator requires a matching bounded per-mesh skin binding";
                return false;
            }
            auto& palette = pose->meshPalettes[meshIndex];
            palette.reserve(skin.jointNodeIndices.size());
            for (size_t joint = 0; joint < skin.jointNodeIndices.size(); ++joint)
            {
                const uint32_t nodeIndex = skin.jointNodeIndices[joint];
                if (nodeIndex >= nodes.size() || !Affine(skin.inverseBind[joint]))
                {
                    error = "Animator skin joint or inverse bind is invalid";
                    return false;
                }
                // The product already ends in model-root space; the Renderer adds item.world once.
                Engine::Matrix matrix = Multiply(skin.inverseBind[joint], pose->nodeWorld[nodeIndex]);
                if (!Affine(matrix))
                {
                    error = "Animator skin palette is nonfinite or singular";
                    return false;
                }
                palette.push_back(matrix);
            }
        }
        out = std::move(pose);
        return true;
    }
}

namespace Engine
{
    bool Animator::SetModel(std::shared_ptr<const Model> model, std::string& error)
    {
        error.clear();
        if (!model || model->Nodes().empty() || model->Nodes().size() > 4096 ||
            model->Meshes().empty() || model->SkeletonSignature().size() != 64)
        {
            error = "Animator requires an uploaded model with hierarchy, meshes and skeleton signature";
            return false;
        }
        try
        {
            std::vector<uint32_t> order;
            std::shared_ptr<const AnimationPose> pose;
            if (!NodeOrder(model->Nodes(), order, error) ||
                !BuildPose(*model, nullptr, 0.0, order, pose, error))
            {
                return false;
            }
            _model = std::move(model);
            _clip.reset();
            _nodeOrder = std::move(order);
            _pose = std::move(pose);
            _mode = PlaybackMode::Loop;
            _timeSeconds = 0.0;
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Animator model preparation failed: ") + exception.what();
            return false;
        }
    }

    bool Animator::SetClip(std::shared_ptr<const AnimationClip> clip, PlaybackMode mode,
        std::string& error)
    {
        error.clear();
        if (!_model || !clip || (mode != PlaybackMode::Loop && mode != PlaybackMode::Clamp) ||
            clip->Data().nodeCount != _model->Nodes().size() ||
            clip->Data().skeletonSignature != _model->SkeletonSignature() ||
            !std::isfinite(clip->Data().durationSeconds) || clip->Data().durationSeconds <= 0.0)
        {
            error = "Animator clip is missing, uninitialized or incompatible with the model";
            return false;
        }
        try
        {
            std::shared_ptr<const AnimationPose> pose;
            if (!BuildPose(*_model, clip.get(), 0.0, _nodeOrder, pose, error))
            {
                return false;
            }
            _clip = std::move(clip);
            _mode = mode;
            _timeSeconds = 0.0;
            _pose = std::move(pose);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Animator clip preparation failed: ") + exception.what();
            return false;
        }
    }

    bool Animator::Advance(double dt, std::string& error)
    {
        error.clear();
        if (!_model || !_clip || !_pose || !std::isfinite(dt) || dt < 0.0 ||
            _clip->Data().nodeCount != _model->Nodes().size() ||
            _clip->Data().skeletonSignature != _model->SkeletonSignature())
        {
            error = "Animator advance requires compatible prepared state and finite nonnegative seconds";
            return false;
        }
        const double duration = _clip->Data().durationSeconds;
        if (!std::isfinite(duration) || duration <= 0.0 || duration > 1000000.0 ||
            !std::isfinite(_timeSeconds) || _timeSeconds < 0.0 || _timeSeconds > duration)
        {
            error = "Animator playback duration or current time is invalid";
            return false;
        }
        double nextTime = _timeSeconds;
        if (_mode == PlaybackMode::Clamp)
        {
            nextTime = dt >= duration - _timeSeconds ? duration : _timeSeconds + dt;
        }
        else
        {
            const double remainder = std::fmod(dt, duration);
            const double remaining = duration - _timeSeconds;
            nextTime = remainder >= remaining ? remainder - remaining : _timeSeconds + remainder;
            if (nextTime >= duration)
            {
                nextTime = 0.0;
            }
        }
        if (nextTime == _timeSeconds)
        {
            return true;
        }
        try
        {
            std::shared_ptr<const AnimationPose> pose;
            if (!BuildPose(*_model, _clip.get(), nextTime, _nodeOrder, pose, error))
            {
                return false;
            }
            _timeSeconds = nextTime;
            _pose = std::move(pose);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Animator advance failed: ") + exception.what();
            return false;
        }
    }

    double Animator::TimeSeconds() const noexcept
    {
        return _timeSeconds;
    }

    std::shared_ptr<const AnimationPose> Animator::Pose() const noexcept
    {
        return _pose;
    }

    void Animator::Reset() noexcept
    {
        _clip.reset();
        _model.reset();
        _pose.reset();
        _nodeOrder.clear();
        _mode = PlaybackMode::Loop;
        _timeSeconds = 0.0;
    }
}
