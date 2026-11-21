#include "NiMath.h"

#include <algorithm>
#include <array>
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

    RE::NiPoint3 cross(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    RE::NiTransform identityTransform() noexcept
    {
        RE::NiTransform t;
        t.MakeIdentity();
        return t;
    }

    // The transform that undoes `t`: compose(t, inverse(t)) is the identity.
    RE::NiTransform inverse(const RE::NiTransform& t) noexcept
    {
        return relative(t, identityTransform());
    }

    void setRows(RE::NiTransform& t, const RE::NiPoint3& x, const RE::NiPoint3& y, const RE::NiPoint3& z) noexcept
    {
        const std::array<RE::NiPoint3, 3> rows{ x, y, z };
        for (int i = 0; i < 3; ++i) {
            t.rotate.entry[i][0] = rows[i].x;
            t.rotate.entry[i][1] = rows[i].y;
            t.rotate.entry[i][2] = rows[i].z;
        }
    }

    // Between two poses: t = 0 is a, 1 is b. Rotation by blended axes, made square again.
    RE::NiTransform blend(const RE::NiTransform& a, const RE::NiTransform& b, float t) noexcept
    {
        RE::NiTransform r = b;
        r.translate = a.translate + (b.translate - a.translate) * t;
        r.scale = a.scale + (b.scale - a.scale) * t;
        const auto x = normalized(axisOf(a, 0) * (1.0f - t) + axisOf(b, 0) * t);
        auto y = axisOf(a, 1) * (1.0f - t) + axisOf(b, 1) * t;
        y = normalized(y - x * dot(y, x));
        setRows(r, x, y, cross(x, y));
        return r;
    }

    float approach(float value, float target, float step) noexcept
    {
        if (value < target) {
            return std::min(value + step, target);
        }
        return std::max(value - step, target);
    }
}
