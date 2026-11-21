// ManualReloadF: the Papyrus side (ManualReloadF.psc): commands the script runs (make, delete, sounds) and the natives it calls
#pragma once

#include <RE/Fallout.h>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>
#include "Runtime.h"

namespace mrf
{
    // ------------------------------------------------------------ talking to the script

    enum Command : std::int32_t
    {
        kNone = 0,
        kSpawn = 1,    // kind, pose, token: a magazine as a physical object
        kDelete = 2,   // ref
        kSound = 3,    // kind = sound index: 0 magazine in, 1 magazine out, 2 slide back, 3 slide home, 4 put away
        kRound = 4,    // the drawn cartridge (a static object; the script answers with AddMag(ref, kRoundToken))
        kCasing = 5,   // pose, impulse, token: a case thrown out of the gun (MRF_MagCasing, its model set from the ammo)
    };

    struct PendingCommand
    {
        std::int32_t type{ kNone };
        std::int32_t kind{ 0 };
        std::uint32_t ref{ 0 };
        std::int32_t token{ 0 };
        std::array<float, 6> pose{};   // x, y, z, angle x, y, z (degrees)
        std::array<float, 4> impulse{};   // x, y, z, magnitude
        float scale{ 1.0f };              // a magazine is made at the gun's scale (FRIK or a mod can scale the gun)
    };

    struct NewMag
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
    };

    struct Shared
    {
        std::mutex lock;
        std::deque<PendingCommand> commands;
        PendingCommand current;
        std::vector<NewMag> newMags;
        // where a magazine that is on its way should appear now (it has moved since it was asked for)
        std::unordered_map<std::int32_t, std::array<float, 6>> fresh;
    };

    extern Shared g_shared;

    void pushCommand(const PendingCommand& command) noexcept;

    // Sounds are played here by the descriptor's editor ID (the plugin's own ones first: their output is 2D,
    // so they need no position). The script's Sound objects are only the fallback.
    bool playByName(const char* name) noexcept;

    void playSound(int index) noexcept;

    std::int32_t PollCommand(std::monostate);

    std::int32_t CommandKind(std::monostate);

    std::int32_t CommandRefID(std::monostate);

    std::int32_t CommandToken(std::monostate);

    std::vector<float> CommandPose(std::monostate);

    std::vector<float> CommandImpulse(std::monostate);

    float CommandScale(std::monostate);

    std::vector<float> FreshPose(std::monostate, std::int32_t token);

    void AddMag(std::monostate, RE::TESObjectREFR* ref, std::int32_t token);

    void Notify(std::monostate, std::int32_t message);

    bool registerNatives(RE::BSScript::IVirtualMachine* vm);
}
