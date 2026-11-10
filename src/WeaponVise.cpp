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
        int weaponHand{ 0 };            // 0 = right, 1 = left
        int forwardAxis{ 1 };           // controller axis that points forward: 0 = X, 1 = Y, 2 = Z
        float viseRadius{ 20.0f };      // weapon hand this close to the vise may clamp
        float viseX{ 33.0f };           // the vise in the workbench model's space
        float viseY{ 26.0f };
        float viseZ{ 68.0f };
        float viseLift{ 9.0f };         // the clamped weapon sits this far above the vise point
        float clampHold{ 0.5f };        // seconds of grip to clamp
        float grabRadius{ 9.0f };       // hand this close to a part grabs it
        float pullDistance{ 8.0f };     // hand moved this far pulls a part off
        float attachRadius{ 9.0f };     // part this close to its place snaps on
        float attachRadiusAny{ 6.0f };  // ...or this close to the weapon when its place is unknown
        float weaponGrabRadius{ 14.0f };
        float unclampHold{ 1.5f };
        float unclampStill{ 6.0f };     // hand must stay within this while holding
        float rackX{ 48.0f };           // loose parts are laid out on the bench top (workbench model space)
        float rackY{ 48.0f };
        float rackZ{ 61.0f };
        float rackStep{ 6.0f };
        int rackPerRow{ 6 };
        bool logGrab{ true };           // log what each grip press near the weapon found
        bool refresh3D{ true };         // reload the weapon's 3D when it does not show a change
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
                else if (key == "bLogGrab") s.logGrab = value != 0.0f;
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
        std::uint32_t weapon{ 0 };
        std::vector<std::uint32_t> mods;
        std::uint64_t revision{ 0 };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> newParts;  // part ref, mod (to frame)
        std::vector<std::uint32_t> inHandParts;                          // spawned for a pull
        std::vector<std::uint32_t> forgottenParts;
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
        auto* parent = root->parent;
        if (looksLikeObject(parent)) {
            root->local = relative(parent->world, world);
        } else {
            root->local = world;
        }
        root->world = world;
        forEachNode(root, [](RE::NiAVObject* object, int depth) {
            if (depth == 0) {
                return;
            }
            auto* parent = object->parent;
            if (looksLikeObject(parent)) {
                object->world = compose(parent->world, object->local);
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
        Clock::time_point gripSince{};
        bool clampSent{ false };

        enum class Mode
        {
            Idle,
            Pull,      // gripping a part on the weapon, not yet pulled off
            Await,     // pulled off; waiting for the script to spawn the part
            Carry,     // carrying a loose part
            Unclamp,   // holding the weapon body
            Done,      // this press already did something; wait for release
        } mode{ Mode::Idle };
        std::uint32_t mod{ 0 };
        std::uint32_t part{ 0 };
        RE::NiPoint3 start{};
        RE::NiTransform offset{};  // part relative to the hand
        Clock::time_point since{};
    };
    std::array<Hand, 2> g_hands_state{};

    struct LoosePart
    {
        std::uint32_t mod{ 0 };
        bool placed{ false };       // home holds where the player left it
        RE::NiTransform home{};
        int carriedBy{ -1 };
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

    bool gripHeld(int index) noexcept
    {
        if (!g_vr) {
            return false;
        }
        const auto device = g_vr->GetTrackedDeviceIndexForControllerRole(index == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
        if (device == vr::k_unTrackedDeviceIndexInvalid) {
            return false;
        }
        vr::VRControllerState_t state{};
        if (!g_vr->GetControllerState(device, &state, sizeof(state))) {
            return false;
        }
        return (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
    }

    bool readHand(int index) noexcept
    {
        auto& hand = g_hands_state[index];
        hand.previousGrip = hand.grip;
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
        hand.grip = gripHeld(index);
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

    RE::NiPoint3 benchPoint(const RE::NiTransform& bench, float x, float y, float z) noexcept
    {
        return compose(bench, pointTransform({ x, y, z })).translate;
    }

    void updateClampGesture() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* bench = lookupRef(g_bench);
        auto& hand = g_hands_state[g_settings.weaponHand == 1 ? 1 : 0];
        RE::NiTransform benchWorld;
        if (!player || !bench || !hand.valid || !benchFrame(bench, benchWorld)) {
            hand.clampSent = false;
            return;
        }
        const auto vise = benchPoint(benchWorld, g_settings.viseX, g_settings.viseY, g_settings.viseZ);
        const bool atVise = distance(hand.point, vise) < g_settings.viseRadius;
        if (!atVise || !player->GetWeaponMagicDrawn()) {
            hand.clampSent = false;
            return;
        }
        if (seconds(g_lastHint) > 20.0f) {
            g_lastHint = Clock::now();
            hud("Тиски: держи грип, чтобы зажать оружие");
        }
        if (!hand.grip) {
            hand.clampSent = false;
            return;
        }
        if (!hand.clampSent && seconds(hand.gripSince) >= g_settings.clampHold) {
            hand.clampSent = true;
            PendingCommand command;
            command.type = kClamp;
            const RE::NiPoint3 at{ vise.x, vise.y, vise.z + g_settings.viseLift };
            command.pose = poseOf(at, headingOf(fingers(hand)));
            {
                std::scoped_lock guard(g_shared.lock);
                g_shared.weaponPose = command.pose;
                g_shared.benchWorld = benchWorld;
                g_shared.benchKnown = true;
            }
            pushCommand(command);
        }
    }

    void startCarry(int index, std::uint32_t partId, const RE::NiTransform& partWorld) noexcept
    {
        auto& hand = g_hands_state[index];
        hand.mode = Hand::Mode::Carry;
        hand.part = partId;
        hand.offset = relative(hand.frame, partWorld);
        auto& part = g_parts[partId];
        part.carriedBy = index;
    }

    // Where a loose part of this mod should go on the weapon, if known.
    bool targetFor(const PartInfo& info, RE::NiAVObject* weapon3D, RE::NiPoint3& out) noexcept
    {
        const auto it = g_slotPlaces.find(info.slot);
        if (it == g_slotPlaces.end()) {
            return false;
        }
        out = compose(weapon3D->world, pointTransform(it->second)).translate;
        return true;
    }

    bool unclampAllowed() noexcept
    {
        std::unordered_set<std::string> filled;
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

    void updateHandAtWeapon(int index, RE::NiAVObject* weapon3D) noexcept
    {
        auto& hand = g_hands_state[index];
        if (!hand.valid) {
            return;
        }
        const bool pressed = hand.grip && !hand.previousGrip;
        const bool released = !hand.grip && hand.previousGrip;

        switch (hand.mode) {
        case Hand::Mode::Idle:
            {
                if (!pressed) {
                    break;
                }
                if (g_settings.logGrab) {
                    float nearestPart = 9999.0f;
                    for (auto& [id, part] : g_parts) {
                        auto* ref = lookupRef(id);
                        auto* root = ref ? ref->Get3D() : nullptr;
                        if (looksLikeObject(root)) {
                            nearestPart = (std::min)(nearestPart, distance(hand.point, centerOf(root)));
                        }
                    }
                    float nearestAnchor = 9999.0f;
                    for (const auto& anchor : g_anchors) {
                        nearestAnchor = (std::min)(nearestAnchor, distance(hand.point, anchor.center));
                    }
                    char text[200]{};
                    std::snprintf(text, sizeof(text), "Grip %s: loose part %.1f, part on weapon %.1f, weapon %.1f (palm-wand %.1f)",
                        index == 0 ? "R" : "L", nearestPart, nearestAnchor, distance(hand.point, centerOf(weapon3D)),
                        distance(hand.point, hand.frame.translate));
                    logNote(text);
                }
                // 1) a loose part
                float best = g_settings.grabRadius;
                std::uint32_t bestPart = 0;
                RE::NiAVObject* best3D = nullptr;
                for (auto& [id, part] : g_parts) {
                    if (part.carriedBy >= 0) {
                        continue;
                    }
                    auto* ref = lookupRef(id);
                    auto* root = ref ? ref->Get3D() : nullptr;
                    if (!looksLikeObject(root)) {
                        continue;
                    }
                    const float d = distance(hand.point, centerOf(root));
                    if (d < best) {
                        best = d;
                        bestPart = id;
                        best3D = root;
                    }
                }
                if (bestPart) {
                    startCarry(index, bestPart, best3D->world);
                    break;
                }
                // 2) a part on the weapon
                best = g_settings.grabRadius;
                const Anchor* bestAnchor = nullptr;
                for (const auto& anchor : g_anchors) {
                    const float d = distance(hand.point, anchor.center);
                    if (d < best) {
                        best = d;
                        bestAnchor = &anchor;
                    }
                }
                if (bestAnchor) {
                    hand.mode = Hand::Mode::Pull;
                    hand.mod = bestAnchor->mod;
                    hand.start = hand.point;
                    hand.offset = relative(hand.frame, looksLikeObject(bestAnchor->node) ? bestAnchor->node->world : weapon3D->world);
                    break;
                }
                // 3) the weapon itself, when no part is close (a near miss should not unclamp)
                bool partClose = false;
                for (const auto& anchor : g_anchors) {
                    partClose = partClose || distance(hand.point, anchor.center) < g_settings.grabRadius * 1.6f;
                }
                if (!partClose && distance(hand.point, centerOf(weapon3D)) < g_settings.weaponGrabRadius) {
                    hand.mode = Hand::Mode::Unclamp;
                    hand.start = hand.point;
                    hand.since = Clock::now();
                }
                break;
            }
        case Hand::Mode::Pull:
            if (!hand.grip) {
                hand.mode = Hand::Mode::Idle;
            } else if (distance(hand.point, hand.start) > g_settings.pullDistance) {
                PendingCommand command;
                command.type = kRemove;
                command.mod = hand.mod;
                const auto at = compose(hand.frame, hand.offset).translate;
                command.pose = poseOf(at, headingOf(fingers(hand)));
                pushCommand(command);
                hand.mode = Hand::Mode::Await;
                hand.since = Clock::now();
            }
            break;
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
                part.home = world;
                part.placed = true;
                const auto* info = partInfo(part.mod);
                RE::NiPoint3 target{};
                const RE::NiPoint3 here = centerOf(root);
                bool snap = false;
                if (info && targetFor(*info, weapon3D, target)) {
                    snap = distance(here, target) < g_settings.attachRadius;
                } else {
                    snap = distance(here, centerOf(weapon3D)) < g_settings.attachRadiusAny + weapon3D->worldBound.fRadius * 0.5f;
                }
                if (snap) {
                    PendingCommand command;
                    command.type = kAttach;
                    command.mod = part.mod;
                    command.part = hand.part;
                    command.pose = poseOf(world.translate, 0.0f);
                    pushCommand(command);
                    part.carriedBy = -2;  // being attached; leave it alone
                    hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
                } else if (!hand.grip) {
                    PendingCommand command;
                    command.type = kDrop;
                    command.part = hand.part;
                    command.pose = poseOf(world.translate, 0.0f);
                    pushCommand(command);
                    part.carriedBy = -1;
                    hand.mode = Hand::Mode::Idle;
                }
                break;
            }
        case Hand::Mode::Unclamp:
            if (!hand.grip || distance(hand.point, hand.start) > g_settings.unclampStill) {
                hand.mode = hand.grip ? Hand::Mode::Done : Hand::Mode::Idle;
            } else if (seconds(hand.since) >= g_settings.unclampHold) {
                if (unclampAllowed()) {
                    PendingCommand command;
                    command.type = kUnclamp;
                    pushCommand(command);
                }
                hand.mode = Hand::Mode::Done;
            }
            break;
        case Hand::Mode::Done:
            if (!hand.grip) {
                hand.mode = Hand::Mode::Idle;
            }
            break;
        }
        (void)released;
    }

    void keepPartsInPlace() noexcept
    {
        for (auto& [id, part] : g_parts) {
            if (part.carriedBy != -1) {
                continue;
            }
            auto* ref = lookupRef(id);
            auto* root = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(root)) {
                continue;
            }
            if (!part.placed) {
                part.home = root->world;
                part.placed = true;
                continue;
            }
            place3D(root, part.home);
        }
    }

    void syncShared() noexcept
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> added;
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
                    g_requiredAtClamp.clear();
                    for (const auto omod : g_shared.mods) {
                        const auto* info = partInfo(omod);
                        if (info && info->required) {
                            g_requiredAtClamp.insert(info->slot);
                        }
                    }
                    g_treeLogged = false;
                }
                g_mods = g_shared.mods;
                g_weapon3D = nullptr;  // re-resolve
                g_revisionTime = Clock::now();
                g_shared.modelOutdated = false;
            }
            added.swap(g_shared.newParts);
            inHand.swap(g_shared.inHandParts);
            forgotten.swap(g_shared.forgottenParts);
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
            for (int index = 0; index < 2; ++index) {
                auto& hand = g_hands_state[index];
                if (hand.mode != Hand::Mode::Await || !hand.grip) {
                    continue;
                }
                if (!looksLikeObject(root)) {
                    // 3D not loaded yet: try again next frame
                    std::scoped_lock guard(g_shared.lock);
                    g_shared.inHandParts.push_back(id);
                    break;
                }
                const auto offset = hand.offset;
                hand.mode = Hand::Mode::Carry;
                hand.part = id;
                hand.offset = offset;
                g_parts[id].carriedBy = index;
                break;
            }
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
            }
            updateClampGesture();
            keepPartsInPlace();
            return;
        }
        auto* weaponRef = lookupRef(g_weapon);
        auto* root = weaponRef ? weaponRef->Get3D() : nullptr;
        if (!looksLikeObject(root)) {
            return;
        }
        if (root != g_weapon3D) {
            g_weapon3D = root;
            if (g_settings.logTree && !g_treeLogged) {
                g_treeLogged = true;
                logTree(root);
            }
            resolveAnchors(root);
            char message[160]{};
            std::snprintf(message, sizeof(message), "Weapon %08X: %zu mods, %zu parts found on the model (by file name: %d)",
                g_weapon, g_mods.size(), g_anchors.size(), g_stemsWork);
            logNote(message);
            for (const auto& anchor : g_anchors) {
                const auto* info = partInfo(anchor.mod);
                std::snprintf(message, sizeof(message), "  part %s -> node '%s'", info ? info->edid.c_str() : "?", nodeName(anchor.node).c_str());
                logNote(message);
            }
        } else {
            // the weapon does not move, but its parts may be re-created
            for (auto& anchor : g_anchors) {
                if (looksLikeObject(anchor.node)) {
                    anchor.center = centerOf(anchor.node);
                }
            }
        }
        // Did the 3D follow the last change? If not, the script reloads it.
        if (g_stemsWork && g_settings.refresh3D && seconds(g_revisionTime) > 0.4f && seconds(g_revisionTime) < 0.5f) {
            std::unordered_set<std::string> expected;
            for (const auto omod : g_mods) {
                const auto* info = partInfo(omod);
                if (info && info->part && !info->stem.empty()) {
                    expected.insert(info->stem);
                }
            }
            const auto present = stemsPresent(root);
            if (expected != present && g_reloadedFor != g_mods) {
                g_reloadedFor = g_mods;
                logNote("Weapon model does not show the new parts yet; asking the script to reload it");
                std::scoped_lock guard(g_shared.lock);
                g_shared.modelOutdated = true;
            }
        }
        updateHandAtWeapon(0, root);
        updateHandAtWeapon(1, root);
        keepPartsInPlace();
    }

    // ------------------------------------------------------------ Papyrus natives

    std::uint32_t idOf(const RE::TESForm* form) noexcept
    {
        return form ? form->GetFormID() : 0;
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

    Mod* CommandMod(std::monostate)
    {
        std::uint32_t id;
        {
            std::scoped_lock guard(g_shared.lock);
            id = g_shared.current.mod;
        }
        return id ? RE::TESForm::GetFormByID<Mod>(id) : nullptr;
    }

    RE::TESObjectREFR* CommandPart(std::monostate)
    {
        std::uint32_t id;
        {
            std::scoped_lock guard(g_shared.lock);
            id = g_shared.current.part;
        }
        return lookupRef(id);
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

    Mod* FallbackFor(std::monostate, Mod* mod)
    {
        const auto* info = mod ? partInfo(mod->GetFormID()) : nullptr;
        return info && info->fallback ? RE::TESForm::GetFormByID<Mod>(info->fallback) : nullptr;
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
        case 4: hud("Оружие в тисках. Тяни детали грипом, подноси новые. Держи грип на оружии, чтобы забрать"); break;
        case 5: hud("Эта деталь сюда не подходит"); break;
        case 6: hud("Оружие снова в руке"); break;
        case 7: hud("Ты отошёл: оружие и детали вернулись в инвентарь"); break;
        default: break;
        }
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
        vm->BindNativeMethod(kScript, "CommandMod", CommandMod, true);
        vm->BindNativeMethod(kScript, "CommandPart", CommandPart, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "RackPose", RackPose, true);
        vm->BindNativeMethod(kScript, "ModelOutdated", ModelOutdated, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        vm->BindNativeMethod(kScript, "FallbackFor", FallbackFor, true);
        vm->BindNativeMethod(kScript, "IsFreeMod", IsFreeMod, true);
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
