// ManualReloadF: the magazine well as a rail: where the magazine sits, its way out, the slide
#pragma once

#include <RE/Fallout.h>
#include "State.h"
#include "RockLink.h"

namespace mrf
{
    // ------------------------------------------------------------ the rail
    float gunScale(const RE::NiAVObject* root) noexcept;

    RE::NiPoint3 axisWorld(const RE::NiAVObject* root) noexcept;

    // The magazine node `s` out of the well.
    RE::NiTransform railPose(const RE::NiAVObject* root, float s) noexcept;

    // A point's place along the rail measured from the seated magazine's origin (more = further out), in the
    // gun's units, and its distance off the rail's line.
    float alongRail(const RE::NiAVObject* root, const RE::NiPoint3& point, float* side = nullptr) noexcept;

    // The top of a magazine whose node stands at `pose`.
    RE::NiPoint3 topOf(const RE::NiTransform& pose) noexcept;

    // Where the palm takes a magazine on the rail.
    RE::NiPoint3 grabPoint(const RE::NiAVObject* root) noexcept;

    RE::NiPoint3 tiltAxis(float degrees) noexcept;

    void learnRail(RE::NiAVObject* root, RE::NiAVObject* magazine) noexcept;

    void learnSlide(RE::NiAVObject* root, RE::NiAVObject* bolt) noexcept;

    // ROCK measures every part of the drawn gun (boxes in the gun's frame): the real ends of the magazine and the
    // place of the slide come from there, not from the nodes' origins or bounds (those are off, and change).
    void useRockBoxes(RE::NiAVObject* root, RE::NiAVObject* bolt, bool atRest) noexcept;

    RE::NiPoint3 slideGrabPoint(const RE::NiAVObject* root, const RE::NiAVObject* bolt) noexcept;
}
