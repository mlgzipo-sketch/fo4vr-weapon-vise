#pragma once
// ROCK 1.0 (provider API major 2): finger poses for the free hand. Optional: without ROCK 1.0 nothing here runs.
#include <cstdint>

namespace rocklink
{
    using LogFn = void (*)(const char*);

    // Connects once (game thread). Only a ROCK.dll whose file version is 1.0 or newer is asked.
    bool connect(LogFn log, bool enabled) noexcept;
    bool connected() noexcept;

    // index: 0 right, 1 left (as the plugin's hands).
    // Takes ROCK's current finger joints of that hand as the grip pose (when ROCK has them all).
    // needCurl: only when the fingers are clearly closed compared with the last open pose seen (noteOpen).
    bool captureGrip(int index, bool needCurl = false) noexcept;
    // The hand's fingers as they are now, while nothing is gripped: the open pose captureGrip compares with.
    void noteOpen(int index) noexcept;
    bool hasGrip() noexcept;
    // Every frame while on: the hand's fingers take the grip pose; off releases them to ROCK.
    void holdFingers(int index, bool on) noexcept;
}
