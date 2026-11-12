// The link to ROCK 1.0 (provider API 2): ROCK is asked to let the free hand take the magazine and the slide
// (AttachOnly part targets with grab points), so that ROCK itself puts the hand and the fingers on them, and it
// is told where this plugin has moved those parts (part drives), so that the held hand follows.
// No game headers here: this file is built with Windows.h.
#pragma once

#include <cstdint>

namespace rocklink
{
    using LogFn = void (*)(const char*);

    struct Pose
    {
        float rotate[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // row * 3 + column of the NiMatrix3
        float translate[3]{};
        float scale{ 1.0f };
    };

    struct Part
    {
        bool        grab{ false };       // the free hand may take it now
        const char* node{ nullptr };     // the node's name in the weapon model
        float       point[3]{};          // where the hand takes it, in that node's own frame
        float       radius{ 8.0f };      // ..within this distance (game units)
        bool        drive{ false };      // the plugin has moved it: its pose against its parent node
        Pose        parentLocal{};
    };

    struct Frame
    {
        bool gun{ false };          // a gun this plugin reloads is drawn
        int  freeHand{ 1 };         // 0 right, 1 left
        bool exclusive{ false };    // the free hand may take nothing else on the gun
        bool keepTargets{ false };  // the hand is closed: what it may take is not changed now
        int  chamber{ 0 };          // 0 unknown, 1 empty, 2 a round is chambered
        Part magazine{};            // group 1
        Part bolt{};                // group 2
    };

    struct Result
    {
        bool active{ false };       // ROCK answered this frame
        bool onMagazine{ false };   // ROCK holds the free hand on the magazine
        bool onBolt{ false };       // ..on the slide / bolt
        bool onGun{ false };        // ..on anything else of the gun (a support grip)
    };

    // Safe to call again after a refusal; `enabled` false = never ask ROCK.
    bool connect(LogFn log, bool enabled) noexcept;
    bool connected() noexcept;
    Result frame(const Frame& frame) noexcept;
    // The box of the drawn gun's magazine (0) or slide / bolt (1) as ROCK measured it, in the gun root's frame.
    bool partBox(int which, float lo[3], float hi[3]) noexcept;
    // The middle of the bolt's handle where it sticks out of one side of the gun (the gun root's frame); false for a slide.
    bool boltHandle(float point[3]) noexcept;
    // ROCK puts a real object (a reference) into the hand's palm (0 right, 1 left); `point` = where on it, in the world, or null.
    bool forceGrab(int hand, std::uint32_t refFormId, const float point[3]) noexcept;
    // The last force grab: 0 unknown, 1 queued, 2 succeeded, 3 rejected, 4 cancelled.
    int forceGrabState(int* failure) noexcept;
    // The reference the hand holds through ROCK now (0 none).
    std::uint32_t heldBy(int hand) noexcept;
    // ROCK lets go of that reference.
    bool releaseHeld(int hand, std::uint32_t refFormId) noexcept;
}
