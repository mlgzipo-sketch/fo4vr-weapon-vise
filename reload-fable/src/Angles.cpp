#include "Angles.h"

#include "Runtime.h"
#include "SafeMemory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mrf
{
    using manualreload::runtime::logNote;

    // Papyrus SetAngle takes three angles whose order and signs are not documented for this
    // engine. All 24 plausible conventions are tried against the first magazine that gets 3D
    // (its reference angles vs. its node rotation); the best one is used from then on.

    using Mat = std::array<std::array<float, 3>, 3>;   // rows = local axes in world (as NiMatrix3)

    Mat toMat(const RE::NiMatrix3& m) noexcept
    {
        Mat r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = m.entry[i][j];
        return r;
    }

    RE::NiMatrix3 fromMat(const Mat& m) noexcept
    {
        RE::NiMatrix3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.entry[i][j] = m[i][j];
        return r;
    }

    Mat mul(const Mat& a, const Mat& b) noexcept
    {
        Mat r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        return r;
    }

    Mat transposed(const Mat& a) noexcept
    {
        Mat r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = a[j][i];
        return r;
    }

    Mat axisRotation(int axis, float angle) noexcept
    {
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        Mat r{};
        r[axis][axis] = 1.0f;
        const int a = (axis + 1) % 3;
        const int b = (axis + 2) % 3;
        r[a][a] = c;
        r[a][b] = -s;
        r[b][a] = s;
        r[b][b] = c;
        return r;
    }

    constexpr std::array<std::array<int, 3>, 6> kOrders{ { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } } };

    // convention: bits 0..2 order, bit 3 sign, bit 4 transpose
    Mat fromAngles(int convention, const std::array<float, 3>& angles) noexcept
    {
        const auto& order = kOrders[convention % 6];
        const float sign = (convention / 6) % 2 ? -1.0f : 1.0f;
        Mat r = axisRotation(order[0], sign * angles[order[0]]);
        r = mul(r, axisRotation(order[1], sign * angles[order[1]]));
        r = mul(r, axisRotation(order[2], sign * angles[order[2]]));
        return convention / 12 ? transposed(r) : r;
    }

    float matError(const Mat& a, const Mat& b) noexcept
    {
        float e = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                e += (a[i][j] - b[i][j]) * (a[i][j] - b[i][j]);
        return e;
    }

    int g_convention{ 6 + 12 };   // a guess (negated XYZ, transposed) until a magazine shows which one is right
    bool g_conventionKnown{ false };

    // Angles (radians) whose rotation under the convention is `target`: a few Gauss-Newton runs.
    std::array<float, 3> solveAngles(int convention, const Mat& target) noexcept
    {
        std::array<float, 3> best{};
        float bestError = 1e9f;
        const std::array<std::array<float, 3>, 4> seeds{ { { 0, 0, 0 }, { 1.5f, 0, 0 }, { 0, 1.5f, 0 }, { 0, 0, 2.5f } } };
        for (auto angles : seeds) {
            for (int iteration = 0; iteration < 40; ++iteration) {
                const Mat now = fromAngles(convention, angles);
                const float error = matError(now, target);
                if (error < 1e-8f) {
                    break;
                }
                // J^T J dx = -J^T r over the 9 entries
                std::array<std::array<float, 9>, 3> jac{};
                for (int k = 0; k < 3; ++k) {
                    auto moved = angles;
                    moved[k] += 1e-3f;
                    const Mat m = fromAngles(convention, moved);
                    for (int i = 0; i < 9; ++i) {
                        jac[k][i] = (m[i / 3][i % 3] - now[i / 3][i % 3]) / 1e-3f;
                    }
                }
                float a[3][4]{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        for (int i = 0; i < 9; ++i) a[r][c] += jac[r][i] * jac[c][i];
                    }
                    a[r][r] += 1e-4f;
                    for (int i = 0; i < 9; ++i) a[r][3] -= jac[r][i] * (now[i / 3][i % 3] - target[i / 3][i % 3]);
                }
                for (int p = 0; p < 3; ++p) {   // Gauss elimination
                    int pivot = p;
                    for (int r = p + 1; r < 3; ++r) if (std::fabs(a[r][p]) > std::fabs(a[pivot][p])) pivot = r;
                    for (int c = 0; c < 4; ++c) std::swap(a[p][c], a[pivot][c]);
                    if (std::fabs(a[p][p]) < 1e-9f) break;
                    for (int r = 0; r < 3; ++r) {
                        if (r == p) continue;
                        const float f = a[r][p] / a[p][p];
                        for (int c = p; c < 4; ++c) a[r][c] -= f * a[p][c];
                    }
                }
                for (int k = 0; k < 3; ++k) {
                    if (std::fabs(a[k][k]) > 1e-9f) angles[k] += std::clamp(a[k][3] / a[k][k], -0.5f, 0.5f);
                }
            }
            const float error = matError(fromAngles(convention, angles), target);
            if (error < bestError) {
                bestError = error;
                best = angles;
            }
        }
        return best;
    }

    // A reference whose angles we set and whose 3D we can see tells which convention the game uses.
    void learnConvention(const RE::TESObjectREFR* ref, const RE::NiAVObject* root) noexcept
    {
        if (g_conventionKnown || !ref || !looksLikeObject(root)) {
            return;
        }
        const std::array<float, 3> angles{ ref->data.angle.x, ref->data.angle.y, ref->data.angle.z };
        if (std::fabs(angles[0]) + std::fabs(angles[1]) < 0.05f) {
            return;   // a turn about Z alone cannot tell the orders apart
        }
        const Mat actual = toMat(root->world.rotate);
        int best = -1;
        float bestError = 1e9f;
        for (int convention = 0; convention < 24; ++convention) {
            const float error = matError(fromAngles(convention, angles), actual);
            if (error < bestError) {
                bestError = error;
                best = convention;
            }
        }
        char message[160]{};
        std::snprintf(message, sizeof(message), "Angle convention: %d fits with error %.4f (angles %.2f %.2f %.2f)",
            best, bestError, angles[0], angles[1], angles[2]);
        logNote(message);
        if (best >= 0 && bestError < 0.02f) {
            g_convention = best;
            g_conventionKnown = true;
        }
    }

    // Degrees for SetAngle that turn an object to `rotation` (rows = its axes in world).
    std::array<float, 3> anglesFor(const RE::NiMatrix3& rotation) noexcept
    {
        const auto radians = solveAngles(g_convention, toMat(rotation));
        return { radians[0] * 57.29578f, radians[1] * 57.29578f, radians[2] * 57.29578f };
    }
}
