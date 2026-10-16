#pragma once

#include <filesystem>
#include <string_view>

namespace Engine::Log
{
    bool Open(const std::filesystem::path& file);
    void Write(std::string_view message);
    void Error(std::string_view message, long code);
    void Close();
}
