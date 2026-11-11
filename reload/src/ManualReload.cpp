// ManualReload: hand reloading for Fallout 4 VR (Into the Radius style), 10mm pistol first.
//
// What the probes (0.0.1, 0.0.2) proved:
//   - the rounds in the gun are EquippedWeaponData::ammoCount and a plugin may set them;
//   - VR refills the gun without calling the player's ReloadWeapon vfunc, so the game's
//     reload is undone by putting the count back whenever it rises without us;
//   - VR reports reloading as gunState 8 (the button press).
//
// 0.1:
//   - reload button with a magazine in and a round chambered (or an empty magazine): the
//     magazine drops out of the gun as a real object that keeps its rounds;
//   - grip with the free hand at the belt: a full magazine appears in the palm and ROCK
//     catches it;
//   - bring a magazine to the empty magazine well: a green ghost shows; let go and it
//     snaps in;
//   - reload button with a magazine in but nothing chambered: racks the slide (until the
//     slide can be pulled by hand).
//
// The plugin watches hands and the gun; ManualReloadMain.psc does what needs the game's
// records (spawning and deleting magazines, ghosts, sounds), polling PollCommand().
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"
#include "ROCK/Client.h"
#include "ROCK/Grab.h"
#include "ROCK/Input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ settings

    struct Settings
    {
        bool  blockReload{ true };
        int   reloadGunState{ 8 };      // raw gunState of VR while reloading
        int   idleGunState{ 12 };       // raw gunState of VR at rest (the moving parts are learned then)
        int   dumpDepth{ 12 };
        int   gunHand{ 0 };             // 0 = right hand holds the gun, 1 = left
        int   gripButton{ 2 };          // OpenVR button id of the grip
        bool  haptics{ true };
        bool  messages{ true };
        float beltLow{ 55.0f };         // belt zone: palm height above the feet, from..to (game units)
        float beltHigh{ 80.0f };
        float beltRadius{ 22.0f };      // ..and at most this far from the body sideways
        float extendDistance{ 3.0f };   // how far the magazine sticks out while the reload button is held
        float dropDistance{ 9.0f };     // how far it slides before it falls free
        float slideSpeed{ 45.0f };      // units per second, out and in
        float wellSide{ 6.0f };         // a magazine must come from below: at most this far off the well's axis
        float freezeSeconds{ 1.6f };    // the game's reload animation is held still this long after a press
        int   triggerButton{ 33 };
        bool  buttonRack{ false };      // reload button racks the slide when the chamber is empty
        float slideGrabRadius{ 12.0f };  // the free hand this close to the slide takes it
        float rackTravel{ 5.0f };       // how far the slide goes back
        float holdRotX{ 0.0f };         // magazine in the hand: turns in the hand bone's frame (degrees)
        float holdRotY{ 0.0f };
        float holdRotZ{ 0.0f };
        float holdUp{ 3.0f };           // ..and how far its top sits above the palm
        float ghostRadius{ 14.0f };     // a magazine this close to the well shows the ghost
        float snapRadius{ 5.0f };       // ..and this close goes in by itself, even still in the hand
        float magTilt{ -1.0f };
        float captureDepth{ 9.0f };     // the well catches a magazine whose top is this close below its mouth
        float slideLockAt{ 0.8f };
        bool  wellCenter{ true };       // put the well's mouth on the gun's middle plane
        float wellShiftX{ 0.0f };       // ..and move it by these (gun frame: x right, y forward, z up)
        float wellShiftY{ 0.0f };
        float wellShiftZ{ 0.0f };
        bool  rockOffhand{ true };      // ask ROCK to keep the free hand off the gun while it reloads      // a slide held open stands this part of its travel back         // the well's slope back from straight down (degrees); below 0 = learned from the model
        float handRadius{ 16.0f };      // a hand this close to a magazine is the one holding it
        float looseLifetime{ 90.0f };   // seconds a dropped magazine stays before it is cleaned up
        int   maxLoose{ 6 };
    };
    Settings g_settings;

    void loadSettings() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/ManualReload.ini");
            std::string line;
            while (file && std::getline(file, line)) {
                const auto equals = line.find('=');
                if (line.empty() || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
                    continue;
                }
                auto key = line.substr(0, equals);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                    key.pop_back();
                }
                const float value = std::strtof(line.c_str() + equals + 1, nullptr);
                auto& s = g_settings;
                if (key == "bBlockReload") s.blockReload = value != 0.0f;
                else if (key == "iReloadGunState") s.reloadGunState = static_cast<int>(value);
                else if (key == "iIdleGunState") s.idleGunState = static_cast<int>(value);
                else if (key == "fMagTilt") s.magTilt = value;
                else if (key == "fCaptureDepth") s.captureDepth = value;
                else if (key == "fSlideLockAt") s.slideLockAt = value;
                else if (key == "bWellCenter") s.wellCenter = value != 0.0f;
                else if (key == "fWellShiftX") s.wellShiftX = value;
                else if (key == "fWellShiftY") s.wellShiftY = value;
                else if (key == "fWellShiftZ") s.wellShiftZ = value;
                else if (key == "bRockOffhand") s.rockOffhand = value != 0.0f;
                else if (key == "iDumpDepth") s.dumpDepth = static_cast<int>(value);
                else if (key == "iGunHand") s.gunHand = value != 0.0f ? 1 : 0;
                else if (key == "iGripButton") s.gripButton = static_cast<int>(value);
                else if (key == "bHaptics") s.haptics = value != 0.0f;
                else if (key == "bMessages") s.messages = value != 0.0f;
                else if (key == "fBeltLow") s.beltLow = value;
                else if (key == "fBeltHigh") s.beltHigh = value;
                else if (key == "fBeltRadius") s.beltRadius = value;
                else if (key == "fGhostRadius") s.ghostRadius = value;
                else if (key == "fSnapRadius") s.snapRadius = value;
                else if (key == "fHandRadius") s.handRadius = value;
                else if (key == "fLooseLifetime") s.looseLifetime = value;
                else if (key == "iMaxLoose") s.maxLoose = static_cast<int>(value);
                else if (key == "fExtendDistance") s.extendDistance = value;
                else if (key == "fDropDistance") s.dropDistance = value;
                else if (key == "fSlideSpeed") s.slideSpeed = value;
                else if (key == "fWellSide") s.wellSide = value;
                else if (key == "fFreezeSeconds") s.freezeSeconds = value;
                else if (key == "iTriggerButton") s.triggerButton = static_cast<int>(value);
                else if (key == "bButtonRack") s.buttonRack = value != 0.0f;
                else if (key == "fSlideGrabRadius") s.slideGrabRadius = value;
                else if (key == "fRackTravel") s.rackTravel = value;
                else if (key == "fHoldRotX") s.holdRotX = value;
                else if (key == "fHoldRotY") s.holdRotY = value;
                else if (key == "fHoldRotZ") s.holdRotZ = value;
                else if (key == "fHoldUp") s.holdUp = value;
            }
        } catch (...) {
        }
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "Settings: bBlockReload=%d iReloadGunState=%d iGunHand=%d belt %.0f..%.0f r%.0f ghost %.0f snap %.0f",
            g_settings.blockReload ? 1 : 0, g_settings.reloadGunState, g_settings.gunHand,
            g_settings.beltLow, g_settings.beltHigh, g_settings.beltRadius, g_settings.ghostRadius, g_settings.snapRadius);
        logNote(message);
    }

    // ------------------------------------------------------------ memory safety

    bool readable(const void* address, std::size_t size) noexcept
    {
        if (!address) {
            return false;
        }
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != 0x1000u /* MEM_COMMIT */) {
            return false;
        }
        constexpr std::uint32_t mask = 0x02u | 0x04u | 0x08u | 0x20u | 0x40u;
        if ((info.protect & mask) == 0 || (info.protect & 0x100u) != 0) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        const auto end = reinterpret_cast<std::uintptr_t>(info.baseAddress) + info.regionSize;
        return start + size <= end;
    }

    bool looksLikeObject(const void* object) noexcept
    {
        if (!object || (reinterpret_cast<std::uintptr_t>(object) & 7) != 0 || !readable(object, sizeof(RE::NiAVObject))) {
            return false;
        }
        static const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
        const auto vtable = *static_cast<const std::uintptr_t*>(object);
        return vtable >= rdata.address() && vtable < rdata.address() + rdata.size();
    }

    std::string nodeName(const RE::NiAVObject* object) noexcept
    {
        if (!looksLikeObject(object)) {
            return {};
        }
        const char* text = object->GetName().data();
        if (!text || !readable(text, 1)) {
            return {};
        }
        std::size_t size = 0;
        while (size < 80 && readable(text + size, 1) && text[size] != '\0') {
            ++size;
        }
        return { text, size };
    }

    template <class F>
    void forEachNode(RE::NiAVObject* object, F&& visit, int depth = 0) noexcept
    {
        if (!looksLikeObject(object) || depth > 40) {
            return;
        }
        if (!visit(object, depth)) {
            return;
        }
        auto* node = object->IsNode();
        if (!node) {
            return;
        }
        auto& children = node->GetRuntimeData().children;
        const auto count = children.size();
        if (count > 256) {
            return;
        }
        for (std::uint16_t index = 0; index < count; ++index) {
            forEachNode(children[index].get(), visit, depth + 1);
        }
    }

    RE::NiAVObject* findNamed(RE::NiAVObject* root, const char* name) noexcept
    {
        RE::NiAVObject* found = nullptr;
        forEachNode(root, [&](RE::NiAVObject* object, int) {
            if (found) {
                return false;
            }
            if (nodeName(object) == name) {
                found = object;
                return false;
            }
            return true;
        });
        return found;
    }

    void setCulled(RE::NiAVObject* object, bool culled) noexcept
    {
        if (!looksLikeObject(object)) {
            return;
        }
        if (culled) {
            object->flags.flags |= 1ull;
        } else {
            object->flags.flags &= ~1ull;
        }
    }

    // ------------------------------------------------------------ the gun

    struct Gun
    {
        RE::TESObjectWEAP*      weapon{ nullptr };
        RE::EquippedWeaponData* data{ nullptr };
        RE::TESObjectWEAP::InstanceData* instance{ nullptr };
    };

    Gun equippedGun(RE::PlayerCharacter* player) noexcept
    {
        Gun gun;
        auto* process = player ? player->currentProcess : nullptr;
        if (!process || !readable(process, sizeof(RE::AIProcess))) {
            return gun;
        }
        auto* middle = process->middleHigh;
        if (!middle || !readable(middle, sizeof(RE::MiddleHighProcessData))) {
            return gun;
        }
        auto& items = middle->equippedItems;
        if (items.size() > 16) {
            return gun;
        }
        for (auto& item : items) {
            auto* form = item.item.object;
            if (!form || !readable(form, sizeof(RE::TESForm)) || form->GetFormType() != RE::ENUM_FORM_ID::kWEAP) {
                continue;
            }
            gun.weapon = static_cast<RE::TESObjectWEAP*>(form);
            gun.instance = static_cast<RE::TESObjectWEAP::InstanceData*>(item.item.instanceData.get());
            auto* data = item.data.get();
            if (data && readable(data, sizeof(RE::EquippedWeaponData))) {
                gun.data = static_cast<RE::EquippedWeaponData*>(data);
            }
            break;
        }
        return gun;
    }

    std::uint32_t inventoryCount(RE::PlayerCharacter* player, const RE::TESForm* object) noexcept
    {
        auto* list = player ? player->inventoryList : nullptr;
        if (!object || !list || !readable(list, sizeof(RE::BGSInventoryList))) {
            return 0;
        }
        std::uint32_t total = 0;
        for (auto& item : list->data) {
            if (item.object != object) {
                continue;
            }
            for (auto* stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
                total += stack->count;
            }
        }
        return total;
    }

    std::uint32_t capacityOf(const Gun& gun) noexcept
    {
        if (gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData))) {
            return gun.instance->ammoCapacity;
        }
        return 0;
    }

    RE::NiAVObject* weaponRoot(RE::PlayerCharacter* player) noexcept
    {
        // VR draws the gun on the first person skeleton.
        RE::NiAVObject* roots[2]{ player->firstPerson3D.get(), player->Get3D() };
        for (auto* root : roots) {
            if (auto* magazine = findNamed(root, "WeaponMagazine")) {
                // climb to the node called "Weapon" so the dump shows the whole gun
                RE::NiAVObject* at = magazine;
                for (int i = 0; i < 12 && at; ++i) {
                    if (nodeName(at) == "Weapon") {
                        return at;
                    }
                    auto* parent = at->parent;
                    at = looksLikeObject(parent) ? parent : nullptr;
                }
                return magazine->parent;
            }
        }
        return nullptr;
    }

    void dumpTree(RE::NiAVObject* root) noexcept
    {
        int lines = 0;
        forEachNode(root, [&](RE::NiAVObject* object, int depth) {
            if (depth > g_settings.dumpDepth || lines > 150) {
                return false;
            }
            ++lines;
            const auto& t = object->local.translate;
            char message[200]{};
            std::snprintf(message, sizeof(message), "  %*s%s%s  local (%.2f, %.2f, %.2f)%s",
                depth * 2, "", nodeName(object).c_str(), object->IsNode() ? "" : " [geom]", t.x, t.y, t.z,
                (object->flags.flags & 1ull) ? " culled" : "");
            logNote(message);
            return true;
        });
    }

    // ------------------------------------------------------------ math

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

    // ------------------------------------------------------------ angles for SetAngle
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

    // How a magazine sits in the free hand: its axes in the hand bone's frame.
    RE::NiMatrix3 holdRotation(const RE::NiMatrix3& hand) noexcept
    {
        const float d = 0.01745329f;
        Mat hold = axisRotation(0, g_settings.holdRotX * d);
        hold = mul(hold, axisRotation(1, g_settings.holdRotY * d));
        hold = mul(hold, axisRotation(2, g_settings.holdRotZ * d));
        // rows of hold are the magazine's axes in hand coordinates; world rows = hold * hand
        return fromMat(mul(hold, toMat(hand)));
    }

    // ------------------------------------------------------------ hands

    vr::IVRSystem* g_vr{ nullptr };

    struct Hand
    {
        bool           valid{ false };
        RE::NiTransform frame{};
        RE::NiPoint3   palm{};
        bool           grip{ false };
        bool           previousGrip{ false };
        std::uint64_t  buttons{ 0 };
        RE::NiMatrix3  bone{};          // the rendered hand bone's rotation
        RE::NiPoint3   fingers{};       // wrist -> index tip, normalized
    };
    std::array<Hand, 2> g_hands;   // 0 right, 1 left

    // ------------------------------------------------------------ ROCK
    // ROCK holds the hands. Through its API (ROCKAPI_QueryInterfaceV1) the free hand is kept off the gun's
    // support grip while it works the slide or a magazine, and a magazine made in the palm is put in the hand.
    namespace rock
    {
        namespace api = ::rock::api;

        api::Client         g_client;
        const api::grab::ApiV1*  g_grab{ nullptr };
        const api::input::ApiV1* g_input{ nullptr };
        bool g_tried{ false };
        bool g_reserved{ false };
        std::array<bool, 2> g_suppressed{};

        api::Hand toRock(int index) noexcept
        {
            return index == 0 ? api::Hand::Right : api::Hand::Left;
        }

        bool ready() noexcept
        {
            if (g_client.owner()) {
                return true;
            }
            if (g_tried) {
                return false;
            }
            g_tried = true;
            const auto module = GetModuleHandleA("ROCK.dll");
            const auto query = module ? reinterpret_cast<api::QueryInterfaceV1>(GetProcAddress(module, api::kQueryExportName)) : nullptr;
            char message[200]{};
            if (!query) {
                std::snprintf(message, sizeof(message), "ROCK API: %s", module ? "ROCK.dll has no ROCKAPI_QueryInterfaceV1" : "ROCK.dll is not loaded");
                logNote(message);
                return false;
            }
            const auto connected = g_client.connect(query, "ManualReload");
            if (connected != api::Status::Ok) {
                std::snprintf(message, sizeof(message), "ROCK API: connect answered %u", static_cast<std::uint32_t>(connected));
                logNote(message);
                return false;
            }
            const auto grab = g_client.acquire(api::grab::kSupportedPermissions, g_grab);
            const auto input = g_client.acquire(api::input::kSupportedPermissions, g_input);
            std::snprintf(message, sizeof(message), "ROCK API connected: grab %u, input %u", static_cast<std::uint32_t>(grab), static_cast<std::uint32_t>(input));
            logNote(message);
            return true;
        }

        bool generations(std::uint32_t& world, std::uint32_t& skeleton, std::uint32_t& provider) noexcept
        {
            if (!ready() || !g_client.core() || !g_client.core()->getFrameSnapshot) {
                return false;
            }
            api::core::SnapshotV1 snap{};
            if (g_client.core()->getFrameSnapshot(g_client.owner(), &snap) != api::Status::Ok) {
                return false;
            }
            world = snap.worldGeneration;
            skeleton = snap.skeletonGeneration;
            provider = snap.providerGeneration;
            return world != 0 && skeleton != 0 && provider != 0;
        }

        void logOnce(const char* what, api::Status status) noexcept
        {
            static std::array<std::uint32_t, 12> seen{};
            static int count = 0;
            const auto value = static_cast<std::uint32_t>(status);
            if (status == api::Status::Ok || count >= 12) {
                return;
            }
            for (int i = 0; i < count; ++i) {
                if (seen[i] == value * 7 + static_cast<std::uint32_t>(what[0])) {
                    return;
                }
            }
            seen[count++] = value * 7 + static_cast<std::uint32_t>(what[0]);
            char message[120]{};
            std::snprintf(message, sizeof(message), "ROCK %s answered %u", what, value);
            logNote(message);
        }

        // Keep the free hand off the gun's support grip (ROCK's reload reservation), renewed every frame.
        void reserveOffhand(bool on) noexcept
        {
            if (!ready() || !g_grab) {
                return;
            }
            if (!on) {
                if (g_reserved) {
                    (void)g_grab->releaseOffhandReservationV1(g_client.owner());
                    g_reserved = false;
                    logInfo("Free hand given back to ROCK");
                }
                return;
            }
            api::grab::OffhandReservationRequestV1 request{};
            if (!generations(request.worldGeneration, request.skeletonGeneration, request.providerGeneration)) {
                return;
            }
            request.reservation = api::grab::OffhandReservation::ReloadReserved;
            request.leaseFrames = 6;
            auto status = g_reserved ? g_grab->renewOffhandReservationV1(g_client.owner(), &request)
                                     : g_grab->acquireOffhandReservationV1(g_client.owner(), &request);
            if (status != api::Status::Ok && g_reserved) {
                status = g_grab->acquireOffhandReservationV1(g_client.owner(), &request);
            }
            logOnce("offhand reservation", status);
            const bool was = g_reserved;
            g_reserved = status == api::Status::Ok;
            if (g_reserved && !was) {
                logInfo("Free hand reserved from the gun's support grip");
            }
        }

        // ROCK ignores this hand's grab presses (our own grip check still sees them).
        void suppressGrab(int index, bool on) noexcept
        {
            if (!ready() || !g_input) {
                return;
            }
            if (!on) {
                if (g_suppressed[index]) {
                    (void)g_input->clearHandInputSuppressionV1(g_client.owner(), toRock(index));
                    g_suppressed[index] = false;
                }
                return;
            }
            api::input::HandInputSuppressionRequestV1 request{};
            if (!generations(request.worldGeneration, request.skeletonGeneration, request.providerGeneration)) {
                return;
            }
            request.hand = toRock(index);
            request.flags = static_cast<std::uint32_t>(api::input::HandInputSuppressionFlagV1::SuppressNormalGrabPress);
            request.leaseFrames = 6;
            const auto status = g_input->setHandInputSuppressionV1(g_client.owner(), &request);
            logOnce("grab suppression", status);
            g_suppressed[index] = status == api::Status::Ok;
        }

        // Put a reference into a hand.
        void forceGrab(int index, std::uint32_t formId, const RE::NiPoint3& at) noexcept
        {
            if (!ready() || !g_grab) {
                return;
            }
            api::grab::ForceGrabRequestV1 request{};
            if (!generations(request.worldGeneration, request.skeletonGeneration, request.providerGeneration)) {
                return;
            }
            request.hand = toRock(index);
            request.targetFormId = formId;
            request.maxDistanceGame = 40.0f;
            request.preferredGrabPointGame[0] = at.x;
            request.preferredGrabPointGame[1] = at.y;
            request.preferredGrabPointGame[2] = at.z;
            std::uint64_t command = 0;
            const auto status = g_grab->requestForceGrabV1(g_client.owner(), &request, &command);
            char message[120]{};
            std::snprintf(message, sizeof(message), "ROCK force grab of %08X into the %s hand: %u", formId, index == 0 ? "right" : "left", static_cast<std::uint32_t>(status));
            logInfo(message);
        }
    }

    vr::TrackedDeviceIndex_t controllerOf(int index) noexcept
    {
        if (!g_vr) {
            return vr::k_unTrackedDeviceIndexInvalid;
        }
        return g_vr->GetTrackedDeviceIndexForControllerRole(index == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    }

    void buzz(int index, unsigned short microseconds) noexcept
    {
        const auto device = controllerOf(index);
        if (g_settings.haptics && device != vr::k_unTrackedDeviceIndexInvalid) {
            g_vr->TriggerHapticPulse(device, 0, microseconds);
        }
    }

    void readHand(int index) noexcept
    {
        auto& hand = g_hands[index];
        hand.previousGrip = hand.grip;
        hand.valid = false;
        hand.grip = false;
        hand.buttons = 0;
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->getTrackedHandTransform) {
            return;
        }
        const auto which = index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left;
        RE::NiTransform wand;
        if (!frik->getTrackedHandTransform(which, FRIKApiV2::TrackedHandKind::Wand, &wand)) {
            return;
        }
        hand.frame = wand;
        hand.frame.scale = 1.0f;
        hand.palm = wand.translate;
        hand.bone = wand.rotate;
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            const float length = distance(tip, wrist);
            if (length < 40.0f && length > 0.1f) {
                hand.palm = (tip + wrist) * 0.5f;
                hand.bone = chain.hand.rotate;
                hand.fingers = (tip - wrist) * (1.0f / length);
            }
        }
        hand.valid = true;
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (g_vr->GetControllerState(device, &state, sizeof(state))) {
                hand.buttons = state.ulButtonPressed;
                hand.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
            }
        }
    }

    void hud(const char* text) noexcept
    {
        if (g_settings.messages && text) {
            RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false, false);
        }
    }

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept
    {
        if (!id) {
            return nullptr;
        }
        auto* form = RE::TESForm::GetFormByID(id);
        return form ? form->As<RE::TESObjectREFR>() : nullptr;
    }

    // ------------------------------------------------------------ talking to the script

    enum Command : std::int32_t
    {
        kNone = 0,
        kSpawn = 1,    // kind, pose, token: a magazine as a physical object
        kDelete = 2,   // ref
        kSound = 3,    // kind = sound index: 0 snap in, 1 drop out, 2 slide
        kGhosts = 4,   // make the ghost models (once per save)
    };

    struct PendingCommand
    {
        std::int32_t type{ kNone };
        std::int32_t kind{ 0 };
        std::uint32_t ref{ 0 };
        std::int32_t token{ 0 };
        std::array<float, 6> pose{};   // x, y, z, angle x, y, z (degrees)
    };

    struct NewMag
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
    };

    struct Shared
    {
        std::mutex lock;
        std::deque<PendingCommand> commands;
        PendingCommand current;
        std::vector<NewMag> newMags;
        std::vector<std::pair<std::uint32_t, std::int32_t>> newGhosts;   // ref, kind
    } g_shared;

    void pushCommand(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
    }

    // ------------------------------------------------------------ state

    constexpr std::uint32_t k10mmPistol = 0x00004822;   // Fallout4.esm
    constexpr int kKinds = 2;                             // 0 standard magazine, 1 large

    struct Loose
    {
        std::uint32_t     rounds{ 0 };
        int               kind{ 0 };
        Clock::time_point born{};
        bool              fromBelt{ false };
        bool              awayFromWell{ false };   // has been clear of the well since it came out
    };

    struct Pending
    {
        std::uint32_t rounds{ 0 };
        int           kind{ 0 };
        bool          fromBelt{ false };
        RE::NiMatrix3 rotation{};
        int           hand{ -1 };      // made in this hand's palm: ROCK is asked to hold it
    };

    struct State
    {
        RE::TESObjectWEAP* weapon{ nullptr };
        bool               managed{ false };     // a gun this mod reloads
        std::uint32_t      lastAmmo{ 0xFFFFFFFF };
        std::uint32_t      lastGunState{ 0xFFFFFFFF };
        bool               magIn{ true };
        bool               chamber{ false };
        std::uint32_t      magRounds{ 0 };
        int                kind{ 0 };
        bool               dumped{ false };
        Clock::time_point  equippedAt{};
        Clock::time_point  lastBelt{};
        RE::NiTransform    wellLocal{};          // P-Mag relative to the gun root
        bool               wellKnown{ false };
        bool               ghostsAsked{ false };
        // the magazine's way out and in
        int                phase{ 0 };            // Phase
        float              offset{ 0.0f };        // how far below its place, along the gun's down axis
        std::uint32_t      outRounds{ 0 };        // rounds in the magazine on its way out
        std::uint64_t      heldMask{ 0 };         // buttons down when reload was pressed
        Clock::time_point  freezeUntil{};
        Clock::time_point  lastFrame{};
        // the moving parts at rest, to hold the game's reload animation still
        std::array<RE::NiTransform, 5> idle{};
        bool               idleKnown{ false };
        // the slide in the free hand
        bool               racking{ false };
        float              rackPull{ 0.0f };
        float              rackGrabY{ 0.0f };
        float              rackGrabHands{ 0.0f };   // distance between the palms when the slide was taken
        float              rackGrabPull{ 0.0f };
        bool               rackCycled{ false };
        bool               slideLocked{ false };  // held back after the last shot until racked
        // a magazine caught by the well, pushed in by the hand
        std::uint32_t      guidedRounds{ 0 };
        int                guidedHand{ 1 };
        float              guidedStart{ 0.0f };    // offset when it was caught
        float              guidedHandStart{ 0.0f };   // the hand's place along the well then
        RE::NiTransform    magnetFrom{};           // where the caught magazine was in the hand
        Clock::time_point  magnetStart{};
        // the gun's magazine node relative to where a loose magazine sits when seated
        RE::NiTransform    nodeInWell{};
        bool               nodeInWellKnown{ false };
        // the gun's magazine drawn in the hand until the real one comes (spawning takes a moment)
        std::int32_t       phantomToken{ 0 };
        int                phantomHand{ 1 };
        std::uint32_t      phantomRounds{ 0 };
        Clock::time_point  phantomSince{};
        Clock::time_point  lastShot{};
        // learned from the gun at rest, in the gun root's frame
        RE::NiPoint3       magAxis{ 0.0f, 0.0f, -1.0f };   // the way the magazine leaves the well
        bool               magAxisKnown{ false };
        RE::NiPoint3       slideCenter{};                   // middle of the slide
        bool               slideCenterKnown{ false };
    };
    State g_state;

    enum Phase : int
    {
        kIn = 0,
        kExtending = 1,    // sticking out while the reload button is held; the free hand may take it
        kSlidingOut = 2,
        kOut = 3,
        kSlidingIn = 4,
        kGuided = 5,       // caught by the well: the hand pushes it up along the well
    };
    constexpr std::array<const char*, 5> kMovingParts{ "WeaponMagazine", "WeaponBolt", "WeaponExtra1", "WeaponExtra2", "WeaponExtra3" };

    std::unordered_map<std::uint32_t, Loose> g_loose;     // magazines lying around or in a hand
    std::unordered_map<std::int32_t, Pending> g_pending;  // spawn token -> what it will be
    std::int32_t g_nextToken{ 1 };
    std::array<std::uint32_t, kKinds> g_ghostRef{};
    std::array<RE::NiAVObject*, kKinds> g_ghostDetached{};

    std::uint32_t looseRounds() noexcept
    {
        std::uint32_t total = 0;
        for (const auto& [ref, loose] : g_loose) {
            total += loose.rounds;
        }
        for (const auto& [token, pending] : g_pending) {
            total += pending.rounds;
        }
        return total;
    }

    void spawnMagazine(int kind, const RE::NiPoint3& at, const RE::NiMatrix3& rotation, std::uint32_t rounds, bool fromBelt, int hand = -1) noexcept
    {
        const auto token = g_nextToken++;
        g_pending[token] = Pending{ rounds, kind, fromBelt, rotation, hand };
        const auto angles = anglesFor(rotation);
        PendingCommand command;
        command.type = kSpawn;
        command.kind = kind;
        command.token = token;
        command.pose = { at.x, at.y, at.z, angles[0], angles[1], angles[2] };
        pushCommand(command);
        char message[160]{};
        std::snprintf(message, sizeof(message), "Spawn magazine kind %d with %u rounds (%s) at (%.0f, %.0f, %.0f)",
            kind, rounds, fromBelt ? "belt" : "ejected", at.x, at.y, at.z);
        logNote(message);
    }

    void deleteMagazine(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
        g_loose.erase(ref);
    }

    void playSound(int index) noexcept
    {
        PendingCommand command;
        command.type = kSound;
        command.kind = index;
        pushCommand(command);
    }

    // ------------------------------------------------------------ the script's side

    std::int32_t PollCommand(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        if (g_shared.commands.empty()) {
            g_shared.current = {};
            return 0;
        }
        g_shared.current = g_shared.commands.front();
        g_shared.commands.pop_front();
        return g_shared.current.type;
    }

    std::int32_t CommandKind(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return g_shared.current.kind;
    }

    std::int32_t CommandRefID(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return static_cast<std::int32_t>(g_shared.current.ref);
    }

    std::int32_t CommandToken(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return g_shared.current.token;
    }

    std::vector<float> CommandPose(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.pose.begin(), g_shared.current.pose.end() };
    }

    void AddMag(std::monostate, RE::TESObjectREFR* ref, std::int32_t token)
    {
        if (!ref) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.newMags.push_back(NewMag{ ref->GetFormID(), token });
    }

    void AddGhost(std::monostate, RE::TESObjectREFR* ref, std::int32_t kind)
    {
        if (!ref || kind < 0 || kind >= kKinds) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.newGhosts.emplace_back(ref->GetFormID(), kind);
    }

    void Notify(std::monostate, std::int32_t message)
    {
        char text[96]{};
        std::snprintf(text, sizeof(text), "Script: %d", message);
        logNote(text);
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "ManualReload";
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandKind", CommandKind, true);
        vm->BindNativeMethod(kScript, "CommandRefID", CommandRefID, true);
        vm->BindNativeMethod(kScript, "CommandToken", CommandToken, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "AddMag", AddMag, true);
        vm->BindNativeMethod(kScript, "AddGhost", AddGhost, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        logNote("Papyrus functions of script ManualReload registered");
        return true;
    }

    // ------------------------------------------------------------ frame

    void takeScriptNews() noexcept
    {
        std::vector<NewMag> mags;
        std::vector<std::pair<std::uint32_t, std::int32_t>> ghosts;
        {
            std::scoped_lock guard(g_shared.lock);
            mags.swap(g_shared.newMags);
            ghosts.swap(g_shared.newGhosts);
        }
        for (const auto& mag : mags) {
            const auto it = g_pending.find(mag.token);
            Loose loose;
            loose.born = Clock::now();
            if (it != g_pending.end()) {
                loose.rounds = it->second.rounds;
                loose.kind = it->second.kind;
                loose.fromBelt = it->second.fromBelt;
                auto* ref = lookupRef(mag.ref);
                auto* mag3D = ref ? ref->Get3D() : nullptr;
                const int hand = it->second.hand;
                if (hand >= 0 && hand < 2 && g_hands[hand].grip && looksLikeObject(mag3D)) {
                    rock::forceGrab(hand, mag.ref, mag3D->world.translate);
                }
                learnConvention(ref, mag3D);
                if (looksLikeObject(mag3D)) {
                    char message[120]{};
                    std::snprintf(message, sizeof(message), "Magazine turned as asked? error %.3f (convention %d%s)",
                        matError(toMat(mag3D->world.rotate), toMat(it->second.rotation)), g_convention, g_conventionKnown ? "" : ", guessed");
                    logNote(message);
                }
                g_pending.erase(it);
            }
            g_loose[mag.ref] = loose;
            char message[120]{};
            std::snprintf(message, sizeof(message), "Magazine %08X is out: %u rounds", mag.ref, loose.rounds);
            logNote(message);
        }
        for (const auto& [ref, kind] : ghosts) {
            g_ghostRef[kind] = ref;
            g_ghostDetached[kind] = nullptr;
            char message[96]{};
            std::snprintf(message, sizeof(message), "Ghost %d is %08X", kind, ref);
            logNote(message);
        }
    }

    void showGhost(int wanted, const RE::NiTransform& world) noexcept
    {
        for (int kind = 0; kind < kKinds; ++kind) {
            auto* ref = lookupRef(g_ghostRef[kind]);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(root)) {
                continue;
            }
            if (g_ghostDetached[kind] != root) {
                g_ghostDetached[kind] = root;
                ref->DetachHavok(root);
            }
            if (kind != wanted) {
                setCulled(root, true);
                continue;
            }
            setCulled(root, false);
            auto at = world;
            at.scale = 1.0f;
            place3D(root, at);
        }
    }

    void setAmmo(const Gun& gun, std::uint32_t value) noexcept
    {
        gun.data->ammoCount = value;
        g_state.lastAmmo = value;
    }

    // Recomputes the world transforms below `node` from its parent, then moves the whole
    // subtree by `shift` (world space). Local transforms are left as they are.
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
            auto* up = object->parent;
            if (looksLikeObject(up)) {
                object->world = compose(up->world, object->local);
            }
            return true;
        });
    }

    // The game fires while ammoCount > 0, so a gun with an empty chamber must read 0.
    void syncAmmo(const Gun& gun) noexcept
    {
        setAmmo(gun, g_state.chamber ? 1u + (g_state.magIn ? g_state.magRounds : 0u) : 0u);
    }

    bool inBelt(RE::PlayerCharacter* player, const RE::NiPoint3& palm, float* heightOut = nullptr, float* sideOut = nullptr) noexcept
    {
        const auto feet = player->GetPosition();
        const float height = palm.z - feet.z;
        const float dx = palm.x - feet.x;
        const float dy = palm.y - feet.y;
        const float side = std::sqrt(dx * dx + dy * dy);
        if (heightOut) *heightOut = height;
        if (sideOut) *sideOut = side;
        return height >= g_settings.beltLow && height <= g_settings.beltHigh && side <= g_settings.beltRadius;
    }

    // A magazine in the palm, turned the way a hand holds one, for ROCK to catch.
    void spawnInHand(const Hand& hand, std::uint32_t rounds, bool fromBelt) noexcept
    {
        const auto rotation = holdRotation(hand.bone);
        RE::NiTransform t;
        t.rotate = rotation;
        const auto up = axisOf(t, 2);
        spawnMagazine(g_state.kind, hand.palm + up * g_settings.holdUp, rotation, rounds, fromBelt, static_cast<int>(&hand - g_hands.data()));
        // which axis of the hand bone runs along the fingers (to tune fHoldRot*)
        RE::NiTransform bone;
        bone.rotate = hand.bone;
        const auto x = axisOf(bone, 0);
        const auto y = axisOf(bone, 1);
        const auto z = axisOf(bone, 2);
        const auto& f = hand.fingers;
        char message[200]{};
        std::snprintf(message, sizeof(message), "Hand bone vs fingers: x %.2f y %.2f z %.2f; up of the bone z: x %.2f y %.2f z %.2f",
            x.x * f.x + x.y * f.y + x.z * f.z, y.x * f.x + y.y * f.y + y.z * f.z, z.x * f.x + z.y * f.y + z.z * f.z, x.z, y.z, z.z);
        logNote(message);
    }

    // The reload button with a magazine in: it starts to come out.
    void startEject(const Gun& gun, const Clock::time_point now) noexcept
    {
        g_state.phase = kExtending;
        g_state.offset = 0.0f;
        g_state.outRounds = g_state.magRounds;
        g_state.magRounds = 0;
        g_state.magIn = false;
        g_state.heldMask = g_hands[g_settings.gunHand].buttons & ~vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton));
        setAmmo(gun, g_state.chamber ? 1u : 0u);
        buzz(g_settings.gunHand, 900);
        g_state.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        char message[160]{};
        std::snprintf(message, sizeof(message), "RELEASE: magazine with %u rounds comes out (buttons held %llX), chamber %s",
            g_state.outRounds, static_cast<unsigned long long>(g_state.heldMask), g_state.chamber ? "loaded" : "empty");
        logNote(message);
    }

    // The magazine leaves the gun: falls free, or into the free hand that took it.
    void handOver(const Hand& hand, std::uint32_t rounds) noexcept;

    void finishEject(RE::NiAVObject* root, RE::NiAVObject* magazine, const Hand* taker) noexcept
    {
        g_state.phase = kOut;
        setCulled(magazine, true);
        const auto rounds = g_state.outRounds;
        g_state.outRounds = 0;
        if (taker) {
            handOver(*taker, rounds);
            buzz(1 - g_settings.gunHand, 1500);
        } else if (looksLikeObject(magazine)) {
            (void)root;
            spawnMagazine(g_state.kind, magazine->world.translate, magazine->world.rotate, rounds, false);
            playSound(1);
        }
        char message[120]{};
        std::snprintf(message, sizeof(message), "EJECT: magazine with %u rounds %s", rounds, taker ? "taken by the hand" : "dropped");
        logNote(message);
    }

    void seated() noexcept
    {
        g_state.phase = kIn;
        g_state.offset = 0.0f;
        playSound(0);
        buzz(0, 2500);
        buzz(1, 2500);
        if (!g_state.chamber) {
            hud(g_state.magRounds > 0 ? "Магазин вставлен. Патронник пуст: передёрни затвор" : "Магазин пустой");
        }
    }

    // Where a loose magazine sits when it is seated, in the gun root's frame. The gun's own magazine node
    // stands off to the side of the visible magazine (x about -5.7 on the 10mm), so the mouth is put on the
    // gun's middle plane (the slide's), plus the ini shifts.
    RE::NiTransform wellFrame() noexcept
    {
        auto frame = g_state.wellLocal;
        if (g_settings.wellCenter && g_state.slideCenterKnown) {
            frame.translate.x = g_state.slideCenter.x;
        }
        frame.translate.x += g_settings.wellShiftX;
        frame.translate.y += g_settings.wellShiftY;
        frame.translate.z += g_settings.wellShiftZ;
        return frame;
    }

    // A point on the gun's magazine as it is drawn: its body, `offset` out of the well.
    RE::NiPoint3 magazineGrabPoint(const RE::NiAVObject* root) noexcept
    {
        const float scale = root->world.scale > 0.0f ? root->world.scale : 1.0f;
        return compose(root->world, wellFrame()).translate + directionToWorld(root->world, g_state.magAxis) * ((g_state.offset + 3.0f) * scale);
    }

    // The middle of the slide in the world, where it stands now.
    RE::NiPoint3 slideGrabPoint(const RE::NiAVObject* root, const RE::NiAVObject* bolt) noexcept
    {
        const float scale = root->world.scale > 0.0f ? root->world.scale : 1.0f;
        if (!g_state.slideCenterKnown) {
            return bolt->world.translate;
        }
        return compose(root->world, pointTransform(g_state.slideCenter)).translate + axisOf(root->world, 1) * (-g_state.rackPull * scale);
    }

    // A point's place along the well (0 = seated, more = further out) and its distance off the well's line.
    float alongWell(const RE::NiAVObject* root, const RE::NiPoint3& point, float* side = nullptr) noexcept
    {
        const auto rel = relative(root->world, pointTransform(point)).translate - wellFrame().translate;
        const float along = dot(rel, g_state.magAxis);
        if (side) {
            const auto off = rel - g_state.magAxis * along;
            *side = std::sqrt(dot(off, off));
        }
        return along;
    }

    // Places a node's world transform and recomputes everything below it.
    void placeSubtree(RE::NiAVObject* node, const RE::NiTransform& world) noexcept
    {
        if (!looksLikeObject(node)) {
            return;
        }
        node->world = world;
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

    // Draws the gun's own magazine where a loose magazine with `pose` would be.
    void placeGunMagazine(const RE::NiAVObject* root, RE::NiAVObject* magazine, RE::NiTransform pose) noexcept
    {
        if (!g_state.nodeInWellKnown || !looksLikeObject(root) || !looksLikeObject(magazine)) {
            return;
        }
        pose.scale = root->world.scale;
        placeSubtree(magazine, compose(pose, g_state.nodeInWell));
    }

    // The pose a loose magazine has `offset` out along the well.
    RE::NiTransform wellPose(const RE::NiAVObject* root, float offset) noexcept
    {
        auto pose = compose(root->world, wellFrame());
        pose.translate += directionToWorld(root->world, g_state.magAxis) * (offset * root->world.scale);
        return pose;
    }

    // Between two poses: t = 0 is a, 1 is b. Rotation by blended axes, made square again.
    RE::NiTransform blend(const RE::NiTransform& a, const RE::NiTransform& b, float t) noexcept
    {
        RE::NiTransform r = b;
        r.translate = a.translate + (b.translate - a.translate) * t;
        std::array<RE::NiPoint3, 3> axes{};
        for (int i = 0; i < 3; ++i) {
            axes[i] = axisOf(a, i) * (1.0f - t) + axisOf(b, i) * t;
        }
        auto x = normalized(axes[0]);
        auto y = axes[1] - x * dot(axes[1], x);
        y = normalized(y);
        const RE::NiPoint3 z{ x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x };
        const std::array<RE::NiPoint3, 3> rows{ x, y, z };
        for (int i = 0; i < 3; ++i) {
            r.rotate.entry[i][0] = rows[i].x;
            r.rotate.entry[i][1] = rows[i].y;
            r.rotate.entry[i][2] = rows[i].z;
        }
        return r;
    }

    // The pose of a magazine held in a hand (as spawnInHand makes it).
    RE::NiTransform heldPose(const Hand& hand) noexcept
    {
        RE::NiTransform pose;
        pose.rotate = holdRotation(hand.bone);
        pose.translate = hand.palm + axisOf(pose, 2) * g_settings.holdUp;
        return pose;
    }

    // A magazine into the hand: the real one is spawned, the gun's own is drawn there until it comes.
    void handOver(const Hand& hand, std::uint32_t rounds) noexcept
    {
        spawnInHand(hand, rounds, false);
        g_state.phantomToken = g_nextToken - 1;
        g_state.phantomHand = static_cast<int>(&hand - g_hands.data());
        g_state.phantomRounds = rounds;
        g_state.phantomSince = Clock::now();
    }

    // The well caught a magazine from the hand: from now on the hand pushes it along the well.
    void catchMagazine(RE::NiAVObject* root, std::uint32_t ref, const Loose& loose, int hand, float along, RE::NiAVObject* mag3D) noexcept
    {
        if (looksLikeObject(mag3D)) {
            g_state.magnetFrom = mag3D->world;
            setCulled(mag3D, true);   // the gun's own magazine takes its place at once
        } else {
            g_state.magnetFrom = wellPose(root, std::clamp(along, 0.0f, g_settings.dropDistance));
        }
        g_state.magnetStart = Clock::now();
        g_state.phase = kGuided;
        g_state.guidedRounds = loose.rounds;
        g_state.guidedHand = hand;
        g_state.offset = std::clamp(along, 0.0f, g_settings.dropDistance);
        g_state.guidedStart = g_state.offset;
        g_state.guidedHandStart = alongWell(root, g_hands[hand].frame.translate);
        deleteMagazine(ref);
        buzz(hand, 600);
        char message[140]{};
        std::snprintf(message, sizeof(message), "CAUGHT: magazine %08X with %u rounds, %.1f out of the well", ref, loose.rounds, g_state.offset);
        logNote(message);
    }

    void seated() noexcept;

    // The hand moves a caught magazine along the well: up to seat it, down to pull it back out.
    void guideMagazine(const Gun& gun, RE::NiAVObject* root) noexcept
    {
        const auto& hand = g_hands[g_state.guidedHand];
        if (!looksLikeObject(root) || !hand.valid || !hand.grip) {
            if (g_state.offset < 1.5f) {
                g_state.magIn = true;
                g_state.magRounds = g_state.guidedRounds;
                g_state.guidedRounds = 0;
                syncAmmo(gun);
                seated();
                logNote("INSERT: let go at the top, the magazine clicks in");
            } else {
                g_state.outRounds = g_state.guidedRounds;   // let go in the well: it slides out and falls
                g_state.guidedRounds = 0;
                g_state.phase = kSlidingOut;
                logNote("Let go in the well: the magazine falls out");
            }
            return;
        }
        float side = 0.0f;
        const float handAlong = alongWell(root, hand.frame.translate, &side);
        const float wanted = g_state.guidedStart + (handAlong - g_state.guidedHandStart);
        if (wanted > g_state.guidedStart + 4.0f || wanted > g_settings.dropDistance + 2.0f) {
            // pulled back out: it is in the hand again
            const auto rounds = g_state.guidedRounds;
            g_state.guidedRounds = 0;
            g_state.phase = kOut;
            g_state.offset = 0.0f;
            handOver(hand, rounds);
            logNote("The magazine was pulled back out of the well");
            return;
        }
        g_state.offset = std::clamp(wanted, 0.0f, g_settings.dropDistance);
        if (g_state.offset <= 0.15f) {
            g_state.magIn = true;
            g_state.magRounds = g_state.guidedRounds;
            g_state.guidedRounds = 0;
            syncAmmo(gun);
            seated();
            char message[120]{};
            std::snprintf(message, sizeof(message), "INSERT: pushed home, %u rounds, ammoCount %u", g_state.magRounds, gun.data->ammoCount);
            logNote(message);
        }
    }

    // The rounds seen on top of a magazine: as many as it holds (up to the ones the model has).
    void showRounds(RE::NiAVObject* magazine, std::uint32_t rounds) noexcept
    {
        if (!looksLikeObject(magazine)) {
            return;
        }
        static const std::array<const char*, 5> names{ "WeaponMagazineChild1", "WeaponMagazineChild2", "WeaponMagazineChild3", "WeaponMagazineChild4", "WeaponMagazineChild5" };
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (auto* round = findNamed(magazine, names[i]); looksLikeObject(round)) {
                setCulled(round, rounds < i + 1);
            }
        }
    }

    void rackSlide(const Gun& gun, const Clock::time_point now) noexcept
    {
        if (g_state.magIn && g_state.magRounds > 0) {
            g_state.magRounds -= 1;
            g_state.chamber = true;
        }
        syncAmmo(gun);
        playSound(2);
        buzz(1 - g_settings.gunHand, 1200);
        g_state.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        char message[120]{};
        std::snprintf(message, sizeof(message), "RACK: chamber %s, magazine %u", g_state.chamber ? "loaded" : "empty", g_state.magRounds);
        logNote(message);
    }

    void looseMagazines(const Gun* gun, RE::NiAVObject* root, RE::PlayerCharacter* player) noexcept
    {
        const auto now = Clock::now();
        const bool wellOpen = gun && g_state.phase == kOut && g_state.wellKnown && looksLikeObject(root);
        RE::NiTransform well{};
        if (wellOpen) {
            well = compose(root->world, wellFrame());
        }
        int ghost = -1;
        std::uint32_t toInsert = 0;
        float insertDepth = 0.0f;
        int insertHand = 1 - g_settings.gunHand;
        RE::NiAVObject* insert3D = nullptr;
        std::uint32_t toStash = 0;
        std::vector<std::uint32_t> gone;
        std::uint32_t oldest = 0;
        auto oldestBorn = now;
        const int freeHand = 1 - g_settings.gunHand;
        for (auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->GetParentCell()) {
                // gone, or put away (ROCK's stash over the shoulder): its rounds are back with the ammo you carry
                if (ref && !ref->IsDeleted() && loose.rounds > 0) {
                    char text[96]{};
                    std::snprintf(text, sizeof(text), "Магазин убран (%u патр. вернулись в запас)", loose.rounds);
                    hud(text);
                }
                gone.push_back(id);
                continue;
            }
            if (!looksLikeObject(mag3D)) {
                continue;
            }
            const auto at = mag3D->world.translate;
            float handDistance = 1e9f;
            int nearHand = -1;
            for (int i = 0; i < 2; ++i) {
                if (g_hands[i].valid) {
                    const float d = distance(at, g_hands[i].palm);
                    if (d < handDistance) {
                        handDistance = d;
                        nearHand = i;
                    }
                }
            }
            const bool inHand = nearHand >= 0 && handDistance < g_settings.handRadius && g_hands[nearHand].grip;
            const bool letGo = nearHand >= 0 && handDistance < g_settings.handRadius && g_hands[nearHand].previousGrip && !g_hands[nearHand].grip;
            if (wellOpen && loose.kind == g_state.kind && !toInsert) {
                // along the well's own slope: the magazine has to come up it from below
                float side = 0.0f;
                const float along = alongWell(root, at, &side);
                if (along > g_settings.captureDepth + 3.0f || side > g_settings.wellSide * 1.5f) {
                    loose.awayFromWell = true;
                }
                const bool below = along > -2.0f && along < g_settings.ghostRadius && side < g_settings.wellSide * 2.0f;
                if (below && inHand) {
                    ghost = loose.kind;
                }
                // the top of the magazine in the mouth of the well: the well takes it, the hand pushes it home
                if (inHand && loose.awayFromWell && side < g_settings.wellSide && along > -1.5f && along < g_settings.captureDepth) {
                    toInsert = id;
                    insertDepth = std::max(along, 0.0f);
                    insertHand = nearHand;
                    insert3D = mag3D;
                }
            }
            if (!toInsert && player && letGo && nearHand == freeHand && inBelt(player, g_hands[freeHand].palm)) {
                toStash = id;
            }
            const float age = std::chrono::duration<float>(now - loose.born).count();
            const bool nearPlayer = handDistance < 60.0f;
            if (age > g_settings.looseLifetime && !nearPlayer) {
                gone.push_back(id);
                PendingCommand command;
                command.type = kDelete;
                command.ref = id;
                pushCommand(command);
                continue;
            }
            if (!nearPlayer && loose.born < oldestBorn) {
                oldestBorn = loose.born;
                oldest = id;
            }
        }
        for (const auto id : gone) {
            g_loose.erase(id);
        }
        if (oldest && static_cast<int>(g_loose.size()) > g_settings.maxLoose) {
            deleteMagazine(oldest);
        }
        if (toInsert && gun) {
            const auto loose = g_loose[toInsert];
            catchMagazine(root, toInsert, loose, insertHand, insertDepth, insert3D);
            ghost = -1;
        } else if (toStash) {
            // put back in the pouch: its rounds go back to the ammo you carry
            const auto rounds = g_loose[toStash].rounds;
            deleteMagazine(toStash);
            playSound(1);
            buzz(freeHand, 700);
            char text[96]{};
            std::snprintf(text, sizeof(text), "Магазин убран в подсумок (%u патр.)", rounds);
            hud(text);
            logNote("STASH: a magazine went back to the pouch");
        }
        showGhost(ghost, well);
    }

    void beltGrab(const Gun& gun, RE::PlayerCharacter* player, std::uint32_t inventory) noexcept
    {
        const int index = 1 - g_settings.gunHand;
        const auto& hand = g_hands[index];
        if (!hand.valid || !hand.grip || hand.previousGrip) {
            return;
        }
        float height = 0.0f;
        float side = 0.0f;
        const bool belt = inBelt(player, hand.palm, &height, &side);
        char message[160]{};
        std::snprintf(message, sizeof(message), "Grip of the free hand: %.0f above the feet, %.0f to the side%s", height, side, belt ? " (belt)" : "");
        logInfo(message);
        if (!belt) {
            return;
        }
        // a hand already on a magazine takes that one, not a new one
        for (const auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (looksLikeObject(mag3D) && distance(mag3D->world.translate, hand.palm) < g_settings.handRadius) {
                return;
            }
        }
        const auto now = Clock::now();
        if (now - g_state.lastBelt < std::chrono::milliseconds(800)) {
            return;
        }
        g_state.lastBelt = now;
        const auto used = gun.data->ammoCount + g_state.outRounds + (g_state.phase == kGuided ? g_state.guidedRounds : 0u) + looseRounds();
        const auto available = inventory > used ? inventory - used : 0u;
        if (available == 0) {
            hud("Патронов для магазина нет");
            buzz(index, 400);
            return;
        }
        auto capacity = capacityOf(gun);
        if (capacity == 0) {
            capacity = 12;
        }
        const auto rounds = available < capacity ? available : capacity;
        spawnInHand(hand, rounds, true);
        buzz(index, 900);
    }

    float approach(float value, float target, float step) noexcept
    {
        if (value < target) {
            return std::min(value + step, target);
        }
        return std::max(value - step, target);
    }

    // The slide: grip it with the free hand, pull it back along the gun, let it go forward.
    // All the way back throws out the chambered round; going forward takes the next one from the magazine.
    void rackByHand(const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* bolt, RE::NiAVObject* magazine, float dt) noexcept
    {
        if (!looksLikeObject(root) || !looksLikeObject(bolt)) {
            g_state.racking = false;
            return;
        }
        const auto& spare = g_hands[1 - g_settings.gunHand];
        const float scale = root->world.scale > 0.0f ? root->world.scale : 1.0f;
        // the controller, not the drawn hand: ROCK moves the drawn hand onto whatever it holds
        const auto handLocal = relative(root->world, pointTransform(spare.frame.translate)).translate;
        if (!g_state.racking && spare.valid && spare.grip && !spare.previousGrip && g_state.phase != kExtending && g_state.phase != kGuided) {
            const float toSlide = distance(spare.palm, slideGrabPoint(root, bolt));
            const float toMag = looksLikeObject(magazine) && g_state.phase != kOut ? distance(spare.palm, magazineGrabPoint(root)) : 1e9f;
            if (toSlide < g_settings.slideGrabRadius && toSlide <= toMag) {
                g_state.racking = true;
                g_state.rackGrabY = handLocal.y + g_state.rackPull;
                g_state.rackGrabHands = distance(spare.frame.translate, g_hands[g_settings.gunHand].frame.translate);
                g_state.rackGrabPull = g_state.rackPull;
                buzz(1 - g_settings.gunHand, 500);
                char message[120]{};
                std::snprintf(message, sizeof(message), "Slide taken by the free hand (%.1f from its middle)", toSlide);
                logInfo(message);
            }
        }
        if (g_state.racking && !(spare.valid && spare.grip)) {
            g_state.racking = false;   // let go: the spring sends it home
        }
        const float lockAt = g_settings.rackTravel * std::clamp(g_settings.slideLockAt, 0.0f, 1.0f);
        if (g_state.racking) {
            // along the gun, and by the hands closing in (when the gun turns after the hand, the first stays put)
            const float byGun = g_state.rackGrabY - handLocal.y;
            const float byHands = g_state.rackGrabPull + (g_state.rackGrabHands - distance(spare.frame.translate, g_hands[g_settings.gunHand].frame.translate)) / scale;
            g_state.rackPull = std::clamp(std::max(byGun, byHands), g_state.slideLocked ? lockAt : 0.0f, g_settings.rackTravel);
            if (g_state.slideLocked && g_state.rackPull >= g_settings.rackTravel * 0.95f) {
                g_state.slideLocked = false;   // pulled off the slide stop: let go and it runs home
                g_state.rackCycled = true;
                playSound(2);
                logNote("RACK: slide pulled off the stop");
            }
        } else if (g_state.slideLocked) {
            g_state.rackPull = approach(g_state.rackPull, lockAt, 80.0f * dt);   // held open after the last shot
        } else {
            g_state.rackPull = approach(g_state.rackPull, 0.0f, 25.0f * g_settings.rackTravel * dt);
        }
        if (!g_state.rackCycled && !g_state.slideLocked && g_state.rackPull >= g_settings.rackTravel * 0.85f) {
            g_state.rackCycled = true;
            if (g_state.chamber) {
                g_state.chamber = false;   // the round flies out (back to the ammo you carry)
            }
            syncAmmo(gun);
            playSound(2);
            buzz(1 - g_settings.gunHand, 1500);
            logNote("RACK: slide all the way back");
        }
        if (g_state.rackCycled && g_state.rackPull <= g_settings.rackTravel * 0.25f) {
            g_state.rackCycled = false;
            if (g_state.magIn && g_state.phase == kIn && g_state.magRounds > 0) {
                g_state.magRounds -= 1;
                g_state.chamber = true;
            }
            syncAmmo(gun);
            playSound(2);
            buzz(1 - g_settings.gunHand, 2000);
            buzz(g_settings.gunHand, 1500);
            char message[120]{};
            std::snprintf(message, sizeof(message), "RACK: slide home, chamber %s, magazine %u", g_state.chamber ? "loaded" : "empty", g_state.magRounds);
            logNote(message);
        }
        if (g_state.rackPull > 0.0f) {
            refreshSubtree(bolt, axisOf(root->world, 1) * (-g_state.rackPull * scale));
        }
    }

    RE::NiPoint3 tiltAxis(float degrees) noexcept
    {
        const float t = degrees * 0.01745329f;
        return { 0.0f, -std::sin(t), -std::cos(t) };   // down, its bottom leaning back (gun +Y is forward)
    }

    // Which way the magazine well runs, in the gun root's frame. Measured with the magazine seated.
    void learnWell(RE::NiAVObject* root) noexcept
    {
        auto* well = findNamed(root, "P-Mag");
        if (!looksLikeObject(well)) {
            return;
        }
        g_state.wellLocal = relative(root->world, well->world);
        g_state.wellKnown = true;
        if (g_state.magAxisKnown) {
            return;
        }
        if (g_settings.magTilt >= 0.0f) {
            g_state.magAxis = tiltAxis(g_settings.magTilt);
            g_state.magAxisKnown = true;
            logNote("Magazine well: slope from fMagTilt");
            return;
        }
        // the magazine's body runs from the well's mouth to its middle
        RE::NiAVObject* body = findNamed(well, "Magazine:0");
        if (!looksLikeObject(body)) {
            forEachNode(well, [&](RE::NiAVObject* object, int depth) {
                if (depth > 0 && !body && !object->IsNode()) {
                    body = object;
                }
                return !body;
            });
        }
        static int reports = 0;
        const bool report = reports < 3;
        char message[320]{};
        RE::NiPoint3 axis = tiltAxis(18.0f);
        const char* how = "a usual pistol slope (18 degrees): the model gave nothing usable";
        if (looksLikeObject(body) && body->worldBound.fRadius > 0.5f && body->worldBound.fRadius < 100.0f) {
            const auto center = relative(root->world, pointTransform(body->worldBound.center)).translate;
            const auto toCenter = center - g_state.wellLocal.translate;
            const auto direction = normalized(toCenter);
            const float slope = std::atan2(-direction.y, -direction.z) * 57.29578f;
            if (report) {
                std::snprintf(message, sizeof(message), "Magazine well: mouth (%.2f, %.2f, %.2f), body middle (%.2f, %.2f, %.2f) r %.2f, direction (%.2f, %.2f, %.2f), slope %.1f deg",
                    g_state.wellLocal.translate.x, g_state.wellLocal.translate.y, g_state.wellLocal.translate.z,
                    center.x, center.y, center.z, body->worldBound.fRadius, direction.x, direction.y, direction.z, slope);
                logNote(message);
            }
            if (dot(toCenter, toCenter) > 1.0f && direction.z < -0.7f && slope > 3.0f && slope < 40.0f) {
                axis = { 0.0f, direction.y, direction.z };
                axis = normalized(axis);
                how = "the magazine model";
            }
        } else if (report) {
            logNote("Magazine well: no magazine body with a bound found under P-Mag");
        }
        if (report) {
            for (int i = 0; i < 3; ++i) {
                const auto a = axisOf(g_state.wellLocal, i);
                std::snprintf(message, sizeof(message), "  P-Mag axis %d in the gun: (%.2f, %.2f, %.2f)", i, a.x, a.y, a.z);
                logNote(message);
            }
            ++reports;
        }
        g_state.magAxis = axis;
        g_state.magAxisKnown = true;
        std::snprintf(message, sizeof(message), "Magazine well: the magazine runs along (%.2f, %.2f, %.2f), from %s", axis.x, axis.y, axis.z, how);
        logNote(message);
    }

    // The middle of the slide at rest.
    void learnSlide(RE::NiAVObject* root, RE::NiAVObject* bolt) noexcept
    {
        if (!looksLikeObject(bolt)) {
            return;
        }
        const auto& bound = bolt->worldBound;
        if (bound.fRadius > 1.0f && bound.fRadius < 100.0f) {
            const bool first = !g_state.slideCenterKnown;
            g_state.slideCenter = relative(root->world, pointTransform(bound.center)).translate;
            g_state.slideCenterKnown = true;
            if (first) {
                char message[160]{};
                std::snprintf(message, sizeof(message), "Slide: middle at (%.1f, %.1f, %.1f), radius %.1f",
                    g_state.slideCenter.x, g_state.slideCenter.y, g_state.slideCenter.z, bound.fRadius);
                logNote(message);
            }
        }
    }

    void update() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D()) {
            return;
        }
        takeScriptNews();
        readHand(0);
        readHand(1);
        const auto now = Clock::now();
        float dt = g_state.lastFrame == Clock::time_point{} ? 0.011f : std::chrono::duration<float>(now - g_state.lastFrame).count();
        g_state.lastFrame = now;
        dt = std::clamp(dt, 0.0f, 0.05f);

        // ghosts: made once, then kept near the player (a far reference does not draw)
        {
            static Clock::time_point lastCheck{};
            bool ask = !g_state.ghostsAsked;
            if (!ask && now - lastCheck > std::chrono::seconds(5)) {
                lastCheck = now;
                const auto feet = player->GetPosition();
                for (int kind = 0; kind < kKinds; ++kind) {
                    auto* ref = lookupRef(g_ghostRef[kind]);
                    if (!ref || !looksLikeObject(ref->Get3D()) || distance(ref->GetPosition(), feet) > 300.0f) {
                        ask = true;
                    }
                }
            }
            if (ask) {
                g_state.ghostsAsked = true;
                lastCheck = now;
                PendingCommand command;
                command.type = kGhosts;
                pushCommand(command);
            }
        }

        const auto gun = equippedGun(player);
        if (gun.weapon != g_state.weapon) {
            // a magazine on its way out when the gun was put away keeps its rounds on the ground... or here: back to the pouch
            g_state.weapon = gun.weapon;
            g_state.managed = gun.weapon && gun.data && gun.weapon->GetFormID() == k10mmPistol;
            g_state.dumped = false;
            g_state.equippedAt = now;
            g_state.wellKnown = false;
            g_state.idleKnown = false;
            g_state.magAxisKnown = false;
            g_state.magAxis = { 0.0f, 0.0f, -1.0f };
            g_state.slideCenterKnown = false;
            g_state.slideLocked = false;
            g_state.racking = false;
            g_state.rackPull = 0.0f;
            g_state.rackCycled = false;
            g_state.lastAmmo = 0xFFFFFFFF;
            g_state.lastGunState = 0xFFFFFFFF;
            g_state.magIn = true;
            g_state.phase = kIn;
            g_state.offset = 0.0f;
            g_state.outRounds = 0;
            const auto total = gun.data ? gun.data->ammoCount : 0u;
            g_state.chamber = total > 0;
            g_state.magRounds = total > 0 ? total - 1 : 0;
            g_state.kind = capacityOf(gun) > 12 ? 1 : 0;
            char message[200]{};
            std::snprintf(message, sizeof(message), "Equipped weapon %08X, capacity %u, rounds %u: %s",
                gun.weapon ? gun.weapon->GetFormID() : 0u, capacityOf(gun), total,
                g_state.managed ? "reloaded by hand" : "not handled (only the 10mm pistol for now)");
            logNote(message);
        }

        if (!g_state.managed) {
            rock::reserveOffhand(false);
            rock::suppressGrab(1 - g_settings.gunHand, false);
            looseMagazines(nullptr, nullptr, player);
            return;
        }
        auto* root = weaponRoot(player);
        const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;

        if (!g_state.dumped && now - g_state.equippedAt > std::chrono::seconds(2) && root) {
            g_state.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        // the reload button (raw VR gunState)
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        const bool pressed = gunState == reloading && g_state.lastGunState != reloading && g_state.lastGunState != 0xFFFFFFFF;
        if (gunState != g_state.lastGunState) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "gunState %u -> %u", g_state.lastGunState, gunState);
            logInfo(message);
        }
        g_state.lastGunState = gunState;

        // the moving parts: remember them at rest, hold them still while the game plays its reload
        std::array<RE::NiAVObject*, 5> parts{};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i] = findNamed(root, kMovingParts[i]);
        }
        auto* magazine = parts[0];
        const bool frozen = g_settings.blockReload && (gunState == reloading || now < g_state.freezeUntil || pressed);
        const auto ammoNow = gun.data->ammoCount;
        const bool atRest = !frozen && g_state.phase == kIn && g_state.offset <= 0.0f &&
                            gunState == static_cast<std::uint32_t>(g_settings.idleGunState) && ammoNow > 0 &&
                            !g_state.racking && g_state.rackPull <= 0.0f && !g_state.slideLocked &&
                            now - g_state.lastShot > std::chrono::milliseconds(600);
        if (atRest) {
            bool all = true;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    g_state.idle[i] = parts[i]->local;
                } else {
                    all = all && i != 0;
                }
            }
            g_state.idleKnown = all;
        }
        if (frozen && g_state.idleKnown) {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    parts[i]->local = g_state.idle[i];
                    refreshSubtree(parts[i], RE::NiPoint3{});
                }
            }
        }
        if (!frozen && g_state.phase == kIn && g_state.offset <= 0.0f && looksLikeObject(root)) {
            learnWell(root);
        }
        if (atRest && looksLikeObject(root)) {
            learnSlide(root, parts[1]);
        }

        // the rounds: shots take them, the game's own refills are undone
        auto ammo = gun.data->ammoCount;
        if (g_state.lastAmmo != 0xFFFFFFFF) {
            if (ammo > g_state.lastAmmo && g_settings.blockReload) {
                char message[120]{};
                std::snprintf(message, sizeof(message), "Game refill %u -> %u reverted", g_state.lastAmmo, ammo);
                logNote(message);
                ammo = g_state.lastAmmo;
                gun.data->ammoCount = ammo;
            } else if (ammo < g_state.lastAmmo) {
                g_state.lastShot = now;
                if (ammo == 0 && g_state.lastAmmo == 1) {
                    g_state.slideLocked = true;   // the last round: the slide stays open
                    logInfo("Last round fired: slide locked open");
                }
                g_state.chamber = ammo > 0;
                if (g_state.magIn) {
                    g_state.magRounds = ammo > 0 ? ammo - 1 : 0;
                }
                char message[120]{};
                std::snprintf(message, sizeof(message), "Shot: %u left (chamber %d, magazine %u)", ammo, g_state.chamber ? 1 : 0, g_state.magRounds);
                logInfo(message);
            }
        }
        g_state.lastAmmo = ammo;

        const bool triggerHeld = (g_hands[g_settings.gunHand].buttons & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.triggerButton))) != 0;
        const bool justEmptied = ammo == 0 && now - g_state.lastShot < std::chrono::milliseconds(2500);
        if (pressed && g_settings.blockReload && (triggerHeld || justEmptied)) {
            // the game reloads by itself when the trigger is pulled on an empty gun: that is a dry click, not a reload
            g_state.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
            logNote("The game's own reload of an empty gun: ignored");
        } else if (pressed && g_settings.blockReload) {
            if (g_state.phase == kIn) {
                if (g_settings.buttonRack && !g_state.chamber && g_state.magRounds > 0) {
                    rackSlide(gun, now);
                } else {
                    startEject(gun, now);
                }
            } else if (g_state.phase == kOut) {
                hud("Магазина нет: возьми новый с пояса (грип свободной рукой у пояса)");
            }
        }

        // the magazine's way out and in
        const int phaseBefore = g_state.phase;
        const float step = g_settings.slideSpeed * dt;
        switch (g_state.phase) {
        case kExtending:
            {
                g_state.offset = approach(g_state.offset, g_settings.extendDistance, step);
                const auto& gunHand = g_hands[g_settings.gunHand];
                const bool held = g_state.heldMask != 0 && (gunHand.buttons & g_state.heldMask) != 0;
                const auto& spare = g_hands[1 - g_settings.gunHand];
                if (spare.valid && spare.grip && !spare.previousGrip && looksLikeObject(magazine) && looksLikeObject(root) &&
                    distance(spare.palm, magazineGrabPoint(root)) < g_settings.handRadius) {
                    finishEject(root, magazine, &spare);
                } else if (!held) {
                    g_state.phase = kSlidingOut;
                }
            }
            break;
        case kSlidingOut:
            g_state.offset = approach(g_state.offset, g_settings.dropDistance, step * 1.4f);
            if (g_state.offset >= g_settings.dropDistance) {
                finishEject(root, magazine, nullptr);
            }
            break;
        case kSlidingIn:
            g_state.offset = approach(g_state.offset, 0.0f, step);
            if (g_state.offset <= 0.0f) {
                seated();
            }
            break;
        case kGuided:
            guideMagazine(gun, root);
            break;
        default:
            break;
        }
        // where the gun's own magazine node stands against a seated loose magazine
        if (g_state.phase == kIn && g_state.offset <= 0.0f && !frozen && looksLikeObject(magazine) && looksLikeObject(root) && g_state.wellKnown) {
            auto seat = compose(root->world, wellFrame());
            g_state.nodeInWell = relative(seat, magazine->world);
            g_state.nodeInWellKnown = true;
        }
        if (g_state.phantomToken && (g_state.phase != kOut || !g_pending.count(g_state.phantomToken) ||
                                        now - g_state.phantomSince > std::chrono::seconds(3))) {
            g_state.phantomToken = 0;
        }
        if (g_state.phase == kOut) {
            if (g_state.phantomToken && g_hands[g_state.phantomHand].valid && g_state.nodeInWellKnown) {
                setCulled(magazine, false);
                placeGunMagazine(root, magazine, heldPose(g_hands[g_state.phantomHand]));
            } else {
                setCulled(magazine, true);
            }
        } else {
            setCulled(magazine, false);
            const float magnet = std::chrono::duration<float>(now - g_state.magnetStart).count() / 0.15f;
            if (g_state.phase == kGuided && magnet < 1.0f && g_state.nodeInWellKnown && looksLikeObject(root)) {
                // the well pulls the magazine straight: from the hand's pose onto the well's line
                const float t = magnet * magnet * (3.0f - 2.0f * magnet);
                placeGunMagazine(root, magazine, blend(g_state.magnetFrom, wellPose(root, g_state.offset), t));
            } else if (g_state.offset > 0.0f && looksLikeObject(magazine) && looksLikeObject(root)) {
                const auto down = directionToWorld(root->world, g_state.magAxis) * (g_state.offset * root->world.scale);
                refreshSubtree(magazine, down);
            }
        }

        // the slide: the game shows it open on an empty gun, but here it is open only after the last shot
        auto* bolt = parts[1];
        if (g_state.idleKnown && looksLikeObject(bolt) &&
            (gun.data->ammoCount == 0 || g_state.slideLocked || g_state.racking || g_state.rackPull > 0.0f)) {
            bolt->local = g_state.idle[1];
            refreshSubtree(bolt, RE::NiPoint3{});
        }
        // ROCK: the free hand stays off the gun's support grip while it works the slide or a magazine
        {
            const int free = 1 - g_settings.gunHand;
            const auto& spare = g_hands[free];
            bool nearSlide = false;
            if (spare.valid && looksLikeObject(root) && looksLikeObject(bolt)) {
                nearSlide = distance(spare.palm, slideGrabPoint(root, bolt)) < g_settings.slideGrabRadius + 6.0f;
            }
            bool holdingLoose = false;
            if (spare.valid && spare.grip) {
                for (const auto& [id, loose] : g_loose) {
                    auto* ref = lookupRef(id);
                    auto* mag3D = ref ? ref->Get3D() : nullptr;
                    if (looksLikeObject(mag3D) && distance(mag3D->world.translate, spare.palm) < g_settings.handRadius) {
                        holdingLoose = true;
                        break;
                    }
                }
            }
            const bool working = g_state.racking || g_state.phase == kExtending || g_state.phase == kGuided;
            const bool reserve = g_settings.rockOffhand && (working || nearSlide || holdingLoose);
            rock::reserveOffhand(reserve);
            rock::suppressGrab(free, g_settings.rockOffhand && (g_state.racking || (nearSlide && !holdingLoose && g_state.phase != kExtending)));
        }
        rackByHand(gun, root, bolt, magazine, dt);

        if ((g_state.phase == kOut || g_state.phase == kIn) && phaseBefore == g_state.phase && !g_state.racking) {
            beltGrab(gun, player, inventory);
        }
        showRounds(magazine, g_state.phase == kIn ? g_state.magRounds : g_state.phase == kGuided ? g_state.guidedRounds :
                             g_state.phantomToken ? g_state.phantomRounds : g_state.outRounds);
        looseMagazines(&gun, root, player);
    }

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        __try {
            update();
        } __except (1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logWarning("A fault inside the frame update was caught; ManualReload keeps running");
            }
        }
    }

    void gameReady() noexcept
    {
        static bool done = false;
        if (!done) {
            done = true;
            const int error = FRIKApiV2::initialize(3);
            char message[192]{};
            if (error != 0) {
                std::snprintf(message, sizeof(message), "FRIK API v2.3+ unavailable (error %d); ManualReload cannot run", error);
                logWarning(message);
            } else {
                const bool hooked = FRIKApiV2::inst->registerFrameCallback("ManualReload",
                    static_cast<std::uint32_t>(FRIKApiV2::FramePhase::AfterWorldFinal), onFrikFrame, nullptr, 0);
                std::snprintf(message, sizeof(message), "FRIK %s, API v2.%u: frame callback %s",
                    FRIKApiV2::inst->getModVersion(), FRIKApiV2::inst->getVersion(), hooked ? "registered" : "FAILED");
                logNote(message);
            }
        }
        if (!g_vr) {
            using GetInterface_t = void* (*)(const char*, vr::EVRInitError*);
            const auto module = REX::W32::GetModuleHandleW(L"openvr_api.dll");
            const auto get = module ? reinterpret_cast<GetInterface_t>(REX::W32::GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
            vr::EVRInitError error = vr::VRInitError_None;
            g_vr = get ? static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error)) : nullptr;
            logNote(g_vr ? "OpenVR ready" : "OpenVR not found: buttons cannot be read");
        }
        // a loaded save: magazines and ghosts of the last session are gone or unknown
        g_loose.clear();
        g_pending.clear();
        g_ghostRef = {};
        g_ghostDetached = {};
        g_state = State{};
        logNote("ManualReload ready (build " MANUALRELOAD_BUILD ")");
    }
}

namespace manualreload::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "ManualReload",
            .pluginVersion = 1,
            .onGameReady = &gameReady,
        };
        return value;
    }

    void onLoad() noexcept
    {
        loadSettings();
        const auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(registerNatives)) {
            logWarning("Could not register Papyrus functions; magazines cannot be spawned");
        }
    }
}
