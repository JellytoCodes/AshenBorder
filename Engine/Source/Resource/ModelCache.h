#pragma once

#include "Graphics/ModelData.h"

#include <filesystem>
#include <string>

namespace Engine
{
    struct ClipData;

    // Validates the complete immutable bundle before publishing out.
    bool ReadModelCache(const std::filesystem::path& resourceRoot,
        const std::filesystem::path& relativeFile, ModelData& out, std::string& error);
    bool ReadClipCache(const std::filesystem::path& resourceRoot,
        const std::filesystem::path& relativeFile, ClipData& out, std::string& error);
    bool ComputeSkeletonSignature(const ModelData& data, std::string& out, std::string& error);
}
