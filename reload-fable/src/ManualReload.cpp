// ManualReloadF: hand reloading for Fallout 4 VR (Into the Radius style), 10mm pistol first.
// A second take on ManualReload (same repo, folder reload/). What is kept from it: the rounds in the gun
// are EquippedWeaponData::ammoCount, the game's own refill is undone, the slide is racked by hand.
//
// What is different here:
//   - near the gun the magazine is never a game object. It is the gun's own magazine node, drawn by this
//     plugin every frame: on a rail along the magazine well, or in the free hand. Nothing is spawned, so
//     nothing pops. A magazine becomes a real object only when it is dropped.
//   - the free hand takes the magazine and is pinned to it (FRIK hand transform) while it is on the rail.
//   - while the free hand works a magazine its grip is hidden from every other reader of the controller
//     (hooks on IVRSystem::GetControllerState*), so ROCK does not take the pistol with that hand.
//   - the reload button is read from the controller: a tap drops the magazine, a hold lets it stick out
//     and wait for the free hand.
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"

#include "GripHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    void note(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logNote(text);
    }

    void detail(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logInfo(text);
    }

    // ------------------------------------------------------------ settings

    struct Settings
    {
        bool  blockReload{ true };
        int   reloadGunState{ 8 };      // raw gunState of VR while reloading
        int   idleGunState{ 12 };       // raw gunState of VR at rest
        int   dumpDepth{ 12 };
        int   gunHand{ 0 };             // 0 = right hand holds the gun, 1 = left
        int   gripButton{ 2 };          // OpenVR button ids
        int   triggerButton{ 33 };
        int   reloadButton{ 7 };        // A / X
        bool  haptics{ true };
        bool  messages{ true };
        bool  hideGrip{ true };         // hide the free hand's grip from other mods while it works a magazine
        bool  blockTwoHand{ true };     // no second-hand hold on the gun while its magazine is out
        float gunRadius{ 28.0f };       // ..within this distance of the gun
        bool  regripAfterSeat{ true };
        bool  spareFromBelt{ true };    // grip at the belt while the gun has its magazine: a spare one as a real object  // give the grip back as soon as the magazine is seated
        float holdSeconds{ 0.25f };     // reload button held this long: the magazine sticks out and waits
        float beltLow{ 55.0f };
        float beltHigh{ 80.0f };
        float beltRadius{ 22.0f };
        float shoulderHeight{ 88.0f };  // let go of a magazine behind you above this height: it is put away
        float extendDistance{ 3.2f };
        float slideSpeed{ 40.0f };
        float railLength{ -1.0f };      // how far the magazine travels to leave the well; below 0 = from the model
        float magTilt{ -1.0f };         // slope of the well back from straight down; below 0 = from the model
        float wellSide{ 3.5f };         // the top of a magazine this close to the well's line is caught
        float captureAhead{ 1.0f };     // ..and this far below the mouth
        float seatAt{ 0.3f };           // pushed this close to home: it clicks in
        float seatRelease{ 1.5f };      // let go this close to home: it clicks in, further out it falls
        float magGrabRadius{ 9.0f };    // the palm this close to a magazine that sticks out takes it
        float grabUp{ 2.0f };           // the palm takes the magazine this far above its bottom
        float holdUp{ -0.5f };           // magazine from the belt: its bottom sits this far below the palm's middle
        float holdSide{ 0.0f };
        float holdForward{ 0.0f };
        float holdRotX{ 90.0f };
        float holdRotY{ 0.0f };
        float holdRotZ{ 0.0f };
        int   holdFlip{ 0 };            // 1 = the magazine from the belt is upside down in the hand: turn it
        float fingerCurl{ 0.35f };      // fingers around a magazine: 0 = fist, 1 = open
        float freezeSeconds{ 1.6f };
        float slideGrabRadius{ 12.0f };
        float rackTravel{ 5.0f };
        float slideLockAt{ 0.8f };
        float handRadius{ 16.0f };
        float looseLifetime{ 90.0f };
        int   maxLoose{ 6 };
        bool  allGuns{ true };          // every gun with a magazine of group 1, not only the 10mm pistol
        bool  nativeSound{ true };      // sounds played by the plugin (the script's Sound objects are not heard)
        int   soundFlags{ 16 };
        bool  showRound{ true };        // a cartridge is seen in the chamber / on top of the magazine while the slide is open
        float roundOpenAt{ 0.3f };      // ..once the slide is back this part of its travel
        float chamberUp{ 0.9f };        // the chambered round: from the magazine's top this much up..
        float chamberForward{ 0.9f };   // ..and this much forward (gun units)
        float roundUp{ 0.0f };          // the top round of the magazine: moved this much up from the magazine's top
        float roundRotX{ 0.0f };        // turns of the round model in the gun's frame (degrees)
        float roundRotY{ 0.0f };
        float roundRotZ{ 0.0f };
        float roundScale{ 1.0f };
        std::string roundModel;         // the model of the drawn round (empty: the gun's ammo casing)
    };
    Settings g_settings;

    void loadSettings() noexcept
    {
        struct Entry { const char* key; int type; void* value; };   // 0 bool, 1 int, 2 float
        auto& s = g_settings;
        const Entry entries[]{
            { "bBlockReload", 0, &s.blockReload }, { "iReloadGunState", 1, &s.reloadGunState }, { "iIdleGunState", 1, &s.idleGunState },
            { "iDumpDepth", 1, &s.dumpDepth }, { "iGunHand", 1, &s.gunHand }, { "iGripButton", 1, &s.gripButton },
            { "iTriggerButton", 1, &s.triggerButton }, { "iReloadButton", 1, &s.reloadButton }, { "bHaptics", 0, &s.haptics },
            { "bMessages", 0, &s.messages }, { "bHideGrip", 0, &s.hideGrip }, { "bRegripAfterSeat", 0, &s.regripAfterSeat },
            { "bBlockTwoHand", 0, &s.blockTwoHand }, { "bSpareFromBelt", 0, &s.spareFromBelt }, { "fGunRadius", 2, &s.gunRadius },
            { "fHoldSeconds", 2, &s.holdSeconds }, { "fBeltLow", 2, &s.beltLow }, { "fBeltHigh", 2, &s.beltHigh },
            { "fBeltRadius", 2, &s.beltRadius }, { "fShoulderHeight", 2, &s.shoulderHeight }, { "fExtendDistance", 2, &s.extendDistance },
            { "fSlideSpeed", 2, &s.slideSpeed }, { "fRailLength", 2, &s.railLength }, { "fMagTilt", 2, &s.magTilt },
            { "fWellSide", 2, &s.wellSide }, { "fCaptureAhead", 2, &s.captureAhead }, { "fSeatAt", 2, &s.seatAt },
            { "fSeatRelease", 2, &s.seatRelease }, { "fMagGrabRadius", 2, &s.magGrabRadius }, { "fGrabUp", 2, &s.grabUp },
            { "fHoldUp", 2, &s.holdUp }, { "fHoldSide", 2, &s.holdSide }, { "fHoldForward", 2, &s.holdForward },
            { "fHoldRotX", 2, &s.holdRotX }, { "fHoldRotY", 2, &s.holdRotY }, { "fHoldRotZ", 2, &s.holdRotZ },
            { "iHoldFlip", 1, &s.holdFlip }, { "fFingerCurl", 2, &s.fingerCurl }, { "fFreezeSeconds", 2, &s.freezeSeconds },
            { "fSlideGrabRadius", 2, &s.slideGrabRadius }, { "fRackTravel", 2, &s.rackTravel }, { "fSlideLockAt", 2, &s.slideLockAt },
            { "fHandRadius", 2, &s.handRadius }, { "fLooseLifetime", 2, &s.looseLifetime }, { "iMaxLoose", 1, &s.maxLoose },
            { "bAllGuns", 0, &s.allGuns }, { "bNativeSound", 0, &s.nativeSound }, { "iSoundFlags", 1, &s.soundFlags }, { "bShowRound", 0, &s.showRound }, { "fRoundOpenAt", 2, &s.roundOpenAt }, { "fChamberUp", 2, &s.chamberUp },
            { "fChamberForward", 2, &s.chamberForward }, { "fRoundUp", 2, &s.roundUp }, { "fRoundRotX", 2, &s.roundRotX },
            { "fRoundRotY", 2, &s.roundRotY }, { "fRoundRotZ", 2, &s.roundRotZ }, { "fRoundScale", 2, &s.roundScale },
        };
        try {
            std::ifstream file("Data/F4SE/Plugins/ManualReloadF.ini");
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
                if (key == "sRoundModel") {
                    auto text = line.substr(equals + 1);
                    const auto first = text.find_first_not_of(" \t");
                    const auto last = text.find_last_not_of(" \t\r");
                    s.roundModel = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
                    continue;
                }
                for (const auto& entry : entries) {
                    if (key == entry.key) {
                        if (entry.type == 0) *static_cast<bool*>(entry.value) = value != 0.0f;
                        else if (entry.type == 1) *static_cast<int*>(entry.value) = static_cast<int>(value);
                        else *static_cast<float*>(entry.value) = value;
                        break;
                    }
                }
            }
        } catch (...) {
        }
        s.gunHand = s.gunHand != 0 ? 1 : 0;
        note("Settings: bBlockReload=%d bHideGrip=%d iGunHand=%d hold %.2fs belt %.0f..%.0f r%.0f tilt %.1f rail %.1f",
            s.blockReload ? 1 : 0, s.hideGrip ? 1 : 0, s.gunHand, s.holdSeconds, s.beltLow, s.beltHigh, s.beltRadius, s.magTilt, s.railLength);
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

    // The magazine the equipped gun wears: its magazine mod's model, looked up in kMagModels (-1: none of ours).
    int magazineKindOf(RE::PlayerCharacter* player, const RE::TESForm* weapon, std::string& model) noexcept
    {
        auto* list = player ? player->inventoryList : nullptr;
        if (!weapon || !list || !readable(list, sizeof(RE::BGSInventoryList))) {
            return -1;
        }
        for (auto& item : list->data) {
            if (item.object != weapon) {
                continue;
            }
            for (auto* stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
                if (!stack->IsEquipped()) {
                    continue;
                }
                auto* extra = stack->extra.get();
                auto* instance = extra ? static_cast<RE::BGSObjectInstanceExtra*>(extra->GetByType(RE::EXTRA_DATA_TYPE::kObjectInstance)) : nullptr;
                if (!instance || !instance->values) {
                    continue;
                }
                for (const auto& entry : instance->GetIndexData()) {
                    auto* form = RE::TESForm::GetFormByID(entry.objectID);
                    if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kOMOD) {
                        continue;
                    }
                    auto* mod = static_cast<RE::BGSMod::Attachment::Mod*>(form);
                    const char* path = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*mod)).GetModel();
                    if (!path || !*path) {
                        continue;
                    }
                    std::string name = path;
                    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name.rfind("meshes\\", 0) == 0) name = name.substr(7);
                    for (std::size_t i = 0; i < kMagModels.size(); ++i) {
                        if (name == kMagModels[i]) {
                            model = path;
                            return static_cast<int>(i);
                        }
                    }
                }
            }
        }
        return -1;
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
        // the nodes above must reach as far as the magazine, or it is culled with them when the gun is out of view
        const float reach = node->worldBound.fRadius > 0.0f && node->worldBound.fRadius < 1000.0f ? node->worldBound.fRadius : 10.0f;
        RE::NiAVObject* up = node->parent;
        for (int i = 0; i < 8 && looksLikeObject(up); ++i) {
            auto& bound = up->worldBound;
            const float needed = distance(bound.center, world.translate) + reach;
            if (bound.fRadius > 0.0f && bound.fRadius < needed && needed < 400.0f) {
                bound.fRadius = needed;
            }
            auto* next = up->parent;
            up = looksLikeObject(next) ? next : nullptr;
        }
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

    vr::IVRSystem* g_vr{ nullptr };

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

    struct Hand
    {
        bool            valid{ false };
        RE::NiTransform wand{};          // the controller
        RE::NiTransform wrist{};         // the hand bone as it is drawn
        RE::NiPoint3    palm{};
        bool            grip{ false };   // as the controller reports it, hidden or not
        bool            previousGrip{ false };
        std::uint64_t   buttons{ 0 };
        std::uint64_t   previousButtons{ 0 };
    };
    std::array<Hand, 2> g_hands;   // 0 right, 1 left

    int freeIndex() noexcept { return 1 - g_settings.gunHand; }
    FRIKApiV2::Hand frikHand(int index) noexcept { return index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left; }

    void readHand(int index) noexcept
    {
        auto& hand = g_hands[index];
        hand.previousGrip = hand.grip;
        hand.previousButtons = hand.buttons;
        hand.valid = false;
        hand.grip = false;
        hand.buttons = 0;
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (griphook::rawState(device, &state, sizeof(state)) || (g_vr && g_vr->GetControllerState(device, &state, sizeof(state)))) {
                hand.buttons = state.ulButtonPressed;
                hand.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
            }
        }
        if (index == freeIndex()) {
            griphook::setDevice(device);
            griphook::setGripMask(vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton)));
            griphook::arbitrate(hand.grip);
        }
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->getTrackedHandTransform) {
            return;
        }
        const auto which = frikHand(index);
        RE::NiTransform wand;
        if (!frik->getTrackedHandTransform(which, FRIKApiV2::TrackedHandKind::Wand, &wand)) {
            return;
        }
        hand.wand = wand;
        hand.wand.scale = 1.0f;
        hand.wrist = hand.wand;
        hand.palm = wand.translate;
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            const float length = distance(tip, wrist);
            if (length < 40.0f && length > 0.1f) {
                hand.palm = (tip + wrist) * 0.5f;
                hand.wrist = chain.hand;
                hand.wrist.scale = 1.0f;
            }
        }
        hand.valid = true;
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

    // ------------------------------------------------------------ the hand on the magazine (FRIK)

    constexpr const char* kFrikTag = "ManualReloadF";
    bool g_pinned{ false };
    bool g_posed{ false };

    void pinHand(const RE::NiTransform& wrist) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->setHandWorldTransform) {
            return;
        }
        auto target = wrist;
        target.scale = 1.0f;
        if (frik->setHandWorldTransform(kFrikTag, frikHand(freeIndex()), target, 120)) {
            g_pinned = true;
        }
    }

    void unpinHand() noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (g_pinned && frik && frik->clearHandWorldTransform) {
            frik->clearHandWorldTransform(kFrikTag, frikHand(freeIndex()));
        }
        g_pinned = false;
    }

    void poseFingers(bool on) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!frik || on == g_posed) {
            return;
        }
        if (on && frik->setHandPoseCustom) {
            const float c = std::clamp(g_settings.fingerCurl, 0.0f, 1.0f);
            FRIKApiV2::HandPoseData pose{};
            const float t = std::min(c + 0.25f, 1.0f);
            pose.thumb = { t, t, t, 0.0f };
            pose.index = { c, c, c, 0.0f };
            pose.middle = { c, c, c, 0.0f };
            pose.ring = { c, c, c, 0.0f };
            pose.pinky = { c, c, c, 0.0f };
            g_posed = frik->setHandPoseCustom(kFrikTag, frikHand(freeIndex()), pose, 120);
        } else if (!on && frik->clearHandPose) {
            frik->clearHandPose(kFrikTag, frikHand(freeIndex()));
            g_posed = false;
        }
    }

    // ------------------------------------------------------------ talking to the script

    enum Command : std::int32_t
    {
        kNone = 0,
        kSpawn = 1,    // kind, pose, token: a magazine as a physical object
        kDelete = 2,   // ref
        kSound = 3,    // kind = sound index: 0 magazine in, 1 magazine out, 2 slide back, 3 slide home, 4 put away
        kRound = 4,    // the drawn cartridge (a static object; the script answers with AddMag(ref, kRoundToken))
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
        // where a magazine that is on its way should appear now (it has moved since it was asked for)
        std::unordered_map<std::int32_t, std::array<float, 6>> fresh;
    } g_shared;

    void pushCommand(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
    }

    // Sounds are played here by the descriptor's editor ID (the plugin's own ones first: their output is 2D,
    // so they need no position). The script's Sound objects are only the fallback.
    bool playByName(const char* name) noexcept
    {
        auto* audio = RE::BSAudioManager::GetSingleton();
        if (!audio || !name || !*name) {
            return false;
        }
        RE::BSSoundHandle handle;
        handle.soundID = 0xFFFFFFFF;
        handle.assumeSuccess = false;
        handle.state = 0;
        audio->GetSoundHandleByName(handle, name, 0.0f, static_cast<std::uint32_t>(g_settings.soundFlags), nullptr);
        if (handle.soundID == 0xFFFFFFFF) {
            return false;
        }
        return handle.FadeInPlay(0);
    }

    void playSound(int index) noexcept
    {
        static const std::array<std::array<const char*, 2>, 5> names{ {
            { "MRF_DescIn", "WPNPistol10mmReloadMagIn" }, { "MRF_DescOut", "WPNPistol10mmReloadMagOut" },
            { "MRF_DescSlideBack", "WPNPistol10mmReloadBoltOpen" }, { "MRF_DescSlideHome", "WPNPistol10mmReloadBoltClose" },
            { "ItemPickUpMetal1Small", nullptr } } };
        static std::array<int, 5> logged{};
        if (g_settings.nativeSound && index >= 0 && index < static_cast<int>(names.size())) {
            for (const char* name : names[index]) {
                if (name && playByName(name)) {
                    if (logged[index]++ == 0) {
                        note("Sound %d: played %s", index, name);
                    }
                    return;
                }
            }
            if (logged[index]++ == 0) {
                note("Sound %d: no descriptor found by name, the script tries", index);
            }
        }
        PendingCommand command;
        command.type = kSound;
        command.kind = index;
        pushCommand(command);
    }

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

    std::vector<float> FreshPose(std::monostate, std::int32_t token)
    {
        std::scoped_lock guard(g_shared.lock);
        const auto it = g_shared.fresh.find(token);
        if (it == g_shared.fresh.end()) {
            return {};
        }
        return { it->second.begin(), it->second.end() };
    }

    void AddMag(std::monostate, RE::TESObjectREFR* ref, std::int32_t token)
    {
        if (!ref) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.newMags.push_back(NewMag{ ref->GetFormID(), token });
    }

    void Notify(std::monostate, std::int32_t message)
    {
        note("Script: %d", message);
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "ManualReloadF";
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandKind", CommandKind, true);
        vm->BindNativeMethod(kScript, "CommandRefID", CommandRefID, true);
        vm->BindNativeMethod(kScript, "CommandToken", CommandToken, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "FreshPose", FreshPose, true);
        vm->BindNativeMethod(kScript, "AddMag", AddMag, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        logNote("Papyrus functions of script ManualReloadF registered");
        return true;
    }

    // ------------------------------------------------------------ state

    constexpr std::uint32_t k10mmPistol = 0x00004822;   // Fallout4.esm

    // Guns with a detachable magazine and a slide / bolt / charging handle (group 1). A gun is handled when the
    // magazine mod it wears has one of these models; the index is the magazine's kind (MRF_Mags order in the ESP).
    constexpr std::array<const char*, 16> kMagModels{
        "weapons\\10mmpistol\\10mmmag01.nif", "weapons\\10mmpistol\\10mmmaglarge.nif",
        "weapons\\machinegun\\mgmagstraight.nif", "weapons\\machinegun\\mgmagdrum.nif",
        "weapons\\combatshotgun\\combatriflemagmedium.nif", "weapons\\combatshotgun\\combatriflemaglarge.nif",
        "weapons\\combatshotgun\\tgundrummagsmall.nif", "weapons\\combatshotgun\\tgundrummag.nif",
        "weapons\\covertsidearm\\covertsidearmmagazine.nif", "weapons\\covertsidearm\\covertsidearmmagazinelarge.nif",
        "weapons\\handmade\\magazines\\45straightmagshort.nif", "weapons\\handmade\\magazines\\45straightmag.nif",
        "weapons\\handmade\\magazines\\piperifledrummag.nif",
        "weapons\\combatshotgun\\combatshotgunsmallmag.nif", "weapons\\combatshotgun\\combatshotgunmediummag.nif",
        "weapons\\combatshotgun\\drummag.nif",
    };
    constexpr float kGravity = 686.0f;                   // game units per second squared

    enum Place : int
    {
        kSeated = 0,     // in the gun
        kRailAuto = 1,   // on the rail, moving by itself: sticking out (button held) or sliding out to fall
        kRailHand = 2,   // on the rail, moved by the free hand that holds it
        kHand = 3,       // in the free hand, clear of the gun
        kGone = 4,       // the gun has no magazine and the free hand holds none of ours
    };

    enum ArmReason : int
    {
        kArmNone = 0,
        kArmRail = 1,    // the palm is at a magazine that sticks out
        kArmBelt = 2,    // the palm is at the belt and the gun wants a magazine
        kArmSlide = 3,   // the palm is at the slide
        kArmBlock = 4,   // the palm is at a gun without a magazine: no second-hand hold yet
    };

    struct Loose
    {
        std::uint32_t     rounds{ 0 };
        int               kind{ 0 };
        Clock::time_point born{};
    };

    struct Pending
    {
        std::uint32_t rounds{ 0 };
        int           kind{ 0 };
        RE::NiMatrix3 rotation{};
    };

    struct State
    {
        RE::TESObjectWEAP* weapon{ nullptr };
        bool               managed{ false };
        std::uint32_t      lastAmmo{ 0xFFFFFFFF };
        std::uint32_t      lastGunState{ 0xFFFFFFFF };
        bool               chamber{ false };
        bool               hasBolt{ true };               // the model has a WeaponBolt node
        int                kind{ 0 };
        bool               dumped{ false };
        Clock::time_point  equippedAt{};
        Clock::time_point  lastBelt{};
        Clock::time_point  lastShot{};
        Clock::time_point  lastFrame{};
        Clock::time_point  freezeUntil{};
        // the moving parts at rest, to hold the game's reload animation still
        std::array<RE::NiTransform, 5> idle{};
        bool               idleKnown{ false };
        // the rail: learned from the gun at rest, in the gun root's frame
        bool               railKnown{ false };
        bool               railLogged{ false };
        RE::NiTransform    magRest{};                      // the magazine node when seated
        RE::NiPoint3       axis{ 0.0f, 0.0f, -1.0f };      // the way out of the well
        float              length{ 8.5f };                 // travel until it is clear of the well
        float              bottomAt{ -3.5f };              // from the node's origin down to the baseplate (negative)
        RE::NiTransform    wristInBolt{};                  // the drawn hand against the slide while it is racked
        bool               rackPinned{ false };
        float              topAt{ 7.0f };                  // from the node's origin (the bottom) up to the top
        RE::NiTransform    looseInNode{};                  // a loose magazine model against the gun's magazine node
        // the magazine
        int                place{ kSeated };
        std::uint32_t      rounds{ 0 };
        float              s{ 0.0f };                      // how far out along the rail
        bool               autoHold{ false };              // sticking out, waiting (the button is held)
        RE::NiTransform    magInWand{};                    // the magazine against the controller that holds it
        RE::NiTransform    wristInMag{};                   // the drawn hand against the magazine
        float              driveS0{ 0.0f };
        float              driveAlong0{ 0.0f };
        bool               mustLeave{ false };             // just pulled out: has to clear the well before it is caught again
        RE::NiTransform    blendFrom{};
        Clock::time_point  blendStart{};
        float              blendSeconds{ 0.0f };
        RE::NiPoint3       lastMagAt{};
        RE::NiPoint3       velocity{};
        bool               velocityValid{ false };
        // the reload button
        bool               buttonDown{ false };
        bool               buttonUsed{ false };
        Clock::time_point  buttonSince{};
        Clock::time_point  buttonEdge{};
        RE::NiTransform    lastDrawn{};
        bool               lastDrawnValid{ false };
        Clock::time_point  axisChecked{};
        bool               axisLogged{ false };
        int                armReason{ kArmNone };
        // a dropped magazine, drawn falling until the real object is there
        bool               dropActive{ false };
        std::int32_t       dropToken{ 0 };
        RE::NiTransform    dropPose{};
        RE::NiPoint3       dropVelocity{};
        std::uint32_t      dropRounds{ 0 };
        Clock::time_point  dropStart{};
        // the slide in the free hand
        bool               racking{ false };
        float              rackPull{ 0.0f };
        float              rackGrabY{ 0.0f };
        float              rackGrabHands{ 0.0f };
        float              rackGrabPull{ 0.0f };
        bool               rackCycled{ false };
        bool               slideLocked{ false };
        RE::NiPoint3       slideCenter{};
        bool               slideCenterKnown{ false };
    };
    State g_state;

    constexpr std::array<const char*, 5> kMovingParts{ "WeaponMagazine", "WeaponBolt", "WeaponExtra1", "WeaponExtra2", "WeaponExtra3" };

    std::unordered_map<std::uint32_t, Loose> g_loose;     // real magazines lying around or held through ROCK
    std::unordered_map<std::int32_t, Pending> g_pending;  // spawn token -> what it will be
    std::int32_t g_nextToken{ 1 };

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

    void setAmmo(const Gun& gun, std::uint32_t value) noexcept
    {
        gun.data->ammoCount = value;
        g_state.lastAmmo = value;
    }

    // The game fires while ammoCount > 0, so a gun with an empty chamber must read 0.
    void syncAmmo(const Gun& gun) noexcept
    {
        setAmmo(gun, g_state.chamber ? 1u + (g_state.place == kSeated ? g_state.rounds : 0u) : 0u);
    }

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
        const auto rel = relative(root->world, pointTransform(point)).translate - g_state.magRest.translate;
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
        return pose.translate + axisOf(pose, 2) * (g_state.topAt * pose.scale);
    }

    // Where the palm takes a magazine on the rail.
    RE::NiPoint3 grabPoint(const RE::NiAVObject* root) noexcept
    {
        const auto pose = railPose(root, g_state.s);
        return pose.translate + axisOf(pose, 2) * ((g_state.bottomAt + g_settings.grabUp) * pose.scale);
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
        if (bound.fRadius > 1.0f && bound.fRadius < 100.0f) {
            const bool first = !g_state.slideCenterKnown;
            g_state.slideCenter = relative(root->world, pointTransform(bound.center)).translate;
            g_state.slideCenterKnown = true;
            if (first) {
                note("Slide: middle at (%.1f, %.1f, %.1f), radius %.1f", g_state.slideCenter.x, g_state.slideCenter.y, g_state.slideCenter.z, bound.fRadius);
            }
        }
    }

    RE::NiPoint3 slideGrabPoint(const RE::NiAVObject* root, const RE::NiAVObject* bolt) noexcept
    {
        if (!g_state.slideCenterKnown) {
            return bolt->world.translate;
        }
        return compose(root->world, pointTransform(g_state.slideCenter)).translate + axisOf(root->world, 1) * (-g_state.rackPull * gunScale(root));
    }

    // ------------------------------------------------------------ magazines as real objects

    std::array<float, 6> poseForScript(const RE::NiTransform& loose) noexcept
    {
        const auto angles = anglesFor(loose.rotate);
        return { loose.translate.x, loose.translate.y, loose.translate.z, angles[0], angles[1], angles[2] };
    }

    // A real magazine where the gun's magazine node would stand at `node`.
    std::int32_t spawnLoose(const RE::NiTransform& node, std::uint32_t rounds, const char* why) noexcept
    {
        const auto loose = compose(node, g_state.looseInNode);
        const auto token = g_nextToken++;
        g_pending[token] = Pending{ rounds, g_state.kind, loose.rotate };
        PendingCommand command;
        command.type = kSpawn;
        command.kind = g_state.kind;
        command.token = token;
        command.pose = poseForScript(loose);
        {
            std::scoped_lock guard(g_shared.lock);
            g_shared.fresh[token] = command.pose;
        }
        pushCommand(command);
        note("Spawn magazine kind %d with %u rounds (%s) at (%.0f, %.0f, %.0f)", g_state.kind, rounds, why, loose.translate.x, loose.translate.y, loose.translate.z);
        return token;
    }

    void setFresh(std::int32_t token, const RE::NiTransform& node) noexcept
    {
        const auto pose = poseForScript(compose(node, g_state.looseInNode));
        std::scoped_lock guard(g_shared.lock);
        if (const auto it = g_shared.fresh.find(token); it != g_shared.fresh.end()) {
            it->second = pose;
        }
    }

    void deleteMagazine(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
        g_loose.erase(ref);
    }

    // ------------------------------------------------------------ the round seen through the open slide
    // A static object of this plugin (MRF_Round) whose model is set from the gun's ammo (its casing) before it is
    // made. The plugin draws its 3D every frame: in the chamber while one is loaded, else on top of the magazine
    // while it has rounds, and hidden when the slide is closed or there is nothing to see.
    constexpr std::int32_t kRoundToken = -7;
    constexpr RE::TESFormID kRoundForm = 0x80E;

    struct RoundState
    {
        std::uint32_t      ref{ 0 };
        bool               asked{ false };
        Clock::time_point  askedAt{};
        Clock::time_point  missingSince{};
        bool               missing{ false };
        RE::TESAmmo*       modelFor{ nullptr };
        bool               modelReady{ false };
        bool               logged{ false };
    };
    RoundState g_round;

    void hideRound() noexcept
    {
        if (g_round.ref) {
            if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                setCulled(ref->Get3D(), true);
            }
        }
    }

    void takeRound(std::uint32_t ref) noexcept
    {
        if (g_round.ref && g_round.ref != ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = ref;
        g_round.asked = false;
        g_round.missing = false;
        note("Round: object %08X is here", ref);
    }

    void dropRound() noexcept
    {
        if (g_round.ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = 0;
        g_round.asked = false;
    }

    // The model of the drawn round: sRoundModel, else the casing of the gun's ammo, else its world model.
    bool prepareRoundModel(const Gun& gun) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        if (g_round.modelReady && g_round.modelFor == ammo) {
            return true;
        }
        g_round.modelFor = ammo;
        g_round.modelReady = false;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kRoundForm, "ManualReloadF.esp") : nullptr;
        auto* stat = form ? form->As<RE::TESObjectSTAT>() : nullptr;
        if (!stat) {
            note("Round: MRF_Round (%03X) was not found in ManualReloadF.esp", kRoundForm);
            return false;
        }
        std::string casing;
        std::string world;
        if (ammo && readable(ammo, sizeof(RE::TESAmmo))) {
            if (const char* m = ammo->shellCasing.GetModel(); m && *m) casing = m;
            if (const char* m = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel(); m && *m) world = m;
        }
        std::string model = !g_settings.roundModel.empty() ? g_settings.roundModel : !casing.empty() ? casing : world;
        note("Round: ammo %08X casing '%s', world model '%s' -> using '%s'", ammo ? ammo->GetFormID() : 0u, casing.c_str(), world.c_str(), model.c_str());
        if (model.empty()) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*stat)).SetModel(model.c_str());
        g_round.modelReady = true;
        if (g_round.ref) {
            dropRound();   // made with another model: make it again
        }
        return true;
    }

    void drawRound(const Gun& gun, RE::NiAVObject* root) noexcept
    {
        auto& st = g_state;
        if (!g_settings.showRound || !looksLikeObject(root) || !st.railKnown) {
            if (g_round.ref) {
                if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                    setCulled(ref->Get3D(), true);
                }
            }
            return;
        }
        const bool open = st.slideLocked || st.rackPull >= g_settings.rackTravel * std::clamp(g_settings.roundOpenAt, 0.05f, 1.0f);
        const bool inChamber = st.chamber && !st.rackCycled;
        const bool onMagazine = !inChamber && st.place == kSeated && st.rounds > 0;
        const bool visible = open && (inChamber || onMagazine);

        if (!g_round.ref) {
            const auto now = Clock::now();
            if (!g_round.asked || now - g_round.askedAt > std::chrono::seconds(5)) {
                if (prepareRoundModel(gun)) {
                    PendingCommand command;
                    command.type = kRound;
                    pushCommand(command);
                    g_round.asked = true;
                    g_round.askedAt = now;
                }
            }
            return;
        }
        auto* ref = lookupRef(g_round.ref);
        auto* model = ref ? ref->Get3D() : nullptr;
        if (!looksLikeObject(model)) {
            const auto now = Clock::now();
            if (!g_round.missing) {
                g_round.missing = true;
                g_round.missingSince = now;
            } else if (now - g_round.missingSince > std::chrono::seconds(3)) {
                note("Round: object %08X has no 3D, made again", g_round.ref);
                dropRound();
                g_round.missing = false;
            }
            return;
        }
        g_round.missing = false;
        if (!visible) {
            setCulled(model, true);
            return;
        }
        // the magazine's top in the gun's frame, then up / forward to the chamber
        const float scale = gunScale(root);
        auto top = compose(root->world, st.magRest);
        const auto up = axisOf(top, 2);
        RE::NiPoint3 at = top.translate + up * ((st.topAt + g_settings.roundUp) * top.scale);
        if (inChamber) {
            at += axisOf(root->world, 2) * (g_settings.chamberUp * scale) + axisOf(root->world, 1) * (g_settings.chamberForward * scale);
        }
        const float d = 0.01745329f;
        Mat turn = axisRotation(0, g_settings.roundRotX * d);
        turn = mul(turn, axisRotation(1, g_settings.roundRotY * d));
        turn = mul(turn, axisRotation(2, g_settings.roundRotZ * d));
        RE::NiTransform pose = identityTransform();
        pose.rotate = fromMat(mul(turn, toMat(root->world.rotate)));
        pose.translate = at;
        pose.scale = scale * (g_settings.roundScale > 0.0f ? g_settings.roundScale : 1.0f);
        setCulled(model, false);
        placeNode(model, pose);
        if (!g_round.logged) {
            g_round.logged = true;
            note("Round: drawn %s, bound radius %.2f", inChamber ? "in the chamber" : "on the magazine", model->worldBound.fRadius);
        }
    }

    void takeScriptNews() noexcept
    {
        std::vector<NewMag> mags;
        {
            std::scoped_lock guard(g_shared.lock);
            mags.swap(g_shared.newMags);
        }
        for (const auto& mag : mags) {
            if (mag.token == kRoundToken) {
                takeRound(mag.ref);
                continue;
            }
            Loose loose;
            loose.born = Clock::now();
            if (const auto it = g_pending.find(mag.token); it != g_pending.end()) {
                loose.rounds = it->second.rounds;
                loose.kind = it->second.kind;
                auto* ref = lookupRef(mag.ref);
                learnConvention(ref, ref ? ref->Get3D() : nullptr);
                g_pending.erase(it);
            }
            {
                std::scoped_lock guard(g_shared.lock);
                g_shared.fresh.erase(mag.token);
            }
            if (g_state.dropActive && g_state.dropToken == mag.token) {
                g_state.dropActive = false;
            }
            g_loose[mag.ref] = loose;
            note("Magazine %08X is out: %u rounds", mag.ref, loose.rounds);
        }
    }

    // ------------------------------------------------------------ zones around the body

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

    // Behind the body and above the shoulders: where loot is put away.
    bool overShoulder(RE::PlayerCharacter* player, const RE::NiPoint3& palm) noexcept
    {
        const auto feet = player->GetPosition();
        const float yaw = player->data.angle.z;
        const float forward = (palm.x - feet.x) * std::sin(yaw) + (palm.y - feet.y) * std::cos(yaw);
        return palm.z - feet.z >= g_settings.shoulderHeight && forward < 0.0f;
    }

    // ------------------------------------------------------------ the magazine's life

    void startBlend(const RE::NiTransform& from, float seconds) noexcept
    {
        g_state.blendFrom = from;
        g_state.blendStart = Clock::now();
        g_state.blendSeconds = seconds;
    }

    RE::NiTransform blended(const RE::NiTransform& target, Clock::time_point now) noexcept
    {
        if (g_state.blendSeconds <= 0.0f) {
            return target;
        }
        const float t = std::chrono::duration<float>(now - g_state.blendStart).count() / g_state.blendSeconds;
        if (t >= 1.0f) {
            g_state.blendSeconds = 0.0f;
            return target;
        }
        return blend(g_state.blendFrom, target, t * t * (3.0f - 2.0f * t));
    }

    void releaseHand() noexcept
    {
        unpinHand();
        poseFingers(false);
    }

    void startEject(const Gun& gun, bool hold, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        st.place = kRailAuto;
        st.s = 0.0f;
        st.autoHold = hold;
        st.blendSeconds = 0.0f;
        syncAmmo(gun);
        playSound(1);
        buzz(g_settings.gunHand, 900);
        st.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        note("RELEASE (%s): magazine with %u rounds comes out, chamber %s", hold ? "held" : "tap", st.rounds, st.chamber ? "loaded" : "empty");
    }

    void seat(const Gun& gun) noexcept
    {
        auto& st = g_state;
        st.place = kSeated;
        st.s = 0.0f;
        st.blendSeconds = 0.0f;
        syncAmmo(gun);
        playSound(0);
        buzz(0, 2500);
        buzz(1, 2500);
        releaseHand();
        if (g_settings.regripAfterSeat) {
            griphook::setOwned(false);   // the grip is seen again: the hand may take the pistol now
        }
        if (!st.chamber && !st.hasBolt && st.rounds > 0) {
            st.rounds -= 1;   // no slide / bolt found on this model: the magazine feeds the chamber by itself
            st.chamber = true;
            syncAmmo(gun);
        }
        if (!st.chamber) {
            hud(st.rounds > 0 ? "Магазин вставлен. Патронник пуст: передёрни затвор" : "Магазин пустой");
        }
        note("INSERT: seated, %u rounds, ammoCount %u, chamber %s", st.rounds, gun.data->ammoCount, st.chamber ? "loaded" : "empty");
    }

    // The magazine leaves our hands and becomes a real object; it is drawn falling until that object is there.
    void startDrop(const RE::NiTransform& node, const RE::NiPoint3& velocity, const char* why) noexcept
    {
        auto& st = g_state;
        st.dropToken = spawnLoose(node, st.rounds, why);
        st.dropActive = true;
        st.dropPose = node;
        st.dropVelocity = velocity;
        st.dropRounds = st.rounds;
        st.dropStart = Clock::now();
        st.rounds = 0;
        st.place = kGone;
        st.blendSeconds = 0.0f;
        releaseHand();
    }

    void stash(const char* where) noexcept
    {
        auto& st = g_state;
        char text[96]{};
        std::snprintf(text, sizeof(text), "Магазин убран (%u патр. вернулись в запас)", st.rounds);
        hud(text);
        note("STASH (%s): magazine with %u rounds put away", where, st.rounds);
        st.rounds = 0;
        st.place = kGone;
        st.blendSeconds = 0.0f;
        playSound(4);
        buzz(freeIndex(), 700);
        releaseHand();
    }

    // The free hand closes on a magazine that sticks out: the palm goes to the magazine, not the magazine to the palm.
    void takeFromRail(RE::NiAVObject* root, const Hand& hand) noexcept
    {
        auto& st = g_state;
        const auto pose = railPose(root, st.s);
        auto shifted = pose;
        shifted.translate -= grabPoint(root) - hand.palm;   // where the magazine would be if the palm were already on it
        st.magInWand = relative(hand.wand, shifted);
        st.wristInMag = relative(shifted, hand.wrist);
        st.driveS0 = st.s;
        st.driveAlong0 = alongRail(root, shifted.translate);
        st.place = kRailHand;
        st.autoHold = false;
        st.blendSeconds = 0.0f;
        buzz(freeIndex(), 1200);
        note("TAKE: the free hand has the magazine, %.1f out of the well", st.s);
    }

    // How a magazine from the belt sits in the hand: upright in the fist, rounds forward.
    RE::NiTransform holdPose(const Hand& hand, float scale) noexcept
    {
        const auto bx = axisOf(hand.wrist, 0);   // along the fingers
        const auto bz = axisOf(hand.wrist, 2);   // across the palm
        const float sign = (freeIndex() == 1 ? 1.0f : -1.0f) * (g_settings.holdFlip ? -1.0f : 1.0f);
        const auto y = normalized(bx);
        auto z = bz * sign;
        z = normalized(z - y * dot(z, y));
        RE::NiTransform base = identityTransform();
        setRows(base, cross(y, z), y, z);
        const float d = 0.01745329f;
        Mat turn = axisRotation(0, g_settings.holdRotX * d);
        turn = mul(turn, axisRotation(1, g_settings.holdRotY * d));
        turn = mul(turn, axisRotation(2, g_settings.holdRotZ * d));
        RE::NiTransform pose = identityTransform();
        pose.rotate = fromMat(mul(turn, toMat(base.rotate)));
        pose.scale = scale;
        pose.translate = hand.palm - axisOf(pose, 2) * ((g_state.bottomAt + g_settings.holdUp) * scale) + axisOf(pose, 0) * (g_settings.holdSide * scale) +
                         axisOf(pose, 1) * (g_settings.holdForward * scale);
        return pose;
    }

    std::uint32_t freshMagazineRounds(const Gun& gun, std::uint32_t inventory) noexcept
    {
        const auto& st = g_state;
        const auto used = gun.data->ammoCount + (st.place != kSeated ? st.rounds : 0u) + looseRounds();
        const auto available = inventory > used ? inventory - used : 0u;
        auto capacity = capacityOf(gun);
        if (capacity == 0) {
            capacity = 12;
        }
        return available < capacity ? available : capacity;
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

    // Real magazines: forgotten when they are gone, cleaned up when old, and taken over by this plugin
    // when the free hand brings one to a gun that has none.
    void looseMagazines(const Gun* gun, RE::NiAVObject* root) noexcept
    {
        const auto now = Clock::now();
        auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        std::vector<std::uint32_t> gone;
        std::uint32_t oldest = 0;
        auto oldestBorn = now;
        std::uint32_t adopt = 0;
        RE::NiTransform adoptPose{};
        for (auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->GetParentCell()) {
                // gone, or put away over the shoulder through ROCK: its rounds are back with the ammo you carry
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
            float handDistance = 1e9f;
            for (const auto& hand : g_hands) {
                if (hand.valid) {
                    handDistance = std::min(handDistance, distance(mag3D->world.translate, hand.palm));
                }
            }
            const float age = std::chrono::duration<float>(now - loose.born).count();
            if (gun && looksLikeObject(root) && st.railKnown && st.place == kGone && !adopt && loose.kind == st.kind && age > 0.3f &&
                free.valid && free.grip && distance(mag3D->world.translate, free.palm) < g_settings.handRadius) {
                adopt = id;
                adoptPose = mag3D->world;
            }
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
        if (adopt) {
            // a real magazine held through ROCK, and the gun is empty: from here on it is ours
            adoptPose.scale = gunScale(root) * st.magRest.scale;
            auto node = compose(adoptPose, inverse(st.looseInNode));
            node.scale = adoptPose.scale;
            st.rounds = g_loose[adopt].rounds;
            st.place = kHand;
            st.magInWand = relative(free.wand, node);
            st.wristInMag = relative(node, free.wrist);
            st.mustLeave = false;
            st.velocityValid = false;
            st.dropActive = false;
            st.blendSeconds = 0.0f;
            deleteMagazine(adopt);
            griphook::setOwned(true);
            note("ADOPT: magazine %08X with %u rounds is now carried by the plugin", adopt, st.rounds);
        }
    }

    // The slide: grip it with the free hand, pull it back along the gun, let it go forward.
    // All the way back throws out the chambered round; going forward takes the next one from the magazine.
    void rackByHand(const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* bolt, float dt) noexcept
    {
        auto& st = g_state;
        if (!looksLikeObject(root) || !looksLikeObject(bolt)) {
            st.racking = false;
            return;
        }
        const auto& spare = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        const float scale = gunScale(root);
        // the controller, not the drawn hand: ROCK moves the drawn hand onto whatever it holds
        const auto handLocal = relative(root->world, pointTransform(spare.wand.translate)).translate;
        const bool handBusy = st.place == kRailHand || st.place == kHand || (griphook::owned() && st.armReason != kArmSlide);
        if (!st.racking && spare.valid && spare.grip && !spare.previousGrip && !handBusy) {
            const float toSlide = distance(spare.palm, slideGrabPoint(root, bolt));
            const float toMag = st.place == kRailAuto && st.railKnown ? distance(spare.palm, grabPoint(root)) : 1e9f;
            if (toSlide < g_settings.slideGrabRadius && toSlide <= toMag) {
                st.racking = true;
                st.rackGrabY = handLocal.y + st.rackPull;
                st.rackGrabHands = distance(spare.wand.translate, firing.wand.translate);
                st.rackGrabPull = st.rackPull;
                buzz(freeIndex(), 500);
                detail("Slide taken by the free hand (%.1f from its middle)", toSlide);
            }
        }
        if (st.racking && !(spare.valid && spare.grip)) {
            st.racking = false;   // let go: the spring sends it home
        }
        const float lockAt = g_settings.rackTravel * std::clamp(g_settings.slideLockAt, 0.0f, 1.0f);
        if (st.racking) {
            // along the gun, and by the hands closing in (when the gun turns after the hand, the first stays put)
            const float byGun = st.rackGrabY - handLocal.y;
            const float byHands = st.rackGrabPull + (st.rackGrabHands - distance(spare.wand.translate, firing.wand.translate)) / scale;
            st.rackPull = std::clamp(std::max(byGun, byHands), st.slideLocked ? lockAt : 0.0f, g_settings.rackTravel);
            if (st.slideLocked && st.rackPull >= g_settings.rackTravel * 0.95f) {
                st.slideLocked = false;   // pulled off the slide stop: let go and it runs home
                st.rackCycled = true;
                playSound(2);
                logNote("RACK: slide pulled off the stop");
            }
        } else if (st.slideLocked) {
            st.rackPull = approach(st.rackPull, lockAt, 80.0f * dt);   // held open after the last shot
        } else {
            st.rackPull = approach(st.rackPull, 0.0f, 25.0f * g_settings.rackTravel * dt);
        }
        if (!st.rackCycled && !st.slideLocked && st.rackPull >= g_settings.rackTravel * 0.85f) {
            st.rackCycled = true;
            if (st.chamber) {
                st.chamber = false;   // the round flies out (back to the ammo you carry)
            }
            syncAmmo(gun);
            playSound(2);
            buzz(freeIndex(), 1500);
            logNote("RACK: slide all the way back");
        }
        if (st.rackCycled && st.rackPull <= g_settings.rackTravel * 0.25f) {
            st.rackCycled = false;
            if (st.place == kSeated && st.rounds > 0) {
                st.rounds -= 1;
                st.chamber = true;
            }
            syncAmmo(gun);
            playSound(3);
            buzz(freeIndex(), 2000);
            buzz(g_settings.gunHand, 1500);
            note("RACK: slide home, chamber %s, magazine %u", st.chamber ? "loaded" : "empty", st.rounds);
        }
        if (st.rackPull > 0.0f) {
            refreshSubtree(bolt, axisOf(root->world, 1) * (-st.rackPull * scale));
        }
    }

    // ------------------------------------------------------------ every frame

    // The gun is gone from the hand while its magazine was in play: the magazine goes back to the pouch.
    void putAway(const char* why) noexcept
    {
        auto& st = g_state;
        if (st.place == kRailAuto || st.place == kRailHand || st.place == kHand) {
            note("The magazine in play is put away (%s): %u rounds stay with the ammo you carry", why, st.rounds);
            st.rounds = 0;
            st.place = kGone;
        }
        releaseHand();
        griphook::setArmed(false);
        st.armReason = kArmNone;
    }

    std::uint64_t buttonMask(int id) noexcept
    {
        return vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(id));
    }

    bool menuOpen() noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (frik && frik->isWristPipboyOpen && frik->isWristPipboyOpen()) {
            return true;
        }
        if (frik && frik->isConfigOpen && frik->isConfigOpen()) {
            return true;
        }
        const auto* ui = RE::UI::GetSingleton();
        return ui && ui->menuMode != 0;
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
        auto& st = g_state;
        const auto now = Clock::now();
        float dt = st.lastFrame == Clock::time_point{} ? 0.011f : std::chrono::duration<float>(now - st.lastFrame).count();
        st.lastFrame = now;
        dt = std::clamp(dt, 0.001f, 0.05f);

        const auto gun = equippedGun(player);
        if (gun.weapon != st.weapon) {
            releaseHand();
            griphook::setArmed(false);
            st = State{};
            st.lastFrame = now;
            st.weapon = gun.weapon;
            dropRound();
            g_round.modelReady = false;
            std::string magModel;
            const int kind = gun.weapon && gun.data ? magazineKindOf(player, gun.weapon, magModel) : -1;
            const bool pistol10 = gun.weapon && gun.data && gun.weapon->GetFormID() == k10mmPistol;
            st.managed = gun.weapon && gun.data && (pistol10 || (kind >= 0 && g_settings.allGuns));
            st.equippedAt = now;
            const auto total = gun.data ? gun.data->ammoCount : 0u;
            st.chamber = total > 0;
            st.rounds = total > 0 ? total - 1 : 0;
            st.kind = kind >= 0 ? kind : (capacityOf(gun) > 12 ? 1 : 0);
            note("Equipped weapon %08X, capacity %u, rounds %u, magazine '%s' (kind %d): %s", gun.weapon ? gun.weapon->GetFormID() : 0u,
                capacityOf(gun), total, magModel.c_str(), kind, st.managed ? "reloaded by hand" : "not handled (no magazine of group 1)");
        }

        auto* root = st.managed ? weaponRoot(player) : nullptr;
        if (!st.managed || !looksLikeObject(root)) {
            if (st.managed) {
                putAway("the gun is not drawn");
            } else {
                releaseHand();
                griphook::setArmed(false);
            }
            looseMagazines(nullptr, nullptr);
            hideRound();
            return;
        }

        const auto& free = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;

        if (!st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
            st.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        // the game's own reload (raw VR gunState)
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        const bool pressed = gunState == reloading && st.lastGunState != reloading && st.lastGunState != 0xFFFFFFFF;
        if (gunState != st.lastGunState) {
            detail("gunState %u -> %u", st.lastGunState, gunState);
        }
        st.lastGunState = gunState;

        // the moving parts: remembered at rest, held still while the game plays its reload
        std::array<RE::NiAVObject*, 5> parts{};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i] = findNamed(root, kMovingParts[i]);
        }
        auto* magazine = parts[0];
        auto* bolt = parts[1];
        if (st.hasBolt != looksLikeObject(bolt)) {
            st.hasBolt = looksLikeObject(bolt);
            note("Slide / bolt node: %s", st.hasBolt ? "found" : "none (rounds are chambered by the magazine)");
        }
        // an empty gun: the game would play its own reload by itself; the parts stay put (the slide stays open)
        const bool frozen = g_settings.blockReload && (gunState == reloading || now < st.freezeUntil || pressed || st.slideLocked || st.lastAmmo == 0);
        const bool quiet = !frozen && st.place == kSeated && st.s <= 0.0f && gunState == static_cast<std::uint32_t>(g_settings.idleGunState) &&
                           !st.racking && st.rackPull <= 0.0f && !st.slideLocked && now - st.lastShot > std::chrono::milliseconds(600) &&
                           now - st.equippedAt > std::chrono::milliseconds(1500);
        if (quiet && gun.data->ammoCount > 0) {
            bool all = true;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    st.idle[i] = parts[i]->local;
                } else {
                    all = all && i != 0;
                }
            }
            st.idleKnown = all;
            learnSlide(root, bolt);
        }
        if (quiet && !st.railKnown && looksLikeObject(magazine)) {
            learnRail(root, magazine);
        }
        if (frozen && st.idleKnown) {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    parts[i]->local = st.idle[i];
                    refreshSubtree(parts[i], RE::NiPoint3{});
                }
            }
        }

        // the rounds: shots take them, the game's own refills are undone
        auto ammo = gun.data->ammoCount;
        if (st.lastAmmo != 0xFFFFFFFF) {
            if (ammo > st.lastAmmo && g_settings.blockReload) {
                note("Game refill %u -> %u reverted", st.lastAmmo, ammo);
                ammo = st.lastAmmo;
                gun.data->ammoCount = ammo;
            } else if (ammo < st.lastAmmo) {
                st.lastShot = now;
                st.chamber = ammo > 0;
                if (st.place == kSeated) {
                    st.rounds = ammo > 0 ? ammo - 1 : 0;
                    if (ammo == 0 && st.lastAmmo == 1) {
                        st.slideLocked = true;   // the last round of a magazine: the slide stays open
                        detail("Last round fired: slide locked open");
                    }
                } else if (ammo != 0) {
                    ammo = 0;                    // no magazine: the chambered round was the only one
                    st.chamber = false;
                    gun.data->ammoCount = 0;
                }
                detail("Shot: %u left (chamber %d, magazine %u)", ammo, st.chamber ? 1 : 0, st.rounds);
            }
        }
        st.lastAmmo = ammo;

        // the reload button: a tap reaches the game (ROCK sends it on release) and shows as gunState; a hold is read here
        const bool triggerHeld = (firing.buttons & buttonMask(g_settings.triggerButton)) != 0;
        const bool buttonNow = (firing.buttons & buttonMask(g_settings.reloadButton)) != 0;
        const bool buttonBefore = (firing.previousButtons & buttonMask(g_settings.reloadButton)) != 0;
        if (buttonNow != buttonBefore) {
            st.buttonEdge = now;
        }
        if (buttonNow && !buttonBefore) {
            st.buttonSince = now;
            st.buttonUsed = false;
        }
        st.buttonDown = buttonNow;
        const bool ready = st.railKnown && looksLikeObject(magazine);
        if (st.buttonDown && !st.buttonUsed && g_settings.blockReload &&
            std::chrono::duration<float>(now - st.buttonSince).count() >= g_settings.holdSeconds) {
            st.buttonUsed = true;
            if (st.place == kSeated && ready && !triggerHeld && !st.racking && !menuOpen()) {
                startEject(gun, true, now);
            }
        }
        if (pressed && g_settings.blockReload) {
            st.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
            const bool byButton = now - st.buttonEdge < std::chrono::milliseconds(700);
            const bool justEmptied = ammo == 0 && now - st.lastShot < std::chrono::milliseconds(2500);
            if (!byButton && (triggerHeld || justEmptied)) {
                // the game reloads by itself when the trigger is pulled on an empty gun: a dry click, not a reload
                logNote("The game's own reload of an empty gun: ignored");
            } else if (st.place == kSeated) {
                if (ready) {
                    startEject(gun, false, now);
                } else {
                    logNote("Reload asked before the gun was measured at rest: ignored");
                }
            } else if (st.place == kRailAuto) {
                st.autoHold = false;
            } else if (st.place == kGone) {
                hud("Магазина нет: возьми новый с пояса (грип свободной рукой у пояса)");
            }
        }

        const float magScale = gunScale(root) * (st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f);

        // the free hand closes
        const bool press = free.valid && free.grip && !free.previousGrip;
        if (press) {
            detail("Free hand grip: %s, reason %d, place %d, hidden reads %u", griphook::owned() ? "ours" : "everyone's", st.armReason,
                st.place, griphook::hiddenCount());
        }
        if (press && griphook::owned()) {
            if (st.armReason == kArmRail && st.place == kRailAuto) {
                takeFromRail(root, free);
            } else if (st.armReason == kArmBelt && st.place == kGone) {
                const auto rounds = freshMagazineRounds(gun, inventory);
                if (rounds == 0) {
                    hud("Патронов для магазина нет");
                } else {
                    const auto pose = holdPose(free, magScale);
                    st.place = kHand;
                    st.rounds = rounds;
                    st.magInWand = relative(free.wand, pose);
                    st.wristInMag = relative(pose, free.wrist);
                    st.mustLeave = false;
                    st.velocityValid = false;
                    st.blendSeconds = 0.0f;
                    st.lastBelt = now;
                    playSound(4);
                    buzz(freeIndex(), 1200);
                    note("BELT: a magazine with %u rounds in the free hand (%u in the pouch)", rounds, inventory);
                    for (int i = 0; i < 3; ++i) {
                        const auto a = axisOf(free.wrist, i);
                        const auto b = axisOf(pose, i);
                        detail("  wrist axis %d (%.2f, %.2f, %.2f), magazine axis %d (%.2f, %.2f, %.2f)", i, a.x, a.y, a.z, i, b.x, b.y, b.z);
                    }
                }
            }
        } else if (press && g_settings.spareFromBelt && st.place != kGone && !st.racking && inBelt(player, free.palm) &&
                   now - st.lastBelt > std::chrono::milliseconds(700)) {
            // the gun still has its magazine: a spare one is a real object for ROCK to hold
            const auto rounds = freshMagazineRounds(gun, inventory);
            st.lastBelt = now;
            if (rounds > 0) {
                spawnLoose(holdPose(free, magScale), rounds, "a spare from the belt");
            }
        }

        // the magazine's way
        switch (st.place) {
        case kRailAuto:
            if (st.autoHold) {
                if (!st.buttonDown) {
                    st.autoHold = false;   // the button is let go: it falls
                }
                st.s = approach(st.s, std::min(g_settings.extendDistance, st.length - 0.5f), g_settings.slideSpeed * dt);
            } else {
                st.s = approach(st.s, st.length + 0.5f, g_settings.slideSpeed * 1.6f * dt);
                if (st.s >= st.length) {
                    startDrop(railPose(root, st.s), axisWorld(root) * (g_settings.slideSpeed * 1.6f * gunScale(root)), "it fell out of the gun");
                }
            }
            break;
        case kRailHand:
            {
                if (!free.valid || !free.grip) {
                    if (st.s <= g_settings.seatRelease) {
                        seat(gun);
                    } else {
                        startDrop(railPose(root, st.s), st.velocityValid ? st.velocity : RE::NiPoint3{}, "let go in the well");
                    }
                    break;
                }
                const auto carried = compose(free.wand, st.magInWand);
                const float target = st.driveS0 + (alongRail(root, carried.translate) - st.driveAlong0);
                const float limit = st.length + g_settings.captureAhead + 1.0f;
                if (target > limit) {
                    // clear of the well: from here it goes with the hand
                    startBlend(st.lastDrawnValid ? st.lastDrawn : railPose(root, st.s), 0.20f);
                    st.magInWand = relative(free.wand, holdPose(free, gunScale(root)));   // held by its baseplate, as ROCK holds it
                    st.place = kHand;
                    st.mustLeave = true;
                    unpinHand();
                    playSound(1);
                    note("OUT: the magazine left the well in the free hand (%u rounds)", st.rounds);
                    break;
                }
                const float before = st.s;
                st.s = std::clamp(target, 0.0f, limit);
                if (static_cast<int>(before) != static_cast<int>(st.s)) {
                    buzz(freeIndex(), 250);   // it rubs along the well
                }
                if (st.s <= g_settings.seatAt) {
                    seat(gun);
                }
            }
            break;
        case kHand:
            {
                const auto target = compose(free.wand, st.magInWand);
                if (!free.valid || !free.grip) {
                    const auto pose = st.lastDrawnValid ? st.lastDrawn : target;
                    if (inBelt(player, free.palm)) {
                        stash("at the belt");
                    } else if (overShoulder(player, free.palm)) {
                        stash("over the shoulder");
                    } else {
                        startDrop(pose, st.velocityValid ? st.velocity : RE::NiPoint3{}, "let go");
                    }
                    break;
                }
                if (!st.railKnown) {
                    break;
                }
                float side = 0.0f;
                const float top = alongRail(root, topOf(target), &side) + st.topAt * st.magRest.scale;   // as if it were on the rail this far out
                const bool upright = dot(axisOf(target, 2), axisWorld(root)) < -0.5f;
                const float mouth = st.length;
                if (st.mustLeave) {
                    if (top > mouth + g_settings.captureAhead + 2.5f || side > g_settings.wellSide + 2.5f) {
                        st.mustLeave = false;
                    }
                } else if (upright && side <= g_settings.wellSide && top <= mouth + g_settings.captureAhead && top >= mouth - 3.0f) {
                    // the mouth of the well takes the top of the magazine: from here it moves only along the well
                    st.place = kRailHand;
                    st.s = std::clamp(top, 0.0f, mouth + g_settings.captureAhead);
                    st.driveS0 = st.s;
                    st.driveAlong0 = alongRail(root, target.translate);
                    startBlend(st.lastDrawnValid ? st.lastDrawn : target, 0.12f);
                    buzz(freeIndex(), 900);
                    note("CATCH: the well has the magazine, %.1f out, %.1f off the line", st.s, side);
                }
            }
            break;
        default:
            break;
        }

        // a dropped magazine falls until the real one is there
        if (st.dropActive) {
            st.dropVelocity.z -= kGravity * dt;
            st.dropPose.translate += st.dropVelocity * dt;
            const float floor = player->GetPosition().z + 1.5f;
            if (st.dropPose.translate.z < floor) {
                st.dropPose.translate.z = floor;
                st.dropVelocity = {};
            }
            setFresh(st.dropToken, st.dropPose);
            if (now - st.dropStart > std::chrono::seconds(3)) {
                st.dropActive = false;
            }
        }

        // where the magazine is drawn
        bool draw = false;
        bool carried = false;
        RE::NiTransform drawn{};
        std::uint32_t shown = st.rounds;
        if (st.place == kRailAuto) {
            drawn = railPose(root, st.s);
            draw = true;
        } else if (st.place == kRailHand) {
            drawn = blended(railPose(root, st.s), now);
            draw = carried = true;
        } else if (st.place == kHand) {
            drawn = blended(compose(free.wand, st.magInWand), now);
            drawn.scale = magScale;
            draw = carried = true;
        } else if (st.place == kGone && st.dropActive) {
            drawn = st.dropPose;
            shown = st.dropRounds;
            draw = true;
        }
        if (carried) {
            if (st.velocityValid) {
                st.velocity = st.velocity * 0.5f + (drawn.translate - st.lastMagAt) * (0.5f / dt);
            } else {
                st.velocity = {};
            }
            st.lastMagAt = drawn.translate;
            st.velocityValid = true;
        } else {
            st.velocityValid = false;
        }
        if (looksLikeObject(magazine)) {
            if (st.place == kSeated) {
                setCulled(magazine, false);
            } else if (draw) {
                setCulled(magazine, false);
                placeNode(magazine, drawn);
            } else {
                setCulled(magazine, true);
            }
            showRounds(magazine, shown);
        }
        st.lastDrawn = drawn;
        st.lastDrawnValid = draw;

        // the slide: the game shows it open on an empty gun, but here it is open only after the last shot
        if (st.idleKnown && looksLikeObject(bolt) && (gun.data->ammoCount == 0 || st.slideLocked || st.racking || st.rackPull > 0.0f)) {
            bolt->local = st.idle[1];
            refreshSubtree(bolt, RE::NiPoint3{});
        }
        rackByHand(gun, root, bolt, dt);
        drawRound(gun, root);

        // the free hand stays on what it holds: the magazine in the well, or the slide
        if (st.racking && looksLikeObject(bolt) && free.valid) {
            if (!st.rackPinned) {
                st.rackPinned = true;
                st.wristInBolt = relative(bolt->world, free.wrist);
            }
            pinHand(compose(bolt->world, st.wristInBolt));
        } else {
            st.rackPinned = false;
            if (st.place == kRailHand && free.valid) {
                pinHand(compose(drawn, st.wristInMag));
            } else {
                unpinHand();
            }
        }
        poseFingers(st.place == kRailHand || st.place == kHand || st.racking);

        looseMagazines(&gun, root);

        // whose the free hand's next grip press will be
        if (!free.grip) {
            int reason = kArmNone;
            if (free.valid) {
                const float toSlide = looksLikeObject(bolt) ? distance(free.palm, slideGrabPoint(root, bolt)) : 1e9f;
                const float toMag = st.place == kRailAuto && st.railKnown ? distance(free.palm, grabPoint(root)) : 1e9f;
                if (toMag < g_settings.magGrabRadius && toMag <= toSlide) {
                    reason = kArmRail;
                } else if (toSlide < g_settings.slideGrabRadius) {
                    reason = kArmSlide;
                } else if (st.place == kGone && inBelt(player, free.palm)) {
                    reason = kArmBelt;
                } else if (g_settings.blockTwoHand && st.place != kSeated && distance(free.palm, root->world.translate) < g_settings.gunRadius) {
                    reason = kArmBlock;
                }
            }
            st.armReason = reason;
            griphook::setArmed(reason != kArmNone);
        }

        // the grip of some controllers is also their third axis
        if (now - st.axisChecked > std::chrono::seconds(3)) {
            st.axisChecked = now;
            const auto device = controllerOf(freeIndex());
            if (g_vr && device != vr::k_unTrackedDeviceIndexInvalid) {
                vr::ETrackedPropertyError error = vr::TrackedProp_Success;
                const auto type = g_vr->GetInt32TrackedDeviceProperty(device, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + 2), &error);
                const bool isGrip = error == vr::TrackedProp_Success && type == vr::k_eControllerAxis_Trigger;
                griphook::setAxis2IsGrip(isGrip);
                if (!st.axisLogged) {
                    st.axisLogged = true;
                    note("Free hand controller: third axis type %d (%s)", static_cast<int>(type), isGrip ? "a grip trigger, hidden with the grip" : "left alone");
                }
            }
        }
    }

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        __try {
            update();
        } __except (1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logWarning("A fault inside the frame update was caught; ManualReloadF keeps running");
            }
        }
    }

    void hookLog(const char* text) noexcept
    {
        logNote(text);
    }

    void gameReady() noexcept
    {
        static bool done = false;
        if (!done) {
            done = true;
            const int error = FRIKApiV2::initialize(3);
            if (error != 0) {
                note("FRIK API v2.3+ unavailable (error %d); ManualReloadF cannot run", error);
            } else {
                const bool hooked = FRIKApiV2::inst->registerFrameCallback(kFrikTag,
                    static_cast<std::uint32_t>(FRIKApiV2::FramePhase::AfterWorldFinal), onFrikFrame, nullptr, 0);
                note("FRIK %s, API v2.%u: frame callback %s", FRIKApiV2::inst->getModVersion(), FRIKApiV2::inst->getVersion(),
                    hooked ? "registered" : "FAILED");
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
        if (g_vr) {
            griphook::install(&hookLog, g_settings.hideGrip);
        }
        // a loaded save: magazines of the last session are gone or unknown
        releaseHand();
        griphook::setArmed(false);
        griphook::setOwned(false);
        g_loose.clear();
        g_pending.clear();
        g_state = State{};
        logNote("ManualReloadF ready (build " MANUALRELOAD_BUILD ")");
    }
}

namespace manualreload::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "ManualReloadF",
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
            logWarning("Could not register Papyrus functions; magazines cannot be dropped as objects");
        }
    }
}
