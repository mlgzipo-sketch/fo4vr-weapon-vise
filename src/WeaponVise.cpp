// WeaponVise: modify weapons by hand at a weapons workbench.
//
// The player brings a drawn weapon to a weapons workbench and holds grip: the
// weapon is clamped there (a keyframed reference). Its mods are then real
// parts: grip on a part and pull it off, bring a part to the weapon and it
// snaps on. Grip on the weapon body and hold to take the weapon back.
//
// Work is split in two:
//   - this plugin reads the hands (FRIK, OpenVR) every frame, finds the parts in the
//     weapon's 3D, carries loose parts and decides what the player did;
//   - the quest script WeaponViseMain does everything that touches the game's
//     inventory and records (drop, AttachMod, RemoveMod, spawning parts). It
//     polls our commands through the native functions of script WeaponVise.
//
// Loose parts are plain MISC references with the part's own model. Those
// models have no collision, so they never fall: this plugin keeps each part
// where the player put it by writing its 3D transform every frame.

#include <format>
#include <string>

#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using weaponvise::runtime::logInfo;
    using weaponvise::runtime::logNote;
    using weaponvise::runtime::logWarning;
    using Mod = RE::BGSMod::Attachment::Mod;

    vr::IVRSystem* g_vr{ nullptr };

    // ------------------------------------------------------------ settings

    struct Settings
    {
        int gripButton{ 2 };            // OpenVR button id: 2 = grip
        int triggerButton{ 33 };        // OpenVR button id: 33 = trigger
        int selectButton{ 33 };         // OpenVR button id (33 = trigger): held, the hand picks parts off the weapon
        bool highlight{ true };         // glow the part a hand holds
        bool haptics{ true };
        int weaponHand{ 0 };            // 0 = right, 1 = left
        int forwardAxis{ 1 };           // controller axis that points forward: 0 = X, 1 = Y, 2 = Z
        float viseRadius{ 20.0f };      // weapon hand this close to the vise may clamp
        float viseX{ 33.0f };           // the vise in the workbench model's space
        float viseY{ 26.0f };
        float viseZ{ 68.0f };
        float tableZ{ 61.0f };   // bench top, where parts without physics are laid
        float viseLift{ 9.0f };         // the clamped weapon sits this far above the vise point
        float clampHold{ 0.5f };        // seconds of grip to clamp
        float grabRadius{ 10.0f };       // hand this close to a part grabs it
        float pullDistance{ 8.0f };     // hand moved this far pulls a part off
        float attachRadius{ 9.0f };     // part this close to its place snaps on
        float attachRadiusAny{ 6.0f };  // ...or this close to the weapon when its place is unknown
        float weaponGrabRadius{ 16.0f };
        float unclampHold{ 1.5f };
        float unclampStill{ 6.0f };     // hand must stay within this while holding
        float rackX{ 48.0f };           // loose parts are laid out on the bench top (workbench model space)
        float rackY{ 48.0f };
        float rackZ{ 61.0f };
        float rackStep{ 6.0f };
        int rackPerRow{ 6 };
        float zoneLift{ 16.0f };        // the weapon hangs in a ball this high above the vise point...
        float zoneRadius{ 18.0f };      // ...of this radius
        float previewRadius{ 15.0f };   // a carried part this close to its place shows the ghost
        float snapTime{ 0.15f };        // seconds of the snap-on animation
        float ghostFade{ 0.5f };        // how see-through the attach ghost is
        float heldRadius{ 15.0f };      // a part held by ROCK this close to its place previews
        bool removeIntoHand{ false };   // false: a removed part drops as a real object for ROCK to take
        bool logGrab{ true };           // log what each grip press near the weapon found
        bool refresh3D{ false };         // reload the weapon's 3D when it does not show a change
        bool logTree{ true };
        bool messages{ true };
    } g_settings;

    void loadSettings() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/WeaponVise.ini");
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
                if (key == "iGripButton") s.gripButton = static_cast<int>(value);
                else if (key == "iWeaponHand") s.weaponHand = static_cast<int>(value);
                else if (key == "iSelectButton") s.selectButton = static_cast<int>(value);
                else if (key == "iTriggerButton") s.triggerButton = static_cast<int>(value);
                else if (key == "bHighlight") s.highlight = value != 0.0f;
                else if (key == "bHaptics") s.haptics = value != 0.0f;
                else if (key == "iForwardAxis") s.forwardAxis = std::clamp(static_cast<int>(value), 0, 2);
                else if (key == "fViseRadius") s.viseRadius = value;
                else if (key == "fViseX") s.viseX = value;
                else if (key == "fViseY") s.viseY = value;
                else if (key == "fViseZ") s.viseZ = value;
                else if (key == "fViseLift") s.viseLift = value;
                else if (key == "fClampHold") s.clampHold = value;
                else if (key == "fGrabRadius") s.grabRadius = value;
                else if (key == "fPullDistance") s.pullDistance = value;
                else if (key == "fAttachRadius") s.attachRadius = value;
                else if (key == "fAttachRadiusAny") s.attachRadiusAny = value;
                else if (key == "fWeaponGrabRadius") s.weaponGrabRadius = value;
                else if (key == "fUnclampHold") s.unclampHold = value;
                else if (key == "fUnclampStill") s.unclampStill = value;
                else if (key == "fRackX") s.rackX = value;
                else if (key == "fRackY") s.rackY = value;
                else if (key == "fRackZ") s.rackZ = value;
                else if (key == "fRackStep") s.rackStep = value;
                else if (key == "iRackPerRow") s.rackPerRow = (std::max)(1, static_cast<int>(value));
                else if (key == "fZoneLift") s.zoneLift = value;
                else if (key == "fZoneRadius") s.zoneRadius = value;
                else if (key == "fPreviewRadius") s.previewRadius = value;
                else if (key == "fTableZ") s.tableZ = value;
                else if (key == "fSnapTime") s.snapTime = value;
                else if (key == "fGhostFade") s.ghostFade = value;
                else if (key == "fHeldRadius") s.heldRadius = value;
                else if (key == "bLogGrab") s.logGrab = value != 0.0f;
                else if (key == "bRemoveIntoHand") s.removeIntoHand = value != 0.0f;
                else if (key == "bRefresh3D") s.refresh3D = value != 0.0f;
                else if (key == "bLogTree") s.logTree = value != 0.0f;
                else if (key == "bMessages") s.messages = value != 0.0f;
            }
        } catch (...) {
        }
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "Settings: grip=%d weaponHand=%d viseRadius=%.0f grab=%.0f pull=%.0f attach=%.0f/%.0f refresh3D=%d",
            g_settings.gripButton, g_settings.weaponHand, g_settings.viseRadius, g_settings.grabRadius,
            g_settings.pullDistance, g_settings.attachRadius, g_settings.attachRadiusAny, g_settings.refresh3D);
        logNote(message);
    }

    // ------------------------------------------------------------ part table

    // One row of WeaponVise_parts.csv (made from Fallout4.esm by tools/gen_parts.py).
    struct PartInfo
    {
        std::uint32_t omod{ 0 };
        std::string edid;
        std::string slot;      // attach point keyword, e.g. ap_gun_Mag
        std::string stem;      // model file name without folder and .nif, lower case
        bool part{ false };    // has a visible model of its own: can be taken off by hand
        bool required{ false };
        std::uint32_t fallback{ 0 };  // fills the slot when this part is taken off
    };
    std::unordered_map<std::uint32_t, PartInfo> g_table;

    std::string lower(std::string_view text)
    {
        std::string out(text);
        for (auto& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    void loadTable() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/WeaponVise_parts.csv");
            std::string line;
            std::getline(file, line);  // header
            while (file && std::getline(file, line)) {
                std::vector<std::string> cells;
                std::stringstream stream(line);
                std::string cell;
                while (std::getline(stream, cell, ',')) {
                    cells.push_back(cell);
                }
                if (cells.size() < 9) {
                    continue;
                }
                PartInfo info;
                info.omod = static_cast<std::uint32_t>(std::strtoul(cells[0].c_str(), nullptr, 16));
                info.edid = cells[1];
                info.slot = cells[3];
                auto model = cells[4];
                const auto slash = model.find_last_of("\\/");
                if (slash != std::string::npos) {
                    model = model.substr(slash + 1);
                }
                if (model.size() > 4) {
                    model.resize(model.size() - 4);
                }
                info.stem = lower(model);
                info.part = cells[6] == "1";
                info.fallback = static_cast<std::uint32_t>(std::strtoul(cells[7].c_str(), nullptr, 16));
                info.required = cells[8] == "1" || cells[8] == "1\r";
                g_table[info.omod] = info;
            }
        } catch (...) {
        }
        char message[96]{};
        std::snprintf(message, sizeof(message), "Part table: %zu weapon mods", g_table.size());
        logNote(message);
    }

    const PartInfo* partInfo(std::uint32_t omod) noexcept
    {
        const auto it = g_table.find(omod);
        return it == g_table.end() ? nullptr : &it->second;
    }

    // ------------------------------------------------------------ messages

    void hud(const char* text) noexcept
    {
        if (!g_settings.messages || !text) {
            return;
        }
        RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false, false);
    }

    const char* slotName(std::string_view slot) noexcept
    {
        if (slot == "ap_gun_Mag") return "магазин";
        if (slot == "ap_gun_Barrel") return "ствол";
        if (slot == "ap_gun_Grip") return "рукоять";
        if (slot == "ap_gun_Scope") return "прицел";
        if (slot == "ap_gun_Muzzle") return "дульное устройство";
        if (slot == "ap_gun_receiver") return "ствольную коробку";
        return "деталь";
    }

    // ------------------------------------------------------------ shared state
    // Written by the Papyrus natives (script threads), read in the frame callback.

    enum Command : int
    {
        kNone = 0,
        kClamp = 1,
        kUnclamp = 2,
        kRemove = 3,
        kAttach = 4,
        kDrop = 5,
        kMove = 6,
        kGhost = 7,
        kSound = 8,
        kFramePose = 9,
    };

    struct PendingCommand
    {
        int type{ kNone };
        std::uint32_t mod{ 0 };
        std::uint32_t part{ 0 };
        std::array<float, 4> pose{};  // x, y, z, heading in degrees
    };

    struct Shared
    {
        std::mutex lock;
        std::uint32_t bench{ 0 };
        std::uint32_t frame{ 0 };   // the zone frame model
        std::uint32_t frameGen{ 0 }; // bumped each time the script (re)places it
        std::uint32_t weapon{ 0 };
        std::vector<std::uint32_t> mods;
        std::uint64_t revision{ 0 };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> newParts;  // part ref, mod (to frame)
        std::vector<std::uint32_t> inHandParts;                          // spawned for a pull
        std::vector<std::uint32_t> forgottenParts;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> newGhosts;  // ghost ref, mod
        std::deque<PendingCommand> commands;
        PendingCommand current;
        std::array<float, 4> weaponPose{};  // where the weapon was clamped
        RE::NiTransform benchWorld{};       // the workbench model at clamp time
        bool benchKnown{ false };
        bool modelOutdated{ false };
    } g_shared;

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

    // Heading (degrees, the game's Z angle) of an object whose +Y should point
    // along this world direction.
    float headingOf(const RE::NiPoint3& direction) noexcept
    {
        return std::atan2(direction.x, direction.y) * 57.29578f;
    }

    // ------------------------------------------------------------ memory safety
    // A fault in the frame callback would take the game down, so scene graph
    // pointers are checked before use.

    bool readable(const void* address, std::size_t size) noexcept
    {
        if (!address) {
            return false;
        }
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != 0x1000u /* MEM_COMMIT */) {
            return false;
        }
        // PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
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
        visit(object, depth);
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

    RE::NiPoint3 centerOf(const RE::NiAVObject* object) noexcept
    {
        const auto& bound = object->worldBound;
        if (bound.fRadius > 0.0f && bound.fRadius < 500.0f) {
            return bound.center;
        }
        return object->world.translate;
    }

    // Places an object's 3D: root at `world`, children follow.
    void place3D(RE::NiAVObject* root, const RE::NiTransform& world) noexcept
    {
        if (!looksLikeObject(root)) {
            return;
        }
        // the bound moves with the object, or the renderer keeps culling it at its old place
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
                return;
            }
            auto* parent = object->parent;
            if (looksLikeObject(parent)) {
                const auto before = object->world;
                object->world = compose(parent->world, object->local);
                moveBound(object, before);
            }
        });
    }

    // ------------------------------------------------------------ frame state

    struct Hand
    {
        bool valid{ false };
        RE::NiTransform frame{};
        RE::NiPoint3 point{};  // middle of the palm: what grabs
        bool grip{ false };
        bool previousGrip{ false };
        bool trigger{ false };
        bool select{ false };      // A / X held: picking a part
        std::uint32_t hover{ 0 };  // part under the hand while picking
        bool previousTrigger{ false };
        bool armed{ false };   // a pulled part has left its place: it may snap back on
        Clock::time_point gripSince{};
        bool clampSent{ false };

        enum class Mode
        {
            Idle,
            Pull,      // gripping a part on the weapon, not yet pulled off
            Await,     // pulled off; waiting for the script to spawn the part
            Carry,     // carrying a loose part
            Unclamp,   // holding the weapon body
            HoldPart,  // gripping a part on the weapon: trigger takes it off
            HoldWeapon, // gripping the weapon body: it follows the hand
            Done,      // this press already did something; wait for release
        } mode{ Mode::Idle };
        std::uint32_t mod{ 0 };
        std::uint32_t part{ 0 };
        RE::NiPoint3 start{};
        RE::NiTransform offset{};  // part relative to the hand
        Clock::time_point since{};
        RE::NiAVObject* hidden{ nullptr };   // real part node hidden under the red ghost
        RE::NiAVObject* holdNode{ nullptr };
        RE::NiAVObject* carried3D{ nullptr };   // carried part taken out of physics
    };
    std::array<Hand, 2> g_hands_state{};
    RE::NiPoint3 g_clampPoint{};      // where the last clamp asked the weapon to go
    Clock::time_point g_clampAt{};
    bool g_missing3DLogged{ false };
    bool g_missing3DForgot{ false };
    Clock::time_point g_missing3DAt{};
    bool g_clampFeedback{ false };   // a weapon was just clamped: buzz and click once

    struct LoosePart
    {
        std::uint32_t mod{ 0 };
        bool placed{ false };       // home holds where the player left it
        RE::NiTransform home{};
        int carriedBy{ -1 };
        RE::NiAVObject* detached{ nullptr };   // 3D we took out of physics while carrying
        int collision{ -1 };        // -1 not checked yet; 0: the model has no physics (ROCK can not hold it)
        bool previewing{ false };   // ghost shown while a gripping hand holds it near its place
        Clock::time_point previewSince{};
    };
    std::unordered_map<std::uint32_t, LoosePart> g_parts;

    struct Anchor
    {
        std::uint32_t mod{ 0 };
        RE::NiAVObject* node{ nullptr };
        RE::NiPoint3 center{};
    };
    std::vector<Anchor> g_anchors;
    std::unordered_map<std::string, RE::NiPoint3> g_slotPlaces;  // last place of each slot, in weapon space
    std::unordered_set<std::string> g_requiredAtClamp;
    std::uint32_t g_weapon{ 0 };
    std::vector<std::uint32_t> g_mods;
    std::vector<std::uint32_t> g_reloadedFor;  // asked for a 3D reload once for this mod list
    std::uint64_t g_revision{ ~0ull };
    RE::NiAVObject* g_weapon3D{ nullptr };
    bool g_treeLogged{ false };
    bool g_treeLoggedLate{ false };
    bool g_stemsWork{ false };
    Clock::time_point g_revisionTime{};
    Clock::time_point g_lastHint{};
    std::uint32_t g_bench{ 0 };

    void pushCommand(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
        char message[128]{};
        std::snprintf(message, sizeof(message), "Command %d mod=%08X part=%08X at (%.0f, %.0f, %.0f) h=%.0f",
            command.type, command.mod, command.part, command.pose[0], command.pose[1], command.pose[2], command.pose[3]);
        logNote(message);
    }

    vr::TrackedDeviceIndex_t controllerOf(int index) noexcept
    {
        if (!g_vr) {
            return vr::k_unTrackedDeviceIndexInvalid;
        }
        return g_vr->GetTrackedDeviceIndexForControllerRole(index == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    }

    // Grip and trigger of one controller.
    void readButtons(int index, bool& grip, bool& trigger, bool& select) noexcept
    {
        grip = false;
        trigger = false;
        select = false;
        const auto device = controllerOf(index);
        if (device == vr::k_unTrackedDeviceIndexInvalid) {
            return;
        }
        vr::VRControllerState_t state{};
        if (!g_vr->GetControllerState(device, &state, sizeof(state))) {
            return;
        }
        grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
        trigger = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.triggerButton))) != 0;
        select = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.selectButton))) != 0;
    }

    void buzz(int index, unsigned short microseconds) noexcept
    {
        const auto device = controllerOf(index);
        if (g_settings.haptics && device != vr::k_unTrackedDeviceIndexInvalid) {
            g_vr->TriggerHapticPulse(device, 0, microseconds);
        }
    }

    bool readHand(int index) noexcept
    {
        auto& hand = g_hands_state[index];
        hand.previousGrip = hand.grip;
        hand.previousTrigger = hand.trigger;
        hand.valid = false;
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->getTrackedHandTransform) {
            return false;
        }
        RE::NiTransform wand;
        if (!frik->getTrackedHandTransform(index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left, FRIKApiV2::TrackedHandKind::Wand, &wand)) {
            return false;
        }
        hand.frame = wand;
        hand.frame.scale = 1.0f;
        hand.point = wand.translate;
        {
            // The palm: halfway between the wrist and the index fingertip of the rendered hand.
            const auto which = index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left;
            FRIKApiV2::ArmChainTransforms chain{};
            chain.structSize = sizeof(chain);
            if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
                const auto tip = frik->getIndexFingerTipPosition(which);
                const auto wrist = chain.hand.translate;
                if (distance(tip, wrist) < 40.0f) {
                    hand.point = (tip + wrist) * 0.5f;
                }
            }
        }
        hand.valid = true;
        readButtons(index, hand.grip, hand.trigger, hand.select);
        if (hand.grip && !hand.previousGrip) {
            hand.gripSince = Clock::now();
        }
        return true;
    }

    RE::NiPoint3 fingers(const Hand& hand) noexcept
    {
        // A local axis in world space is a row of the rotation (world = R^T * v).
        const int axis = g_settings.forwardAxis;
        return { hand.frame.rotate.entry[axis][0], hand.frame.rotate.entry[axis][1], hand.frame.rotate.entry[axis][2] };
    }

    float seconds(Clock::time_point since) noexcept
    {
        return std::chrono::duration<float>(Clock::now() - since).count();
    }

    // ------------------------------------------------------------ weapon 3D

    void logTree(RE::NiAVObject* root) noexcept
    {
        logNote("Weapon 3D tree (name, depth, world position, bound radius):");
        int lines = 0;
        forEachNode(root, [&](RE::NiAVObject* object, int depth) {
            if (lines++ > 300) {
                return;
            }
            char line[200]{};
            const auto& p = object->world.translate;
            std::snprintf(line, sizeof(line), "  %*s%s  (%.1f, %.1f, %.1f) r=%.1f%s", depth * 2, "",
                nodeName(object).c_str(), p.x, p.y, p.z, object->worldBound.fRadius, object->IsNode() ? "" : " [geometry]");
            logNote(line);
        });
    }

    // The node of an attached part: one named after the part's model file,
    // else one named after its slot ("Mag", "Scope", ...).
    RE::NiAVObject* findPartNode(RE::NiAVObject* root, const PartInfo& info, bool& byStem) noexcept
    {
        RE::NiAVObject* byName = nullptr;
        RE::NiAVObject* bySlot = nullptr;
        const std::string slotWord = lower(info.slot.size() > 7 ? info.slot.substr(7) : info.slot);  // ap_gun_Mag -> mag
        forEachNode(root, [&](RE::NiAVObject* object, int depth) {
            if (depth == 0 || byName) {
                return;
            }
            const auto name = lower(nodeName(object));
            if (name.empty()) {
                return;
            }
            if (!info.stem.empty() && name.find(info.stem) != std::string::npos) {
                byName = object;
            } else if (!bySlot && !slotWord.empty() && name.find(slotWord) != std::string::npos) {
                bySlot = object;
            }
        });
        byStem = byName != nullptr;
        return byName ? byName : bySlot;
    }

    // Model stems of all part mods in the table whose model shows in this 3D.
    std::unordered_set<std::string> stemsPresent(RE::NiAVObject* root) noexcept
    {
        std::unordered_set<std::string> stems;
        std::vector<std::string> names;
        forEachNode(root, [&](RE::NiAVObject* object, int) { names.push_back(lower(nodeName(object))); });
        for (const auto& [id, info] : g_table) {
            if (!info.part || info.stem.empty()) {
                continue;
            }
            for (const auto& name : names) {
                if (name.find(info.stem) != std::string::npos) {
                    stems.insert(info.stem);
                    break;
                }
            }
        }
        return stems;
    }

    void resolveAnchors(RE::NiAVObject* root) noexcept
    {
        g_anchors.clear();
        const auto weaponFrame = root->world;
        for (const auto omod : g_mods) {
            const auto* info = partInfo(omod);
            if (!info || !info->part) {
                continue;
            }
            bool byStem = false;
            auto* node = findPartNode(root, *info, byStem);
            if (!node) {
                continue;
            }
            g_stemsWork = g_stemsWork || byStem;
            Anchor anchor{ omod, node, centerOf(node) };
            g_anchors.push_back(anchor);
            g_slotPlaces[info->slot] = relative(weaponFrame, pointTransform(anchor.center)).translate;
        }
    }

    // ------------------------------------------------------------ per frame

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept
    {
        if (!id) {
            return nullptr;
        }
        auto* form = RE::TESForm::GetFormByID(id);
        return form ? form->As<RE::TESObjectREFR>() : nullptr;
    }

    std::array<float, 4> poseOf(const RE::NiPoint3& at, float heading) noexcept
    {
        return { at.x, at.y, at.z, heading };
    }

    // The workbench model's world transform (the reference's own when its 3D is not loaded).
    bool benchFrame(RE::TESObjectREFR* bench, RE::NiTransform& out) noexcept
    {
        if (!bench) {
            return false;
        }
        auto* root = bench->Get3D();
        if (looksLikeObject(root)) {
            out = root->world;
            if (out.scale <= 0.0f || out.scale > 10.0f) {
                out.scale = 1.0f;
            }
            return true;
        }
        return false;
    }

    bool hasCollision(RE::NiAVObject* root) noexcept
    {
        bool found = false;
        forEachNode(root, [&](RE::NiAVObject* object, int) {
            if (object->collisionObject) {
                found = true;
            }
        });
        return found;
    }

    RE::NiPoint3 benchPoint(const RE::NiTransform& bench, float x, float y, float z) noexcept
    {
        return compose(bench, pointTransform({ x, y, z })).translate;
    }

    // The vise area: a sphere above the vise where a weapon let go of stays in the air.
    bool viseArea(RE::NiPoint3& center) noexcept
    {
        RE::NiTransform benchWorld;
        if (!benchFrame(lookupRef(g_bench), benchWorld)) {
            return false;
        }
        center = benchPoint(benchWorld, g_settings.viseX, g_settings.viseY, g_settings.viseZ + g_settings.zoneLift);
        return true;
    }

    bool inViseArea(const RE::NiPoint3& point) noexcept
    {
        RE::NiPoint3 center;
        return viseArea(center) && distance(point, center) < g_settings.zoneRadius;
    }

    void soundCue(int index) noexcept
    {
        PendingCommand command;
        command.type = kSound;
        command.pose = { static_cast<float>(index), 0.0f, 0.0f, 0.0f };
        pushCommand(command);
    }

    // Weapon in hand, let go of in the vise area: it stays there.
    void updateClampGesture() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (int index = 0; index < 2; ++index) {
            auto& hand = g_hands_state[index];
            if (!player || !hand.valid || !inViseArea(hand.point)) {
                continue;
            }
            // a weapon let go here (drawn or carried loose) is looked for by the script
            const bool released = hand.previousGrip && !hand.grip;
            if (released && seconds(hand.gripSince) >= 0.15f) {
                PendingCommand command;
                command.type = kClamp;
                command.mod = g_weapon ? 1 : 0;   // 1: the vise holds a weapon that is not here: replace it
                command.pose = poseOf(hand.point, headingOf(fingers(hand)));
                pushCommand(command);
                g_clampPoint = hand.point;
                g_clampAt = Clock::now();
            }
        }
    }

    // ------------------------------------------------------------ outlines

    struct Ghost
    {
        std::uint32_t ref{ 0 };
        RE::NiAVObject* detached{ nullptr };
    };
    std::unordered_map<std::uint32_t, Ghost> g_ghosts;     // by mod
    std::unordered_map<std::uint32_t, int> g_ghostColor;   // colour last asked of the script, by mod

    struct GhostWish
    {
        int color{ 0 };  // 1 remove (red), 2 attach (green)
        RE::NiTransform world{};
    };
    std::unordered_map<std::uint32_t, GhostWish> g_ghostWish;  // this frame

    void wishGhost(std::uint32_t mod, int color, const RE::NiTransform& world) noexcept
    {
        g_ghostWish[mod] = GhostWish{ color, world };
    }

    bool ghostShowing(std::uint32_t mod) noexcept
    {
        const auto it = g_ghosts.find(mod);
        if (it == g_ghosts.end()) {
            return false;
        }
        auto* ref = lookupRef(it->second.ref);
        return ref && looksLikeObject(ref->Get3D());
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

    void applyGhosts() noexcept
    {
        // colours: the script plays the outline effect; it makes the ghost on first use
        std::unordered_set<std::uint32_t> mods;
        for (const auto& [mod, wish] : g_ghostWish) {
            mods.insert(mod);
        }
        for (const auto& [mod, color] : g_ghostColor) {
            mods.insert(mod);
        }
        for (const auto mod : mods) {
            const auto wish = g_ghostWish.find(mod);
            const int color = wish == g_ghostWish.end() ? 0 : wish->second.color;
            auto& sent = g_ghostColor[mod];
            if (sent != color) {
                sent = color;
                PendingCommand command;
                command.type = kGhost;
                command.mod = mod;
                command.pose = { static_cast<float>(color), 0.0f, 0.0f, 0.0f };
                pushCommand(command);
                if (color == 2) {
                    soundCue(0);
                }
            }
        }
        // log what happens to each wished ghost once per change (to see why one would not show)
        static std::unordered_map<std::uint32_t, int> logged;
        for (const auto& [mod, wish] : g_ghostWish) {
            const auto it = g_ghosts.find(mod);
            auto* ref = it != g_ghosts.end() ? lookupRef(it->second.ref) : nullptr;
            auto* root = ref ? ref->Get3D() : nullptr;
            const int state = !ref ? 1 : !looksLikeObject(root) ? 2 : 3;
            if (logged[mod] != state * 10 + wish.color) {
                logged[mod] = state * 10 + wish.color;
                char text[200]{};
                std::snprintf(text, sizeof(text), "Ghost %08X colour %d: %s at (%.0f, %.0f, %.0f)", mod, wish.color,
                    state == 1 ? "no ghost reference yet" : state == 2 ? "ghost has no 3D" : "shown",
                    wish.world.translate.x, wish.world.translate.y, wish.world.translate.z);
                logNote(text);
            }
        }
        for (auto& [mod, state] : logged) {
            if (!g_ghostWish.contains(mod)) {
                state = 0;
            }
        }
        // models: placed here every frame, hidden when not wanted
        for (auto& [mod, ghost] : g_ghosts) {
            auto* ref = lookupRef(ghost.ref);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(root)) {
                continue;
            }
            if (ghost.detached != root) {
                ghost.detached = root;
                ref->DetachHavok(root);
            }
            const auto wish = g_ghostWish.find(mod);
            if (wish == g_ghostWish.end()) {
                setCulled(root, true);
                continue;
            }
            setCulled(root, false);
            auto world = wish->second.world;
            world.scale = 1.0f;   // exactly the part's size (1.06 looked inflated)
            place3D(root, world);
            if (auto* fade = root->IsFadeNode()) {
                fade->GetFadeNodeRuntimeData().currentFade = wish->second.color == 2 ? g_settings.ghostFade : 1.0f;
            }
        }
        g_ghostWish.clear();
    }

    // ------------------------------------------------------------ the weapon in the vise

    // Where each slot's part sits, in weapon space (the part model's root goes there).
    std::unordered_map<std::string, RE::NiTransform> g_slotFrames;
    RE::NiTransform g_weaponWorld{};
    bool g_weaponWorldSet{ false };
    int g_weaponHeldBy{ -1 };

    bool slotWorld(const PartInfo& info, RE::NiTransform& out) noexcept
    {
        const auto it = g_slotFrames.find(info.slot);
        if (it == g_slotFrames.end()) {
            return false;
        }
        out = compose(g_weaponWorld, it->second);
        return true;
    }

    // The attach point of a part node: a "P-..." node inside it or just above it.
    RE::NiAVObject* attachPointOf(RE::NiAVObject* node) noexcept
    {
        RE::NiAVObject* found = nullptr;
        forEachNode(node, [&](RE::NiAVObject* object, int depth) {
            if (!found && depth <= 3 && nodeName(object).rfind("P-", 0) == 0) {
                found = object;
            }
        });
        auto* up = node;
        for (int i = 0; !found && i < 3 && looksLikeObject(up); ++i) {
            if (nodeName(up).rfind("P-", 0) == 0) {
                found = up;
            }
            up = up->parent;
        }
        return found ? found : node;
    }

    bool unclampAllowed() noexcept
    {
        std::unordered_set<std::string> filled;
        // the player may take the weapon in any state: no magazine is their own business
        return true;
        for (const auto omod : g_mods) {
            if (const auto* info = partInfo(omod)) {
                filled.insert(info->slot);
            }
        }
        for (const auto& slot : g_requiredAtClamp) {
            if (!filled.contains(slot)) {
                char text[160]{};
                std::snprintf(text, sizeof(text), "Сначала поставь %s", slotName(slot));
                hud(text);
                return false;
            }
        }
        return true;
    }

    const Anchor* nearestAnchor(const RE::NiPoint3& point, float radius) noexcept
    {
        const Anchor* best = nullptr;
        for (const auto& anchor : g_anchors) {
            const float d = distance(point, anchor.center);
            if (d < radius) {
                radius = d;
                best = &anchor;
            }
        }
        return best;
    }

    // ------------------------------------------------------------ snapping a part on

    struct Snap
    {
        std::uint32_t part{ 0 };
        std::uint32_t mod{ 0 };
        RE::NiTransform from{};
        RE::NiTransform to{};
        Clock::time_point start{};
        bool detached{ false };
    };
    std::vector<Snap> g_snaps;

    RE::NiMatrix3 blend(const RE::NiMatrix3& a, const RE::NiMatrix3& b, float t) noexcept
    {
        RE::NiMatrix3 m;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                m.entry[r][c] = a.entry[r][c] + (b.entry[r][c] - a.entry[r][c]) * t;
            }
        }
        // re-orthonormalise the rows
        auto row = [&](int r) { return RE::NiPoint3{ m.entry[r][0], m.entry[r][1], m.entry[r][2] }; };
        auto set = [&](int r, const RE::NiPoint3& v) { m.entry[r][0] = v.x; m.entry[r][1] = v.y; m.entry[r][2] = v.z; };
        auto norm = [](RE::NiPoint3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return l > 1e-6f ? v * (1.0f / l) : v; };
        auto cross = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return RE::NiPoint3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };
        const auto x = norm(row(0));
        const auto z = norm(cross(x, row(1)));
        const auto y = cross(z, x);
        set(0, x);
        set(1, y);
        set(2, z);
        return m;
    }

    void startSnap(std::uint32_t partId, LoosePart& part, const RE::NiTransform& from, const RE::NiTransform& to) noexcept
    {
        part.carriedBy = -3;
        Snap snap;
        snap.part = partId;
        snap.mod = part.mod;
        snap.from = from;
        snap.to = to;
        snap.start = Clock::now();
        g_snaps.push_back(snap);
    }

    void updateSnaps() noexcept
    {
        for (auto it = g_snaps.begin(); it != g_snaps.end();) {
            auto* ref = lookupRef(it->part);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(root)) {
                it = g_snaps.erase(it);
                continue;
            }
            if (!it->detached) {
                it->detached = true;
                ref->DetachHavok(root);
            }
            const float t = (std::min)(1.0f, seconds(it->start) / (std::max)(0.01f, g_settings.snapTime));
            const float e = t * t * (3.0f - 2.0f * t);
            RE::NiTransform world;
            world.translate = it->from.translate + (it->to.translate - it->from.translate) * e;
            world.rotate = blend(it->from.rotate, it->to.rotate, e);
            world.scale = 1.0f;
            place3D(root, world);
            if (t >= 1.0f) {
                PendingCommand command;
                command.type = kAttach;
                command.mod = it->mod;
                command.part = it->part;
                command.pose = poseOf(world.translate, 0.0f);
                pushCommand(command);
                soundCue(2);
                it = g_snaps.erase(it);
                continue;
            }
            ++it;
        }
    }

    // A part close to its place shows where it goes; let go there, it snaps on.
    bool previewAttach(const LoosePart& part, const RE::NiAVObject* root, const RE::NiTransform& partWorld, RE::NiTransform& target) noexcept
    {
        const auto* info = partInfo(part.mod);
        if (!info || !slotWorld(*info, target)) {
            return false;
        }
        if (std::find(g_mods.begin(), g_mods.end(), part.mod) != g_mods.end()) {
            return false;  // already on
        }
        if (distance(partWorld.translate, target.translate) < g_settings.previewRadius) {
            return true;
        }
        // held crooked: the part's middle near where its middle goes counts too
        const auto localCenter = relative(root->world, pointTransform(centerOf(root))).translate;
        const auto now = compose(partWorld, pointTransform(localCenter)).translate;
        const auto there = compose(target, pointTransform(localCenter)).translate;
        return distance(now, there) < g_settings.previewRadius;
    }

    // ------------------------------------------------------------ hands at the weapon

    void endHoldPart(Hand& hand) noexcept
    {
        setCulled(hand.hidden, false);
        hand.hidden = nullptr;
    }

    void updateHandAtWeapon(int index, RE::NiAVObject* weapon3D) noexcept
    {
        auto& hand = g_hands_state[index];
        if (!hand.valid) {
            return;
        }
        const bool pressed = hand.grip && !hand.previousGrip;
        const bool triggered = hand.trigger && !hand.previousTrigger;

        switch (hand.mode) {
        case Hand::Mode::Idle:
            {
                // A / X held: the part under the hand shows in red; grip takes it off
                const auto* anchor = hand.select ? nearestAnchor(hand.point, g_settings.grabRadius) : nullptr;
                const std::uint32_t hover = anchor ? anchor->mod : 0;
                if (hover != hand.hover) {
                    endHoldPart(hand);
                    hand.hover = hover;
                    hand.holdNode = anchor ? anchor->node : nullptr;
                    if (hover) {
                        buzz(index, 800);
                        soundCue(1);
                    }
                }
                RE::NiTransform hoverSlot;
                const auto* hoverInfo = hover ? partInfo(hover) : nullptr;
                bool hoverKnown = hoverInfo && slotWorld(*hoverInfo, hoverSlot);
                if (hover && !hoverKnown && looksLikeObject(anchor->node)) {
                    // slot not found on the model: the part's own node stands in for it
                    hoverSlot = anchor->node->world;
                    hoverSlot.scale = 1.0f;
                    hoverKnown = true;
                }
                if (hover && hoverKnown) {
                    wishGhost(hover, 1, hoverSlot);
                }
                if (!pressed) {
                    break;
                }
                if (!hover && !hand.select) {
                    // a loose part with no physics (the ported barrel): ROCK can not take it, we carry it
                    std::uint32_t best = 0;
                    RE::NiAVObject* bestRoot = nullptr;
                    float bestDistance = g_settings.grabRadius * 1.5f;
                    for (auto& [id, part] : g_parts) {
                        auto* ref = lookupRef(id);
                        auto* root = ref ? ref->Get3D() : nullptr;
                        if (part.carriedBy != -1 || part.collision != 0 || !looksLikeObject(root)) {
                            continue;
                        }
                        const float d = distance(hand.point, centerOf(root));
                        if (d < bestDistance) {
                            bestDistance = d;
                            best = id;
                            bestRoot = root;
                        }
                    }
                    if (best) {
                        hand.part = best;
                        hand.offset = relative(hand.frame, bestRoot->world);
                        hand.armed = true;
                        hand.carried3D = bestRoot;
                        hand.mode = Hand::Mode::Carry;
                        g_parts[best].carriedBy = index;
                        buzz(index, 1500);
                        soundCue(3);
                        break;
                    }
                }
                const float toWeapon = distance(hand.point, centerOf(weapon3D));
                const auto* touched = nearestAnchor(hand.point, g_settings.grabRadius);
                if (g_settings.logGrab) {
                    char text[200]{};
                    std::snprintf(text, sizeof(text), "Grip %s%s: part %s, weapon %.1f",
                        index == 0 ? "R" : "L", hand.select ? " +A" : "",
                        anchor ? nodeName(anchor->node).c_str() : "-", toWeapon);
                    logNote(text);
                }
                if (hover) {
                    endHoldPart(hand);
                    hand.hover = 0;
                    PendingCommand command;
                    command.type = kRemove;
                    command.mod = hover;
                    // The part appears as a normal physical object right in the palm while grip is
                    // still held: ROCK's grab leeway (fGrabInputLeewaySeconds) then takes it with the fingers.
                    const auto from = looksLikeObject(anchor->node) ? anchor->node->world : weapon3D->world;
                    hand.offset = relative(hand.frame, from);
                    command.pose = g_settings.removeIntoHand
                        ? poseOf(compose(hand.frame, hand.offset).translate, headingOf(fingers(hand)))
                        : poseOf(hand.point, headingOf(fingers(hand)));
                    pushCommand(command);
                    hand.mod = hover;
                    hand.armed = false;
                    hand.mode = g_settings.removeIntoHand ? Hand::Mode::Await : Hand::Mode::Done;
                    hand.since = Clock::now();
                    buzz(index, 3000);
                    soundCue(3);
                } else if ((touched || toWeapon < g_settings.weaponGrabRadius) && g_weaponHeldBy < 0 && !hand.select) {
                    // grip anywhere on the weapon: it becomes a normal physical weapon right in the hand
                    // and ROCK takes it with the fingers (the press is still held), like the parts
                    PendingCommand command;
                    command.type = kUnclamp;
                    command.mod = 2;   // flag: leave it loose for ROCK, do not put it in the inventory
                    pushCommand(command);
                    buzz(index, 2500);
                    soundCue(3);
                    hand.mode = Hand::Mode::Done;
                }
                break;
            }
        case Hand::Mode::HoldPart:
            {
                const auto* info = partInfo(hand.mod);
                RE::NiTransform slot;
                const bool known = info && slotWorld(*info, slot);
                if (!hand.grip) {
                    endHoldPart(hand);
                    hand.mode = Hand::Mode::Idle;
                    break;
                }
                if (known) {
                    wishGhost(hand.mod, 1, slot);
                    // the outline stands in for the real part once it is showing
                    if (!hand.hidden && ghostShowing(hand.mod) && looksLikeObject(hand.holdNode)) {
                        hand.hidden = hand.holdNode;
                        setCulled(hand.hidden, true);
                    }
                }
                if (triggered) {
                    endHoldPart(hand);
                    PendingCommand command;
                    command.type = kRemove;
                    command.mod = hand.mod;
                    const auto from = known ? slot : weapon3D->world;
                    command.pose = poseOf(from.translate, headingOf({ from.rotate.entry[1][0], from.rotate.entry[1][1], from.rotate.entry[1][2] }));
                    pushCommand(command);
                    hand.offset = relative(hand.frame, from);
                    hand.armed = false;
                    hand.mode = Hand::Mode::Await;
                    hand.since = Clock::now();
                    buzz(index, 3000);
                    soundCue(3);
                }
                break;
            }
        case Hand::Mode::HoldWeapon:
            {
                g_weaponWorld = compose(hand.frame, hand.offset);
                if (!hand.grip) {
                    g_weaponHeldBy = -1;
                    hand.mode = Hand::Mode::Idle;
                    const auto& r = g_weaponWorld.rotate;
                    if (inViseArea(g_weaponWorld.translate) || !unclampAllowed()) {
                        PendingCommand command;
                        command.type = kMove;
                        command.pose = poseOf(g_weaponWorld.translate, headingOf({ r.entry[1][0], r.entry[1][1], r.entry[1][2] }));
                        pushCommand(command);
                        buzz(index, 1500);
                        soundCue(2);
                    } else {
                        // taken out of the vise area: it is yours again
                        PendingCommand command;
                        command.type = kUnclamp;
                        pushCommand(command);
                        buzz(index, 3000);
                    }
                }
                break;
            }
        case Hand::Mode::Await:
            if (seconds(hand.since) > 3.0f) {
                hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
            }
            break;
        case Hand::Mode::Carry:
            {
                auto it = g_parts.find(hand.part);
                auto* ref = lookupRef(hand.part);
                auto* root = ref ? ref->Get3D() : nullptr;
                if (it == g_parts.end() || !looksLikeObject(root)) {
                    hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
                    break;
                }
                auto& part = it->second;
                const auto world = compose(hand.frame, hand.offset);
                place3D(root, world);
                RE::NiTransform target;
                const bool closeBy = previewAttach(part, root, world, target);
                const auto* info = partInfo(part.mod);
                RE::NiTransform slot;
                if (info && slotWorld(*info, slot)) {
                    hand.armed = hand.armed || distance(world.translate, slot.translate) > g_settings.previewRadius * 1.5f;
                }
                const bool show = closeBy && hand.armed;
                if (show) {
                    wishGhost(part.mod, 2, target);
                }
                if (!hand.grip) {
                    if (show) {
                        startSnap(hand.part, part, world, target);
                    } else {
                        // the script rebuilds its physics where it was let go
                        PendingCommand command;
                        command.type = kDrop;
                        command.part = hand.part;
                        command.pose = poseOf(world.translate, 0.0f);
                        RE::NiTransform benchWorld;
                        RE::NiPoint3 center;
                        if (part.collision == 0 && viseArea(center) && benchFrame(lookupRef(g_bench), benchWorld)
                            && distance(world.translate, center) < 90.0f) {
                            // it can not fall by itself: lay it on the table under the hand
                            command.pose[2] = benchPoint(benchWorld, 0.0f, 0.0f, g_settings.tableZ).z;
                        }
                        pushCommand(command);
                        part.carriedBy = -1;
                    }
                    hand.mode = Hand::Mode::Idle;
                }
                break;
            }
        case Hand::Mode::Done:
            if (!hand.grip) {
                hand.mode = Hand::Mode::Idle;
            }
            break;
        }
    }

    // Parts held by ROCK: show where they go, snap them on when let go there.
    void updateHeldParts(RE::NiAVObject* weapon3D) noexcept
    {
        for (auto& [id, part] : g_parts) {
            if (part.carriedBy != -1) {
                continue;
            }
            auto* ref = lookupRef(id);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (looksLikeObject(root) && root != part.detached && part.collision < 0) {
                part.collision = hasCollision(root) ? 1 : 0;
                if (g_settings.logGrab) {
                    logNote("Part " + std::to_string(id) + (part.collision ? " has physics" : " has NO physics: the mod carries it"));
                }
            }
            if (!looksLikeObject(root) || distance(root->world.translate, centerOf(weapon3D)) > 60.0f) {
                continue;
            }
            // which hand holds it (ROCK has it): a gripping hand right at it
            int holder = -1;
            for (int index = 0; index < 2; ++index) {
                const auto& hand = g_hands_state[index];
                const bool gripping = hand.grip || hand.previousGrip;
                if (hand.valid && gripping && (hand.mode == Hand::Mode::Idle || hand.mode == Hand::Mode::Done) && distance(hand.point, centerOf(root)) < g_settings.heldRadius) {
                    holder = index;
                }
            }
            RE::NiTransform target;
            if (holder < 0 || !previewAttach(part, root, root->world, target)) {
                part.previewing = false;
                continue;
            }
            const auto& hand = g_hands_state[holder];
            if (hand.grip) {
                if (!part.previewing) {
                    part.previewing = true;
                    part.previewSince = Clock::now();
                }
                wishGhost(part.mod, 2, target);
            } else if (part.previewing && seconds(part.previewSince) > 0.25f) {
                // let go while its ghost was showing: it snaps on
                part.previewing = false;
                buzz(holder, 2000);
                startSnap(id, part, root->world, target);
            } else {
                part.previewing = false;
            }
        }
    }

    void syncShared() noexcept
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> added;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ghosts;
        std::vector<std::uint32_t> inHand;
        std::vector<std::uint32_t> forgotten;
        {
            std::scoped_lock guard(g_shared.lock);
            g_bench = g_shared.bench;
            if (g_shared.revision != g_revision) {
                g_revision = g_shared.revision;
                if (g_shared.weapon != g_weapon) {
                    g_weapon = g_shared.weapon;
                    g_slotPlaces.clear();
                    g_slotFrames.clear();
                    g_requiredAtClamp.clear();
                    g_missing3DLogged = false;
                    g_missing3DForgot = false;
                    g_weaponWorldSet = false;
                    g_weaponHeldBy = -1;
                    for (const auto omod : g_shared.mods) {
                        const auto* info = partInfo(omod);
                        if (info && info->required) {
                            g_requiredAtClamp.insert(info->slot);
                        }
                    }
                    g_treeLogged = false;
                    g_treeLoggedLate = false;
                    g_clampFeedback = g_weapon != 0;
                    if (!g_weapon) {
                        g_ghosts.clear();
                        g_ghostColor.clear();
                        g_snaps.clear();
                    }
                }
                g_mods = g_shared.mods;
                g_weapon3D = nullptr;  // re-resolve
                // the model may be rebuilt: hidden nodes are checked again before they are touched
                for (auto& hand : g_hands_state) {
                    hand.hidden = nullptr;
                    hand.holdNode = nullptr;
                    hand.hover = 0;
                    if (hand.mode == Hand::Mode::HoldPart) {
                        hand.mode = Hand::Mode::Done;
                    }
                }
                g_revisionTime = Clock::now();
                g_shared.modelOutdated = false;
            }
            added.swap(g_shared.newParts);
            ghosts.swap(g_shared.newGhosts);
            inHand.swap(g_shared.inHandParts);
            forgotten.swap(g_shared.forgottenParts);
        }
        for (const auto& [ref, mod] : ghosts) {
            g_ghosts[mod] = Ghost{ ref, nullptr };
        }
        for (const auto id : forgotten) {
            g_parts.erase(id);
            for (auto& hand : g_hands_state) {
                if (hand.part == id && hand.mode == Hand::Mode::Carry) {
                    hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
                }
            }
        }
        for (const auto& [id, mod] : added) {
            LoosePart part;
            part.mod = mod;
            g_parts[id] = part;
        }
        // A part pulled off the weapon goes straight into the hand that pulled it.
        for (const auto id : inHand) {
            auto* ref = lookupRef(id);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(root)) {
                // 3D not loaded yet: try again next frame
                std::scoped_lock guard(g_shared.lock);
                g_shared.inHandParts.push_back(id);
                continue;
            }
            if (g_parts[id].collision < 0) {
                g_parts[id].collision = hasCollision(root) ? 1 : 0;
            }
            bool taken = false;
            for (int index = 0; index < 2 && g_settings.removeIntoHand; ++index) {
                auto& hand = g_hands_state[index];
                if (hand.mode != Hand::Mode::Await || !hand.grip) {
                    continue;
                }
                // hand.offset stays what it was when the part was taken (as in 0.4)
                hand.mode = Hand::Mode::Carry;
                hand.carried3D = nullptr;
                hand.part = id;
                g_parts[id].carriedBy = index;
                taken = true;
                break;
            }
            if (!taken) {
                // nobody carries it (grip let go, or bRemoveIntoHand=0): it becomes a normal
                // object right where it came off, falls onto the bench and ROCK takes it.
                // A keyframed part left alone used to hang in the air for good.
                for (auto& hand : g_hands_state) {
                    if (hand.mode == Hand::Mode::Await) {
                        hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
                    }
                }
                PendingCommand command;
                command.type = kDrop;
                command.part = id;
                command.pose = poseOf(root->world.translate, 0.0f);
                RE::NiTransform benchWorld;
                if (g_parts[id].collision == 0 && benchFrame(lookupRef(g_bench), benchWorld)) {
                    command.pose[2] = benchPoint(benchWorld, 0.0f, 0.0f, g_settings.tableZ).z;
                }
                pushCommand(command);
                g_parts[id].carriedBy = -1;
            }
        }
    }

    // The clamped weapon must not be a physics object: ROCK would grab it as a loose
    // weapon, and its cached bodies go stale when a mod change rebuilds the model.
    std::uint64_t g_detachedRevision{ ~0ull };
    RE::NiAVObject* g_detached3D{ nullptr };
    Clock::time_point g_detachTime{};
    int g_detachPasses{ 0 };

    void keepWeaponOutOfPhysics(RE::TESObjectREFR* weapon, RE::NiAVObject* root) noexcept
    {
        if (root != g_detached3D || g_revision != g_detachedRevision) {
            g_detached3D = root;
            g_detachedRevision = g_revision;
            g_detachPasses = 0;
            g_detachTime = Clock::now();
        }
        // right away, then again a little later: a mod change may rebuild the collision
        static constexpr float passes[] = { 0.0f, 0.25f, 0.75f, 2.0f };
        if (g_detachPasses >= 4) {
            // and every half second after that: moving the reference (SetPosition) brings physics back
            if (seconds(g_detachTime) < 2.5f) {
                return;
            }
            g_detachTime = Clock::now() - std::chrono::milliseconds(2000);
            weapon->DetachHavok(root);
            return;
        }
        if (seconds(g_detachTime) < passes[g_detachPasses]) {
            return;
        }
        ++g_detachPasses;
        const bool done = weapon->DetachHavok(root);
        if (g_detachPasses == 1) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "Weapon %08X taken out of physics (%d)", weapon->GetFormID(), done);
            logNote(text);
        }
    }

    // Where a slot sits on each kind of weapon, remembered across clamps and game restarts:
    // a weapon clamped with its grip or magazine already off still takes them back.
    std::unordered_map<std::string, RE::NiTransform> g_slotMemory;
    bool g_slotMemoryLoaded{ false };
    constexpr const char* kSlotFile = "Data/F4SE/Plugins/WeaponVise_slots.txt";

    std::string slotKey(std::uint32_t base, const std::string& slot)
    {
        char text[16]{};
        std::snprintf(text, sizeof(text), "%08X", base);
        return std::string(text) + " " + slot;
    }

    void loadSlotMemory() noexcept
    {
        if (g_slotMemoryLoaded) {
            return;
        }
        g_slotMemoryLoaded = true;
        try {
            std::ifstream file(kSlotFile);
            std::string base, slot;
            while (file >> base >> slot) {
                RE::NiTransform t{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        file >> t.rotate.entry[r][c];
                    }
                }
                file >> t.translate.x >> t.translate.y >> t.translate.z;
                t.scale = 1.0f;
                if (!file) {
                    break;
                }
                g_slotMemory[base + " " + slot] = t;
            }
        } catch (...) {
        }
    }

    void saveSlotMemory() noexcept
    {
        try {
            std::ofstream file(kSlotFile, std::ios::trunc);
            for (const auto& [key, t] : g_slotMemory) {
                file << key;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        file << ' ' << t.rotate.entry[r][c];
                    }
                }
                file << ' ' << t.translate.x << ' ' << t.translate.y << ' ' << t.translate.z << '\n';
            }
        } catch (...) {
        }
    }

    void resolveSlots(RE::NiAVObject* root) noexcept
    {
        loadSlotMemory();
        auto* ref = lookupRef(g_weapon);
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        const std::uint32_t baseId = base ? base->GetFormID() : 0;
        bool changed = false;
        for (const auto& anchor : g_anchors) {
            const auto* info = partInfo(anchor.mod);
            if (!info) {
                continue;
            }
            auto* point = attachPointOf(anchor.node);
            const auto frame = relative(root->world, point->world);
            g_slotFrames[info->slot] = frame;
            if (baseId) {
                auto& kept = g_slotMemory[slotKey(baseId, info->slot)];
                if (distance(kept.translate, frame.translate) > 0.2f || kept.scale == 0.0f) {
                    kept = frame;
                    kept.scale = 1.0f;
                    changed = true;
                }
            }
        }
        // slots of parts that are off this weapon now: from the same kind of weapon seen before
        if (baseId) {
            const auto prefix = slotKey(baseId, "");
            for (const auto& [key, frame] : g_slotMemory) {
                if (key.rfind(prefix, 0) == 0) {
                    g_slotFrames.try_emplace(key.substr(prefix.size()), frame);
                }
            }
        }
        if (changed) {
            saveSlotMemory();
        }
    }

    // The zone frame: corner brackets around the spot above the vise, shown while a weapon is near it.
    void updateFrame() noexcept
    {
        std::uint32_t id;
        std::uint32_t gen;
        {
            std::scoped_lock guard(g_shared.lock);
            id = g_shared.frame;
            gen = g_shared.frameGen;
        }
        auto* ref = id ? lookupRef(id) : nullptr;
        auto* root = ref ? ref->Get3D() : nullptr;
        if (!looksLikeObject(root)) {
            return;
        }
        RE::NiPoint3 center;
        RE::NiTransform bench;
        const bool known = viseArea(center) && benchFrame(lookupRef(g_bench), bench);
        bool show = false;
        bool inside = false;
        if (known) {
            const auto& hand = g_hands_state[g_settings.weaponHand == 1 ? 1 : 0];
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!g_weapon) {
                // a hand coming close (with a drawn weapon or one carried loose)
                (void)player;
                for (const auto& h : g_hands_state) {
                    if (h.valid && distance(h.point, center) < g_settings.zoneRadius * 2.5f) {
                        show = true;
                        inside = inside || inViseArea(h.point);
                    }
                }
            } else if (g_weaponHeldBy >= 0) {
                // the clamped weapon is being carried
                show = true;
                inside = inViseArea(g_weaponWorld.translate);
            }
        }
        // the reference itself goes to the zone once, so the game knows where the frame is
        static std::uint64_t placedFor = 0;
        const std::uint64_t key = ((static_cast<std::uint64_t>(id) << 32) | g_bench) ^ (static_cast<std::uint64_t>(gen) << 48);
        if (known && placedFor != key) {
            placedFor = key;
            PendingCommand command;
            command.type = kFramePose;
            command.pose = poseOf(center, headingOf({ bench.rotate.entry[1][0], bench.rotate.entry[1][1], bench.rotate.entry[1][2] }));
            pushCommand(command);
        }
        setCulled(root, !show);
        if (!show) {
            return;
        }
        RE::NiTransform world{};
        world.rotate = bench.rotate;
        world.translate = center;
        world.scale = 1.0f;
        place3D(root, world);
        if (auto* fade = root->IsFadeNode()) {
            fade->GetFadeNodeRuntimeData().currentFade = inside ? 1.0f : 0.35f;
        }
    }

    void update() noexcept
    {
        readHand(0);
        readHand(1);
        syncShared();

        if (!g_weapon) {
            for (auto& hand : g_hands_state) {
                hand.mode = Hand::Mode::Idle;
                hand.hidden = nullptr;
                hand.hover = 0;
            }
            updateClampGesture();
            updateFrame();
            return;
        }
        auto* weaponRef = lookupRef(g_weapon);
        auto* root = weaponRef ? weaponRef->Get3D() : nullptr;
        if (!looksLikeObject(root)) {
            if (!g_missing3DLogged) {
                g_missing3DLogged = true;
                g_missing3DAt = Clock::now();
                logNote("Clamped weapon has no 3D yet");
            } else if (!g_missing3DForgot && seconds(g_missing3DAt) > 3.0f) {
                // gone from the world (in a backpack, another cell, an old save): let it go,
                // or the vise would wait for it forever and nothing else would work
                g_missing3DForgot = true;
                logNote("Clamped weapon still has no 3D: the vise lets it go");
                PendingCommand command; command.type = kUnclamp; command.mod = 3; pushCommand(command);
            }
            return;
        }
        g_missing3DLogged = false;
        g_missing3DForgot = false;
        if (g_clampFeedback) {
            g_clampFeedback = false;
            buzz(0, 2000);
            buzz(1, 2000);
            soundCue(2);
        }
        if (!g_weaponWorldSet) {
            // wait until the script's SetPosition has reached the 3D, or the weapon would be pinned
            // where it was dropped (in the hand) and look like it vanished
            if (distance(root->world.translate, g_clampPoint) > 40.0f && seconds(g_clampAt) < 2.0f) {
                return;
            }
            g_weaponWorld = root->world;
            g_weaponWorld.scale = 1.0f;
            g_weaponWorldSet = true;
            char message[160]{};
            std::snprintf(message, sizeof(message), "Weapon pinned at (%.0f, %.0f, %.0f), %.0f from the clamp point",
                g_weaponWorld.translate.x, g_weaponWorld.translate.y, g_weaponWorld.translate.z,
                distance(g_weaponWorld.translate, g_clampPoint));
            logNote(message);
        }
        // the weapon stays exactly where it was left (or follows the hand holding it)
        place3D(root, g_weaponWorld);
        if (root != g_weapon3D) {
            g_weapon3D = root;
            if (g_settings.logTree && !g_treeLogged) {
                g_treeLogged = true;
                logTree(root);
            }
            resolveAnchors(root);
            resolveSlots(root);
            char message[160]{};
            std::snprintf(message, sizeof(message), "Weapon %08X: %zu mods, %zu parts found on the model, %zu slots known",
                g_weapon, g_mods.size(), g_anchors.size(), g_slotFrames.size());
            logNote(message);
            for (const auto& anchor : g_anchors) {
                const auto* info = partInfo(anchor.mod);
                std::snprintf(message, sizeof(message), "  part %s -> node '%s', attach point '%s'", info ? info->edid.c_str() : "?",
                    nodeName(anchor.node).c_str(), nodeName(attachPointOf(anchor.node)).c_str());
                logNote(message);
            }
        }
        if (g_settings.logTree && !g_treeLoggedLate && seconds(g_revisionTime) > 1.5f) {
            g_treeLoggedLate = true;
            logNote("Tree again, 1.5 s after the change:");
            logTree(root);
            if (auto* parent = root->parent; looksLikeObject(parent)) {
                auto& children = parent->GetRuntimeData().children;
                std::string text = "Parent '" + nodeName(parent) + "' children:";
                for (std::uint16_t i = 0; i < children.size() && i < 20; ++i) {
                    text += " '" + nodeName(children[i].get()) + "'";
                }
                logNote(text);
            }
        }
        // parts may be re-created after a change
        bool changed = false;
        for (auto& anchor : g_anchors) {
            if (looksLikeObject(anchor.node)) {
                anchor.center = centerOf(anchor.node);
            } else {
                changed = true;
            }
        }
        if (changed || seconds(g_revisionTime) < 1.0f) {
            resolveAnchors(root);
            resolveSlots(root);
        }
        keepWeaponOutOfPhysics(weaponRef, root);
        {
            // the clamped weapon is not above the vise (an old save, a moved bench): another may take its place
            RE::NiPoint3 zone;
            if (g_weaponHeldBy < 0 && viseArea(zone) && distance(g_weaponWorld.translate, zone) > g_settings.zoneRadius * 3.0f) {
                updateClampGesture();
            }
        }
        updateHandAtWeapon(0, root);
        updateHandAtWeapon(1, root);
        updateHeldParts(root);
        updateSnaps();
        applyGhosts();
        updateFrame();
    }

    // ------------------------------------------------------------ Papyrus natives

    std::uint32_t idOf(const RE::TESForm* form) noexcept
    {
        return form ? form->GetFormID() : 0;
    }

    void SetFrame(std::monostate, RE::TESObjectREFR* frame)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.frame = idOf(frame);
        ++g_shared.frameGen;
    }

    void SetWorkbench(std::monostate, RE::TESObjectREFR* bench)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.bench = idOf(bench);
    }

    void SetWeapon(std::monostate, RE::TESObjectREFR* weapon, std::vector<Mod*> mods)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.weapon = idOf(weapon);
        g_shared.mods.clear();
        for (auto* mod : mods) {
            if (mod) {
                g_shared.mods.push_back(mod->GetFormID());
            }
        }
        ++g_shared.revision;
        std::string text = "SetWeapon " + std::to_string(g_shared.weapon) + " mods:";
        for (const auto id : g_shared.mods) {
            const auto* info = partInfo(id);
            text += " ";
            text += info ? info->edid : std::to_string(id);
        }
        logNote(text);
    }

    void AddPart(std::monostate, RE::TESObjectREFR* part, Mod* mod, bool inHand)
    {
        if (!part || !mod) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.newParts.emplace_back(part->GetFormID(), mod->GetFormID());
        if (inHand) {
            g_shared.inHandParts.push_back(part->GetFormID());
        }
    }

    void ForgetPart(std::monostate, RE::TESObjectREFR* part)
    {
        if (!part) {
            return;
        }
        std::scoped_lock guard(g_shared.lock);
        g_shared.forgottenParts.push_back(part->GetFormID());
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

    std::int32_t CommandFlag(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return static_cast<std::int32_t>(g_shared.current.mod);
    }

    // Ids, not forms: returning a form made the VM crash in CreateObject (Papyrus calls Game.GetForm).
    std::int32_t CommandModID(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return static_cast<std::int32_t>(g_shared.current.mod);
    }

    bool RemoveIntoHand(std::monostate)
    {
        return g_settings.removeIntoHand;
    }

    std::int32_t CommandPartID(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return static_cast<std::int32_t>(g_shared.current.part);
    }

    std::vector<float> CommandPose(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.pose.begin(), g_shared.current.pose.end() };
    }

    // Where the i-th loose part is laid out: a row beside the clamped weapon.
    std::vector<float> RackPose(std::monostate, std::int32_t index)
    {
        std::array<float, 4> weapon;
        RE::NiTransform bench;
        bool benchKnown = false;
        {
            std::scoped_lock guard(g_shared.lock);
            weapon = g_shared.weaponPose;
            bench = g_shared.benchWorld;
            benchKnown = g_shared.benchKnown;
        }
        if (benchKnown) {
            const int perRow = g_settings.rackPerRow;
            const float x = g_settings.rackX + g_settings.rackStep * static_cast<float>(index / perRow);
            const float y = g_settings.rackY + g_settings.rackStep * static_cast<float>(index % perRow);
            const auto at = benchPoint(bench, x, y, g_settings.rackZ);
            const RE::NiPoint3 benchForward{ bench.rotate.entry[1][0], bench.rotate.entry[1][1], bench.rotate.entry[1][2] };
            return { at.x, at.y, at.z, headingOf(benchForward) + 90.0f };
        }
        const float rackStart = 14.0f;
        const float heading = weapon[3] * 0.01745329f;
        const float rightX = std::cos(heading);
        const float rightY = -std::sin(heading);
        const float along = rackStart + g_settings.rackStep * static_cast<float>(index % 6);
        const float back = static_cast<float>(index / 6) * g_settings.rackStep;
        const float forwardX = std::sin(heading);
        const float forwardY = std::cos(heading);
        return {
            weapon[0] + rightX * along - forwardX * back,
            weapon[1] + rightY * along - forwardY * back,
            weapon[2],
            weapon[3],
        };
    }

    std::int32_t FallbackID(std::monostate, Mod* mod)
    {
        const auto* info = mod ? partInfo(mod->GetFormID()) : nullptr;
        return info ? static_cast<std::int32_t>(info->fallback) : 0;
    }

    void AddGhost(std::monostate, RE::TESObjectREFR* ghost, Mod* mod)
    {
        if (!ghost || !mod) {
            logNote("AddGhost: no reference or mod");
            return;
        }
        logNote("AddGhost " + std::to_string(ghost->GetFormID()) + " for mod " + std::to_string(mod->GetFormID()));
        std::scoped_lock guard(g_shared.lock);
        g_shared.newGhosts.emplace_back(ghost->GetFormID(), mod->GetFormID());
    }

    bool IsFreeMod(std::monostate, Mod* mod)
    {
        if (!mod) {
            return false;
        }
        const auto id = mod->GetFormID();
        for (const auto& [omod, info] : g_table) {
            if (info.fallback == id) {
                return true;
            }
        }
        return false;
    }

    bool ModelOutdated(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        const bool outdated = g_shared.modelOutdated;
        g_shared.modelOutdated = false;
        return outdated;
    }

    void Notify(std::monostate, std::int32_t message)
    {
        switch (message) {
        case 1: hud("Сначала возьми оружие в руку"); break;
        case 2: hud("В тиски пока можно зажать только 10-мм пистолет"); break;
        case 3: hud("Не получилось положить оружие в тиски"); break;
        case 4: hud("Оружие в тисках. Держи курок и наведи руку на деталь, грип: снять. Грип за рукоять: забрать оружие"); break;
        case 5: hud("Эта деталь сюда не подходит"); break;
        case 6: hud("Оружие снова в руке"); break;
        case 7: hud("Ты отошёл: оружие и детали вернулись в инвентарь"); break;
        case 10: break;  // took the weapon ROCK dropped
        default: break;
        }
        logNote("Notify " + std::to_string(message));
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "WeaponVise";
        vm->BindNativeMethod(kScript, "SetWorkbench", SetWorkbench, true);
        vm->BindNativeMethod(kScript, "SetWeapon", SetWeapon, true);
        vm->BindNativeMethod(kScript, "AddPart", AddPart, true);
        vm->BindNativeMethod(kScript, "ForgetPart", ForgetPart, true);
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandModID", CommandModID, true);
        vm->BindNativeMethod(kScript, "CommandPartID", CommandPartID, true);
        vm->BindNativeMethod(kScript, "RemoveIntoHand", RemoveIntoHand, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "RackPose", RackPose, true);
        vm->BindNativeMethod(kScript, "ModelOutdated", ModelOutdated, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        vm->BindNativeMethod(kScript, "FallbackID", FallbackID, true);
        vm->BindNativeMethod(kScript, "IsFreeMod", IsFreeMod, true);
        vm->BindNativeMethod(kScript, "AddGhost", AddGhost, true);
        vm->BindNativeMethod(kScript, "SetFrame", SetFrame, true);
        vm->BindNativeMethod(kScript, "CommandFlag", CommandFlag, true);
        logNote("Papyrus functions of script WeaponVise registered");
        return true;
    }

    // ------------------------------------------------------------ frame hook

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        __try {
            update();
        } __except (1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logWarning("A fault inside the frame update was caught; WeaponVise keeps running");
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
                std::snprintf(message, sizeof(message), "FRIK API v2.3+ unavailable (error %d); WeaponVise cannot see the hands", error);
                logWarning(message);
            } else {
                const bool hooked = FRIKApiV2::inst->registerFrameCallback("WeaponVise",
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
            logNote(g_vr ? "OpenVR ready: reading the grip button" : "OpenVR not found: grip cannot be read");
        }
        logNote("WeaponVise ready (build " WEAPONVISE_BUILD ")");
    }
}

namespace weaponvise::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "WeaponVise",
            .pluginVersion = WEAPONVISE_VERSION,
            .onGameReady = &gameReady,
        };
        return value;
    }

    void onLoad() noexcept
    {
        loadSettings();
        loadTable();
        const auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(registerNatives)) {
            logWarning("Could not register Papyrus functions; the workbench script will not work");
        }
    }
}
