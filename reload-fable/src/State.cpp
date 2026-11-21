#include "State.h"

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
#include "NiMath.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    State g_state;

    RE::NiPoint3 g_liveTop{};               // the top of the magazine in the hand, last measured (shown with the zones)
    std::chrono::steady_clock::time_point g_liveTopAt{};

    rocklink::Result g_rock;   // what ROCK answered last frame

    bool rockOn() noexcept { return g_settings.rockParts && g_rock.active; }

    std::unordered_map<std::uint32_t, Loose> g_loose;     // real magazines lying around or held through ROCK
    std::unordered_map<std::int32_t, Pending> g_pending;  // spawn token -> what it will be
    std::int32_t g_nextToken{ 1 };

    std::uint32_t looseRounds() noexcept
    {
        std::uint32_t total = 0;
        for (const auto& [ref, loose] : g_loose) {
            total += loose.rounds;
        }
        for (const auto& [token, pending] : g_pending) {
            total += pending.rounds;
        }
        return total;
    }

    void setAmmo(const Gun& gun, std::uint32_t value) noexcept
    {
        gun.data->ammoCount = value;
        g_state.lastAmmo = value;
    }

    // The game fires while ammoCount > 0, so a gun with an empty chamber must read 0.

    void syncAmmo(const Gun& gun) noexcept
    {
        setAmmo(gun, g_state.chamber ? 1u + (g_state.place == kSeated ? g_state.rounds : 0u) : 0u);
    }
}
