// RigVR: a load-bearing rig for Fallout 4 VR (Into the Radius style).
//
// A mannequin stands in your settlement and wears the rig. You take pouches (magazine pouch, first aid,
// grenades, holster, dump pouch, ammo loops) from the shelf beside it and hang them where you like, then put the
// rig on: every pouch is on your body where it was on the mannequin. Let go of an item over a pouch and it goes
// in; grip at a pouch and what is in it is in your hand.
//
// How it is built:
//   - everything you see is drawn by this plugin: static objects (made by the script) whose 3D is placed every
//     frame on the mannequin or on your body (FRIK bones), the same way ManualReloadF draws its round.
//   - what a pouch holds is a reservation of items that really are in your inventory (the game keeps counting
//     them, weight and all). Taking one out drops it from the inventory into your hand, where ROCK holds it.
//   - the rig (where each pouch hangs, what it holds) is kept by the quest script in the save.
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using rigvr::runtime::logInfo;
    using rigvr::runtime::logNote;
    using rigvr::runtime::logWarning;

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

    // ------------------------------------------------------------ the rig's shape

    constexpr int kSlots = 11;
    constexpr int kPouchTypes = 6;   // 1..6
    constexpr int kItemsPerPouch = 4;

    enum PouchType : int
    {
        kEmpty = 0,
        kMag = 1,
        kMed = 2,
        kGrenade = 3,
        kHolster = 4,
        kDump = 5,
        kBandolier = 6,
    };

    const char* pouchName(int type) noexcept
    {
        switch (type) {
        case kMag: return "Подсумок для магазинов";
        case kMed: return "Аптечка";
        case kGrenade: return "Гранатный подсумок";
        case kHolster: return "Кобура";
        case kDump: return "Сбросной мешок";
        case kBandolier: return "Патронташ";
        default: return "";
        }
    }

    // Index of each static object in the script's RigVR_Stats list.
    enum StatKind : int
    {
        kStatMannequin = 0,
        kStatRig = 1,
        kStatGhost = 2,
        kStatCursor = 3,
        kStatPouch0 = 3,   // + pouch type
    };

    struct SlotPlace
    {
        int   group;     // 0 belt (anchored at the hips), 1 chest
        float x, y, z;   // right, forward, up from the anchor (game units)
        float yaw;       // the pouch turned this much around the vertical (degrees, + = to the right)
    };

    // Default places: 6 on the chest panel, 5 around the belt. Each can be moved in RigVR.ini (Slot0=x,y,z,yaw).
    std::array<SlotPlace, kSlots> g_places{ {
        { 1, -8.0f, 9.0f, 4.0f, 0.0f },
        { 1, 0.0f, 9.5f, 4.0f, 0.0f },
        { 1, 8.0f, 9.0f, 4.0f, 0.0f },
        { 1, -8.0f, 9.0f, -6.0f, 0.0f },
        { 1, 0.0f, 9.5f, -6.0f, 0.0f },
        { 1, 8.0f, 9.0f, -6.0f, 0.0f },
        { 0, -13.0f, 1.0f, -2.0f, -90.0f },
        { 0, -9.0f, 8.0f, 0.0f, -35.0f },
        { 0, 9.0f, 8.0f, 0.0f, 35.0f },
        { 0, 13.0f, 1.0f, -4.0f, 90.0f },
        { 0, 0.0f, -10.5f, 0.0f, 180.0f },
    } };

    int capacityOf(int type) noexcept
    {
        switch (type) {
        case kMag: return 3;
        case kMed: return 4;
        case kGrenade: return 2;
        case kHolster: return 1;
        case kDump: return 12;
        case kBandolier: return 12;
        default: return 0;
        }
    }

    // ------------------------------------------------------------ settings

    struct Settings
    {
        int   gunHand{ 0 };            // 0 = right hand holds the gun
        int   gripButton{ 2 };
        bool  haptics{ true };
        bool  messages{ true };
        float slotRadius{ 8.0f };      // the palm this close to a pouch / a free place
        float mannequinReach{ 150.0f };// the mannequin is used within this distance
        float wearRadius{ 20.0f };     // both hands this close to the rig's chest to put it on / take it off
        float wearWindow{ 0.5f };      // ..both grips pressed within this many seconds
        float packSeconds{ 1.5f };     // hold the grip on the mannequin's head this long to fold it up
        float storeRadius{ 16.0f };    // an item let go this close to the palm over a pouch goes in
        float mouthRadius{ 12.0f };    // a chem / food from a pouch this close to the mouth is used
        float holsterCooldown{ 1.0f };
        float beltHeight{ 66.0f };     // used without FRIK bones (and on the mannequin)
        float chestHeight{ 92.0f };
        float beltUp{ 0.0f };          // move the belt places up / down on the body
        float chestUp{ 0.0f };
        float bodyForward{ 0.0f };     // move all places forward / back on the body
        float bodyScale{ 1.0f };       // places on the body are this much further out than on the mannequin
        bool  useBones{ true };
        bool  showRigOnBody{ false };  // the rig's outline on yourself (it is always shown on the mannequin)
        bool  showPouchesOnBody{ true };
        float trayX{ 30.0f };          // the shelf of pouches beside the mannequin
        float trayY{ 4.0f };
        float trayZ{ 104.0f };
        float trayStep{ 9.0f };
    };
    Settings g_settings;

    bool parseFloats(const std::string& text, float* out, int count) noexcept
    {
        const char* at = text.c_str();
        for (int i = 0; i < count; ++i) {
            char* end = nullptr;
            out[i] = std::strtof(at, &end);
            if (end == at) {
                return false;
            }
            at = end;
            while (*at == ' ' || *at == ',' || *at == '\t') {
                ++at;
            }
        }
        return true;
    }

    void loadSettings() noexcept
    {
        struct Entry { const char* key; int type; void* value; };
        auto& s = g_settings;
        const Entry entries[]{
            { "iGunHand", 1, &s.gunHand }, { "iGripButton", 1, &s.gripButton }, { "bHaptics", 0, &s.haptics },
            { "bMessages", 0, &s.messages }, { "fSlotRadius", 2, &s.slotRadius }, { "fMannequinReach", 2, &s.mannequinReach },
            { "fWearRadius", 2, &s.wearRadius }, { "fWearWindow", 2, &s.wearWindow }, { "fPackSeconds", 2, &s.packSeconds },
            { "fStoreRadius", 2, &s.storeRadius }, { "fMouthRadius", 2, &s.mouthRadius }, { "fHolsterCooldown", 2, &s.holsterCooldown },
            { "fBeltHeight", 2, &s.beltHeight }, { "fChestHeight", 2, &s.chestHeight }, { "fBeltUp", 2, &s.beltUp },
            { "fChestUp", 2, &s.chestUp }, { "fBodyForward", 2, &s.bodyForward }, { "fBodyScale", 2, &s.bodyScale },
            { "bUseBones", 0, &s.useBones }, { "bShowRigOnBody", 0, &s.showRigOnBody }, { "bShowPouchesOnBody", 0, &s.showPouchesOnBody },
            { "fTrayX", 2, &s.trayX }, { "fTrayY", 2, &s.trayY }, { "fTrayZ", 2, &s.trayZ }, { "fTrayStep", 2, &s.trayStep },
        };
        try {
            std::ifstream file("Data/F4SE/Plugins/RigVR.ini");
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
                const auto valueText = line.substr(equals + 1);
                if (key.rfind("Slot", 0) == 0 && key.size() <= 6) {
                    const int index = std::atoi(key.c_str() + 4);
                    float v[4]{};
                    if (index >= 0 && index < kSlots && parseFloats(valueText, v, 4)) {
                        g_places[index].x = v[0];
                        g_places[index].y = v[1];
                        g_places[index].z = v[2];
                        g_places[index].yaw = v[3];
                    }
                    continue;
                }
                const float value = std::strtof(valueText.c_str(), nullptr);
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
        note("Settings: gun hand %d, slot radius %.1f, bones %d, belt %.0f chest %.0f", s.gunHand, s.slotRadius, s.useBones ? 1 : 0,
            s.beltHeight, s.chestHeight);
    }

    // ------------------------------------------------------------ memory safety

    bool readable(const void* address, std::size_t size) noexcept
    {
        if (!address) {
            return false;
        }
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != 0x1000u) {
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

    // ------------------------------------------------------------ math (the game's row convention: world = local * parent)

    float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

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

    // A frame standing upright, turned by `yaw` (radians; 0 = facing +Y, growing to the right like the game's angle).
    struct Frame
    {
        RE::NiPoint3 origin{};
        float        yaw{ 0.0f };

        RE::NiPoint3 right() const noexcept { return { std::cos(yaw), -std::sin(yaw), 0.0f }; }
        RE::NiPoint3 forward() const noexcept { return { std::sin(yaw), std::cos(yaw), 0.0f }; }

        RE::NiPoint3 at(float x, float y, float z) const noexcept
        {
            return origin + right() * x + forward() * y + RE::NiPoint3{ 0.0f, 0.0f, z };
        }
    };

    RE::NiTransform uprightTransform(const RE::NiPoint3& at, float yaw) noexcept
    {
        RE::NiTransform t;
        const RE::NiPoint3 r{ std::cos(yaw), -std::sin(yaw), 0.0f };
        const RE::NiPoint3 f{ std::sin(yaw), std::cos(yaw), 0.0f };
        // rows are the local axes in world space
        t.rotate.entry[0][0] = r.x; t.rotate.entry[0][1] = r.y; t.rotate.entry[0][2] = r.z;
        t.rotate.entry[1][0] = f.x; t.rotate.entry[1][1] = f.y; t.rotate.entry[1][2] = f.z;
        t.rotate.entry[2][0] = 0.0f; t.rotate.entry[2][1] = 0.0f; t.rotate.entry[2][2] = 1.0f;
        t.translate = at;
        t.scale = 1.0f;
        return t;
    }

    float yawOfRight(const RE::NiPoint3& rightVector, float fallback) noexcept
    {
        // right = (cos a, -sin a)  ->  a = atan2(-right.y, right.x)
        const float length = std::sqrt(rightVector.x * rightVector.x + rightVector.y * rightVector.y);
        if (length < 1.0f) {
            return fallback;
        }
        return std::atan2(-rightVector.y, rightVector.x);
    }

    constexpr float kDegrees = 3.14159265f / 180.0f;

    // Places a reference's 3D: the root at `world` (local too, so the engine keeps it), children follow,
    // bounds move along and the nodes above are made to reach it (or it is culled with them).
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
            if (auto* up = object->parent; looksLikeObject(up)) {
                const auto before = object->world;
                object->world = compose(up->world, object->local);
                moveBound(object, before);
            }
            return true;
        });
        const float reach = root->worldBound.fRadius > 0.0f && root->worldBound.fRadius < 1000.0f ? root->worldBound.fRadius : 20.0f;
        RE::NiAVObject* up = root->parent;
        for (int i = 0; i < 8 && looksLikeObject(up); ++i) {
            auto& bound = up->worldBound;
            const float needed = distance(bound.center, world.translate) + reach;
            if (bound.fRadius > 0.0f && bound.fRadius < needed && needed < 20000.0f) {
                bound.fRadius = needed;
            }
            auto* next = up->parent;
            up = looksLikeObject(next) ? next : nullptr;
        }
    }

    // ------------------------------------------------------------ game helpers

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept
    {
        if (!id) {
            return nullptr;
        }
        auto* form = RE::TESForm::GetFormByID(id);
        if (!form || !readable(form, sizeof(RE::TESObjectREFR))) {
            return nullptr;
        }
        return form->As<RE::TESObjectREFR>();
    }

    RE::TESForm* lookupForm(std::uint32_t id) noexcept
    {
        return id ? RE::TESForm::GetFormByID(id) : nullptr;
    }

    std::uint32_t inventoryCount(RE::PlayerCharacter* player, std::uint32_t formID) noexcept
    {
        auto* object = lookupForm(formID);
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

    // The gun the player holds (not fists, grenades or mines): its base form.
    RE::TESObjectWEAP* equippedGun(RE::PlayerCharacter* player) noexcept
    {
        auto* process = player ? player->currentProcess : nullptr;
        if (!process || !readable(process, sizeof(RE::AIProcess))) {
            return nullptr;
        }
        auto* middle = process->middleHigh;
        if (!middle || !readable(middle, sizeof(RE::MiddleHighProcessData))) {
            return nullptr;
        }
        auto& items = middle->equippedItems;
        if (items.size() > 16) {
            return nullptr;
        }
        for (auto& item : items) {
            auto* form = item.item.object;
            if (!form || !readable(form, sizeof(RE::TESForm)) || form->GetFormType() != RE::ENUM_FORM_ID::kWEAP) {
                continue;
            }
            auto* weapon = static_cast<RE::TESObjectWEAP*>(form);
            if (weapon->weaponData.type.get() == RE::WEAPON_TYPE::kGun) {
                return weapon;
            }
        }
        return nullptr;
    }

    std::string modelOf(RE::TESForm* form) noexcept
    {
        if (!form || form->GetFormType() != RE::ENUM_FORM_ID::kMISC) {
            return {};
        }
        auto* misc = static_cast<RE::TESObjectMISC*>(form);
        const char* path = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).GetModel();
        std::string text = path ? path : "";
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // Can this item go into a pouch of this type?
    bool accepts(int type, RE::TESForm* form) noexcept
    {
        if (!form) {
            return false;
        }
        const auto kind = form->GetFormType();
        switch (type) {
        case kMag:
            return kind == RE::ENUM_FORM_ID::kAMMO || (kind == RE::ENUM_FORM_ID::kMISC && modelOf(form).find("mag") != std::string::npos);
        case kMed:
            return kind == RE::ENUM_FORM_ID::kALCH;
        case kGrenade:
            if (kind == RE::ENUM_FORM_ID::kWEAP) {
                const auto t = static_cast<RE::TESObjectWEAP*>(form)->weaponData.type.get();
                return t == RE::WEAPON_TYPE::kGrenade || t == RE::WEAPON_TYPE::kMine;
            }
            return false;
        case kHolster:
            return kind == RE::ENUM_FORM_ID::kWEAP && static_cast<RE::TESObjectWEAP*>(form)->weaponData.type.get() == RE::WEAPON_TYPE::kGun;
        case kDump:
            return kind == RE::ENUM_FORM_ID::kMISC || kind == RE::ENUM_FORM_ID::kALCH || kind == RE::ENUM_FORM_ID::kAMMO ||
                   kind == RE::ENUM_FORM_ID::kBOOK || kind == RE::ENUM_FORM_ID::kNOTE || kind == RE::ENUM_FORM_ID::kKEYM ||
                   kind == RE::ENUM_FORM_ID::kINGR || kind == RE::ENUM_FORM_ID::kARMO ||
                   (kind == RE::ENUM_FORM_ID::kWEAP && static_cast<RE::TESObjectWEAP*>(form)->weaponData.type.get() != RE::WEAPON_TYPE::kGun);
        case kBandolier:
            return kind == RE::ENUM_FORM_ID::kAMMO;
        default:
            return false;
        }
    }

    // ------------------------------------------------------------ controllers and hands

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
        bool         valid{ false };
        RE::NiPoint3 palm{};
        bool         grip{ false };
        bool         previousGrip{ false };
        bool pressed() const noexcept { return grip && !previousGrip; }
        bool released() const noexcept { return !grip && previousGrip; }
    };
    std::array<Hand, 2> g_hands;   // 0 right, 1 left

    FRIKApiV2::Hand frikHand(int index) noexcept { return index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left; }

    void readHand(int index) noexcept
    {
        auto& hand = g_hands[index];
        hand.previousGrip = hand.grip;
        hand.valid = false;
        hand.grip = false;
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid && g_vr) {
            vr::VRControllerState_t state{};
            if (g_vr->GetControllerState(device, &state, sizeof(state))) {
                hand.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
            }
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
        hand.palm = wand.translate;
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            const float length = distance(tip, wrist);
            if (length < 40.0f && length > 0.1f) {
                hand.palm = (tip + wrist) * 0.5f;
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

    bool bone(const char* name, RE::NiPoint3& out) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!g_settings.useBones || !frik || !frik->getBoneWorldTransform) {
            return false;
        }
        RE::NiTransform t;
        if (!frik->getBoneWorldTransform(name, &t)) {
            return false;
        }
        out = t.translate;
        return true;
    }

    // ------------------------------------------------------------ talking to the script

    enum Command : std::int32_t
    {
        kNone = 0,
        kSpawn = 1,         // kind = static index, token
        kDelete = 2,        // ref
        kSetPos = 3,        // ref, pose
        kMoveToPlayer = 4,  // ref
        kTakeItem = 5,      // form: one less in the inventory
        kGiveItem = 6,      // form: one more in the inventory
        kStoreRef = 7,      // ref: picked up into the inventory
        kDropToHand = 8,    // form, pose, token: out of the inventory into the hand
        kConsume = 9,       // ref: used (a chem is injected, food eaten)
        kEquip = 10,        // form
        kUnequip = 11,      // form
        kSound = 12,        // kind: 0 snap, 1 pick up, 2 focus, 3 put in
        kSave = 13,
        kPack = 14,         // ref: the mannequin is folded back into the kit
    };

    struct PendingCommand
    {
        std::int32_t type{ kNone };
        std::int32_t kind{ 0 };
        std::uint32_t ref{ 0 };
        std::uint32_t form{ 0 };
        std::int32_t token{ 0 };
        std::array<float, 6> pose{};
    };

    struct NewRef
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
    };

    struct Shared
    {
        std::mutex lock;
        std::deque<PendingCommand> commands;
        PendingCommand current;
        std::vector<NewRef> newRefs;
        std::vector<std::int32_t> incoming;   // the saved rig, pushed by the script after a load
        bool stateArrived{ false };
        std::uint32_t mannequinArrived{ 0 };
        bool mannequinChanged{ false };
        std::array<std::uint32_t, 1 + kPouchTypes> forms{};   // 0 kit, 1..6 pouch items
        std::string hudText;   // a message from a script thread, shown on the next frame
    } g_shared;

    void queueHud(const char* text) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.hudText = text ? text : "";
    }

    void push(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
    }

    void pushSimple(std::int32_t type, std::uint32_t ref = 0, std::uint32_t form = 0, std::int32_t kind = 0) noexcept
    {
        PendingCommand c;
        c.type = type;
        c.ref = ref;
        c.form = form;
        c.kind = kind;
        push(c);
    }

    void sound(int index) noexcept { pushSimple(kSound, 0, 0, index); }

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

    std::int32_t CommandKind(std::monostate) { std::scoped_lock guard(g_shared.lock); return g_shared.current.kind; }
    std::int32_t CommandRefID(std::monostate) { std::scoped_lock guard(g_shared.lock); return static_cast<std::int32_t>(g_shared.current.ref); }
    std::int32_t CommandFormID(std::monostate) { std::scoped_lock guard(g_shared.lock); return static_cast<std::int32_t>(g_shared.current.form); }
    std::int32_t CommandToken(std::monostate) { std::scoped_lock guard(g_shared.lock); return g_shared.current.token; }

    std::vector<float> CommandPose(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.pose.begin(), g_shared.current.pose.end() };
    }

    void AddRef(std::monostate, RE::TESObjectREFR* ref, std::int32_t token)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.newRefs.push_back(NewRef{ ref ? ref->GetFormID() : 0u, token });
    }

    void SetForm(std::monostate, std::int32_t index, std::int32_t formID)
    {
        if (index >= 0 && index < static_cast<std::int32_t>(g_shared.forms.size())) {
            std::scoped_lock guard(g_shared.lock);
            g_shared.forms[index] = static_cast<std::uint32_t>(formID);
        }
    }

    void SetMannequin(std::monostate, RE::TESObjectREFR* ref)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.mannequinArrived = ref ? ref->GetFormID() : 0u;
        g_shared.mannequinChanged = true;
    }

    void ClearState(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.incoming.clear();
        g_shared.stateArrived = false;
    }

    void PushState(std::monostate, std::int32_t value)
    {
        std::scoped_lock guard(g_shared.lock);
        if (g_shared.incoming.size() < 512) {
            g_shared.incoming.push_back(value);
        }
    }

    void StateDone(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.stateArrived = true;
    }

    std::vector<std::int32_t> GetState(std::monostate);

    void Notify(std::monostate, std::int32_t message)
    {
        note("Script: %d", message);
        switch (message) {
        case 1: logWarning("The script could not make a drawn object"); break;
        case 2: queueHud("Не получилось достать предмет"); break;
        case 3: queueHud("Не получилось поставить манекен"); break;
        case 10: queueHud("Подсумки не бросают: повесь его на манекен (полка справа от манекена)"); break;
        case 20: queueHud("Разгрузка: в инвентаре (Разное) манекен и подсумки. Выбрось манекен из Пип-Боя, чтобы поставить его"); break;
        default: break;
        }
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "RigVR";
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandKind", CommandKind, true);
        vm->BindNativeMethod(kScript, "CommandRefID", CommandRefID, true);
        vm->BindNativeMethod(kScript, "CommandFormID", CommandFormID, true);
        vm->BindNativeMethod(kScript, "CommandToken", CommandToken, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "AddRef", AddRef, true);
        vm->BindNativeMethod(kScript, "SetForm", SetForm, true);
        vm->BindNativeMethod(kScript, "SetMannequin", SetMannequin, true);
        vm->BindNativeMethod(kScript, "ClearState", ClearState, true);
        vm->BindNativeMethod(kScript, "PushState", PushState, true);
        vm->BindNativeMethod(kScript, "StateDone", StateDone, true);
        vm->BindNativeMethod(kScript, "GetState", GetState, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        logNote("Papyrus functions of script RigVR registered");
        return true;
    }

    // ------------------------------------------------------------ the rig (kept in the save)

    struct Stack
    {
        std::uint32_t form{ 0 };
        std::int32_t  count{ 0 };
    };

    struct Pouch
    {
        int type{ kEmpty };
        std::array<Stack, kItemsPerPouch> items{};

        int total() const noexcept
        {
            int n = 0;
            for (const auto& s : items) n += s.form ? s.count : 0;
            return n;
        }
    };

    struct Rig
    {
        bool worn{ false };
        std::uint32_t mannequin{ 0 };
        std::array<Pouch, kSlots> slots{};
    };

    std::mutex g_rigLock;   // GetState runs on a script thread
    Rig g_rig;
    bool g_rigLoaded{ false };
    bool g_dirty{ false };
    Clock::time_point g_lastSave{};

    constexpr std::int32_t kStateVersion = 1;

    std::vector<std::int32_t> GetState(std::monostate)
    {
        std::scoped_lock guard(g_rigLock);
        std::vector<std::int32_t> out;
        out.push_back(kStateVersion);
        out.push_back(g_rig.worn ? 1 : 0);
        out.push_back(static_cast<std::int32_t>(g_rig.mannequin));
        out.push_back(kSlots);
        for (const auto& pouch : g_rig.slots) {
            out.push_back(pouch.type);
            for (const auto& s : pouch.items) {
                out.push_back(static_cast<std::int32_t>(s.form));
                out.push_back(s.count);
            }
        }
        return out;
    }

    void loadState(const std::vector<std::int32_t>& in) noexcept
    {
        std::scoped_lock guard(g_rigLock);
        g_rig = Rig{};
        if (in.size() >= 4 && in[0] == kStateVersion) {
            g_rig.worn = in[1] != 0;
            g_rig.mannequin = static_cast<std::uint32_t>(in[2]);
            const int slots = std::min<int>(in[3], kSlots);
            std::size_t at = 4;
            for (int i = 0; i < slots && at + 1 + 2 * kItemsPerPouch <= in.size(); ++i) {
                auto& pouch = g_rig.slots[i];
                pouch.type = std::clamp(in[at++], 0, kPouchTypes);
                for (auto& s : pouch.items) {
                    s.form = static_cast<std::uint32_t>(in[at++]);
                    s.count = std::max(0, in[at++]);
                    if (s.count == 0) s.form = 0;
                }
            }
        }
        g_rigLoaded = true;
        int pouches = 0;
        for (const auto& p : g_rig.slots) pouches += p.type != kEmpty ? 1 : 0;
        note("Rig loaded: %s, mannequin %08X, %d pouches", g_rig.worn ? "worn" : "on the mannequin", g_rig.mannequin, pouches);
    }

    void markDirty() noexcept { g_dirty = true; }

    bool addToPouch(Pouch& pouch, std::uint32_t form) noexcept
    {
        if (pouch.total() >= capacityOf(pouch.type)) {
            return false;
        }
        for (auto& s : pouch.items) {
            if (s.form == form) {
                ++s.count;
                return true;
            }
        }
        for (auto& s : pouch.items) {
            if (!s.form) {
                s.form = form;
                s.count = 1;
                return true;
            }
        }
        return false;
    }

    // The last kind put in that is still in the inventory; its count goes down by one.
    std::uint32_t takeFromPouch(Pouch& pouch, RE::PlayerCharacter* player) noexcept
    {
        for (int i = kItemsPerPouch - 1; i >= 0; --i) {
            auto& s = pouch.items[i];
            if (!s.form || s.count <= 0) {
                continue;
            }
            const auto have = static_cast<std::int32_t>(inventoryCount(player, s.form));
            if (have <= 0) {
                // used up some other way (Pip-Boy, a reload): forget it
                s = Stack{};
                continue;
            }
            s.count = std::min(s.count, have) - 1;
            const auto form = s.form;
            if (s.count <= 0) {
                s = Stack{};
            }
            return form;
        }
        return 0;
    }

    // ------------------------------------------------------------ what is drawn

    struct Visual
    {
        std::uint32_t ref{ 0 };
        int  kind{ -1 };          // static index the ref was made of
        int  wanted{ -1 };        // static index wanted now (-1: none)
        bool show{ false };
        RE::NiTransform pose{};
        bool requested{ false };
        Clock::time_point requestedAt{};
        Clock::time_point lastMove{};
        Clock::time_point missingSince{};
    };

    // 0..10 the pouches, 11 the rig, 12 the cursor, 13..18 the shelf, 19..29 free places on the mannequin, 30 pouch in a hand
    constexpr int kVisPouch = 0;
    constexpr int kVisRig = kSlots;
    constexpr int kVisCursor = kVisRig + 1;
    constexpr int kVisTray = kVisCursor + 1;
    constexpr int kVisGhost = kVisTray + kPouchTypes;
    constexpr int kVisHeld = kVisGhost + kSlots;
    constexpr int kVisuals = kVisHeld + 1;
    std::array<Visual, kVisuals> g_vis;

    void want(int index, int kind, const RE::NiTransform& pose) noexcept
    {
        auto& v = g_vis[index];
        v.wanted = kind;
        v.show = kind >= 0;
        v.pose = pose;
    }

    void hideVisual(int index) noexcept
    {
        g_vis[index].show = false;
    }

    void forgetVisuals() noexcept
    {
        for (auto& v : g_vis) {
            v = Visual{};
        }
    }

    void onDroppedToHand(std::uint32_t ref, std::int32_t token) noexcept;

    void takeNewRefs() noexcept
    {
        std::vector<NewRef> refs;
        {
            std::scoped_lock guard(g_shared.lock);
            refs.swap(g_shared.newRefs);
        }
        for (const auto& r : refs) {
            if (r.token >= 0 && r.token < kVisuals) {
                auto& v = g_vis[r.token];
                if (v.ref && v.ref != r.ref) {
                    pushSimple(kDelete, v.ref);
                }
                v.ref = r.ref;
                v.requested = false;
                v.missingSince = {};
                detail("Visual %d is ref %08X (kind %d)", r.token, r.ref, v.kind);
            } else if (r.token >= 1000) {
                onDroppedToHand(r.ref, r.token);
            }
        }
    }

    void updateVisuals(RE::PlayerCharacter* player, Clock::time_point now) noexcept
    {
        for (int i = 0; i < kVisuals; ++i) {
            auto& v = g_vis[i];
            if (v.wanted >= 0 && v.wanted != v.kind) {
                if (v.ref) {
                    pushSimple(kDelete, v.ref);
                    v.ref = 0;
                }
                v.kind = v.wanted;
                v.requested = false;
            }
            if (v.kind < 0) {
                continue;
            }
            if (!v.ref) {
                if (v.show && (!v.requested || now - v.requestedAt > std::chrono::seconds(4))) {
                    PendingCommand c;
                    c.type = kSpawn;
                    c.kind = v.kind;
                    c.token = i;
                    push(c);
                    v.requested = true;
                    v.requestedAt = now;
                }
                continue;
            }
            auto* ref = lookupRef(v.ref);
            if (!ref || ref->IsDeleted()) {
                v.ref = 0;
                continue;
            }
            auto* root = ref->Get3D();
            if (!looksLikeObject(root)) {
                // left behind in another cell, or not loaded yet
                if (v.show) {
                    if (v.missingSince == Clock::time_point{}) {
                        v.missingSince = now;
                    } else if (now - v.missingSince > std::chrono::seconds(2) && now - v.lastMove > std::chrono::seconds(3)) {
                        pushSimple(kMoveToPlayer, v.ref);
                        v.lastMove = now;
                    }
                }
                continue;
            }
            v.missingSince = {};
            if (!v.show) {
                setCulled(root, true);
                continue;
            }
            setCulled(root, false);
            place3D(root, v.pose);
            // the reference itself must stay near what is drawn, or the game unloads / culls it
            if (distance(ref->GetPosition(), v.pose.translate) > 300.0f && now - v.lastMove > std::chrono::seconds(2)) {
                PendingCommand c;
                c.type = kSetPos;
                c.ref = v.ref;
                c.pose = { v.pose.translate.x, v.pose.translate.y, v.pose.translate.z, 0.0f, 0.0f, 0.0f };
                push(c);
                v.lastMove = now;
            }
        }
        (void)player;
    }

    // ------------------------------------------------------------ frames: the mannequin and the body

    struct Frames
    {
        Frame belt;
        Frame chest;
        float scale{ 1.0f };
    };

    Frames mannequinFrames(RE::TESObjectREFR* mannequin) noexcept
    {
        Frames f;
        const auto pos = mannequin->GetPosition();
        const float yaw = mannequin->data.angle.z;
        f.belt.origin = { pos.x, pos.y, pos.z + g_settings.beltHeight };
        f.belt.yaw = yaw;
        f.chest.origin = { pos.x, pos.y, pos.z + g_settings.chestHeight };
        f.chest.yaw = yaw;
        f.scale = 1.0f;
        return f;
    }

    bool g_bonesLogged{ false };

    Frames bodyFrames(RE::PlayerCharacter* player) noexcept
    {
        Frames f;
        const auto feet = player->GetPosition();
        const float playerYaw = player->data.angle.z;
        f.scale = g_settings.bodyScale;
        RE::NiPoint3 pelvis, chest, lThigh, rThigh, lArm, rArm;
        const bool hasPelvis = bone("Pelvis", pelvis);
        const bool hasChest = bone("Chest", chest) || bone("SPINE2", chest);
        const bool hasThighs = bone("LLeg_Thigh", lThigh) && bone("RLeg_Thigh", rThigh);
        const bool hasArms = bone("LArm_UpperArm", lArm) && bone("RArm_UpperArm", rArm);
        const float beltYaw = hasThighs ? yawOfRight(rThigh - lThigh, playerYaw) : playerYaw;
        const float chestYaw = hasArms ? yawOfRight(rArm - lArm, beltYaw) : beltYaw;
        f.belt.yaw = beltYaw;
        f.chest.yaw = chestYaw;
        f.belt.origin = hasPelvis ? pelvis : RE::NiPoint3{ feet.x, feet.y, feet.z + g_settings.beltHeight };
        f.chest.origin = hasChest ? chest : RE::NiPoint3{ f.belt.origin.x, f.belt.origin.y, f.belt.origin.z + (g_settings.chestHeight - g_settings.beltHeight) };
        f.belt.origin.z += g_settings.beltUp;
        f.chest.origin.z += g_settings.chestUp;
        f.belt.origin += f.belt.forward() * g_settings.bodyForward;
        f.chest.origin += f.chest.forward() * g_settings.bodyForward;
        if (!g_bonesLogged) {
            g_bonesLogged = true;
            note("Body: pelvis %s (%.0f above feet), chest %s (%.0f), thighs %s, arms %s; yaw belt %.0f chest %.0f player %.0f",
                hasPelvis ? "bone" : "fallback", f.belt.origin.z - feet.z, hasChest ? "bone" : "fallback", f.chest.origin.z - feet.z,
                hasThighs ? "yes" : "no", hasArms ? "yes" : "no", beltYaw / kDegrees, chestYaw / kDegrees, playerYaw / kDegrees);
        }
        return f;
    }

    RE::NiTransform slotPose(const Frames& f, int slot, float extraScale = 1.0f) noexcept
    {
        const auto& p = g_places[slot];
        const auto& frame = p.group == 0 ? f.belt : f.chest;
        const float s = f.scale * extraScale;
        return uprightTransform(frame.at(p.x * s, p.y * s, p.z), frame.yaw + p.yaw * kDegrees);
    }

    RE::NiTransform trayPose(const Frames& f, int index) noexcept
    {
        const float z = g_settings.trayZ - g_settings.beltHeight - index * g_settings.trayStep;
        return uprightTransform(f.belt.at(g_settings.trayX, g_settings.trayY, z), f.belt.yaw - 90.0f * kDegrees);
    }

    // ------------------------------------------------------------ state of the frame

    struct State
    {
        int heldType{ kEmpty };      // a pouch in a hand (taken from the shelf or the mannequin)
        int heldHand{ -1 };
        std::array<Clock::time_point, 2> gripAt{};
        Clock::time_point packStart{};
        int packHand{ -1 };
        Clock::time_point holsterAt{};
        Clock::time_point lastFrame{};
        std::array<int, 2> hover{ -1, -1 };   // pouch slot each hand is at (body)
        bool wearArmed{ true };
    } g_state;

    // Items dropped from a pouch into a hand: a chem / food among them is used at the mouth.
    struct InHand
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
        Clock::time_point at{};
    };
    std::vector<InHand> g_inHand;
    std::int32_t g_nextToken{ 1000 };

    void onDroppedToHand(std::uint32_t ref, std::int32_t token) noexcept
    {
        if (!ref) {
            return;
        }
        for (auto& h : g_inHand) {
            if (h.token == token) {
                h.ref = ref;
                return;
            }
        }
    }

    int nearestSlot(const Frames& f, const RE::NiPoint3& palm, bool wantFilled, float radius) noexcept
    {
        int best = -1;
        float bestDistance = radius;
        for (int i = 0; i < kSlots; ++i) {
            const bool filled = g_rig.slots[i].type != kEmpty;
            if (filled != wantFilled) {
                continue;
            }
            const float d = distance(slotPose(f, i).translate, palm);
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    // A loose item near the palm (the one the hand just let go of).
    RE::TESObjectREFR* looseItemNear(RE::PlayerCharacter* player, const RE::NiPoint3& palm, int pouchType) noexcept
    {
        auto* cell = player->GetParentCell();
        if (!cell || !readable(cell, sizeof(RE::TESObjectCELL))) {
            return nullptr;
        }
        RE::TESObjectREFR* best = nullptr;
        float bestDistance = g_settings.storeRadius;
        const auto count = cell->references.size();
        if (count > 100000) {
            return nullptr;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ref = cell->references[i].get();
            if (!ref || ref == player || !readable(ref, sizeof(RE::TESObjectREFR)) || ref->IsDisabled() || ref->IsDeleted()) {
                continue;
            }
            auto* base = ref->GetObjectReference();
            if (!base || !readable(base, sizeof(RE::TESForm)) || !accepts(pouchType, base)) {
                continue;
            }
            bool ours = false;
            for (const auto& v : g_vis) {
                ours = ours || v.ref == ref->GetFormID();
            }
            for (std::size_t k = 1; k < g_shared.forms.size(); ++k) {
                ours = ours || g_shared.forms[k] == base->GetFormID();
            }
            if (ours || !ref->Get3D()) {
                continue;
            }
            const float d = distance(ref->GetPosition(), palm);
            if (d < bestDistance) {
                bestDistance = d;
                best = ref;
            }
        }
        return best;
    }

    std::array<float, 6> poseAt(const RE::NiPoint3& p) noexcept
    {
        return { p.x, p.y, p.z, 0.0f, 0.0f, 0.0f };
    }

    // ------------------------------------------------------------ at the mannequin

    void atMannequin(RE::PlayerCharacter* player, RE::TESObjectREFR* mannequin, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        const auto f = mannequinFrames(mannequin);
        const bool rigHere = !g_rig.worn;

        // the shelf: one of each pouch you carry and have not hung up
        for (int t = 1; t <= kPouchTypes; ++t) {
            const auto have = inventoryCount(player, g_shared.forms[t]);
            const bool inHand = st.heldType == t;
            if (have > (inHand ? 1u : 0u)) {
                want(kVisTray + t - 1, kStatPouch0 + t, trayPose(f, t - 1));
            } else {
                hideVisual(kVisTray + t - 1);
            }
        }
        if (rigHere) {
            want(kVisRig, kStatRig, uprightTransform(mannequin->GetPosition(), f.belt.yaw));
        }

        RE::NiTransform cursorPose{};
        bool cursor = false;

        for (int h = 0; h < 2; ++h) {
            const auto& hand = g_hands[h];
            if (!hand.valid) {
                continue;
            }
            // a pouch in this hand
            if (st.heldType != kEmpty && st.heldHand == h) {
                const int slot = rigHere ? nearestSlot(f, hand.palm, false, g_settings.slotRadius * 1.5f) : -1;
                if (slot >= 0) {
                    cursor = true;
                    cursorPose = slotPose(f, slot);
                }
                if (hand.released()) {
                    if (slot >= 0) {
                        g_rig.slots[slot] = Pouch{};
                        g_rig.slots[slot].type = st.heldType;
                        pushSimple(kTakeItem, 0, g_shared.forms[st.heldType]);
                        sound(0);
                        buzz(h, 2500);
                        note("Pouch type %d hung on place %d", st.heldType, slot);
                        markDirty();
                    } else {
                        sound(1);
                    }
                    st.heldType = kEmpty;
                    st.heldHand = -1;
                }
                continue;
            }
            if (!hand.pressed()) {
                continue;
            }
            // a pouch off the mannequin
            if (rigHere) {
                const int slot = nearestSlot(f, hand.palm, true, g_settings.slotRadius);
                if (slot >= 0 && st.heldType == kEmpty) {
                    st.heldType = g_rig.slots[slot].type;
                    st.heldHand = h;
                    g_rig.slots[slot] = Pouch{};
                    pushSimple(kGiveItem, 0, g_shared.forms[st.heldType]);
                    sound(1);
                    buzz(h, 1500);
                    markDirty();
                    continue;
                }
            }
            // a pouch from the shelf
            bool took = false;
            for (int t = 1; t <= kPouchTypes && !took; ++t) {
                if (st.heldType != kEmpty || !g_vis[kVisTray + t - 1].show) {
                    continue;
                }
                if (distance(trayPose(f, t - 1).translate, hand.palm) < g_settings.slotRadius) {
                    st.heldType = t;
                    st.heldHand = h;
                    sound(1);
                    buzz(h, 1500);
                    hud(pouchName(t));
                    took = true;
                }
            }
            if (took) {
                continue;
            }
            // fold the mannequin up: hold the grip on its head
            const auto head = f.chest.at(0.0f, 0.0f, 116.0f - g_settings.chestHeight);
            if (distance(head, hand.palm) < 10.0f) {
                st.packStart = now;
                st.packHand = h;
                hud("Держи грип, чтобы сложить манекен");
                continue;
            }
            st.gripAt[h] = now;
        }

        // folding
        if (st.packHand >= 0) {
            const auto& hand = g_hands[st.packHand];
            if (!hand.grip) {
                st.packHand = -1;
            } else if (std::chrono::duration<float>(now - st.packStart).count() > g_settings.packSeconds) {
                st.packHand = -1;
                note("Mannequin %08X folded", g_rig.mannequin);
                pushSimple(kPack, g_rig.mannequin);
                {
                    std::scoped_lock guard(g_rigLock);
                    g_rig.mannequin = 0;
                }
                sound(1);
                hud("Манекен сложен в инвентарь");
                markDirty();
                return;
            }
        }

        // put the rig on: both grips at the mannequin's chest at once
        const auto chestPoint = f.chest.at(0.0f, 6.0f, -4.0f);
        const bool both = g_hands[0].grip && g_hands[1].grip;
        if (!both) {
            st.wearArmed = true;
        }
        if (rigHere && both && st.wearArmed && st.heldType == kEmpty &&
            distance(g_hands[0].palm, chestPoint) < g_settings.wearRadius && distance(g_hands[1].palm, chestPoint) < g_settings.wearRadius &&
            std::chrono::duration<float>(st.gripAt[0] > st.gripAt[1] ? st.gripAt[0] - st.gripAt[1] : st.gripAt[1] - st.gripAt[0]).count() < g_settings.wearWindow) {
            st.wearArmed = false;
            g_rig.worn = true;
            sound(0);
            buzz(0, 3000);
            buzz(1, 3000);
            hud("Разгрузка надета");
            note("Rig put on");
            markDirty();
        }

        // free places
        for (int i = 0; i < kSlots; ++i) {
            if (rigHere && g_rig.slots[i].type == kEmpty && st.heldType != kEmpty) {
                want(kVisGhost + i, kStatGhost, slotPose(f, i));
            } else {
                hideVisual(kVisGhost + i);
            }
        }
        if (cursor) {
            want(kVisCursor, kStatCursor, cursorPose);
        } else {
            hideVisual(kVisCursor);
        }
        if (rigHere) {
            for (int i = 0; i < kSlots; ++i) {
                const int type = g_rig.slots[i].type;
                if (type != kEmpty) {
                    want(kVisPouch + i, kStatPouch0 + type, slotPose(f, i));
                } else {
                    hideVisual(kVisPouch + i);
                }
            }
        }
    }

    // ------------------------------------------------------------ on the body

    void onBody(RE::PlayerCharacter* player, RE::TESObjectREFR* mannequin, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        const auto f = bodyFrames(player);

        for (int i = 0; i < kSlots; ++i) {
            const int type = g_rig.slots[i].type;
            if (type != kEmpty && g_settings.showPouchesOnBody) {
                want(kVisPouch + i, kStatPouch0 + type, slotPose(f, i));
            } else {
                hideVisual(kVisPouch + i);
            }
        }
        if (g_settings.showRigOnBody) {
            const auto feetish = RE::NiPoint3{ f.belt.origin.x, f.belt.origin.y, f.belt.origin.z - g_settings.beltHeight };
            want(kVisRig, kStatRig, uprightTransform(feetish, f.belt.yaw));
        } else {
            hideVisual(kVisRig);
        }

        auto* gun = equippedGun(player);
        const bool cooling = std::chrono::duration<float>(now - st.holsterAt).count() < g_settings.holsterCooldown;
        RE::NiTransform cursorPose{};
        bool cursor = false;

        for (int h = 0; h < 2; ++h) {
            const auto& hand = g_hands[h];
            st.hover[h] = -1;
            if (!hand.valid) {
                continue;
            }
            const int slot = nearestSlot(f, hand.palm, true, g_settings.slotRadius);
            st.hover[h] = slot;
            if (slot < 0) {
                continue;
            }
            auto& pouch = g_rig.slots[slot];
            const bool gunHand = h == g_settings.gunHand;

            if (pouch.type == kHolster && gunHand) {
                // holster: let go of (or press) the grip with a gun at an empty holster to put it away
                if (gun && pouch.total() == 0 && (hand.released() || hand.pressed()) && !cooling) {
                    pouch.items[0] = Stack{ gun->GetFormID(), 1 };
                    pushSimple(kUnequip, 0, gun->GetFormID());
                    st.holsterAt = now;
                    sound(0);
                    buzz(h, 2500);
                    markDirty();
                    continue;
                }
                // ..and grip at a full holster with an empty hand to draw it
                if (!gun && pouch.total() > 0 && hand.pressed() && !cooling) {
                    const auto form = pouch.items[0].form;
                    pouch = Pouch{ kHolster };
                    if (inventoryCount(player, form) > 0) {
                        pushSimple(kEquip, 0, form);
                        sound(1);
                        buzz(h, 2000);
                    } else {
                        hud("Этого оружия у тебя больше нет");
                    }
                    st.holsterAt = now;
                    markDirty();
                    continue;
                }
            }

            cursor = cursor || hand.grip;
            if (cursor && hand.grip) {
                cursorPose = slotPose(f, slot);
            }

            // let go of an item over a pouch: it goes in
            if (hand.released() && pouch.type != kHolster) {
                if (auto* ref = looseItemNear(player, hand.palm, pouch.type)) {
                    auto* base = ref->GetObjectReference();
                    if (addToPouch(pouch, base->GetFormID())) {
                        pushSimple(kStoreRef, ref->GetFormID());
                        sound(3);
                        buzz(h, 1500);
                        markDirty();
                        detail("Item %08X (%08X) into pouch %d", ref->GetFormID(), base->GetFormID(), slot);
                    } else {
                        hud("Подсумок полон");
                    }
                }
                continue;
            }

            if (!hand.pressed()) {
                continue;
            }
            if (pouch.type == kHolster) {
                // a loose gun let go of over the holster is taken above; with the gun hand busy nothing else to do
                continue;
            }
            if (pouch.type == kMag) {
                // ManualReloadF hands out magazines at the belt by itself; this pouch only keeps the ones you put in
                if (pouch.total() == 0) {
                    buzz(h, 500);
                    continue;
                }
            }
            const auto form = takeFromPouch(pouch, player);
            if (!form) {
                buzz(h, 400);
                hud("Пусто");
                markDirty();
                continue;
            }
            if (pouch.type == kGrenade) {
                pushSimple(kEquip, 0, form);
                hud("Граната выбрана");
                sound(1);
                buzz(h, 1500);
                // the grenade stays in the pouch as a reservation: put it back
                addToPouch(pouch, form);
                continue;
            }
            PendingCommand c;
            c.type = kDropToHand;
            c.form = form;
            c.pose = poseAt(hand.palm);
            c.token = g_nextToken++;
            push(c);
            g_inHand.push_back(InHand{ 0, c.token, now });
            sound(1);
            buzz(h, 1500);
            markDirty();
        }
        if (cursor) {
            want(kVisCursor, kStatCursor, cursorPose);
        } else {
            hideVisual(kVisCursor);
        }

        // take it off: both grips at your own chest while the mannequin is near
        if (mannequin) {
            const auto chestPoint = f.chest.at(0.0f, 6.0f, -4.0f);
            const bool both = g_hands[0].grip && g_hands[1].grip;
            if (!both) {
                st.wearArmed = true;
            }
            for (int h = 0; h < 2; ++h) {
                if (g_hands[h].pressed()) {
                    st.gripAt[h] = now;
                }
            }
            if (both && st.wearArmed && !gun && distance(g_hands[0].palm, chestPoint) < g_settings.wearRadius &&
                distance(g_hands[1].palm, chestPoint) < g_settings.wearRadius &&
                std::chrono::duration<float>(st.gripAt[0] > st.gripAt[1] ? st.gripAt[0] - st.gripAt[1] : st.gripAt[1] - st.gripAt[0]).count() < g_settings.wearWindow) {
                st.wearArmed = false;
                g_rig.worn = false;
                sound(0);
                buzz(0, 3000);
                buzz(1, 3000);
                hud("Разгрузка снята и висит на манекене");
                note("Rig taken off");
                markDirty();
            }
        }
    }

    // A chem or food from a pouch brought to the mouth is used.
    void checkMouth(RE::PlayerCharacter* player, Clock::time_point now) noexcept
    {
        if (g_inHand.empty()) {
            return;
        }
        RE::NiPoint3 head;
        if (!bone("Head", head)) {
            const auto feet = player->GetPosition();
            head = { feet.x, feet.y, feet.z + 115.0f };
        }
        const float yaw = player->data.angle.z;
        const RE::NiPoint3 mouth = head + RE::NiPoint3{ std::sin(yaw), std::cos(yaw), 0.0f } * 4.0f - RE::NiPoint3{ 0.0f, 0.0f, 5.0f };
        for (auto it = g_inHand.begin(); it != g_inHand.end();) {
            if (now - it->at > std::chrono::seconds(60)) {
                it = g_inHand.erase(it);
                continue;
            }
            auto* ref = it->ref ? lookupRef(it->ref) : nullptr;
            if (it->ref && (!ref || ref->IsDeleted() || ref->IsDisabled())) {
                it = g_inHand.erase(it);
                continue;
            }
            if (ref) {
                auto* base = ref->GetObjectReference();
                if (base && base->GetFormType() == RE::ENUM_FORM_ID::kALCH && distance(ref->GetPosition(), mouth) < g_settings.mouthRadius) {
                    pushSimple(kConsume, it->ref);
                    buzz(0, 1000);
                    buzz(1, 1000);
                    it = g_inHand.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // ------------------------------------------------------------ the frame

    void update() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D()) {
            return;
        }
        const auto now = Clock::now();
        {
            std::vector<std::int32_t> incoming;
            bool arrived = false;
            std::uint32_t mannequin = 0;
            bool mannequinChanged = false;
            std::string hudText;
            {
                std::scoped_lock guard(g_shared.lock);
                if (g_shared.stateArrived) {
                    incoming.swap(g_shared.incoming);
                    g_shared.stateArrived = false;
                    arrived = true;
                }
                if (!g_shared.hudText.empty()) {
                    hudText.swap(g_shared.hudText);
                }
                if (g_shared.mannequinChanged) {
                    mannequin = g_shared.mannequinArrived;
                    g_shared.mannequinChanged = false;
                    mannequinChanged = true;
                }
            }
            if (!hudText.empty()) {
                hud(hudText.c_str());
            }
            if (arrived) {
                loadState(incoming);
                forgetVisuals();
                g_state = State{};
                g_inHand.clear();
            }
            if (mannequinChanged) {
                {
                    std::scoped_lock guard(g_rigLock);
                    if (g_rig.mannequin && g_rig.mannequin != mannequin) {
                        pushSimple(kPack, g_rig.mannequin);   // only one mannequin: the old one is folded
                    }
                    g_rig.mannequin = mannequin;
                }
                note("Mannequin set up: %08X", mannequin);
                hud("Манекен поставлен. Бери подсумки с полки справа от него");
                markDirty();
            }
        }
        if (!g_rigLoaded) {
            return;
        }
        takeNewRefs();
        readHand(0);
        readHand(1);

        if (menuOpen()) {
            for (int i = 0; i < kVisuals; ++i) {
                if (i >= kVisTray) hideVisual(i);
            }
            updateVisuals(player, now);
            return;
        }

        auto* mannequin = lookupRef(g_rig.mannequin);
        if (mannequin && (mannequin->IsDeleted() || mannequin->IsDisabled())) {
            mannequin = nullptr;
        }
        const bool mannequinNear = mannequin && mannequin->Get3D() && distance(mannequin->GetPosition(), player->GetPosition()) < g_settings.mannequinReach;

        for (int i = 0; i < kVisuals; ++i) {
            g_vis[i].show = false;
        }
        if (mannequinNear) {
            atMannequin(player, mannequin, now);
        } else if (g_state.heldType != kEmpty) {
            g_state.heldType = kEmpty;   // walked away with it: it is back in the inventory anyway
            g_state.heldHand = -1;
        }
        if (g_rig.worn) {
            onBody(player, mannequinNear ? mannequin : nullptr, now);
        }
        if (g_state.heldType != kEmpty && g_state.heldHand >= 0 && g_hands[g_state.heldHand].valid) {
            const auto& hand = g_hands[g_state.heldHand];
            const float yaw = player->data.angle.z;
            want(kVisHeld, kStatPouch0 + g_state.heldType, uprightTransform(hand.palm - RE::NiPoint3{ 0.0f, 0.0f, 2.0f }, yaw));
        }
        checkMouth(player, now);
        updateVisuals(player, now);

        if (g_dirty && now - g_lastSave > std::chrono::milliseconds(500)) {
            g_dirty = false;
            g_lastSave = now;
            pushSimple(kSave);
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
                logWarning("A fault inside the frame update was caught; RigVR keeps running");
            }
        }
    }

    constexpr const char* kFrikTag = "RigVR";

    void gameReady() noexcept
    {
        static bool done = false;
        if (!done) {
            done = true;
            const int error = FRIKApiV2::initialize(3);
            if (error != 0) {
                note("FRIK API v2.3+ unavailable (error %d); RigVR cannot run", error);
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
        // a loaded save: the script deletes the drawn objects of the last session and sends the rig again
        forgetVisuals();
        g_state = State{};
        g_inHand.clear();
        g_bonesLogged = false;
        logNote("RigVR ready (build " RIGVR_BUILD ")");
    }
}

namespace rigvr::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "RigVR",
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
            logWarning("Could not register Papyrus functions; RigVR cannot work");
        }
    }
}
