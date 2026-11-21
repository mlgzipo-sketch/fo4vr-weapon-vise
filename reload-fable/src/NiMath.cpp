#include "NiMath.h"

#include <cmath>

namespace mrf
{

    float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Transforms in the game's row convention: world = local * parent.
    RE::NiTransform compose(const RE::NiTransform& parent, const RE::NiTransform& local) noexcept
    {
        RE::NiTransform world{};
        world.rotate = local.rotate * parent.rotate;
        world.translate = parent.translate + parent.rotate.Transpose() * local.translate * parent.scale;
        world.scale = local.scale * parent.scale;
        return world;
    }

    RE::NiTransform relative(const RE::NiTransform& parent, const RE::NiTransform& world) noexcept
    {
        const float scale = parent.scale > 0.0f ? parent.scale : 1.0f;
        RE::NiTransform local{};
        local.rotate = world.rotate * parent.rotate.Transpose();
        local.translate = parent.rotate * (world.translate - parent.translate) * (1.0f / scale);
        local.scale = world.scale / scale;
        return local;
    }

    RE::NiTransform pointTransform(const RE::NiPoint3& point) noexcept
    {
        RE::NiTransform transform;
        transform.translate = point;
        return transform;
    }

    // A local axis in world space is a row of the rotation (world = R^T * v).
    RE::NiPoint3 axisOf(const RE::NiTransform& t, int axis) noexcept
    {
        return { t.rotate.entry[axis][0], t.rotate.entry[axis][1], t.rotate.entry[axis][2] };
    }

    float dot(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    RE::NiPoint3 normalized(const RE::NiPoint3& v) noexcept
    {
        const float length = std::sqrt(dot(v, v));
        if (length < 1e-5f) {
            return { 0.0f, 0.0f, -1.0f };
        }
        return v * (1.0f / length);
    }

    // A direction given in a transform's own frame, in world space.
    RE::NiPoint3 directionToWorld(const RE::NiTransform& t, const RE::NiPoint3& v) noexcept
    {
        return axisOf(t, 0) * v.x + axisOf(t, 1) * v.y + axisOf(t, 2) * v.z;
    }
}
