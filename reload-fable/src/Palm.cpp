#include "Palm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Runtime.h"
#include "Rail.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ core2: the palm on a part

    bool inBox(const RE::NiPoint3& lo, const RE::NiPoint3& hi, const RE::NiPoint3& p, float margin) noexcept
    {
        return p.x >= lo.x - margin && p.x <= hi.x + margin && p.y >= lo.y - margin && p.y <= hi.y + margin && p.z >= lo.z - margin &&
               p.z <= hi.z + margin;
    }

    RE::NiPoint3 inGun(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept
    {
        return relative(root->world, pointTransform(world)).translate;
    }

    // The point of the magazine the well catches (zones file "point magtop"; else its top), or the same point at
    // the other end (a cell goes in either way round), for a magazine node standing at `node`.

    RE::NiPoint3 catchPointOf(const RE::NiTransform& node, bool otherEnd) noexcept
    {
        const auto& st = g_state;
        if (!st.catchPointKnown) {
            return topOf(node);
        }
        auto local = st.catchPointInNode;
        if (otherEnd) {
            const RE::NiPoint3 w{ dot(axisOf(st.magRest, 0), st.axis), dot(axisOf(st.magRest, 1), st.axis), dot(axisOf(st.magRest, 2), st.axis) };
            local = local - w * (2.0f * dot(local, w));
        }
        return compose(node, pointTransform(local)).translate;
    }

    // The catch, as drawn: the magazine's top (either end, for a cell) inside the well's cylinder, which runs
    // from its mouth out along the way out as far as `ahead` (and `below` into the gun). `along` is how far out
    // past the mouth that end is, `radial` how far off the cylinder's line.

    bool inWellZone(const RE::NiAVObject* root, const RE::NiTransform& node, float sideMax, float ahead, float below, float* along, float* radial) noexcept
    {
        const auto& st = g_state;
        float bestAlong = 1e9f, bestRadial = 1e9f;
        bool inside = false;
        const auto test = [&](const RE::NiPoint3& world) {
            const auto d = inGun(root, world) - st.zoneMouth;
            const float a = dot(d, st.axis);
            const auto off = d - st.axis * a;
            const float r = std::sqrt(dot(off, off));
            const bool in = a <= ahead && a >= -below && r <= sideMax;
            if (in || (!inside && r + std::max(0.0f, a - ahead) < bestRadial + std::max(0.0f, bestAlong - ahead))) {
                bestAlong = a;
                bestRadial = r;
            }
            inside = inside || in;
        };
        if (st.catchPointKnown) {
            test(catchPointOf(node, false));
            if (st.cellFed) {
                test(catchPointOf(node, true));
            }
        } else {
            test(topOf(node));
            if (st.cellFed) {
                test(node.translate + (axisOf(node, 0) * st.sideInNode.x + axisOf(node, 1) * st.sideInNode.y + axisOf(node, 2) * st.bottomAt) * node.scale);
            }
        }
        if (along) {
            *along = bestAlong;
        }
        if (radial) {
            *radial = bestRadial;
        }
        return inside;
    }

    // How far out of its seat a magazine node stands (the rail's own measure), whatever its top.

    float seatAlong(const RE::NiAVObject* root, const RE::NiTransform& node) noexcept
    {
        const auto rel = inGun(root, node.translate) - g_state.magRest.translate;
        return dot(rel, g_state.axis);
    }

    // The magazine is touched: the palm is in its box as it stands now (moved out along the well by s).

    bool palmOnMagazine(const RE::NiAVObject* root, const RE::NiPoint3& palm) noexcept
    {
        const auto& st = g_state;
        if (!st.magBoxKnown) {
            return distance(palm, grabPoint(root)) < g_settings.magGrabRadius;
        }
        return inBox(st.magLo, st.magHi, inGun(root, palm) - st.axis * st.s, g_settings.contactMargin);
    }
}
