// ManualReloadF: rotations as 3x3 arrays, and the angle convention of Papyrus SetAngle (found at runtime)
#pragma once

#include <RE/Fallout.h>

#include <array>

namespace mrf
{
    // Papyrus SetAngle takes three angles whose order and signs are not documented for this
    // engine. All 24 plausible conventions are tried against the first magazine that gets 3D
    // (its reference angles vs. its node rotation); the best one is used from then on.
    using Mat = std::array<std::array<float, 3>, 3>;   // rows = local axes in world (as NiMatrix3)
    Mat toMat(const RE::NiMatrix3& m) noexcept;
    RE::NiMatrix3 fromMat(const Mat& m) noexcept;
    Mat mul(const Mat& a, const Mat& b) noexcept;
    Mat transposed(const Mat& a) noexcept;
    Mat axisRotation(int axis, float angle) noexcept;
    // convention: bits 0..2 order, bit 3 sign, bit 4 transpose
    Mat fromAngles(int convention, const std::array<float, 3>& angles) noexcept;
    float matError(const Mat& a, const Mat& b) noexcept;
    extern int g_convention;   // the SetAngle convention in use (0..23)
    extern bool g_conventionKnown;
    // Angles (radians) whose rotation under the convention is `target`: a few Gauss-Newton runs.
    std::array<float, 3> solveAngles(int convention, const Mat& target) noexcept;
    // A reference whose angles we set and whose 3D we can see tells which convention the game uses.
    void learnConvention(const RE::TESObjectREFR* ref, const RE::NiAVObject* root) noexcept;
    // Degrees for SetAngle that turn an object to `rotation` (rows = its axes in world).
    std::array<float, 3> anglesFor(const RE::NiMatrix3& rotation) noexcept;
}
