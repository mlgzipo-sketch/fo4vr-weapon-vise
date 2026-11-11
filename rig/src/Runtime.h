#pragma once

#include <cstdint>
#include <string_view>

namespace rigvr::runtime
{
    struct Definition
    {
        const char* pluginName{ nullptr };
        std::uint32_t pluginVersion{ 1 };
        // Game data is loaded (also after each save load): hook up FRIK and OpenVR.
        void (*onGameReady)() noexcept{ nullptr };
    };

    [[nodiscard]] const Definition& definition() noexcept;
    // Called once from F4SEPlugin_Load after F4SE::Init.
    void onLoad() noexcept;
    // Detail for testing; written only with bDebugLog=1 in RigVR.ini.
    void logInfo(std::string_view message) noexcept;
    // Startup and settings lines, always written.
    void logNote(std::string_view message) noexcept;
    void logWarning(std::string_view message) noexcept;
}
