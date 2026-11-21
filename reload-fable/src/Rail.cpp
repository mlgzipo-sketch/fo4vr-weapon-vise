#include "Rail.h"

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

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ the rail

    float gunScale(const RE::NiAVObject* root) noexcept
    {
        return root->world.scale > 0.0f ? root->world.scale : 1.0f;
    }

    RE::NiPoint3 axisWorld(const RE::NiAVObject* root) noexcept
    {
        return directionToWorld(root->world, g_state.axis);
    }

    // The magazine node `s` out of the well.

    RE::NiTransform railPose(const RE::NiAVObject* root, float s) noexcept
    {
        auto pose = compose(root->world, g_state.magRest);
        pose.translate += axisWorld(root) * (s * gunScale(root));
        return pose;
    }

    // A point's place along the rail measured from the seated magazine's origin (more = further out), in the
    // gun's units, and its distance off the rail's line.

    float alongRail(const RE::NiAVObject* root, const RE::NiPoint3& point, float* side = nullptr) noexcept
    {
        const auto rel = relative(root->world, pointTransform(point)).translate - g_state.magRest.translate - (side ? g_state.sideInGun : RE::NiPoint3{});
        const float along = dot(rel, g_state.axis);
        if (side) {
            const auto off = rel - g_state.axis * along;
            *side = std::sqrt(dot(off, off));
        }
        return along;
    }

    // The top of a magazine whose node stands at `pose`.

    RE::NiPoint3 topOf(const RE::NiTransform& pose) noexcept
    {
        return pose.translate + (axisOf(pose, 0) * g_state.sideInNode.x + axisOf(pose, 1) * g_state.sideInNode.y + axisOf(pose, 2) * g_state.topAt) * pose.scale;
    }

    // Where the palm takes a magazine on the rail.

    RE::NiPoint3 grabPoint(const RE::NiAVObject* root) noexcept
    {
        const auto pose = railPose(root, g_state.s);
        return pose.translate + (axisOf(pose, 0) * g_state.sideInNode.x + axisOf(pose, 1) * g_state.sideInNode.y +
                                 axisOf(pose, 2) * (g_state.bottomAt + g_settings.grabUp)) * pose.scale;
    }

    RE::NiPoint3 tiltAxis(float degrees) noexcept
    {
        const float t = degrees * 0.01745329f;
        return { 0.0f, -std::sin(t), -std::cos(t) };   // down, its bottom leaning back (gun +Y is forward)
    }

    void learnRail(RE::NiAVObject* root, RE::NiAVObject* magazine) noexcept
    {
        if (!looksLikeObject(root) || !looksLikeObject(magazine)) {
            return;
        }
        auto& st = g_state;
        st.magRest = relative(root->world, magazine->world);
        const auto up = axisOf(st.magRest, 2);   // the magazine node's own Z: the rounds are stacked along it
        const char* how = "the magazine node";
        if (g_settings.magTilt >= 0.0f) {
            st.axis = tiltAxis(g_settings.magTilt);
            how = "fMagTilt";
        } else if (up.z > 0.6f) {
            st.axis = normalized(up * -1.0f);
        } else {
            st.axis = tiltAxis(12.0f);
            how = "a usual slope (the node gave nothing usable)";
        }
        float top = 0.0f;
        if (auto* round = findNamed(magazine, "WeaponMagazineChild1"); looksLikeObject(round)) {
            top = round->local.translate.z;
        }
        st.topAt = top > 3.0f && top < 16.0f ? top : 7.0f;
        st.length = g_settings.railLength > 0.0f ? g_settings.railLength : st.topAt + 1.2f;
        // the baseplate: the lowest point of the magazine's bound along its own axis
        st.bottomAt = -3.5f;
        const auto& bound = magazine->worldBound;
        const float nodeScale = magazine->world.scale > 0.0f ? magazine->world.scale : 1.0f;
        const float middle = relative(magazine->world, pointTransform(bound.center)).translate.z;
        const float low = middle - bound.fRadius / nodeScale;
        const bool measured = bound.fRadius / nodeScale > 4.0f && bound.fRadius / nodeScale < 9.0f && low < -0.5f && low > -8.0f;
        if (measured) {
            st.bottomAt = low;
        }
        if (auto* attach = findNamed(magazine, "P-Mag"); looksLikeObject(attach)) {
            st.looseInNode = relative(magazine->world, attach->world);
        } else {
            st.looseInNode = identityTransform();
        }
        // the Thompson's drums (kinds 6, 7) are worn 13 off their node: the real one is made where the drawn one is.
        // Measured on the drum's own geometry only (the node's whole bound takes in the rounds and the other children).
        if (st.kind == 6 || st.kind == 7) {
            auto* geometry = findNamed(magazine, st.kind == 6 ? "TGunDrumMagSmall:0" : "TGunDrumMag:0");
            if (looksLikeObject(geometry) && geometry->worldBound.fRadius > 1.0f) {
                const auto inGun = relative(magazine->world, pointTransform(geometry->worldBound.center)).translate;
                const auto& m = kLooseMiddle[static_cast<std::size_t>(st.kind)];
                const auto turned = axisOf(st.looseInNode, 0) * m.x + axisOf(st.looseInNode, 1) * m.y + axisOf(st.looseInNode, 2) * m.z;
                auto want = inGun - turned;
                const float off = distance(want, st.looseInNode.translate);
                if (off < 8.0f || off > 20.0f) {
                    // not what the drum's geometry says in ROCK's box (13.4 behind the node, logs of 0.47): taken from there
                    want = st.looseInNode.translate + axisOf(st.looseInNode, 1) * -14.0f;
                }
                if (distance(want, st.looseInNode.translate) > 6.0f) {
                    if (!st.railLogged) {
                        note("Rail: the drum is worn %.1f off its node (middle at (%.2f, %.2f, %.2f)): a real one is made there too",
                            distance(want, st.looseInNode.translate), inGun.x, inGun.y, inGun.z);
                    }
                    st.looseInNode.translate = want;
                }
            }
        }
        st.railKnown = true;
        if (!st.railLogged) {
            st.railLogged = true;
            note("Rail: magazine node at (%.2f, %.2f, %.2f) in the gun, scale %.3f, gun scale %.3f", st.magRest.translate.x, st.magRest.translate.y,
                st.magRest.translate.z, st.magRest.scale, root->world.scale);
            for (int i = 0; i < 3; ++i) {
                const auto a = axisOf(st.magRest, i);
                note("  magazine node axis %d in the gun: (%.2f, %.2f, %.2f)", i, a.x, a.y, a.z);
            }
            note("Rail: way out (%.2f, %.2f, %.2f) from %s, slope %.1f deg back; top %.2f above the origin (%s), travel %.2f", st.axis.x, st.axis.y, st.axis.z,
                how, std::atan2(-st.axis.y, -st.axis.z) * 57.29578f, st.topAt, top > 3.0f && top < 16.0f ? "top round" : "default", st.length);
            note("Rail: baseplate %.2f below the origin (%s; bound middle %.2f, radius %.2f)", -st.bottomAt, measured ? "measured" : "default", middle, bound.fRadius);
            note("Rail: loose model against the node: (%.2f, %.2f, %.2f)", st.looseInNode.translate.x, st.looseInNode.translate.y, st.looseInNode.translate.z);
        }
    }

    void learnSlide(RE::NiAVObject* root, RE::NiAVObject* bolt) noexcept
    {
        if (!looksLikeObject(bolt)) {
            return;
        }
        const auto& bound = bolt->worldBound;
        if (!g_state.boltBoxUsed && bound.fRadius > 1.0f && bound.fRadius < 100.0f) {
            const bool first = !g_state.slideCenterKnown;
            g_state.slideCenter = relative(root->world, pointTransform(bound.center)).translate;
            g_state.slideCenterKnown = true;
            if (first) {
                g_state.slidePointInBolt = relative(bolt->world, pointTransform(bound.center)).translate;
                g_state.slidePointKnown = true;
                note("Slide: middle at (%.1f, %.1f, %.1f), radius %.1f", g_state.slideCenter.x, g_state.slideCenter.y, g_state.slideCenter.z, bound.fRadius);
            }
        }
    }

    // ROCK measures every part of the drawn gun (boxes in the gun's frame): the real ends of the magazine and the
    // place of the slide come from there, not from the nodes' origins or bounds (those are off, and change).

    void useRockBoxes(RE::NiAVObject* root, RE::NiAVObject* bolt, bool atRest) noexcept
    {
        auto& st = g_state;
        float lo[3]{}, hi[3]{};
        if (!st.magBoxUsed && st.railKnown && rocklink::partBox(0, lo, hi)) {
            st.magBoxUsed = true;
            st.magLo = { lo[0], lo[1], lo[2] };
            st.magHi = { hi[0], hi[1], hi[2] };
            st.magBoxKnown = true;
            const RE::NiPoint3 middle{ (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
            const float half[3]{ (hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f };
            const float a[3]{ std::fabs(st.axis.x), std::fabs(st.axis.y), std::fabs(st.axis.z) };
            const int k = a[0] > a[1] && a[0] > a[2] ? 0 : (a[1] > a[2] ? 1 : 2);
            const float halfLength = half[k] / std::max(a[k], 0.3f);
            const float out = dot(middle - st.magRest.translate, st.axis);   // the middle, this far towards the way out
            const float scale = st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f;
            if (halfLength > 1.5f && halfLength < 25.0f && std::fabs(out) < 30.0f) {
                const float before = st.bottomAt;
                st.bottomAt = -(out + halfLength) / scale;
                st.sideInGun = (middle - st.magRest.translate) - st.axis * out;
                st.sideInNode = { dot(axisOf(st.magRest, 0), st.sideInGun) / scale, dot(axisOf(st.magRest, 1), st.sideInGun) / scale, 0.0f };
                if (std::sqrt(dot(st.sideInGun, st.sideInGun)) < 1.0f) {
                    st.sideInGun = {};      // the node is on the magazine's line (the 10mm pistol)
                    st.sideInNode = {};
                } else {
                    note("  the magazine is (%.1f, %.1f) off its node's line: the hand and the well follow the magazine itself", st.sideInNode.x, st.sideInNode.y);
                }
                const float top = (halfLength - out) / scale;
                note("ROCK box of the magazine: (%.1f %.1f %.1f)..(%.1f %.1f %.1f); baseplate %.2f below the node (was %.2f), top %.2f above (node says %.2f)",
                    lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], -st.bottomAt, -before, top, st.topAt);
                if (std::fabs(top - st.topAt) > 2.5f && !st.zoneWell) {
                    // the node is not at the magazine (SMG and others): the box is the truth
                    st.topAt = top;
                    st.length = g_settings.railLength > 0.0f ? g_settings.railLength : std::min(2.0f * halfLength, top + 1.2f + std::max(0.0f, -out));
                    note("  the top and the travel follow the box: top %.2f, travel %.2f", st.topAt, st.length);
                }
            }
        }
        if (!st.boltBoxUsed && atRest && looksLikeObject(bolt) && rocklink::partBox(1, lo, hi)) {
            st.boltBoxUsed = true;
            if (!st.zoneSlide) {
                st.slideLo = { lo[0], lo[1], lo[2] };
                st.slideHi = { hi[0], hi[1], hi[2] };
            }
            st.slideBoxKnown = true;
            const float at = std::clamp(g_settings.slideGrabAt, 0.0f, 1.0f);
            const float high = std::clamp(g_settings.slideGrabHigh, 0.0f, 1.0f);
            st.slideCenter = { (lo[0] + hi[0]) * 0.5f, lo[1] + (hi[1] - lo[1]) * at, lo[2] + (hi[2] - lo[2]) * high };
            float handle[3]{};
            if (rocklink::boltHandle(handle)) {
                st.slideCenter = { handle[0], handle[1], handle[2] };   // a charging handle: the hand goes to the handle itself
            }
            if (st.zoneSlide) {
                st.slideCenter = (st.slideLo + st.slideHi) * 0.5f;   // where the slide box was drawn in Blender
            }
            st.slideCenterKnown = true;
            st.slidePointInBolt = relative(bolt->world, pointTransform(compose(root->world, pointTransform(st.slideCenter)).translate)).translate;
            st.slidePointKnown = true;
            note("ROCK box of the slide: (%.1f %.1f %.1f)..(%.1f %.1f %.1f); the hand takes it at (%.1f, %.1f, %.1f)", lo[0], lo[1], lo[2], hi[0], hi[1],
                hi[2], st.slideCenter.x, st.slideCenter.y, st.slideCenter.z);
        }
    }

    RE::NiPoint3 slideGrabPoint(const RE::NiAVObject* root, const RE::NiAVObject* bolt) noexcept
    {
        if (!g_state.slideCenterKnown) {
            return bolt->world.translate;
        }
        return compose(root->world, pointTransform(g_state.slideCenter)).translate + axisOf(root->world, 1) * (-g_state.rackPull * gunScale(root));
    }
}
