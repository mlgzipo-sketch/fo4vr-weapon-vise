// ManualReloadF: vector and transform math in the game's row convention (world = local * parent)
#pragma once

#include <RE/Fallout.h>

namespace mrf
{
    float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept;
    // Transforms in the game's row convention: world = local * parent.
    RE::NiTransform compose(const RE::NiTransform& parent, const RE::NiTransform& local) noexcept;
    RE::NiTransform relative(const RE::NiTransform& parent, const RE::NiTransform& world) noexcept;
    RE::NiTransform pointTransform(const RE::NiPoint3& point) noexcept;
    // A local axis in world space is a row of the rotation (world = R^T * v).
    RE::NiPoint3 axisOf(const RE::NiTransform& t, int axis) noexcept;
    float dot(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept;
    RE::NiPoint3 normalized(const RE::NiPoint3& v) noexcept;
    // A direction given in a transform's own frame, in world space.
    RE::NiPoint3 directionToWorld(const RE::NiTransform& t, const RE::NiPoint3& v) noexcept;
}
