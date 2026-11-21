// ManualReloadF: the VR controllers and the hands (FRIK): reading them, buzzing, pinning the free hand on a part; HUD messages
#pragma once

#include <RE/Fallout.h>
#include <array>
#include <cstdint>
#include "api/FRIKApiV2.h"
#include "api/openvr.h"
#include "Runtime.h"

namespace mrf
{
    using frik::api::FRIKApiV2;

    extern vr::IVRSystem* g_vr;

    vr::TrackedDeviceIndex_t controllerOf(int index) noexcept;

    void buzz(int index, unsigned short microseconds) noexcept;

    struct Hand
    {
        bool            valid{ false };
        RE::NiTransform wand{};          // the controller
        RE::NiTransform wrist{};         // the hand bone as it is drawn
        RE::NiPoint3    palm{};
        bool            grip{ false };   // as the controller reports it, hidden or not
        bool            previousGrip{ false };
        std::uint64_t   buttons{ 0 };
        std::uint64_t   previousButtons{ 0 };
        RE::NiTransform wristInWand{};   // where the drawn hand sat on the controller while nothing pinned it
        RE::NiPoint3    palmInWand{};
        bool            inWandKnown{ false };
        std::array<RE::NiPoint3, 5> axes{};   // the controller's axes (x, y): the stick is one of them
    };

    extern std::array<Hand, 2> g_hands;

    extern bool g_pinned;

    int freeIndex() noexcept;
    FRIKApiV2::Hand frikHand(int index) noexcept;

    void readHand(int index) noexcept;

    void hud(const char* text) noexcept;

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept;

    // ------------------------------------------------------------ the hand on the magazine (FRIK)

    inline constexpr const char* kFrikTag = "ManualReloadF";

    extern bool g_posed;

    void pinHand(const RE::NiTransform& wrist) noexcept;

    // The hand on a turning part (a crank, a handle): the palm stays on the point of the part it took, the wrist
    // keeps the turn of the controller (so it is not wrung round, and does not swing out on a wider circle).
    RE::NiTransform handOnPart(const RE::NiTransform& wristNow, const RE::NiTransform& part, const RE::NiPoint3& gripInPart, const RE::NiPoint3& palmInWrist) noexcept;

    void unpinHand() noexcept;

    void poseFingers(bool on) noexcept;
}
