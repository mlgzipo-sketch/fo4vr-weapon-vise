// F4SE entry points and logging.
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <REL/Relocation.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>
#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
    std::shared_ptr<spdlog::logger> g_logger;

    bool g_verbose{ false };

    bool readDebugLogSetting() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/WeaponVise.ini");
            std::string line;
            while (file && std::getline(file, line)) {
                const auto equals = line.find('=');
                if (line.empty() || line[0] == ';' || equals == std::string::npos) {
                    continue;
                }
                auto key = line.substr(0, equals);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                    key.pop_back();
                }
                if (key == "bDebugLog") {
                    return std::strtof(line.c_str() + equals + 1, nullptr) != 0.0f;
                }
            }
        } catch (...) {
        }
        return false;
    }

    void initializeLogger()
    {
        // F4SE::log::log_directory() needs F4SE::Init, which runs after Query,
        // so before that it drops the "Fallout4VR" part of the path.
        wchar_t* documents{ nullptr };
        const auto result = REX::W32::SHGetKnownFolderPath(
            REX::W32::FOLDERID_Documents, REX::W32::KF_FLAG_DEFAULT, nullptr, std::addressof(documents));
        std::unique_ptr<wchar_t[], decltype(&REX::W32::CoTaskMemFree)> documentsPath(documents, REX::W32::CoTaskMemFree);
        if (!documentsPath || result != 0) {
            return;
        }
        auto directory = std::filesystem::path(documentsPath.get()) / "My Games" / "Fallout4VR" / "F4SE";
        const auto& example = weaponvise::runtime::definition();
        directory /= std::string(example.pluginName) + ".log";
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            directory.string(),
            2 * 1024 * 1024,
            2,
            true);
        g_logger = std::make_shared<spdlog::logger>(example.pluginName, std::move(sink));
        g_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::info);
        g_verbose = readDebugLogSetting();
    }

    void onF4SEMessage(F4SE::MessagingInterface::Message* message) noexcept
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case F4SE::MessagingInterface::kGameLoaded:
        case F4SE::MessagingInterface::kGameDataReady:
        case F4SE::MessagingInterface::kPostLoadGame:
        case F4SE::MessagingInterface::kNewGame:
            if (const auto ready = weaponvise::runtime::definition().onGameReady) {
                ready();
            }
            break;
        default:
            break;
        }
    }
}

namespace weaponvise::runtime
{
    void logInfo(const std::string_view message) noexcept
    {
        if (g_verbose) {
            logNote(message);
        }
    }

    void logNote(const std::string_view message) noexcept
    {
        try {
            if (g_logger) {
                g_logger->info("{}", message);
            }
        } catch (...) {
        }
    }

    void logWarning(const std::string_view message) noexcept
    {
        try {
            if (g_logger) {
                g_logger->warn("{}", message);
            }
        } catch (...) {
        }
    }
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info) noexcept
{
    try {
        if (!a_f4se || !a_info) {
            return false;
        }

        initializeLogger();
        const auto& example = weaponvise::runtime::definition();
        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = example.pluginName;
        a_info->version = example.pluginVersion;

        if (a_f4se->IsEditor()) {
            weaponvise::runtime::logWarning("Editor runtime is unsupported");
            return false;
        }
        if (!REL::Module::IsVR()) {
            weaponvise::runtime::logWarning("Fallout 4 VR runtime is required");
            return false;
        }

        const auto executableVersion = REL::Module::get().version();
        if (executableVersion != F4SE::RUNTIME_VR_1_2_72) {
            weaponvise::runtime::logWarning(
                "Unsupported Fallout4VR.exe version; 1.2.72 is required");
            return false;
        }

        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "Query passed: F4SE=%s loaderRuntime=%s executable=%s",
            a_f4se->F4SEVersion().string().c_str(),
            a_f4se->RuntimeVersion().string().c_str(),
            executableVersion.string().c_str());
        weaponvise::runtime::logNote(message);
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(
    const F4SE::LoadInterface* a_f4se) noexcept
{
    try {
        if (!a_f4se) {
            return false;
        }
        F4SE::Init(a_f4se, false);
        weaponvise::runtime::onLoad();
        const auto* messaging = F4SE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(onF4SEMessage)) {
            weaponvise::runtime::logWarning("F4SE messaging registration failed");
            return false;
        }
        weaponvise::runtime::logNote("Plugin loaded; waiting for game data");
        return true;
    } catch (...) {
        return false;
    }
}
