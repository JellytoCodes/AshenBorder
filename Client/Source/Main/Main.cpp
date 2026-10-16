#include "App/Application.h"
#include "App/ApplicationDesc.h"
#include "Core/LastFieldApp.h"
#include "Core/Log.h"

#include <Windows.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    std::filesystem::path GetExecutablePath()
    {
        constexpr std::size_t kMaximumPathCapacity = 32768;
        std::vector<wchar_t> pathBuffer(512);

        for (;;)
        {
            const DWORD length = GetModuleFileNameW(
                nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));

            if (length == 0)
            {
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(),
                    "GetModuleFileNameW failed");
            }

            if (length < pathBuffer.size())
            {
                return std::filesystem::path(std::wstring(pathBuffer.data(), length));
            }

            if (pathBuffer.size() >= kMaximumPathCapacity)
            {
                throw std::runtime_error("Executable path exceeds the supported length");
            }

            pathBuffer.resize(pathBuffer.size() * 2);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    std::wstring_view arguments(commandLine != nullptr ? commandLine : L"");
    const std::size_t first = arguments.find_first_not_of(L" \t");

    if (first == std::wstring_view::npos)
    {
        arguments = {};
    }
    else
    {
        const std::size_t last = arguments.find_last_not_of(L" \t");
        arguments = arguments.substr(first, last - first + 1);
    }

    if (!arguments.empty() && arguments != L"--smoke")
    {
        Engine::Log::Write("AshenBorder: supported option is --smoke");
        return 2;
    }

    const bool smoke = arguments == L"--smoke";

    try
    {
        const std::filesystem::path executablePath = GetExecutablePath();
        const std::filesystem::path projectRoot =
            executablePath.parent_path().parent_path().parent_path().parent_path();

        Engine::ApplicationDesc desc;
        desc.title = L"SAVE MYSELF: ASHEN BORDER";
        desc.width = 1280;
        desc.height = 720;
        desc.resourceRoot = projectRoot / L"Resources";
        desc.shaderRoot = projectRoot / L"Engine" / L"Shaders";
        desc.logFile = projectRoot / L"Saved" / L"AshenBorder.log";
        desc.fixedSeconds = 1.0 / 60.0;
        desc.maxCatchUp = 4;
        desc.visible = !smoke;

        Engine::Application application;

        if (!application.Initialize(desc, std::make_unique<Client::LastFieldApp>(desc.resourceRoot, smoke)))
        {
            application.Shutdown();
            return 1;
        }

        const int result = application.Run();
        application.Shutdown();
        return result;
    }
    catch (const std::exception& error)
    {
        Engine::Log::Write(std::string("AshenBorder: entry failed: ") + error.what());
        return 1;
    }
}
