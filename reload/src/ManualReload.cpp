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
        int   dumpDepth{ 12 };
        int   gunHand{ 0 };             // 0 = right hand holds the gun, 1 = left
        int   gripButton{ 2 };          // OpenVR button id of the grip
        bool  haptics{ true };
        bool  messages{ true };
        float beltLow{ 35.0f };         // belt zone: palm height above the feet, from..to (game units)
        float beltHigh{ 95.0f };
        float beltRadius{ 40.0f };      // ..and at most this far from the body sideways
        float ghostRadius{ 14.0f };     // a magazine this close to the well shows the ghost
        float snapRadius{ 4.0f };       // ..and this close goes in by itself
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

    float headingOf(const RE::NiPoint3& direction) noexcept
    {
        return std::atan2(direction.x, direction.y) * 57.29578f;
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

    // ------------------------------------------------------------ hands

    vr::IVRSystem* g_vr{ nullptr };

    struct Hand
    {
        bool           valid{ false };
        RE::NiTransform frame{};
        RE::NiPoint3   palm{};
        bool           grip{ false };
        bool           previousGrip{ false };
    };
    std::array<Hand, 2> g_hands;   // 0 right, 1 left

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
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            if (distance(tip, wrist) < 40.0f) {
                hand.palm = (tip + wrist) * 0.5f;
            }
        }
        hand.valid = true;
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (g_vr->GetControllerState(device, &state, sizeof(state))) {
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
        std::array<float, 4> pose{};
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
    };

    struct Pending
    {
        std::uint32_t rounds{ 0 };
        int           kind{ 0 };
        bool          fromBelt{ false };
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
    };
    State g_state;

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

    void spawnMagazine(int kind, const RE::NiPoint3& at, float heading, std::uint32_t rounds, bool fromBelt) noexcept
    {
        const auto token = g_nextToken++;
        g_pending[token] = Pending{ rounds, kind, fromBelt };
        PendingCommand command;
        command.type = kSpawn;
        command.kind = kind;
        command.token = token;
        command.pose = { at.x, at.y, at.z, heading };
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

    void ejectMagazine(const Gun& gun, RE::NiAVObject* root) noexcept
    {
        g_state.magIn = false;
        const auto rounds = g_state.magRounds;
        g_state.magRounds = 0;
        setAmmo(gun, g_state.chamber ? 1u : 0u);
        if (g_state.wellKnown && looksLikeObject(root)) {
            const auto well = compose(root->world, g_state.wellLocal);
            spawnMagazine(g_state.kind, well.translate, headingOf(axisOf(root->world, 1)), rounds, false);
        }
        playSound(1);
        buzz(g_settings.gunHand, 1500);
        char message[120]{};
        std::snprintf(message, sizeof(message), "EJECT: magazine with %u rounds out, chamber %s", rounds, g_state.chamber ? "loaded" : "empty");
        logNote(message);
    }

    void insertMagazine(const Gun& gun, std::uint32_t ref, const Loose& loose) noexcept
    {
        g_state.magIn = true;
        g_state.magRounds = loose.rounds;
        setAmmo(gun, (g_state.chamber ? 1u : 0u) + g_state.magRounds);
        deleteMagazine(ref);
        playSound(0);
        buzz(0, 2500);
        buzz(1, 2500);
        char message[120]{};
        std::snprintf(message, sizeof(message), "INSERT: magazine %08X with %u rounds, ammoCount now %u", ref, g_state.magRounds, gun.data->ammoCount);
        logNote(message);
        if (!g_state.chamber) {
            hud(g_state.magRounds > 0 ? "Магазин вставлен. Патронник пуст: нажми перезарядку, чтобы передёрнуть затвор" : "Магазин пустой");
        }
    }

    void rackSlide(const Gun& gun) noexcept
    {
        if (g_state.magIn && g_state.magRounds > 0) {
            g_state.magRounds -= 1;
            g_state.chamber = true;
        }
        setAmmo(gun, (g_state.chamber ? 1u : 0u) + (g_state.magIn ? g_state.magRounds : 0u));
        playSound(2);
        buzz(1 - g_settings.gunHand, 1200);
        char message[120]{};
        std::snprintf(message, sizeof(message), "RACK: chamber %s, magazine %u", g_state.chamber ? "loaded" : "empty", g_state.magRounds);
        logNote(message);
    }

    void looseMagazines(const Gun* gun, RE::NiAVObject* root, std::uint32_t inventory) noexcept
    {
        const auto now = Clock::now();
        const bool wellOpen = gun && !g_state.magIn && g_state.wellKnown && looksLikeObject(root);
        RE::NiTransform well{};
        if (wellOpen) {
            well = compose(root->world, g_state.wellLocal);
        }
        int ghost = -1;
        std::uint32_t toInsert = 0;
        std::vector<std::uint32_t> gone;
        // oldest first beyond the limit
        std::uint32_t oldest = 0;
        auto oldestBorn = now;
        for (auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (!ref || ref->IsDeleted()) {
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
                const float d = distance(at, well.translate);
                if (d < g_settings.ghostRadius && (inHand || letGo)) {
                    ghost = loose.kind;
                }
                if ((d < g_settings.ghostRadius && letGo) || (d < g_settings.snapRadius && !inHand)) {
                    toInsert = id;
                }
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
            insertMagazine(*gun, toInsert, loose);
            ghost = -1;
        }
        showGhost(ghost, well);
        (void)inventory;
    }

    void beltGrab(const Gun& gun, RE::PlayerCharacter* player, std::uint32_t inventory) noexcept
    {
        const int index = 1 - g_settings.gunHand;
        const auto& hand = g_hands[index];
        if (!hand.valid || !hand.grip || hand.previousGrip) {
            return;
        }
        const auto feet = player->GetPosition();
        const float height = hand.palm.z - feet.z;
        const float dx = hand.palm.x - feet.x;
        const float dy = hand.palm.y - feet.y;
        const float side = std::sqrt(dx * dx + dy * dy);
        char message[160]{};
        std::snprintf(message, sizeof(message), "Grip of the free hand: %.0f above the feet, %.0f to the side", height, side);
        logInfo(message);
        if (height < g_settings.beltLow || height > g_settings.beltHigh || side > g_settings.beltRadius) {
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
        const auto used = gun.data->ammoCount + looseRounds();
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
        spawnMagazine(g_state.kind, hand.palm, headingOf(axisOf(hand.frame, 0)), rounds, true);
        buzz(index, 900);
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

        // ghosts: made once, then kept near the player (a far reference does not draw)
        {
            static Clock::time_point lastCheck{};
            const auto checkNow = Clock::now();
            bool ask = !g_state.ghostsAsked;
            if (!ask && checkNow - lastCheck > std::chrono::seconds(5)) {
                lastCheck = checkNow;
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
                lastCheck = checkNow;
                PendingCommand command;
                command.type = kGhosts;
                pushCommand(command);
            }
        }

        const auto gun = equippedGun(player);
        const auto now = Clock::now();
        if (gun.weapon != g_state.weapon) {
            g_state.weapon = gun.weapon;
            g_state.managed = gun.weapon && gun.data && gun.weapon->GetFormID() == k10mmPistol;
            g_state.dumped = false;
            g_state.equippedAt = now;
            g_state.wellKnown = false;
            g_state.lastAmmo = 0xFFFFFFFF;
            g_state.lastGunState = 0xFFFFFFFF;
            g_state.magIn = true;
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

        auto* root = g_state.managed ? weaponRoot(player) : nullptr;
        const auto inventory = (gun.data && gun.data->ammo) ? inventoryCount(player, gun.data->ammo) : 0u;
        if (!g_state.managed) {
            looseMagazines(nullptr, nullptr, 0);
            return;
        }

        if (!g_state.dumped && now - g_state.equippedAt > std::chrono::seconds(2) && root) {
            g_state.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        auto* magazine = findNamed(root, "WeaponMagazine");
        if (g_state.magIn) {
            setCulled(magazine, false);
            auto* well = findNamed(root, "P-Mag");
            if (looksLikeObject(well) && looksLikeObject(root)) {
                g_state.wellLocal = relative(root->world, well->world);
                g_state.wellKnown = true;
            }
        } else {
            setCulled(magazine, true);
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

        // the reload button
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        const bool pressed = gunState == reloading && g_state.lastGunState != reloading && g_state.lastGunState != 0xFFFFFFFF;
        g_state.lastGunState = gunState;
        if (pressed && g_settings.blockReload) {
            if (g_state.magIn) {
                if (!g_state.chamber && g_state.magRounds > 0) {
                    rackSlide(gun);
                } else {
                    ejectMagazine(gun, root);
                    setCulled(magazine, true);
                }
            } else {
                hud("Магазина нет: возьми новый с пояса (грип свободной рукой у пояса)");
            }
        }

        beltGrab(gun, player, inventory);
        looseMagazines(&gun, root, inventory);
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
