#include "Nodes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Angles.h"
#include "NiMath.h"
#include "SafeMemory.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // Places an object's 3D: root at `world`, children follow.
    void place3D(RE::NiAVObject* root, const RE::NiTransform& world) noexcept
    {
        if (!looksLikeObject(root)) {
            return;
        }
        const auto moveBound = [](RE::NiAVObject* object, const RE::NiTransform& before) {
            auto& bound = object->worldBound;
            if (bound.fRadius > 0.0f && bound.fRadius < 1000.0f) {
                const auto local = relative(before, pointTransform(bound.center)).translate;
                bound.center = compose(object->world, pointTransform(local)).translate;
            }
        };
        auto* parent = root->parent;
        if (looksLikeObject(parent)) {
            root->local = relative(parent->world, world);
        } else {
            root->local = world;
        }
        const auto rootBefore = root->world;
        root->world = world;
        moveBound(root, rootBefore);
        forEachNode(root, [&](RE::NiAVObject* object, int depth) {
            if (depth == 0) {
                return true;
            }
            auto* up = object->parent;
            if (looksLikeObject(up)) {
                const auto before = object->world;
                object->world = compose(up->world, object->local);
                moveBound(object, before);
            }
            return true;
        });
    }

    // A transform as ROCK's API takes it.

    rocklink::Pose toLinkPose(const RE::NiTransform& t) noexcept
    {
        rocklink::Pose pose{};
        const auto m = toMat(t.rotate);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                pose.rotate[r * 3 + c] = m[r][c];
            }
        }
        pose.translate[0] = t.translate.x;
        pose.translate[1] = t.translate.y;
        pose.translate[2] = t.translate.z;
        pose.scale = t.scale > 0.0f ? t.scale : 1.0f;
        return pose;
    }

    // The nodes above must reach as far as a part carried away from the gun, or it is culled with them.

    void reachAncestors(RE::NiAVObject* node, const RE::NiPoint3& point) noexcept
    {
        const float reach = node->worldBound.fRadius > 0.0f && node->worldBound.fRadius < 1000.0f ? node->worldBound.fRadius : 10.0f;
        RE::NiAVObject* up = node->parent;
        for (int i = 0; i < 8 && looksLikeObject(up); ++i) {
            auto& bound = up->worldBound;
            const float needed = distance(bound.center, point) + reach;
            if (bound.fRadius > 0.0f && bound.fRadius < needed && needed < 400.0f) {
                bound.fRadius = needed;
            }
            auto* next = up->parent;
            up = looksLikeObject(next) ? next : nullptr;
        }
    }

    // Places a node's world transform, recomputes everything below it and moves the bounds along
    // (a shape whose bound stays behind is culled as if it were still there).

    void placeNode(RE::NiAVObject* node, const RE::NiTransform& world) noexcept
    {
        if (!looksLikeObject(node)) {
            return;
        }
        const auto moveBound = [](RE::NiAVObject* object, const RE::NiTransform& before) {
            auto& bound = object->worldBound;
            if (bound.fRadius > 0.0f && bound.fRadius < 1000.0f) {
                const auto local = relative(before, pointTransform(bound.center)).translate;
                bound.center = compose(object->world, pointTransform(local)).translate;
            }
        };
        const auto rootBefore = node->world;
        node->world = world;
        moveBound(node, rootBefore);
        forEachNode(node, [&](RE::NiAVObject* object, int depth) {
            if (depth == 0) {
                return true;
            }
            if (auto* up = object->parent; looksLikeObject(up)) {
                const auto before = object->world;
                object->world = compose(up->world, object->local);
                moveBound(object, before);
            }
            return true;
        });
        reachAncestors(node, world.translate);
    }

    // A node somebody else has placed away from the gun (ROCK holds the magazine): its bounds are brought along,
    // or it is culled as soon as it leaves the gun's own bound.

    void carryBounds(RE::NiAVObject* node, const RE::NiPoint3& middle) noexcept
    {
        forEachNode(node, [&](RE::NiAVObject* object, int) {
            auto& bound = object->worldBound;
            if (bound.fRadius > 0.0f && bound.fRadius < 1000.0f) {
                bound.center = middle;
            }
            return true;
        });
        reachAncestors(node, middle);
    }

    // Recomputes the world transforms below `node` from its parent, then moves the subtree by `shift`.

    void refreshSubtree(RE::NiAVObject* node, const RE::NiPoint3& shift) noexcept
    {
        if (!looksLikeObject(node) || !looksLikeObject(node->parent)) {
            return;
        }
        node->world = compose(node->parent->world, node->local);
        node->world.translate += shift;
        forEachNode(node, [&](RE::NiAVObject* object, int depth) {
            if (depth == 0) {
                return true;
            }
            if (auto* up = object->parent; looksLikeObject(up)) {
                object->world = compose(up->world, object->local);
            }
            return true;
        });
    }
}
