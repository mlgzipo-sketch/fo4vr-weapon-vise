// ManualReloadF: placing nodes of the game's scene graph by hand (world transforms, bounds, culling)
#pragma once

#include <RE/Fallout.h>
#include "RockLink.h"
#include "Runtime.h"

namespace mrf
{
    // Places an object's 3D: root at `world`, children follow.
    void place3D(RE::NiAVObject* root, const RE::NiTransform& world) noexcept;

    // A transform as ROCK's API takes it.
    rocklink::Pose toLinkPose(const RE::NiTransform& t) noexcept;

    // The nodes above must reach as far as a part carried away from the gun, or it is culled with them.
    void reachAncestors(RE::NiAVObject* node, const RE::NiPoint3& point) noexcept;

    // Places a node's world transform, recomputes everything below it and moves the bounds along
    // (a shape whose bound stays behind is culled as if it were still there).
    void placeNode(RE::NiAVObject* node, const RE::NiTransform& world) noexcept;

    // A node somebody else has placed away from the gun (ROCK holds the magazine): its bounds are brought along,
    // or it is culled as soon as it leaves the gun's own bound.
    void carryBounds(RE::NiAVObject* node, const RE::NiPoint3& middle) noexcept;

    // Recomputes the world transforms below `node` from its parent, then moves the subtree by `shift`.
    void refreshSubtree(RE::NiAVObject* node, const RE::NiPoint3& shift) noexcept;
}
