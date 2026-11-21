// ManualReloadF: geometry of ways drawn in Blender: a turn about a hinge, then a straight move; points along a way
#pragma once

#include <RE/Fallout.h>
#include <cstddef>
#include <vector>

namespace mrf
{
    // A way drawn in Blender whose first points lie on a circle: that part turns (about `axis` through `pivot`, by
    // `angle`, over the first `arcAt` of the way), the rest of the way is a straight move.
    struct ArcFit
    {
        bool         arc{ false };
        RE::NiPoint3 axis{}, pivot{};
        float        angle{ 0.0f };
        float        arcAt{ 0.0f };
        std::size_t  k{ 0 };
    };

    RE::NiPoint3 rotateAbout(const RE::NiPoint3& v, const RE::NiPoint3& axis, float angle) noexcept;

    ArcFit fitArc(const std::vector<RE::NiPoint3>& p, const std::vector<float>& at) noexcept;

    // `rest` (a part, gun root frame) turned about axis/pivot by angle, then moved by lin.
    RE::NiTransform turnedPose(const RE::NiTransform& rest, const RE::NiPoint3& axis, const RE::NiPoint3& pivot, float angle, const RE::NiPoint3& lin) noexcept;

    // A point on a polyline at share t (0..1) of its length.
    RE::NiPoint3 alongWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, float t) noexcept;

    // The share of the way (0..1) nearest to q, and how far q is from it.
    float nearestOnWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, const RE::NiPoint3& q, float* away) noexcept;
}
