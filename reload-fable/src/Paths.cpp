#include "Paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "NiMath.h"
#include "Runtime.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    RE::NiPoint3 rotateAbout(const RE::NiPoint3& v, const RE::NiPoint3& axis, float angle) noexcept
    {
        const float c = std::cos(angle), s = std::sin(angle);
        return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
    }

    ArcFit fitArc(const std::vector<RE::NiPoint3>& p, const std::vector<float>& at) noexcept
    {
        ArcFit fit{};
        for (std::size_t k = p.size() - 1; p.size() >= 3 && k >= 2; --k) {
            const auto a = p[0], b = p[k / 2], c = p[k];
            const auto ab = b - a, ac = c - a;
            const auto nrm = cross(ab, ac);
            const float n2 = dot(nrm, nrm);
            if (n2 < 1e-4f) {
                continue;
            }
            const auto center = a + (cross(nrm, ab) * dot(ac, ac) + cross(ac, nrm) * dot(ab, ab)) * (1.0f / (2.0f * n2));
            const float r = distance(a, center);
            const auto axis = normalized(nrm);
            bool ok = r > 0.3f && r < 60.0f;
            for (std::size_t i = 0; ok && i <= k; ++i) {
                ok = std::fabs(distance(p[i], center) - r) < 0.08f * r + 0.05f && std::fabs(dot(p[i] - a, axis)) < 0.1f;
            }
            if (!ok) {
                continue;
            }
            const auto u = a - center, v = c - center;
            fit.angle = std::atan2(dot(cross(u, v), axis), dot(u, v));
            fit.axis = fit.angle < 0.0f ? axis * -1.0f : axis;
            fit.angle = std::fabs(fit.angle);
            fit.pivot = center;
            fit.arc = fit.angle > 0.05f;
            fit.arcAt = k < at.size() ? at[k] : 1.0f;
            fit.k = k;
            return fit;
        }
        return fit;
    }

    // `rest` (a part, gun root frame) turned about axis/pivot by angle, then moved by lin.

    RE::NiTransform turnedPose(const RE::NiTransform& rest, const RE::NiPoint3& axis, const RE::NiPoint3& pivot, float angle, const RE::NiPoint3& lin) noexcept
    {
        RE::NiTransform out = rest;
        out.translate = rotateAbout(rest.translate - pivot, axis, angle) + pivot + lin;
        setRows(out, rotateAbout(axisOf(rest, 0), axis, angle), rotateAbout(axisOf(rest, 1), axis, angle), rotateAbout(axisOf(rest, 2), axis, angle));
        return out;
    }

    // A point on a polyline at share t (0..1) of its length.

    RE::NiPoint3 alongWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, float t) noexcept
    {
        if (pts.empty()) {
            return RE::NiPoint3{};
        }
        for (std::size_t i = 1; i < pts.size() && i < at.size(); ++i) {
            if (t <= at[i]) {
                const float span = std::max(at[i] - at[i - 1], 1e-5f);
                const float f = std::clamp((t - at[i - 1]) / span, 0.0f, 1.0f);
                return pts[i - 1] + (pts[i] - pts[i - 1]) * f;
            }
        }
        return pts.back();
    }

    // The share of the way (0..1) nearest to q, and how far q is from it.

    float nearestOnWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, const RE::NiPoint3& q, float* away) noexcept
    {
        float best = 1e9f, bestT = 0.0f;
        for (std::size_t i = 1; i < pts.size() && i < at.size(); ++i) {
            const auto seg = pts[i] - pts[i - 1];
            const float len2 = std::max(dot(seg, seg), 1e-6f);
            const float f = std::clamp(dot(q - pts[i - 1], seg) / len2, 0.0f, 1.0f);
            const float d = distance(q, pts[i - 1] + seg * f);
            if (d < best) {
                best = d;
                bestT = at[i - 1] + (at[i] - at[i - 1]) * f;
            }
        }
        if (away) {
            *away = best;
        }
        return bestT;
    }
}
