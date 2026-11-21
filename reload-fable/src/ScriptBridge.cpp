#include "ScriptBridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Settings.h"
#include "Log.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    Shared g_shared;

    void pushCommand(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
    }

    // Sounds are played here by the descriptor's editor ID (the plugin's own ones first: their output is 2D,
    // so they need no position). The script's Sound objects are only the fallback.

    bool playByName(const char* name) noexcept
    {
        auto* audio = RE::BSAudioManager::GetSingleton();
        if (!audio || !name || !*name) {
            return false;
        }
        RE::BSSoundHandle handle;
        handle.soundID = 0xFFFFFFFF;
        handle.assumeSuccess = false;
        handle.state = 0;
        audio->GetSoundHandleByName(handle, name, 0.0f, static_cast<std::uint32_t>(g_settings.soundFlags), nullptr);
        if (handle.soundID == 0xFFFFFFFF) {
            return false;
        }
        return handle.FadeInPlay(0);
    }

    void playSound(int index) noexcept
    {
        static const std::array<std::array<const char*, 2>, 5> names{ {
            { "MRF_DescIn", "WPNPistol10mmReloadMagIn" }, { "MRF_DescOut", "WPNPistol10mmReloadMagOut" },
            { "MRF_DescSlideBack", "WPNPistol10mmReloadBoltOpen" }, { "MRF_DescSlideHome", "WPNPistol10mmReloadBoltClose" },
            { "ItemPickUpMetal1Small", nullptr } } };
        static std::array<int, 5> logged{};
        if (g_settings.nativeSound && index >= 0 && index < static_cast<int>(names.size())) {
            for (const char* name : names[index]) {
                if (name && playByName(name)) {
                    if (logged[index]++ == 0) {
                        note("Sound %d: played %s", index, name);
                    }
                    return;
                }
            }
            if (logged[index]++ == 0) {
                note("Sound %d: no descriptor found by name, the script tries", index);
            }
        }
        PendingCommand command;
        command.type = kSound;
        command.kind = index;
        pushCommand(command);
    }

    std::int32_t PollCommand(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        if (g_shared.commands.empty()) {
            g_shared.current = {};
            return 0;
        }
        g_shared.current = g_shared.commands.front();
        g_shared.commands.pop_front();
        return g_shared.current.type;
    }

    std::int32_t CommandKind(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return g_shared.current.kind;
    }

    std::int32_t CommandRefID(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return static_cast<std::int32_t>(g_shared.current.ref);
    }

    std::int32_t CommandToken(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return g_shared.current.token;
    }

    std::vector<float> CommandPose(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.pose.begin(), g_shared.current.pose.end() };
    }

    std::vector<float> CommandImpulse(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.impulse.begin(), g_shared.current.impulse.end() };
    }

    float CommandScale(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return g_shared.current.scale;
    }

    std::vector<float> FreshPose(std::monostate, std::int32_t token)
    {
        std::scoped_lock guard(g_shared.lock);
        const auto it = g_shared.fresh.find(token);
        if (it == g_shared.fresh.end()) {
            return {};
        }
        return { it->second.begin(), it->second.end() };
    }

    void AddMag(std::monostate, RE::TESObjectREFR* ref, std::int32_t token)
    {
        if (!ref) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.newMags.push_back(NewMag{ ref->GetFormID(), token });
    }

    void Notify(std::monostate, std::int32_t message)
    {
        note("Script: %d", message);
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "ManualReloadF";
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandKind", CommandKind, true);
        vm->BindNativeMethod(kScript, "CommandRefID", CommandRefID, true);
        vm->BindNativeMethod(kScript, "CommandToken", CommandToken, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "FreshPose", FreshPose, true);
        vm->BindNativeMethod(kScript, "CommandImpulse", CommandImpulse, true);
        vm->BindNativeMethod(kScript, "CommandScale", CommandScale, true);
        vm->BindNativeMethod(kScript, "AddMag", AddMag, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        logNote("Papyrus functions of script ManualReloadF registered");
        return true;
    }
}
