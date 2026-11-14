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
#include "RockLink.h"

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
#include <sstream>
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
        float holdUp{ 2.0f };           // magazine from the belt: its bottom sits this far below the palm's middle
        float holdSide{ 0.0f };
        float holdForward{ 0.0f };
        float holdRotX{ 0.0f };
        float holdRotY{ 0.0f };
        float holdRotZ{ 0.0f };
        int   holdFlip{ 0 };            // 1 = the magazine from the belt is upside down in the hand: turn it
        float fingerCurl{ 0.35f };      // fingers around a magazine: 0 = fist, 1 = open
        float freezeSeconds{ 1.6f };
        float slideGrabRadius{ 8.0f };
        float slideGrabAt{ 0.2f };      // the hand takes the slide this part of its length from its rear end..
        float slideGrabHigh{ 0.85f };   // ..and this part of its height from its underside (1 = the very top)
        float rackTravel{ 5.0f };
        float slideLockAt{ 0.8f };
        float handRadius{ 16.0f };
        float looseLifetime{ 90.0f };
        int   maxLoose{ 6 };
        bool  allGuns{ true };         // every gun with a magazine of group 1, not only the 10mm pistol
        bool  rockParts{ true };        // ROCK 1.0 holds the free hand on the magazine and the slide (its hand and fingers)
        bool  rockBeltHandover{ true }; // a magazine from the belt: the grip is shown to ROCK so that it closes the hand on it
        bool  rockPlacesHeld{ true };   // a magazine ROCK holds in the hand is drawn where ROCK has it (one attachment, not two)
        bool  realInHand{ false };      // a magazine out of the well is a real object ROCK holds (always so with bCore2)
        bool  core2{ true };            // 0.12 core: parts move along their way by the palm on them (no grab point), a magazine out of the gun is ROCK's
        float contactMargin{ 2.5f };
        float catchSide{ 2.0f };
        float catchAhead{ 9.5f };       // core2: ..and from this far below the mouth (the gun's collision keeps a real magazine out of the well)
        float pushFactor{ 0.45f };      // core2: an open palm moves the slide only this part of the hand's motion (it slips; the grip takes it fully)        // core2: a magazine is caught by the well only this close to its line (from below, not from the side)
        bool  stopEmptyReload{ true };  // the game's own reload of an empty gun is cut short (its state is set back to idle)
        int   markButton{ 32 };         // the free hand's stick click: writes a numbered mark and the whole state to the log
        bool  traceState{ true };       // every change of what the plugin thinks (where the magazine is, the slide, who holds what)    // the palm this close to a part's box touches it (gun units)
        bool  recordedBolt{ true };     // the bolt / slide moves along the recorded way (a bolt action lifts, then goes back)
        bool  autoRecord{ true };       // a gun whose reload is not recorded yet is left to the game for one reload, which is recorded
        bool  recordAnim{ false };      // the plugin keeps its hands off and writes down how the game's own reload moves every part
        bool  breech{ true };           // revolvers and the break-action shotgun are opened and loaded by hand
        float cylinderSwing{ -80.0f };  // how far a revolver's cylinder swings out (degrees; minus = to the gun's left)
        float breakAngle{ -35.0f };     // how far a break-action gun opens (degrees; minus = barrels down)
        float flickSpeed{ 150.0f };     // the gun jerked this fast (game units a second) closes it
        float flickTurn{ 3.5f };        // ..or turned this fast (a flick of the wrist, radians a second)
        float hingeHold{ 0.0f };      // open barrels swing on their hinge; a pull shut weaker than this (rad/s^2) does not move them
        float breechRadius{ 6.0f };     // rounds this close to the open breech go in
        float chamberReach{ 3.0f };     // ..or this close to a chamber of the zones file (gun units)
        bool  holdPoseOnExit{ false };  // a magazine pulled out of the well turns to the pose of one from the belt
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

    bool realHand() noexcept { return g_settings.realInHand || g_settings.core2; }

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
            { "fSlideGrabRadius", 2, &s.slideGrabRadius }, { "fSlideGrabAt", 2, &s.slideGrabAt }, { "fSlideGrabHigh", 2, &s.slideGrabHigh }, { "fRackTravel", 2, &s.rackTravel }, { "fSlideLockAt", 2, &s.slideLockAt },
            { "fHandRadius", 2, &s.handRadius }, { "fLooseLifetime", 2, &s.looseLifetime }, { "iMaxLoose", 1, &s.maxLoose },
            { "bAllGuns", 0, &s.allGuns }, { "bRockParts", 0, &s.rockParts }, { "bRockBeltHandover", 0, &s.rockBeltHandover },
            { "bHoldPoseOnExit", 0, &s.holdPoseOnExit }, { "bBreech", 0, &s.breech }, { "bRecordAnim", 0, &s.recordAnim }, { "bAutoRecord", 0, &s.autoRecord }, { "bRecordedBolt", 0, &s.recordedBolt }, { "bRealInHand", 0, &s.realInHand }, { "bCore2", 0, &s.core2 }, { "fContactMargin", 2, &s.contactMargin }, { "iMarkButton", 1, &s.markButton }, { "bStopEmptyReload", 0, &s.stopEmptyReload }, { "fCatchSide", 2, &s.catchSide }, { "fCatchAhead", 2, &s.catchAhead }, { "fPushFactor", 2, &s.pushFactor }, { "bTraceState", 0, &s.traceState }, { "fCylinderSwing", 2, &s.cylinderSwing },
            { "fBreakAngle", 2, &s.breakAngle }, { "fFlickSpeed", 2, &s.flickSpeed }, { "fFlickTurn", 2, &s.flickTurn }, { "fHingeHold", 2, &s.hingeHold }, { "fBreechRadius", 2, &s.breechRadius }, { "fChamberReach", 2, &s.chamberReach }, { "bRockPlacesHeld", 0, &s.rockPlacesHeld }, { "bNativeSound", 0, &s.nativeSound }, { "iSoundFlags", 1, &s.soundFlags }, { "bShowRound", 0, &s.showRound }, { "fRoundOpenAt", 2, &s.roundOpenAt }, { "fChamberUp", 2, &s.chamberUp },
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

    // Guns with a detachable magazine and a slide / bolt / charging handle (group 1). A gun is handled when the
    // magazine mod it wears has one of these models; the index is the magazine's kind (MRF_Mags order in the ESP).
    constexpr std::uint32_t kLaserGun = 0x0009983B;
    constexpr std::uint32_t kGatlingLaser = 0x000E27BC;
    constexpr int kLaserCellKind = 19;     // MRF_MagLaserCell
    constexpr int kSyringerKind = 20;      // MRF_MagSyringer
    constexpr int kFusionCoreKind = 21;    // MRF_MagFusionCore
    constexpr std::uint32_t kMinigun = 0x0001F669;
    constexpr int kMinigunKind = 22;       // MRF_MagMinigun: a box of 5mm stands for the drum (the game has no drum model of its own)
    constexpr std::array<const char*, 23> kMagModels{
        "weapons\\10mmpistol\\10mmmag01.nif", "weapons\\10mmpistol\\10mmmaglarge.nif",
        "weapons\\machinegun\\mgmagstraight.nif", "weapons\\machinegun\\mgmagdrum.nif",
        "weapons\\combatshotgun\\combatriflemagmedium.nif", "weapons\\combatshotgun\\combatriflemaglarge.nif",
        "weapons\\combatshotgun\\tgundrummagsmall.nif", "weapons\\combatshotgun\\tgundrummag.nif",
        "weapons\\covertsidearm\\covertsidearmmagazine.nif", "weapons\\covertsidearm\\covertsidearmmagazinelarge.nif",
        "weapons\\handmade\\magazines\\45straightmagshort.nif", "weapons\\handmade\\magazines\\45straightmag.nif",
        "weapons\\handmade\\magazines\\piperifledrummag.nif",
        "weapons\\combatshotgun\\combatshotgunsmallmag.nif", "weapons\\combatshotgun\\combatshotgunmediummag.nif",
        "weapons\\combatshotgun\\drummag.nif",
        "ammo\\308\\308smallmag.nif", "ammo\\308\\308mediummag.nif", "ammo\\308\\308largemag.nif",
        "weapons\\laserweapons\\fusioncell.nif", "weapons\\handmade\\magazines\\syringermag.nif",
        "ammo\\fusioncore\\fusioncore.nif", "ammo\\5mm\\5mmammo.nif",
    };

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
                    if (name == "weapons\\combatshotgun\\combatriflemagsmall.nif") {
                        model = path;
                        return 4;   // the combat rifle's small magazine: handled (and spawned) as its medium one
                    }
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

    // A way drawn in Blender whose first points lie on a circle: that part turns (about `axis` through `pivot`, by
    // `angle`, over the first `arcAt` of the way), the rest of the way is a straight move.
    struct ArcFit
    {
        bool         arc{ false };
        RE::NiPoint3 axis{}, pivot{};
        float        angle{ 0.0f };
        float        arcAt{ 0.0f };
        std::size_t  k{ 0 };
    };

    RE::NiPoint3 rotateAbout(const RE::NiPoint3& v, const RE::NiPoint3& axis, float angle) noexcept
    {
        const float c = std::cos(angle), s = std::sin(angle);
        return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
    }

    ArcFit fitArc(const std::vector<RE::NiPoint3>& p, const std::vector<float>& at) noexcept
    {
        ArcFit fit{};
        for (std::size_t k = p.size() - 1; p.size() >= 3 && k >= 2; --k) {
            const auto a = p[0], b = p[k / 2], c = p[k];
            const auto ab = b - a, ac = c - a;
            const auto nrm = cross(ab, ac);
            const float n2 = dot(nrm, nrm);
            if (n2 < 1e-4f) {
                continue;
            }
            const auto center = a + (cross(nrm, ab) * dot(ac, ac) + cross(ac, nrm) * dot(ab, ab)) * (1.0f / (2.0f * n2));
            const float r = distance(a, center);
            const auto axis = normalized(nrm);
            bool ok = r > 0.3f && r < 60.0f;
            for (std::size_t i = 0; ok && i <= k; ++i) {
                ok = std::fabs(distance(p[i], center) - r) < 0.08f * r + 0.05f && std::fabs(dot(p[i] - a, axis)) < 0.1f;
            }
            if (!ok) {
                continue;
            }
            const auto u = a - center, v = c - center;
            fit.angle = std::atan2(dot(cross(u, v), axis), dot(u, v));
            fit.axis = fit.angle < 0.0f ? axis * -1.0f : axis;
            fit.angle = std::fabs(fit.angle);
            fit.pivot = center;
            fit.arc = fit.angle > 0.05f;
            fit.arcAt = k < at.size() ? at[k] : 1.0f;
            fit.k = k;
            return fit;
        }
        return fit;
    }

    // `rest` (a part, gun root frame) turned about axis/pivot by angle, then moved by lin.
    RE::NiTransform turnedPose(const RE::NiTransform& rest, const RE::NiPoint3& axis, const RE::NiPoint3& pivot, float angle, const RE::NiPoint3& lin) noexcept
    {
        RE::NiTransform out = rest;
        out.translate = rotateAbout(rest.translate - pivot, axis, angle) + pivot + lin;
        setRows(out, rotateAbout(axisOf(rest, 0), axis, angle), rotateAbout(axisOf(rest, 1), axis, angle), rotateAbout(axisOf(rest, 2), axis, angle));
        return out;
    }

    // A point on a polyline at share t (0..1) of its length.
    RE::NiPoint3 alongWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, float t) noexcept
    {
        if (pts.empty()) {
            return RE::NiPoint3{};
        }
        for (std::size_t i = 1; i < pts.size() && i < at.size(); ++i) {
            if (t <= at[i]) {
                const float span = std::max(at[i] - at[i - 1], 1e-5f);
                const float f = std::clamp((t - at[i - 1]) / span, 0.0f, 1.0f);
                return pts[i - 1] + (pts[i] - pts[i - 1]) * f;
            }
        }
        return pts.back();
    }

    // The share of the way (0..1) nearest to q, and how far q is from it.
    float nearestOnWay(const std::vector<RE::NiPoint3>& pts, const std::vector<float>& at, const RE::NiPoint3& q, float* away) noexcept
    {
        float best = 1e9f, bestT = 0.0f;
        for (std::size_t i = 1; i < pts.size() && i < at.size(); ++i) {
            const auto seg = pts[i] - pts[i - 1];
            const float len2 = std::max(dot(seg, seg), 1e-6f);
            const float f = std::clamp(dot(q - pts[i - 1], seg) / len2, 0.0f, 1.0f);
            const float d = distance(q, pts[i - 1] + seg * f);
            if (d < best) {
                best = d;
                bestT = at[i - 1] + (at[i] - at[i - 1]) * f;
            }
        }
        if (away) {
            *away = best;
        }
        return bestT;
    }

    // Guns that open instead of taking a magazine: a revolver's cylinder swings out, a break-action gun folds.
    struct Breech
    {
        int               mech{ 0 };        // 0 none, 1 revolver, 2 break action, 3 one big round at a breech (Fat Man, missile launcher)
        bool              checked{ false };
        bool              known{ false };   // the closed pose is remembered
        RE::NiTransform   rest{};
        std::vector<std::pair<RE::NiAVObject*, RE::NiTransform>> kids;   // the nodes below, as they stand closed
        bool              open{ false };
        float             t{ 0.0f };        // 0 closed .. 1 open
        std::uint32_t     live{ 0 };        // rounds in it
        std::uint32_t     spent{ 0 };       // fired cases in it
        std::uint32_t     held{ 0 };        // rounds in the free hand
        std::uint32_t     lastAmmo{ 0xFFFFFFFF };
        Clock::time_point toggled{};
        Clock::time_point opened{};
        Clock::time_point freezeUntil{};
        RE::NiPoint3      prevAt{};
        bool              prevKnown{ false };
        RE::NiPoint3      velocity{};
        RE::NiPoint3      lastVelocity{};
        RE::NiPoint3      accel{};          // the gun's acceleration, smoothed (tracking noise would shake the barrels)
        Clock::time_point upSince{};        // the muzzle has pointed up since
        float             omega{ 0.0f };    // the open barrels' swing on the hinge (rad/s)
        bool              driving{ false };   // the free hand turns the opening part about its hinge
        float             driveAngle0{ 0.0f };
        float             driveT0{ 0.0f };
        bool              pushing{ false };   // the open palm is against the opening part
        float             pushAngle{ 0.0f };
        RE::NiTransform   restInRoot{};       // the opening part closed, in the gun root's frame (zones are drawn there)
        RE::NiTransform   drawnLocal{};       // ..as drawn last frame (opened): ROCK's collision follows it
        bool              drawn{ false };
        Clock::time_point unlatchedAt{};             // the free hand last pressed the latch (zones file: box latch)
        RE::NiPoint3      prevForward{}, prevSide{};   // the gun's turn last frame (a flick of the wrist closes it)
        bool              turnKnown{ false };
        float             ejectPushed{ 0.0f };          // how far the hand has pushed the ejector rod
        RE::NiPoint3      ejectPrev{};
        bool              ejectTouch{ false };
        float             lever{ 0.0f };                // zones file "route lever" (the Fat Man's handle): 0 home .. 1 pulled back
        bool              leverHeld{ false };
        bool              leverRestKnown{ false };
        bool              leverDrawn{ false };
        bool              leverBlocked{ false };        // the handle is out: the gun does not fire
        RE::NiTransform   leverRestLocal{}, leverRestInRoot{};
    };

    struct State
    {
        Breech             breech{};
        bool               recordTold{ false };
        RE::TESObjectWEAP* weapon{ nullptr };
        bool               managed{ false };
        std::uint32_t      lastAmmo{ 0xFFFFFFFF };
        std::uint32_t      lastGunState{ 0xFFFFFFFF };
        bool               chamber{ false };
        bool               hasBolt{ true };               // the model has a WeaponBolt node
        bool               cellFed{ false };
        bool               hasZonesFile{ false };         // drawn in Blender: no recording of the game's reload is needed              // an energy gun: the cell feeds it, nothing to rack
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
        // ROCK
        RE::NiPoint3       slidePointInBolt{};             // where the hand takes the slide, in the bolt node's frame
        bool               slidePointKnown{ false };
        Clock::time_point  lastDrive{};                    // ROCK was last told to move a part
        Clock::time_point  handoverAt{};                   // a magazine from the belt: when the grip is shown to ROCK
        bool               handoverDue{ false };
        bool               rockWasOnBolt{ false };
        bool               boltAction{ false };
        std::int32_t       handToken{ 0 };                 // a real magazine on its way into the free hand
        bool               handTokenDropped{ false };      // ..let go before it came: it just falls
        bool               handStash{ false };             // ..let go at the belt / over the shoulder: it goes back            // the recording turns the bolt up first: worked by hand after every shot
        RE::NiPoint3       handInBolt{};                   // the hand against the bolt when taken (recorded path)
        RE::NiPoint3       sideInNode{};                   // the magazine's middle line off the node's own Z line, in the node's frame
        RE::NiPoint3       sideInGun{};                    // ..the same in the gun's frame
        RE::NiPoint3       prevDrawnAt{};                  // last frame: the magazine in the hand and its parent node,
        RE::NiPoint3       prevParentAt{};                 // to tell ROCK where they will be when it uses the pose
        bool               prevDriveKnown{ false };
        Clock::time_point  forceTargets{};                 // until then ROCK is told what may be taken even with the hand closed
        bool               magBoxUsed{ false };            // the magazine's size was taken from ROCK's box
        bool               boltBoxUsed{ false };
        Clock::time_point  seatedAt{};
        bool               rockWasOnMagazine{ false };
        // core2: the parts as ROCK measured them at rest (gun root frame), the palm touching them
        bool               slideBoxKnown{ false };
        bool               zoneSlide{ false };   // the slide box came from the zones file: ROCK's boxes do not replace it
        bool               zoneWell{ false };    // ..and the well's mouth and travel
        RE::NiPoint3       slideLo{}, slideHi{};
        std::vector<RE::NiPoint3> route;                   // zones file: the slide / handle's way (a polyline, gun root frame), first point at rest
        std::vector<float> routeAt;                        // ..each point's share of the way's length (0..1)
        std::unordered_map<std::string, std::pair<RE::NiPoint3, RE::NiPoint3>> zoneBoxes;   // zones file: "box NAME lo .. hi .."
        std::vector<RE::NiPoint3> chambers;                // zones file: "point chamberN x y z" (the closed gun)
        std::unordered_map<std::string, std::vector<RE::NiPoint3>> zoneRoutes;   // zones file: "route NAME x y z .."
        ArcFit             slideFit{};                     // the slide's way: a turn first (a bolt handle lifted), then straight
        ArcFit             zoneHinge{};                    // zones file "route cylinder/barrels/breech": the opening part's turn
        std::string        magNodeName{ "WeaponMagazine" };   // the node that carries the magazine (pipe guns: the one above P-Mag)
        bool               magNodeChecked{ false };
        bool               singleShot{ false };            // zones file "socket nuke/missile": one big round goes in at the breech
        RE::NiPoint3       breechMouth{};                  // ..where it goes in (gun root frame)
        float              breechReach{ 6.0f };
        bool               leverKnown{ false };            // zones file "route lever": the handle (WeaponBolt) that opens the breech
        std::vector<RE::NiPoint3> leverPts;
        std::vector<float> leverAt;
        ArcFit             leverFit{};                     // ..turned first, then pulled straight
        bool               crankKnown{ false };            // zones file "route crank": a full circle (the laser musket)
        RE::NiPoint3       crankCenter{}, crankAxis{}, crankStart{};
        bool               crankRestKnown{ false };
        RE::NiTransform    crankRest{};                    // the crank (WeaponMagazine) in the gun root frame at rest
        bool               crankHeld{ false };
        float              crankAngle{ 0.0f }, crankPrev{ 0.0f }, crankTurned{ 0.0f };
        bool               magBoxKnown{ false };
        RE::NiPoint3       magLo{}, magHi{};
        bool               pushing{ false };               // the open palm moves the slide / bolt
        RE::NiPoint3       prevHandLocal{};                // the controller in the gun's frame last frame
        bool               prevHandKnown{ false };
        bool               resting{ false };               // let go half in: the magazine stays in the well where it is
        bool               handSpare{ false };             // the real magazine on its way is a spare from the belt
        std::uint32_t      handRef{ 0 };                   // the real magazine ROCK is to hold in the free hand
        Clock::time_point  handRefAt{};
        Clock::time_point  grabTriedAt{};
        int                grabTries{ 0 };
        bool               grabDone{ false };
        float              exitAt{ 0.0f };
        Clock::time_point  stopReloadUntil{};             // the game's reload of an empty gun is being cut short
        bool               zonesEarly{ false };            // ..read once before ROCK's boxes came
        bool               zonesChecked{ false };          // ManualReloadF_zones/<weapon>.txt read (or written) for this gun
        bool               zonesFromFile{ false };
        float              zoneCatchSide{ -1.0f };
        float              zoneCatchAhead{ -1.0f };                 // core2: out this far along the well and the magazine has left it
    };
    State g_state;
    rocklink::Result g_rock;   // what ROCK answered last frame

    bool rockOn() noexcept { return g_settings.rockParts && g_rock.active; }

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

    // ------------------------------------------------------------ core2: the palm on a part

    bool inBox(const RE::NiPoint3& lo, const RE::NiPoint3& hi, const RE::NiPoint3& p, float margin) noexcept
    {
        return p.x >= lo.x - margin && p.x <= hi.x + margin && p.y >= lo.y - margin && p.y <= hi.y + margin && p.z >= lo.z - margin &&
               p.z <= hi.z + margin;
    }

    RE::NiPoint3 inGun(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept
    {
        return relative(root->world, pointTransform(world)).translate;
    }

    // The magazine is touched: the palm is in its box as it stands now (moved out along the well by s).
    bool palmOnMagazine(const RE::NiAVObject* root, const RE::NiPoint3& palm) noexcept
    {
        const auto& st = g_state;
        if (!st.magBoxKnown) {
            return distance(palm, grabPoint(root)) < g_settings.magGrabRadius;
        }
        return inBox(st.magLo, st.magHi, inGun(root, palm) - st.axis * st.s, g_settings.contactMargin);
    }

    // ------------------------------------------------------------ core2: the zones, editable (Blender) per gun
    // Data/F4SE/Plugins/ManualReloadF_zones/<weapon>.txt, in the gun root's frame (game units):
    //   magazine lo x y z hi x y z     the box the palm touches to take / push the magazine (seated)
    //   slide lo x y z hi x y z        ..the slide / bolt (at rest)
    //   well side S ahead A            the well catches a magazine this close to its line, from A below the mouth
    //   well mouth x y z axis x y z    (written for reference: where the magazine's top leaves the well, the way out)
    //   slide route x y z x y z ...   the slide / charging handle's way (first point at rest), instead of the recorded one
    // Written from what ROCK measured when there is no file; read instead of it when there is.
    // At equip: the opening part's turn drawn in Blender (route cylinder / barrels / breech), so a gun opens by
    // hand even with no recording of the game's reload.
    void loadZoneRoutes(std::uint32_t weapon) noexcept
    {
        auto& st = g_state;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_zones/%08X.txt", weapon);
        std::ifstream in(path);
        st.hasZonesFile = static_cast<bool>(in);
        std::vector<std::string> lines;
        std::unordered_map<std::string, RE::NiPoint3> boxMiddle;
        for (std::string line; in && std::getline(in, line);) {
            std::istringstream words(line);
            std::string head, name, word;
            words >> head >> name;
            RE::NiPoint3 lo{}, hi{};
            if (head == "box" && words >> word >> lo.x >> lo.y >> lo.z >> word >> hi.x >> hi.y >> hi.z) {
                boxMiddle[name] = (lo + hi) * 0.5f;
            } else if (head == "socket" && (name == "nuke" || name == "missile") && words >> word >> lo.x >> lo.y >> lo.z) {
                st.singleShot = true;
                st.breechMouth = lo;
                float side = 0.0f;
                while (words >> word) {
                    if (word == "side" && words >> side) {
                        st.breechReach = std::clamp(side, 3.0f, 10.0f);
                    }
                }
                note("ZONES: one %s goes in at (%.1f %.1f %.1f), reach %.1f", name.c_str(), lo.x, lo.y, lo.z, st.breechReach);
            }
            lines.push_back(line);
        }
        for (const auto& line : lines) {
            std::istringstream words(line);
            std::string head, name;
            words >> head >> name;
            if (head != "route" || (name != "cylinder" && name != "barrels" && name != "breech" && name != "crank" && name != "lever")) {
                continue;
            }
            std::vector<RE::NiPoint3> pts;
            float x, y, z;
            while (words >> x >> y >> z) {
                pts.push_back(RE::NiPoint3{ x, y, z });
            }
            if (name == "crank") {
                if (pts.size() >= 5) {
                    const std::size_t n = distance(pts.front(), pts.back()) < 0.5f ? pts.size() - 1 : pts.size();
                    RE::NiPoint3 c{};
                    for (std::size_t i = 0; i < n; ++i) c = c + pts[i];
                    c = c * (1.0f / static_cast<float>(n));
                    const auto axis = normalized(cross(pts[0] - c, pts[n / 4] - c));
                    if (dot(axis, axis) > 0.5f) {
                        st.crankKnown = true;
                        st.crankCenter = c;
                        st.crankAxis = axis;
                        st.crankStart = pts[0];
                        note("ZONES: crank turns about (%.2f %.2f %.2f) at (%.1f %.1f %.1f), handle radius %.1f", axis.x, axis.y, axis.z, c.x, c.y, c.z,
                            distance(pts[0], c));
                    }
                }
                continue;
            }
            // the way starts where the part stands closed: a route drawn from the open end is turned round
            if (const auto box = boxMiddle.find(name); box != boxMiddle.end() && pts.size() >= 2 &&
                                                       distance(pts.back(), box->second) < distance(pts.front(), box->second)) {
                std::reverse(pts.begin(), pts.end());
                note("ZONES: route %s was drawn from its open end: turned round", name.c_str());
            }
            std::vector<float> at(pts.size(), 0.0f);
            float total = 0.0f;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                total += distance(pts[i], pts[i - 1]);
                at[i] = total;
            }
            for (auto& a : at) {
                a = total > 0.0f ? a / total : 0.0f;
            }
            const auto fit = fitArc(pts, at);
            if (name == "lever") {
                if (pts.size() >= 2 && total > 0.5f) {
                    st.leverKnown = true;
                    st.leverPts = pts;
                    st.leverAt = at;
                    st.leverFit = fit;
                    note("ZONES: the handle turns %.0f deg (first %.0f%% of its way), then goes straight; way %.1f long", fit.arc ? fit.angle * 57.29578f : 0.0f,
                        fit.arc ? fit.arcAt * 100.0f : 0.0f, total);
                }
                continue;
            }
            if (fit.arc && fit.angle > 0.2f) {
                st.zoneHinge = fit;
                note("ZONES: %s opens %.0f deg about (%.2f %.2f %.2f) at (%.1f %.1f %.1f) (route from Blender)", name.c_str(), fit.angle * 57.29578f, fit.axis.x,
                    fit.axis.y, fit.axis.z, fit.pivot.x, fit.pivot.y, fit.pivot.z);
            } else {
                note("ZONES: route %s is not a turn (%zu points): ignored", name.c_str(), pts.size());
            }
        }
    }

    void zonesFor(std::uint32_t weapon, bool mayWrite = true) noexcept
    {
        auto& st = g_state;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_zones/%08X.txt", weapon);
        std::ifstream in(path);
        if (!in && !mayWrite) {
            note("ZONES: no %s (the gun's own distances are used)", path);
            return;
        }
        if (!in) {
            std::error_code ignored;
            std::filesystem::create_directories("Data/F4SE/Plugins/ManualReloadF_zones", ignored);
            std::ofstream out(path);
            if (!out) {
                return;
            }
            const float scale = st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f;
            const auto mouth = st.magRest.translate + st.axis * (st.length - st.topAt * scale);   // the magazine's top as it leaves the well
            char text[256];
            out << "; ManualReloadF zones of weapon " << std::hex << weapon << std::dec << ", gun root frame (game units). Edit, or move the cubes in Blender.\n";
            std::snprintf(text, sizeof(text), "magazine lo %.3f %.3f %.3f hi %.3f %.3f %.3f\n", st.magLo.x, st.magLo.y, st.magLo.z, st.magHi.x, st.magHi.y, st.magHi.z);
            out << text;
            if (st.slideBoxKnown) {
                std::snprintf(text, sizeof(text), "slide lo %.3f %.3f %.3f hi %.3f %.3f %.3f\n", st.slideLo.x, st.slideLo.y, st.slideLo.z, st.slideHi.x, st.slideHi.y,
                    st.slideHi.z);
                out << text;
            }
            std::snprintf(text, sizeof(text), "well side %.3f ahead %.3f\n", g_settings.catchSide, g_settings.catchAhead);
            out << text;
            std::snprintf(text, sizeof(text), "well mouth %.3f %.3f %.3f axis %.4f %.4f %.4f\n", mouth.x, mouth.y, mouth.z, st.axis.x, st.axis.y, st.axis.z);
            out << text;
            note("ZONES: %s written from ROCK's boxes (edit it, or in Blender)", path);
            return;
        }
        std::string line;
        int used = 0;
        st.chambers.clear();
        while (std::getline(in, line)) {
            std::vector<std::string> words;
            std::string word;
            for (const char c : line) {
                if (c == ' ' || c == '\t' || c == '\r') {
                    if (!word.empty()) words.push_back(word), word.clear();
                } else {
                    word += c;
                }
            }
            if (!word.empty()) words.push_back(word);
            if (words.empty() || words[0][0] == ';') {
                continue;
            }
            auto num = [&](std::size_t i) { return i < words.size() ? std::strtof(words[i].c_str(), nullptr) : 0.0f; };
            if ((words[0] == "magazine" || words[0] == "slide") && words.size() >= 9 && words[1] == "lo" && words[5] == "hi") {
                const RE::NiPoint3 lo{ std::min(num(2), num(6)), std::min(num(3), num(7)), std::min(num(4), num(8)) };
                const RE::NiPoint3 hi{ std::max(num(2), num(6)), std::max(num(3), num(7)), std::max(num(4), num(8)) };
                if (words[0] == "magazine") {
                    st.magLo = lo, st.magHi = hi, st.magBoxKnown = true;
                } else {
                    st.slideLo = lo, st.slideHi = hi, st.slideBoxKnown = true, st.zoneSlide = true;
                    if (st.slideCenterKnown) {
                        st.slideCenter = (lo + hi) * 0.5f;
                    }
                }
                ++used;
            } else if (words[0] == "well" && words.size() >= 9 && words[1] == "mouth" && words[5] == "axis") {
                const RE::NiPoint3 mouth{ num(2), num(3), num(4) };
                const RE::NiPoint3 axis{ num(6), num(7), num(8) };
                if (dot(axis, axis) > 0.25f) {
                    st.axis = normalized(axis);
                    const float scale = st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f;
                    st.length = std::max(2.0f, dot(mouth - st.magRest.translate, st.axis) + st.topAt * scale);
                    st.zoneWell = true;
                    // a cell, a core or a drum sits whole in its box: it is out once it has moved its own size along the way
                    // (also when the mouth was put in the middle of the magazine, which leaves almost no way out)
                    const float straight = std::max({ std::fabs(st.axis.x), std::fabs(st.axis.y), std::fabs(st.axis.z) });
                    const float raw = dot(mouth - st.magRest.translate, st.axis) + st.topAt * scale;
                    if (st.magBoxKnown && ((st.cellFed && straight > 0.95f) || raw < 3.0f)) {
                        const auto size = st.magHi - st.magLo;
                        st.length = std::fabs(size.x * st.axis.x) + std::fabs(size.y * st.axis.y) + std::fabs(size.z * st.axis.z) + 0.5f;
                        note("ZONES: the magazine's own size along the way out: it leaves after %.1f", st.length);
                    }
                    note("ZONES: the well's mouth at (%.1f %.1f %.1f), way out (%.2f %.2f %.2f): the magazine leaves it after %.1f", mouth.x, mouth.y, mouth.z,
                        st.axis.x, st.axis.y, st.axis.z, st.length);
                    ++used;
                }
            } else if (words[0] == "box" && words.size() >= 10 && words[2] == "lo" && words[6] == "hi") {
                const RE::NiPoint3 lo{ std::min(num(3), num(7)), std::min(num(4), num(8)), std::min(num(5), num(9)) };
                const RE::NiPoint3 hi{ std::max(num(3), num(7)), std::max(num(4), num(8)), std::max(num(5), num(9)) };
                st.zoneBoxes[words[1]] = { lo, hi };
                note("ZONES: box %s (%.1f %.1f %.1f)..(%.1f %.1f %.1f)", words[1].c_str(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                ++used;
            } else if (words[0] == "route" && words.size() >= 8) {
                auto& pts = st.zoneRoutes[words[1]];
                pts.clear();
                for (std::size_t i = 2; i + 2 < words.size(); i += 3) {
                    pts.push_back(RE::NiPoint3{ num(i), num(i + 1), num(i + 2) });
                }
                ++used;
            } else if (words[0] == "point" && words.size() >= 5 && words[1].rfind("chamber", 0) == 0) {
                st.chambers.push_back(RE::NiPoint3{ num(2), num(3), num(4) });
                ++used;
            } else if (words[0] == "slide" && words.size() >= 8 && words[1] == "route") {
                st.route.clear();
                for (std::size_t i = 2; i + 2 < words.size(); i += 3) {
                    st.route.push_back(RE::NiPoint3{ num(i), num(i + 1), num(i + 2) });
                }
                st.routeAt.assign(st.route.size(), 0.0f);
                float total = 0.0f;
                for (std::size_t i = 1; i < st.route.size(); ++i) {
                    total += distance(st.route[i], st.route[i - 1]);
                    st.routeAt[i] = total;
                }
                if (st.route.size() >= 2 && total > 0.2f) {
                    for (auto& at : st.routeAt) {
                        at /= total;
                    }
                    st.slideFit = fitArc(st.route, st.routeAt);
                    if (st.slideFit.arc && st.slideFit.angle > 0.35f) {
                        st.boltAction = true;
                    }
                    note("ZONES: the slide's way from the file: %zu points, %.1f long; turns %.0f deg about (%.2f %.2f %.2f) at (%.1f %.1f %.1f) over the first %.0f%%",
                        st.route.size(), total, st.slideFit.angle * 57.29578f, st.slideFit.axis.x, st.slideFit.axis.y, st.slideFit.axis.z, st.slideFit.pivot.x,
                        st.slideFit.pivot.y, st.slideFit.pivot.z, st.slideFit.arcAt * 100.0f);
                    ++used;
                } else {
                    st.route.clear();
                    st.routeAt.clear();
                }
            } else if (words[0] == "well" && words.size() >= 5 && words[1] == "side" && words[3] == "ahead") {
                st.zoneCatchSide = num(2);
                st.zoneCatchAhead = num(4);
                ++used;
            }
        }
        st.zonesFromFile = used > 0;
        note("ZONES: %s read, %d zones (magazine (%.1f %.1f %.1f)..(%.1f %.1f %.1f), slide (%.1f %.1f %.1f)..(%.1f %.1f %.1f), well side %.1f ahead %.1f)", path, used,
            st.magLo.x, st.magLo.y, st.magLo.z, st.magHi.x, st.magHi.y, st.magHi.z, st.slideLo.x, st.slideLo.y, st.slideLo.z, st.slideHi.x, st.slideHi.y,
            st.slideHi.z, st.zoneCatchSide, st.zoneCatchAhead);
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
            auto& st = g_state;
            if (st.handToken != 0 && mag.token == st.handToken) {
                st.handToken = 0;
                if (st.handTokenDropped) {
                    st.handSpare = false;
                    if (st.handStash) {
                        deleteMagazine(mag.ref);
                        hud("Магазин убран, патроны вернулись в запас");
                    }
                } else if (st.place == kHand || st.handSpare) {
                    // the real one is in the palm: the gun's own magazine is hidden, ROCK closes the hand on the real one
                    if (st.place == kHand) {
                        st.place = kGone;
                        st.rounds = 0;
                    }
                    st.handSpare = false;
                    st.velocityValid = false;
                    griphook::setOwned(false);
                    note("HAND: magazine %08X is now held as a real object (ROCK)", mag.ref);
                    if (g_settings.core2) {
                        st.handRef = mag.ref;
                        st.handRefAt = Clock::now();
                        st.grabTriedAt = st.handRefAt;
                        st.grabTries = 1;
                        st.grabDone = false;
                        const auto& palm = g_hands[freeIndex()].palm;
                        const float point[3]{ palm.x, palm.y, palm.z };
                        auto* ref = lookupRef(mag.ref);
                        auto* mag3D = ref ? ref->Get3D() : nullptr;
                        if (const auto held = rocklink::heldBy(freeIndex()); held != 0 && held != mag.ref) {
                            rocklink::releaseHeld(freeIndex(), held);
                        }
                        if (looksLikeObject(mag3D)) {
                            const auto c = mag3D->worldBound.center;
                            const float middle[3]{ c.x, c.y, c.z };
                            rocklink::forceGrab(freeIndex(), mag.ref, middle);
                        } else {
                            rocklink::forceGrab(freeIndex(), mag.ref, point);
                        }
                    }
                } else {
                    deleteMagazine(mag.ref);   // it was already pushed into the well
                }
            }
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
        st.resting = false;
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
        st.seatedAt = Clock::now();
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
        if (!g_settings.core2) {
            shifted.translate -= grabPoint(root) - hand.palm;   // where the magazine would be if the palm were already on it
        }   // core2: taken wherever the palm is on it, nothing jumps
        st.magInWand = relative(hand.wand, shifted);
        st.wristInMag = relative(shifted, hand.wrist);
        st.driveS0 = st.s;
        st.driveAlong0 = alongRail(root, shifted.translate);
        st.place = kRailHand;
        st.autoHold = false;
        st.resting = false;
        st.exitAt = 0.0f;
        st.blendSeconds = 0.0f;
        buzz(freeIndex(), 1200);
        note("TAKE: the free hand has the magazine, %.1f out of the well", st.s);
    }

    // The well's way out as the magazine itself carries it (its own frame at rest), turned with a loose magazine:
    // a cell or a drum that leaves sideways is lined up when this points along the well, not its own Z.
    RE::NiPoint3 wayOutOf(const RE::NiTransform& node) noexcept
    {
        const auto& st = g_state;
        const RE::NiPoint3 w{ dot(axisOf(st.magRest, 0), st.axis), dot(axisOf(st.magRest, 1), st.axis), dot(axisOf(st.magRest, 2), st.axis) };
        return axisOf(node, 0) * w.x + axisOf(node, 1) * w.y + axisOf(node, 2) * w.z;
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
        pose.translate = hand.palm - axisOf(pose, 2) * ((g_state.bottomAt + g_settings.holdUp) * scale) +
                         axisOf(pose, 0) * ((g_settings.holdSide - g_state.sideInNode.x) * scale) - axisOf(pose, 1) * (g_state.sideInNode.y * scale) +
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
                if (!realHand()) {
                    adopt = id;
                    adoptPose = mag3D->world;
                } else {
                    // a real magazine in the hand: the mouth of the well takes its top, then it moves only along the well
                    auto node = compose(mag3D->world, inverse(st.looseInNode));
                    node.scale = gunScale(root) * st.magRest.scale;
                    float side = 0.0f;
                    const float top = alongRail(root, topOf(node), &side) + st.topAt * st.magRest.scale;
                    const float tilt = -dot(wayOutOf(node), axisWorld(root));   // -1: lined up with the well
                    const bool upright = tilt < (g_settings.core2 ? -0.7f : -0.5f);
                    const float mouth = st.length;
                    const float sideMax = g_settings.core2 ? (st.zoneCatchSide > 0.0f ? st.zoneCatchSide : g_settings.catchSide) : g_settings.wellSide;
                    // a cell, a core or a drum is taken anywhere along its short way in, not only at the mouth
                    const float below = st.cellFed ? mouth + 1.5f : (g_settings.core2 ? 1.5f : 3.0f);
                    const float ahead = g_settings.core2 ? (st.zoneCatchAhead > 0.0f ? st.zoneCatchAhead : g_settings.catchAhead) : g_settings.captureAhead;
                    if (upright && side <= sideMax && top <= mouth + ahead && top >= mouth - below) {
                        st.rounds = loose.rounds;
                        st.place = kRailHand;
                        st.s = std::clamp(top, 0.0f, mouth + ahead);
                        st.exitAt = st.s + 1.5f;
                        st.driveS0 = st.s;
                        st.driveAlong0 = alongRail(root, node.translate);
                        st.magInWand = relative(free.wand, node);
                        st.wristInMag = relative(node, free.wrist);
                        st.mustLeave = false;
                        st.velocityValid = false;
                        st.dropActive = false;
                        startBlend(node, 0.12f);
                        gone.push_back(id);
                        rocklink::releaseHeld(freeIndex(), id);
                        if (st.handRef == id) {
                            st.handRef = 0;
                        }
                        PendingCommand command;
                        command.type = kDelete;
                        command.ref = id;
                        pushCommand(command);
                        griphook::setOwned(true);
                        buzz(freeIndex(), 900);
                        note("CATCH: the well has the real magazine %08X (%u rounds), %.1f out, %.1f off the line", id, st.rounds, st.s, side);
                        continue;
                    }
                    static Clock::time_point missTold{};
                    if (top < mouth + ahead + 6.0f && side < sideMax + 6.0f && now - missTold > std::chrono::milliseconds(400)) {
                        missTold = now;   // near the well and not taken: why
                        note("NEAR WELL: magazine %08X top %.1f below the mouth (catch from %.1f), %.1f off the line (max %.1f), upright %.2f (needs > %.2f)",
                            id, top - mouth, ahead, side, sideMax, -tilt, g_settings.core2 ? 0.7f : 0.5f);
                    }
                }
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

    // ------------------------------------------------------------ the game's reload, written down

    // While bRecordAnim is on the plugin leaves the gun alone; each time the game plays its own reload,
    // every node of the gun is taken against the gun's root each frame, and what moved is worked out:
    // a turn about a hinge (axis, pivot, angle) or a slide along a line (direction, travel), with when it
    // happens, and nodes that show up or vanish (shells, a speedloader).
    struct AnimRecorder
    {
        bool                                   active{ false };
        Clock::time_point                      start{};
        Clock::time_point                      lastSeen{};
        std::uint32_t                          weapon{ 0 };
        std::vector<std::string>               names;
        std::vector<RE::NiAVObject*>           nodes;
        std::vector<float>                     times;
        std::vector<std::vector<RE::NiTransform>> frames;   // [frame][node], against the gun root
        std::vector<std::vector<std::uint8_t>> culled;
        int                                    takes{ 0 };
    };
    AnimRecorder g_rec;
    std::unordered_map<std::uint32_t, bool> g_recorded;   // weapon -> a recording of its reload is on disk
    bool g_playStale{ false };                              // a new recording: read it again

    bool hasRecording(std::uint32_t weapon) noexcept
    {
        const auto known = g_recorded.find(weapon);
        if (known != g_recorded.end()) {
            return known->second;
        }
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_anim/%08X.txt", weapon);
        // a take under a second (cut short) does not count: the gun is recorded again
        bool there = false;
        if (std::ifstream file(path); file) {
            std::string first;
            std::getline(file, first);
            const auto over = first.find(" over ");
            there = over != std::string::npos && std::strtof(first.c_str() + over + 6, nullptr) >= 1.0f;
        }
        g_recorded[weapon] = there;
        return there;
    }

    struct M3 { float m[3][3]; };   // columns = the node's axes in the root's frame

    M3 colsOf(const RE::NiTransform& t) noexcept
    {
        M3 r{};
        for (int c = 0; c < 3; ++c) {
            const auto a = axisOf(t, c);
            r.m[0][c] = a.x; r.m[1][c] = a.y; r.m[2][c] = a.z;
        }
        return r;
    }
    M3 mulAB(const M3& a, const M3& b) noexcept
    {
        M3 r{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }
    M3 transposed(const M3& a) noexcept
    {
        M3 r{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
        return r;
    }
    RE::NiPoint3 mulV(const M3& a, const RE::NiPoint3& v) noexcept
    {
        return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
    }
    float angleOf(const M3& d) noexcept
    {
        const float c = (d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0f) * 0.5f;
        return std::acos(std::clamp(c, -1.0f, 1.0f));
    }
    float det3(const M3& a) noexcept
    {
        return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) - a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
               a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
    }
    bool solve3(const M3& a, const RE::NiPoint3& b, RE::NiPoint3& x) noexcept
    {
        const float d = det3(a);
        if (std::fabs(d) < 1e-6f) return false;
        float out[3]{};
        const float bv[3]{ b.x, b.y, b.z };
        for (int c = 0; c < 3; ++c) {
            M3 m = a;
            for (int r = 0; r < 3; ++r) m.m[r][c] = bv[r];
            out[c] = det3(m) / d;
        }
        x = { out[0], out[1], out[2] };
        return true;
    }

    void finishRecording() noexcept
    {
        auto& r = g_rec;
        r.active = false;
        const auto frameCount = r.frames.size();
        if (frameCount < 10) {
            note("RECORD: too short (%zu frames), dropped", frameCount);
            return;
        }
        ++r.takes;
        char folder[] = "Data/F4SE/Plugins/ManualReloadF_anim";
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        char path[160];
        std::snprintf(path, sizeof(path), "%s/%08X.txt", folder, r.weapon);
        std::ofstream out(path, std::ios::trunc);
        char line[512];
        std::snprintf(line, sizeof(line), "; ManualReloadF: the game's reload of weapon %08X, %zu frames over %.2f s, %zu nodes\n", r.weapon, frameCount,
            r.times.back(), r.names.size());
        out << line;
        note("RECORD: weapon %08X, %zu frames over %.2f s -> %s", r.weapon, frameCount, r.times.back(), path);
        std::vector<std::size_t> moving;
        for (std::size_t n = 0; n < r.names.size(); ++n) {
            const auto& rest = r.frames[0][n];
            const auto restCols = colsOf(rest);
            float bestAngle = 0.0f, bestMove = 0.0f;
            std::size_t angleAt = 0, moveAt = 0;
            for (std::size_t f = 1; f < frameCount; ++f) {
                const auto& now = r.frames[f][n];
                const float a = angleOf(mulAB(colsOf(now), transposed(restCols)));
                const auto d = now.translate - rest.translate;
                const float m = std::sqrt(dot(d, d));
                if (a > bestAngle) { bestAngle = a; angleAt = f; }
                if (m > bestMove) { bestMove = m; moveAt = f; }
            }
            bool showsUp = false, vanishes = false;
            for (std::size_t f = 1; f < frameCount; ++f) {
                if (r.culled[f][n] != r.culled[f - 1][n]) {
                    (r.culled[f][n] ? vanishes : showsUp) = true;
                    std::snprintf(line, sizeof(line), "event %.3f %s %s\n", r.times[f], r.culled[f][n] ? "hide" : "show", r.names[n].c_str());
                    out << line;
                }
            }
            if (bestAngle < 0.05f && bestMove < 0.3f) {
                continue;
            }
            // the frames it is away from rest (over a fifth of its peak)
            float from = -1.0f, to = -1.0f;
            for (std::size_t f = 1; f < frameCount; ++f) {
                const auto& now = r.frames[f][n];
                const float a = angleOf(mulAB(colsOf(now), transposed(restCols)));
                const auto d = now.translate - rest.translate;
                const float m = std::sqrt(dot(d, d));
                if ((bestAngle >= 0.05f && a > bestAngle * 0.2f) || (bestAngle < 0.05f && m > bestMove * 0.2f)) {
                    if (from < 0.0f) from = r.times[f];
                    to = r.times[f];
                }
            }
            moving.push_back(n);
            if (bestAngle >= 0.05f) {
                const auto& peak = r.frames[angleAt][n];
                const M3 d = mulAB(colsOf(peak), transposed(restCols));
                const float s = std::sin(bestAngle);
                RE::NiPoint3 axis{ d.m[2][1] - d.m[1][2], d.m[0][2] - d.m[2][0], d.m[1][0] - d.m[0][1] };
                axis = axis * (1.0f / (2.0f * (std::fabs(s) > 1e-4f ? s : 1e-4f)));
                const float len = std::sqrt(dot(axis, axis));
                if (len > 1e-4f) axis = axis * (1.0f / len);
                // the pivot: the point the turn leaves where it was (I - D) p = t1 - D t0, pinned along the axis
                M3 a{};
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a.m[i][j] = (i == j ? 1.0f : 0.0f) - d.m[i][j];
                const float ax[3]{ axis.x, axis.y, axis.z };
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a.m[i][j] += ax[i] * ax[j];
                const auto rhs = peak.translate - mulV(d, rest.translate) + axis * dot(axis, rest.translate);
                RE::NiPoint3 pivot = rest.translate;
                const bool pivotOk = solve3(a, rhs, pivot);
                // how far the pivot really stays put (a pure hinge leaves it still)
                const auto moved = mulV(d, pivot - rest.translate) + peak.translate - pivot;
                std::snprintf(line, sizeof(line),
                    "hinge %s angle %.1f axis %.3f %.3f %.3f pivot %.3f %.3f %.3f slip %.2f%s from %.3f to %.3f peak %.3f move %.2f\n", r.names[n].c_str(),
                    bestAngle * 57.29578f, axis.x, axis.y, axis.z, pivot.x, pivot.y, pivot.z, std::sqrt(dot(moved, moved)), pivotOk ? "" : " (no pivot)", from, to,
                    r.times[angleAt], bestMove);
            } else {
                const auto d = r.frames[moveAt][n].translate - r.frames[0][n].translate;
                const auto dir = d * (1.0f / bestMove);
                std::snprintf(line, sizeof(line), "slide %s travel %.2f dir %.3f %.3f %.3f from %.3f to %.3f peak %.3f\n", r.names[n].c_str(), bestMove, dir.x,
                    dir.y, dir.z, from, to, r.times[moveAt]);
            }
            out << line;
            line[std::strlen(line) - 1] = '\0';
            logNote(line);
            (void)showsUp;
            (void)vanishes;
        }
        // the moving nodes frame by frame, for working them out offline: time, then per node x y z and the 3 axes
        out << "; frames: t | name x y z ax ay az bx by bz cx cy cz (axes of the node in the gun root's frame)\n";
        for (std::size_t f = 0; f < frameCount; ++f) {
            for (const auto n : moving) {
                const auto& t = r.frames[f][n];
                const auto c = colsOf(t);
                std::snprintf(line, sizeof(line), "f %.3f %s %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %d\n", r.times[f], r.names[n].c_str(),
                    t.translate.x, t.translate.y, t.translate.z, c.m[0][0], c.m[1][0], c.m[2][0], c.m[0][1], c.m[1][1], c.m[2][1], c.m[0][2], c.m[1][2],
                    c.m[2][2], r.culled[f][n] ? 1 : 0);
                out << line;
            }
        }
        g_recorded[r.weapon] = true;
        g_playStale = true;
        hud("Запись перезарядки сохранена: со следующего раза ствол заряжается руками");
    }

    void recordReload(RE::PlayerCharacter* player, const Gun& gun, Clock::time_point now) noexcept
    {
        auto& r = g_rec;
        auto* root = findNamed(player->firstPerson3D.get(), "Weapon");
        if (!looksLikeObject(root) || !gun.weapon) {
            if (r.active) finishRecording();
            return;
        }
        const bool reloading = static_cast<std::uint32_t>(player->gunState) == static_cast<std::uint32_t>(g_settings.reloadGunState);
        if (reloading) {
            r.lastSeen = now;
        }
        if (!r.active) {
            if (!reloading) {
                return;
            }
            r = AnimRecorder{ .takes = r.takes };
            r.active = true;
            r.start = now;
            r.lastSeen = now;
            r.weapon = gun.weapon->GetFormID();
            forEachNode(root, [&](RE::NiAVObject* object, int depth) {
                if (depth > 30 || r.nodes.size() >= 400) return false;
                auto name = nodeName(object);
                if (!name.empty()) {
                    for (auto& c : name) if (c == ' ') c = '_';
                    r.names.push_back(name);
                    r.nodes.push_back(object);
                }
                return true;
            });
            note("RECORD: the game's reload of %08X started, %zu nodes", r.weapon, r.nodes.size());
        }
        const float t = std::chrono::duration<float>(now - r.start).count();
        std::vector<RE::NiTransform> frame(r.nodes.size());
        std::vector<std::uint8_t> hidden(r.nodes.size());
        for (std::size_t n = 0; n < r.nodes.size(); ++n) {
            auto* object = r.nodes[n];
            if (looksLikeObject(object)) {
                frame[n] = relative(root->world, object->world);
                hidden[n] = (object->flags.flags & 1ull) ? 1 : 0;
            } else {
                frame[n] = r.frames.empty() ? identityTransform() : r.frames.back()[n];
                hidden[n] = 1;
            }
        }
        r.times.push_back(t);
        r.frames.push_back(std::move(frame));
        r.culled.push_back(std::move(hidden));
        // keeps going 2.5 s after the state ends (the game's animation runs on after it), at most 8 s
        if ((!reloading && now - r.lastSeen > std::chrono::milliseconds(2500)) || t > 8.0f) {
            finishRecording();
        }
    }


    // ------------------------------------------------------------ the game's reload, played back by hand

    // A recording (ManualReloadF_anim/<weapon>.txt) read back: the moving nodes frame by frame against the gun root,
    // and the hinge of the part that opens (WeaponMagazine: a cylinder, the barrels). The opening part and whatever
    // moves with it are posed from the recording at a point of its opening move chosen by the plugin or the hand.
    struct PlayedAnim
    {
        bool                                      loaded{ false };
        std::uint32_t                             weapon{ 0 };
        std::vector<std::string>                  names;
        std::vector<float>                        times;
        std::vector<std::vector<RE::NiTransform>> frames;   // [frame][node]
        float                                     openTime{ 0.0f };
        float                                     openAngle{ 0.0f };   // radians
        RE::NiPoint3                              axis{};              // hinge, gun root frame
        RE::NiPoint3                              pivot{};
        std::vector<int>                          posed;               // the nodes posed (indices in names)
        std::vector<RE::NiAVObject*>              nodes;               // resolved on the drawn gun
        RE::NiAVObject*                           rootSeen{ nullptr };
        bool                                      parsed{ false };     // frames are there (whatever opens)
        std::unordered_map<std::string, int>      index;
        // the bolt / slide (WeaponBolt): its way back, from rest to where it goes furthest
        bool                                      boltKnown{ false };
        int                                       bolt{ -1 };
        std::size_t                               boltFrom{ 0 };
        std::size_t                               boltTo{ 0 };
        float                                     boltTurn{ 0.0f };    // radians it turns on the way (a bolt action lifts its handle)
    };
    PlayedAnim g_play;

    bool loadPlayed(std::uint32_t weapon) noexcept
    {
        auto& p = g_play;
        if (g_playStale) {
            g_playStale = false;
            p.weapon = 0;
        }
        if (p.weapon == weapon) {
            return p.loaded;
        }
        p = PlayedAnim{};
        p.weapon = weapon;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_anim/%08X.txt", weapon);
        std::ifstream file(path);
        if (!file) {
            return false;
        }
        struct HingeLine { float angle; RE::NiPoint3 axis, pivot; float from; };
        std::unordered_map<std::string, HingeLine> hinges;
        std::unordered_map<std::string, int> index;
        std::string line;
        float lastTime = -1.0f;
        while (std::getline(file, line)) {
            std::vector<std::string> words;
            {
                std::size_t at = 0;
                while (at < line.size()) {
                    while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
                    const auto start = at;
                    while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') ++at;
                    if (at > start) words.emplace_back(line.substr(start, at - start));
                }
            }
            const auto num = [&](std::size_t i) { return i < words.size() ? std::strtof(words[i].c_str(), nullptr) : 0.0f; };
            const auto after = [&](const char* key) -> std::size_t {
                for (std::size_t i = 0; i < words.size(); ++i) if (words[i] == key) return i + 1;
                return words.size();
            };
            if (words.size() > 12 && words[0] == "hinge") {
                HingeLine h{};
                h.angle = num(after("angle"));
                const auto ax = after("axis");
                h.axis = { num(ax), num(ax + 1), num(ax + 2) };
                const auto pv = after("pivot");
                h.pivot = { num(pv), num(pv + 1), num(pv + 2) };
                h.from = num(after("from"));
                hinges[words[1]] = h;
                continue;
            }
            if (words.size() < 15 || words[0] != "f") {
                continue;
            }
            const float t = num(1);
            const std::string name = words[2];
            float v[12]{};
            for (int k = 0; k < 12; ++k) v[k] = num(3 + k);
            if (t != lastTime) {
                lastTime = t;
                p.times.push_back(t);
                p.frames.emplace_back(p.names.size(), identityTransform());
            }
            auto found = index.find(name);
            if (found == index.end()) {
                if (p.frames.size() > 1) {
                    continue;   // every node is in the first frame
                }
                found = index.emplace(name, static_cast<int>(p.names.size())).first;
                p.names.emplace_back(name);
                p.frames.back().push_back(identityTransform());
            }
            RE::NiTransform tr = identityTransform();
            setRows(tr, { v[3], v[4], v[5] }, { v[6], v[7], v[8] }, { v[9], v[10], v[11] });
            tr.translate = { v[0], v[1], v[2] };
            p.frames.back()[found->second] = tr;
        }
        p.parsed = p.frames.size() >= 10;
        p.index = index;
        if (const auto boltAt = index.find("WeaponBolt"); p.parsed && boltAt != index.end()) {
            const int n = boltAt->second;
            const auto restCols = colsOf(p.frames[0][n]);
            std::vector<float> away(p.frames.size(), 0.0f);
            std::size_t peak = 0;
            for (std::size_t f = 1; f < p.frames.size(); ++f) {
                const auto d = p.frames[f][n].translate - p.frames[0][n].translate;
                away[f] = std::sqrt(dot(d, d)) + 3.0f * angleOf(mulAB(colsOf(p.frames[f][n]), transposed(restCols)));
                if (away[f] > away[peak]) peak = f;
            }
            if (away[peak] > 0.5f) {
                std::size_t from = peak;
                while (from > 0 && away[from - 1] > away[peak] * 0.02f) --from;
                from = from > 0 ? from - 1 : 0;
                p.bolt = n;
                p.boltFrom = from;
                p.boltTo = peak;
                for (std::size_t f = from; f <= peak; ++f) {
                    p.boltTurn = std::max(p.boltTurn, angleOf(mulAB(colsOf(p.frames[f][n]), transposed(restCols))));
                }
                p.boltKnown = true;
                note("PLAY: bolt / slide path of %08X: frames %zu..%zu (%.2f..%.2f s), turns %.1f deg on the way%s", weapon, from, peak, p.times[from],
                    p.times[peak], p.boltTurn * 57.29578f, p.boltTurn > 0.35f ? ": a bolt action, worked by hand after every shot" : "");
            }
        }
        const auto main = hinges.find("WeaponMagazine");
        const auto mainIndex = index.find("WeaponMagazine");
        if (p.frames.size() < 10 || main == hinges.end() || mainIndex == index.end()) {
            detail("PLAY: the recording of %08X has no hinge on WeaponMagazine (%zu frames)", weapon, p.frames.size());
            return false;
        }
        p.openAngle = main->second.angle * 0.01745329f;
        p.axis = normalized(main->second.axis);
        p.pivot = main->second.pivot;
        const auto restCols = colsOf(p.frames[0][mainIndex->second]);
        p.openTime = p.times.back();
        for (std::size_t f = 1; f < p.frames.size(); ++f) {
            if (angleOf(mulAB(colsOf(p.frames[f][mainIndex->second]), transposed(restCols))) >= p.openAngle * 0.97f) {
                p.openTime = p.times[f];
                break;
            }
        }
        // posed: everything that is on the way by the time it is open, except parts that swing on their own axis
        // below the opening part (a speedloader, shells thrown out)
        for (std::size_t n = 0; n < p.names.size(); ++n) {
            const auto& name = p.names[n];
            const auto h = hinges.find(name);
            bool moving = true;
            if (h != hinges.end()) {
                const bool sameHinge = std::fabs(dot(normalized(h->second.axis), p.axis)) > 0.97f && std::fabs(h->second.angle - main->second.angle) < 3.0f;
                const bool early = h->second.from <= p.openTime + 0.01f;
                const bool own = name.rfind("WeaponMagazineChild", 0) == 0 || name.find("Shell") != std::string::npos ||
                                 name.find("SpeedLoader") != std::string::npos || name.find("Bullet") != std::string::npos ||
                                 name.find("Casing") != std::string::npos;
                moving = early && (sameHinge || !own);
            } else {
                moving = false;   // slides are left to the game (triggers, sights)
            }
            if (moving) {
                p.posed.push_back(static_cast<int>(n));
            }
        }
        p.loaded = true;
        std::string list;
        for (const auto n : p.posed) {
            list += p.names[n] + " ";
        }
        note("PLAY: recording of %08X, %zu frames; opens %.1f deg about (%.2f, %.2f, %.2f) at (%.2f, %.2f, %.2f) by %.2f s; posed: %s", weapon, p.frames.size(),
            main->second.angle, p.axis.x, p.axis.y, p.axis.z, p.pivot.x, p.pivot.y, p.pivot.z, p.openTime, list.c_str());
        return true;
    }

    RE::NiTransform playedAt(const PlayedAnim& p, int node, float time) noexcept
    {
        std::size_t hi = 1;
        while (hi < p.times.size() - 1 && p.times[hi] < time) {
            ++hi;
        }
        const std::size_t lo = hi - 1;
        const float span = p.times[hi] - p.times[lo];
        const float k = span > 1e-5f ? std::clamp((time - p.times[lo]) / span, 0.0f, 1.0f) : 0.0f;
        const auto& a = p.frames[lo][node];
        const auto& b = p.frames[hi][node];
        RE::NiTransform out = identityTransform();
        out.translate = a.translate + (b.translate - a.translate) * k;
        const auto x = normalized(axisOf(a, 0) + (axisOf(b, 0) - axisOf(a, 0)) * k);
        auto y = axisOf(a, 1) + (axisOf(b, 1) - axisOf(a, 1)) * k;
        y = normalized(y - x * dot(y, x));
        setRows(out, x, y, cross(x, y));
        return out;
    }

    // Poses the opening part as the recording has it at `amount` (0 closed .. 1 open) of its opening move.
    bool posePlayed(RE::NiAVObject* root, float amount) noexcept
    {
        auto& p = g_play;
        if (!p.loaded) {
            return false;
        }
        if (p.rootSeen != root || p.nodes.size() != p.posed.size()) {
            p.rootSeen = root;
            p.nodes.clear();
            for (const auto n : p.posed) {
                p.nodes.push_back(findNamed(root, p.names[n].c_str()));
            }
        }
        const float time = std::clamp(amount, 0.0f, 1.0f) * p.openTime;
        std::unordered_map<RE::NiAVObject*, RE::NiTransform> placed;
        std::vector<RE::NiAVObject*> tops;
        for (std::size_t i = 0; i < p.posed.size(); ++i) {
            auto* node = p.nodes[i];
            if (!looksLikeObject(node) || !looksLikeObject(node->parent)) {
                continue;
            }
            auto recorded = playedAt(p, p.posed[i], time);
            recorded.scale = root->world.scale > 0.0f ? node->world.scale / root->world.scale : 1.0f;
            const auto world = compose(root->world, recorded);
            const auto up = placed.find(node->parent);
            const bool parentPosed = up != placed.end();
            node->local = relative(parentPosed ? up->second : node->parent->world, world);
            placed[node] = world;
            if (!parentPosed) {
                tops.push_back(node);
            }
        }
        for (auto* node : tops) {
            refreshSubtree(node, RE::NiPoint3{});
        }
        return true;
    }

    // The angle of a point about the hinge (gun root frame), for turning the opening part by hand.
    float hingeAngle(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept
    {
        const auto& p = g_play;
        const auto at = relative(root->world, pointTransform(world)).translate - p.pivot;
        const auto flat = at - p.axis * dot(at, p.axis);
        auto u = std::fabs(p.axis.z) < 0.9f ? RE::NiPoint3{ 0.0f, 0.0f, 1.0f } : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
        u = normalized(u - p.axis * dot(u, p.axis));
        const auto v = cross(p.axis, u);
        return std::atan2(dot(flat, v), dot(flat, u));
    }

    // The bolt as the recording has it at `s` (0 rest .. 1 furthest back), in the gun root's frame.
    RE::NiTransform boltAt(float s) noexcept
    {
        const auto& p = g_play;
        const float time = p.times[p.boltFrom] + std::clamp(s, 0.0f, 1.0f) * (p.times[p.boltTo] - p.times[p.boltFrom]);
        if (s <= 0.0f) {
            return p.frames[0][p.bolt];
        }
        return playedAt(p, p.bolt, time);
    }

    // core2: the recorded way measured by how far the bolt really goes, not by time. The game's animation pulls the
    // slide back in a third of its take and holds it there: by time, most of the hand's way would move nothing, and the
    // rest would throw the slide. Here 0..1 is the distance along the way (turns count by a point 3 units off the
    // axis), ending where the slide is furthest back (a bolt action keeps its whole way).
    struct BoltArc
    {
        std::uint32_t weapon{ 0 };
        std::size_t   frames{ 0 };
        std::vector<float> s;      // time fraction of each sample
        std::vector<float> u;      // distance fraction of each sample
    };
    BoltArc g_arc;

    void buildArc() noexcept
    {
        auto& a = g_arc;
        const auto& p = g_play;
        if (a.weapon == p.weapon && a.frames == p.frames.size() && !a.s.empty()) {
            return;
        }
        a = BoltArc{};
        a.weapon = p.weapon;
        a.frames = p.frames.size();
        constexpr int kSamples = 240;
        const RE::NiPoint3 probes[3]{ { 0.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 3.0f } };
        std::vector<std::array<RE::NiPoint3, 3>> at(kSamples + 1);
        for (int i = 0; i <= kSamples; ++i) {
            const auto t = boltAt(static_cast<float>(i) / kSamples);
            for (int k = 0; k < 3; ++k) {
                at[i][k] = compose(t, pointTransform(probes[k])).translate;
            }
        }
        // where the slide is furthest from rest (by its origin)
        int last = kSamples;
        if (p.boltTurn <= 0.35f) {
            float farthest = 0.0f;
            for (int i = 0; i <= kSamples; ++i) {
                farthest = std::max(farthest, distance(at[i][0], at[0][0]));
            }
            for (int i = 0; i <= kSamples; ++i) {
                if (distance(at[i][0], at[0][0]) >= farthest * 0.98f) {
                    last = std::max(i, 1);
                    break;
                }
            }
        }
        std::vector<float> length(last + 1, 0.0f);
        for (int i = 1; i <= last; ++i) {
            float step = 0.0f;
            for (int k = 0; k < 3; ++k) {
                step = std::max(step, distance(at[i][k], at[i - 1][k]));
            }
            length[i] = length[i - 1] + step;
        }
        const float total = std::max(length[last], 0.001f);
        for (int i = 0; i <= last; ++i) {
            a.s.push_back(static_cast<float>(i) / kSamples);
            a.u.push_back(length[i] / total);
        }
        note("CORE2: the slide's way is %.1f long; it is furthest back at %.0f%% of the recorded time", total, 100.0f * last / kSamples);
    }

    RE::NiTransform boltArc(float u) noexcept
    {
        buildArc();
        const auto& a = g_arc;
        u = std::clamp(u, 0.0f, 1.0f);
        if (a.u.size() < 2) {
            return boltAt(u);
        }
        const auto it = std::lower_bound(a.u.begin(), a.u.end(), u);
        const std::size_t j = std::clamp<std::size_t>(static_cast<std::size_t>(it - a.u.begin()), 1, a.u.size() - 1);
        const float u0 = a.u[j - 1], u1 = a.u[j];
        const float f = u1 > u0 ? (u - u0) / (u1 - u0) : 0.0f;
        return boltAt(a.s[j - 1] + (a.s[j] - a.s[j - 1]) * f);
    }

    RE::NiTransform boltWay(float s) noexcept { return g_settings.core2 ? boltArc(s) : boltAt(s); }

    // The zones file's way for the slide / handle: how far it stands off its rest at u (0..1 of the way).
    bool hasRoute() noexcept { return g_settings.core2 && g_state.route.size() >= 2; }

    RE::NiPoint3 routeShift(float u) noexcept
    {
        const auto& st = g_state;
        if (st.route.size() < 2) {
            return {};
        }
        u = std::clamp(u, 0.0f, 1.0f);
        std::size_t j = 1;
        while (j + 1 < st.route.size() && st.routeAt[j] < u) {
            ++j;
        }
        const float a = st.routeAt[j - 1], b = st.routeAt[j];
        const float f = b > a ? std::clamp((u - a) / (b - a), 0.0f, 1.0f) : 1.0f;
        return st.route[j - 1] + (st.route[j] - st.route[j - 1]) * f - st.route[0];
    }

    // Where the slide's way has the part at u: turned by `angle`, moved by `lin`.
    void routeMotion(float u, float& angle, RE::NiPoint3& lin) noexcept
    {
        const auto& st = g_state;
        const auto& f = st.slideFit;
        u = std::clamp(u, 0.0f, 1.0f);
        if (f.arc && f.arcAt > 0.0f && u <= f.arcAt) {
            angle = f.angle * u / f.arcAt;
            lin = {};
            return;
        }
        angle = f.arc ? f.angle : 0.0f;
        lin = routeShift(u) - (f.arc ? st.route[f.k] - st.route[0] : RE::NiPoint3{});
    }

    RE::NiPoint3 routeApply(float u, const RE::NiPoint3& p) noexcept
    {
        float angle;
        RE::NiPoint3 lin;
        routeMotion(u, angle, lin);
        const auto& f = g_state.slideFit;
        return f.arc ? rotateAbout(p - f.pivot, f.axis, angle) + f.pivot + lin : p + lin;
    }

    RE::NiPoint3 routeInverse(float u, const RE::NiPoint3& p) noexcept
    {
        float angle;
        RE::NiPoint3 lin;
        routeMotion(u, angle, lin);
        const auto& f = g_state.slideFit;
        return f.arc ? rotateAbout(p - lin - f.pivot, f.axis, -angle) + f.pivot : p - lin;
    }

    bool boltPathFor(const Gun& gun) noexcept
    {
        if (!gun.weapon) {
            return false;
        }
        loadPlayed(gun.weapon->GetFormID());
        return g_play.parsed && g_play.boltKnown && g_play.weapon == gun.weapon->GetFormID();
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
        const bool route = hasRoute();   // the zones file's way wins over the recorded one
        const bool path = !route && g_settings.recordedBolt && boltPathFor(gun) && looksLikeObject(bolt->parent);
        const float travel = g_settings.rackTravel;
        if (path && !st.boltAction && g_play.boltTurn > 0.35f) {
            st.boltAction = true;
        }
        // the controller, not the drawn hand: ROCK moves the drawn hand onto whatever it holds
        const auto handLocal = relative(root->world, pointTransform(spare.wand.translate)).translate;
        const bool handBusy = st.place == kRailHand || st.place == kHand || (griphook::owned() && st.armReason != kArmSlide) ||
                              (g_settings.core2 && (st.handRef != 0 || st.handToken != 0));
        const bool rockTook = g_rock.onBolt && !st.rockWasOnBolt && spare.valid && spare.grip && !handBusy && st.place != kRailAuto &&
                              Clock::now() - st.seatedAt > std::chrono::milliseconds(700);
        st.rockWasOnBolt = g_rock.onBolt;
        const float lockAt = g_settings.rackTravel * std::clamp(g_settings.slideLockAt, 0.0f, 1.0f);
        if (g_settings.core2) {
            // core2: no grab point. The palm in the slide's box moves it along its recorded way by the hand's own motion
            // along that way (motion across it is ignored): gripped it stays taken until the grip opens, open-handed
            // it is pushed only while the palm is on it. Let go, a slide springs home, a bolt action stays put.
            const float s0 = std::clamp(st.rackPull / travel, 0.0f, 1.0f);
            auto wayPoint = [&](float s) {
                s = std::clamp(s, 0.0f, 1.0f);
                if (route) {
                    return routeApply(s, st.handInBolt);
                }
                if (path) {
                    return compose(boltWay(s), pointTransform(st.handInBolt)).translate;
                }
                return st.handInBolt + RE::NiPoint3{ 0.0f, -s * travel, 0.0f };
            };
            auto shiftNow = [&]() {
                if (route) {
                    return routeShift(s0);
                }
                if (path) {
                    return boltWay(s0).translate - boltWay(0.0f).translate;
                }
                return RE::NiPoint3{ 0.0f, -st.rackPull, 0.0f };
            };
            const auto palmLocal = inGun(root, spare.palm);
            bool onSlide = false;
            if (spare.valid) {
                onSlide = st.slideBoxKnown ? inBox(st.slideLo, st.slideHi, route ? routeInverse(s0, palmLocal) : palmLocal - shiftNow(), g_settings.contactMargin)
                                           : distance(spare.palm, slideGrabPoint(root, bolt)) < g_settings.slideGrabRadius;
            }
            const bool magFirst = st.place == kRailAuto && palmOnMagazine(root, spare.palm);   // a magazine sticking out comes first
            const bool justSeated = Clock::now() - st.seatedAt < std::chrono::milliseconds(700);
            const bool mayTouch = spare.valid && !handBusy && !magFirst && !justSeated;
            auto anchor = [&]() {
                // the hand's point on the part, kept as the part moves: the way the hand goes is this point's way
                if (route) {
                    st.handInBolt = routeInverse(s0, handLocal);
                } else if (path) {
                    st.handInBolt = relative(boltWay(s0), pointTransform(handLocal)).translate;
                } else {
                    st.handInBolt = handLocal + RE::NiPoint3{ 0.0f, s0 * travel, 0.0f };
                }
            };
            if (!st.racking && mayTouch && spare.grip && ((!spare.previousGrip && onSlide) || rockTook)) {
                st.racking = true;
                st.pushing = false;
                anchor();
                buzz(freeIndex(), 500);
                detail("CORE2: the slide is taken (palm in its box), %.2f of its way", s0);
            }
            if (st.racking && !(spare.valid && spare.grip)) {
                st.racking = false;
            }
            const bool push = !st.racking && mayTouch && !spare.grip && onSlide;
            if (push && !st.pushing) {
                anchor();
                detail("CORE2: the open palm is on the slide, %.2f of its way", s0);
            }
            st.pushing = push;
            if ((st.racking || st.pushing) && st.prevHandKnown) {
                const float e = 0.03f;
                const float a = std::clamp(s0 - e, 0.0f, 1.0f);
                const float b = std::clamp(s0 + e, 0.0f, 1.0f);
                const auto tangent = (wayPoint(b) - wayPoint(a)) * (1.0f / std::max(b - a, 0.001f));
                const float tt = dot(tangent, tangent);
                if (tt > 1e-4f) {
                    const auto moved = handLocal - st.prevHandLocal;
                    float ds = dot(moved, tangent) / tt;
                    ds = std::clamp(ds, -0.4f, 0.4f);
                    if (st.pushing) {
                        ds *= std::clamp(g_settings.pushFactor, 0.0f, 1.0f);   // an open palm slips on it
                        if (dot(moved, moved) > 36.0f) {
                            ds = 0.0f;   // a jump of the tracking, not a push
                        }
                    }
                    const float lo = st.slideLocked ? lockAt / travel : 0.0f;
                    st.rackPull = std::clamp(s0 + ds, lo, 1.0f) * travel;
                }
            } else if (st.slideLocked) {
                st.rackPull = approach(st.rackPull, lockAt, 80.0f * dt);
            } else if (!st.boltAction) {
                st.rackPull = approach(st.rackPull, 0.0f, 25.0f * travel * dt);
            }
            if (st.slideLocked && st.rackPull >= travel * 0.95f) {
                st.slideLocked = false;   // pulled off the slide stop: let go and it runs home
                st.rackCycled = true;
                playSound(2);
                logNote("RACK: slide pulled off the stop");
            }
        } else {
            if (!st.racking && spare.valid && spare.grip && ((!spare.previousGrip && !handBusy) || rockTook)) {
                const float toSlide = st.place != kRailAuto ? distance(spare.palm, slideGrabPoint(root, bolt)) : 1e9f;
                const float toMag = st.place == kRailAuto && st.railKnown ? distance(spare.palm, grabPoint(root)) : 1e9f;
                if (rockTook || (toSlide < g_settings.slideGrabRadius && toSlide <= toMag)) {
                    st.racking = true;
                    st.rackGrabY = handLocal.y + st.rackPull;
                    st.rackGrabHands = distance(spare.wand.translate, firing.wand.translate);
                    st.rackGrabPull = st.rackPull;
                    if (path) {
                        st.handInBolt = relative(boltAt(st.rackPull / travel), pointTransform(handLocal)).translate;
                    }
                    buzz(freeIndex(), 500);
                    detail("Slide taken by the free hand (%.1f from its middle)", toSlide);
                }
            }
            if (st.racking && !(spare.valid && spare.grip)) {
                st.racking = false;   // let go: the spring sends it home
            }
            if (st.racking && path) {
                // the hand's point on the bolt follows the recorded way back (a bolt action: up, then back)
                const float cur = st.rackPull / travel;
                // the pull along the bolt's way back counts fully, the rest (lifting the handle) only partly:
                // pulling back lifts the handle by itself instead of holding the bolt where it is
                const auto travelled = boltAt(1.0f).translate - boltAt(0.0f).translate;
                const auto wayBack = dot(travelled, travelled) > 0.09f ? normalized(travelled) : RE::NiPoint3{};
                float best = cur, bestDistance = 1e9f;
                for (int k = -35; k <= 35; ++k) {
                    const float s = std::clamp(cur + k * 0.01f, 0.0f, 1.0f);
                    const auto at = compose(boltAt(s), pointTransform(st.handInBolt)).translate;
                    const auto off = at - handLocal;
                    const float along = dot(off, wayBack);
                    const auto side = off - wayBack * along;
                    const float d = along * along + 0.1f * dot(side, side);
                    if (d < bestDistance) {
                        bestDistance = d;
                        best = s;
                    }
                }
                st.rackPull = std::clamp(best * travel, st.slideLocked ? lockAt : 0.0f, travel);
                if (st.slideLocked && st.rackPull >= travel * 0.95f) {
                    st.slideLocked = false;
                    st.rackCycled = true;
                    playSound(2);
                    logNote("RACK: slide pulled off the stop");
                }
            } else if (st.racking) {
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
            } else if (!st.boltAction) {
                st.rackPull = approach(st.rackPull, 0.0f, 25.0f * g_settings.rackTravel * dt);
            }
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
        if (route) {
            if ((st.rackPull > 0.0f || st.racking) && looksLikeObject(bolt->parent)) {
                // the bolt stands at rest here (set before): turn it and move it along the way
                const auto rest = relative(root->world, bolt->world);
                float angle;
                RE::NiPoint3 lin;
                routeMotion(st.rackPull / travel, angle, lin);
                const auto& f = st.slideFit;
                const auto posed = f.arc ? turnedPose(rest, f.axis, f.pivot, angle, lin) : turnedPose(rest, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, RE::NiPoint3{}, 0.0f, lin);
                bolt->local = relative(bolt->parent->world, compose(root->world, posed));
                refreshSubtree(bolt, RE::NiPoint3{});
            }
        } else if (path) {
            if (st.rackPull > 0.0f || st.racking) {
                auto recorded = boltWay(st.rackPull / travel);
                recorded.scale = bolt->world.scale / scale;
                bolt->local = relative(bolt->parent->world, compose(root->world, recorded));
                refreshSubtree(bolt, RE::NiPoint3{});
            }
        } else if (st.rackPull > 0.0f) {
            refreshSubtree(bolt, axisOf(root->world, 1) * (-st.rackPull * scale));
        }
    }

    // ------------------------------------------------------------ every frame

    // core2: a real magazine on its way into the free hand, or held there by ROCK. ROCK is asked again while the
    // grip is held and it has not taken it; let go, it falls (at the belt or over the shoulder it is put away).
    void realMagazineInHand(RE::PlayerCharacter* player) noexcept
    {
        auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        const auto now = Clock::now();
        if (st.handToken && st.handSpare && !(free.valid && free.grip) && !st.handTokenDropped) {
            st.handTokenDropped = true;   // let go before the spare came
            st.handStash = inBelt(player, free.palm) || overShoulder(player, free.palm);
        }
        if (!st.handRef) {
            return;
        }
        const auto held = rocklink::heldBy(freeIndex());
        if (held == st.handRef) {
            if (!st.grabDone) {
                st.grabDone = true;
                note("CORE2: ROCK holds magazine %08X in the palm (%.2f s, %d asks)", st.handRef,
                    std::chrono::duration<float>(now - st.handRefAt).count(), st.grabTries);
            }
            return;
        }
        if (st.grabDone) {
            // ROCK let it go: put away at the belt / over the shoulder, else it is just a magazine lying there
            if (inBelt(player, free.palm) || overShoulder(player, free.palm)) {
                const auto it = g_loose.find(st.handRef);
                const auto rounds = it != g_loose.end() ? it->second.rounds : 0u;
                deleteMagazine(st.handRef);
                char text[96]{};
                std::snprintf(text, sizeof(text), "Магазин убран (%u патр. вернулись в запас)", rounds);
                hud(text);
                playSound(4);
            }
            note("CORE2: magazine %08X is let go", st.handRef);
            st.handRef = 0;
            return;
        }
        int failure = 0;
        const int state = rocklink::forceGrabState(&failure);
        if (!(free.valid && free.grip) || now - st.handRefAt > std::chrono::milliseconds(2000)) {
            note("CORE2: ROCK did not take magazine %08X (grab state %d, failure %d, held %08X): it falls", st.handRef, state, failure, held);
            rocklink::cancelGrab();   // a request still queued would keep the magazine claimed: it would hang in the air, not to be taken
            st.handRef = 0;
            return;
        }
        // queued (1): ROCK is on it, asking again only answers "hand busy"; asked again after it was rejected or cancelled
        if (st.grabTries < 5 && now - st.grabTriedAt > std::chrono::milliseconds(250) && state != 1 && state != 2) {
            st.grabTriedAt = now;
            ++st.grabTries;
            detail("CORE2: asking ROCK again for magazine %08X (state %d, failure %d, held %08X)", st.handRef, state, failure, held);
            if (held != 0) {
                // ROCK still has the hand on the gun's magazine part (pulled out of the well): it lets go first
                const bool let = rocklink::releaseHeld(freeIndex(), held);
                detail("CORE2: ROCK is asked to let go of %08X first: %d", held, let ? 1 : 0);
            }
            auto* ref = lookupRef(st.handRef);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            const auto c = looksLikeObject(mag3D) ? mag3D->worldBound.center : free.palm;
            const float middle[3]{ c.x, c.y, c.z };
            rocklink::forceGrab(freeIndex(), st.handRef, middle);
        }
    }

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

    // ------------------------------------------------------------ revolvers and break actions

    // Turns the rows (the node's own axes as its parent sees them) about one of the parent's axes.
    void turnRows(RE::NiMatrix3& rotation, int axis, float angle) noexcept
    {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        auto m = toMat(rotation);
        for (auto& row : m) {
            const float x = row[0], y = row[1], z = row[2];
            if (axis == 1) {
                row[0] = x * c + z * sn;
                row[2] = -x * sn + z * c;
            } else {
                row[1] = y * c - z * sn;
                row[2] = y * sn + z * c;
            }
        }
        rotation = fromMat(m);
    }

    // The laser musket: the free hand takes the crank's handle and winds it round; each full turn charges one more shot.
    void updateCrank(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        const auto& spare = g_hands[freeIndex()];
        rocklink::Frame link{};
        link.gun = true;
        link.freeHand = freeIndex();
        link.magazine.node = kMovingParts[0];
        link.bolt.node = kMovingParts[1];
        g_rock = rocklink::frame(link);

        auto* crank = findNamed(root, kMovingParts[0]);
        if (!looksLikeObject(crank) || !looksLikeObject(crank->parent)) {
            return;
        }
        if (!st.crankRestKnown) {
            if (now - st.equippedAt < std::chrono::milliseconds(700)) {
                return;
            }
            st.crankRest = relative(root->world, crank->world);
            st.crankRestKnown = true;
            note("Crank: rest pose remembered, %u of %u charges", gun.data->ammoCount, capacityOf(gun));
        }
        const auto inRoot = [&](const RE::NiPoint3& p) { return relative(root->world, pointTransform(p)).translate; };
        const auto& axis = st.crankAxis;
        const auto e1 = normalized((st.crankStart - st.crankCenter) - axis * dot(st.crankStart - st.crankCenter, axis));
        const auto e2 = cross(axis, e1);
        const auto handAngle = [&](const RE::NiPoint3& p) {
            const auto v = inRoot(p) - st.crankCenter;
            return std::atan2(dot(v, e2), dot(v, e1));
        };
        const auto handle = rotateAbout(st.crankStart - st.crankCenter, axis, st.crankAngle) + st.crankCenter;
        const float scale = gunScale(root);
        const bool onHandle = spare.valid && distance(inRoot(spare.palm), handle) < 4.5f;
        const bool press = spare.valid && spare.grip && !spare.previousGrip;
        if (!spare.grip) {
            st.armReason = onHandle ? kArmBlock : kArmNone;
            griphook::setArmed(onHandle);   // the grip on the handle is ours, not a second-hand hold
        }
        if (press && onHandle && griphook::owned()) {
            st.crankHeld = true;
            st.crankPrev = handAngle(spare.palm);
            buzz(freeIndex(), 700);
            detail("Crank: the free hand takes the handle");
        }
        if (st.crankHeld && !(spare.valid && spare.grip)) {
            st.crankHeld = false;
            detail("Crank: let go after %.0f deg", st.crankTurned * 57.29578f);
        }
        if (st.crankHeld) {
            const float a = handAngle(spare.palm);
            float turn = a - st.crankPrev;
            while (turn > 3.14159265f) turn -= 6.2831853f;
            while (turn < -3.14159265f) turn += 6.2831853f;
            st.crankPrev = a;
            st.crankAngle += turn;
            st.crankTurned += std::fabs(turn);
            if (std::fabs(turn) > 0.15f) {
                buzz(freeIndex(), 300);
            }
            if (st.crankTurned >= 6.2831853f) {
                st.crankTurned -= 6.2831853f;
                const auto capacity = std::max(capacityOf(gun), 1u);
                const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;
                if (gun.data->ammoCount < capacity && inventory > gun.data->ammoCount) {
                    gun.data->ammoCount += 1;
                    playSound(0);
                    buzz(freeIndex(), 1500);
                    buzz(g_settings.gunHand, 1500);
                    note("CRANK: one more charge, %u of %u", gun.data->ammoCount, capacity);
                } else {
                    hud(inventory <= gun.data->ammoCount ? "Нет батарей" : "Заряжено полностью");
                    note("CRANK: a full turn, nothing to charge (%u of %u, %u cells)", gun.data->ammoCount, capacity, inventory);
                }
            }
        }
        const auto posed = turnedPose(st.crankRest, axis, st.crankCenter, st.crankAngle, RE::NiPoint3{});
        crank->local = relative(crank->parent->world, compose(root->world, posed));
        refreshSubtree(crank, RE::NiPoint3{});
        (void)scale;
    }

    // The reload button opens and closes the gun. Open, with the muzzle up, everything falls out of it. Rounds
    // come from the belt in the free hand (a full clip for a revolver, one shell at a time for a break action)
    // and go in at the breech. The free hand on the gun, or a jerk of the gun, closes it.
    void updateBreech(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now, float dt) noexcept
    {
        auto& st = g_state;
        auto& b = st.breech;
        const auto& spare = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        hideRound();
        looseMagazines(nullptr, nullptr);
        rocklink::Frame link{};
        link.gun = true;
        link.freeHand = freeIndex();
        link.magazine.node = kMovingParts[0];
        link.bolt.node = kMovingParts[1];
        if (b.drawn) {
            link.magazine.drive = true;   // ROCK's collision of the opening part goes where it is drawn
            link.magazine.parentLocal = toLinkPose(b.drawnLocal);
        }
        g_rock = rocklink::frame(link);

        auto* mag = findNamed(root, kMovingParts[0]);
        if (!looksLikeObject(mag) || !looksLikeObject(mag->parent)) {
            return;
        }
        const bool revolver = b.mech == 1;
        const bool single = b.mech == 3;   // a mini nuke / a missile: nothing is left in it after the shot
        const bool played = gun.weapon && loadPlayed(gun.weapon->GetFormID());
        const bool zoneHinge = !played && st.zoneHinge.arc;   // no recording: the turn drawn in Blender
        if (zoneHinge) {
            g_play.axis = st.zoneHinge.axis;
            g_play.pivot = st.zoneHinge.pivot;
            g_play.openAngle = st.zoneHinge.angle;
        }
        const bool hinged = played || zoneHinge;
        std::array<RE::NiAVObject*, 5> child{};
        for (int i = 0; i < 5; ++i) {
            char name[32]{};
            std::snprintf(name, sizeof(name), "WeaponMagazineChild%d", i + 1);
            child[i] = findNamed(mag, name);
        }
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        const bool reloadStarted = gunState == reloading && st.lastGunState != reloading;
        if (gunState != st.lastGunState) {
            detail("gunState %u -> %u", st.lastGunState, gunState);
        }
        if (reloadStarted) {
            b.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        }
        st.lastGunState = gunState;
        const auto capacity = std::max(capacityOf(gun), 1u);
        const float scale = gunScale(root);

        if (!st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
            st.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        // the closed pose
        if (!b.known) {
            const bool idle = gunState == static_cast<std::uint32_t>(g_settings.idleGunState) && now - st.equippedAt > std::chrono::milliseconds(700);
            if (!idle) {
                return;
            }
            b.rest = mag->local;
            b.kids.clear();
            forEachNode(mag, [&](RE::NiAVObject* object, int depth) {
                if (depth > 0 && object->IsNode() && b.kids.size() < 40) {
                    b.kids.emplace_back(object, object->local);
                }
                return true;
            });
            b.known = true;
            b.restInRoot = relative(root->world, mag->world);
            if (st.leverKnown) {
                if (auto* bolt = findNamed(root, "WeaponBolt"); looksLikeObject(bolt) && looksLikeObject(bolt->parent)) {
                    b.leverRestLocal = bolt->local;
                    b.leverRestInRoot = relative(root->world, bolt->world);
                    b.leverRestKnown = true;
                }
            }
            if (!st.zonesChecked && gun.weapon) {
                st.zonesChecked = true;
                zonesFor(gun.weapon->GetFormID(), false);
            }
            b.live = gun.data->ammoCount;
            b.lastAmmo = b.live;
            note("Breech: closed pose remembered (%zu nodes below), %u of %u rounds in it", b.kids.size(), b.live, capacity);
        }

        // the game's own reload: its animation moves the opening part off where it stands closed (in VR the gun's
        // state does not always say "reloading"), or at the latest the game refills the gun
        bool gameReloaded = false;
        if (!b.open && b.t <= 0.0f && gunState != 0 && now - b.toggled > std::chrono::milliseconds(800) && now >= b.freezeUntil) {
            const float moved = distance(mag->local.translate, b.rest.translate);
            const float turned = 1.0f - std::min({ dot(axisOf(mag->local, 0), axisOf(b.rest, 0)), dot(axisOf(mag->local, 1), axisOf(b.rest, 1)),
                                                 dot(axisOf(mag->local, 2), axisOf(b.rest, 2)) });
            if (moved > 0.3f || turned > 0.003f) {
                gameReloaded = true;
                detail("The game moves the opening part (%.2f off, turn %.4f): its reload began", moved, turned);
            }
        }

        // the rounds
        auto ammo = gun.data->ammoCount;
        const bool leverOut = st.leverKnown && b.lever > 0.05f;
        if (b.open) {
            if (ammo != 0) {
                gun.data->ammoCount = 0;   // an open gun does not fire, and the game's own refill is undone
            }
            b.lastAmmo = 0;
        } else if (leverOut) {
            if (ammo != 0) {
                gun.data->ammoCount = 0;   // the handle is pulled out: no shot until it is put back
            }
            b.leverBlocked = true;
        } else {
            if (b.leverBlocked) {
                b.leverBlocked = false;   // the handle is home again: the round in it can be fired
                gun.data->ammoCount = b.live;
                ammo = b.live;
                b.lastAmmo = b.live;
            }
            if (ammo > b.lastAmmo && g_settings.blockReload) {
                note("Game refill %u -> %u reverted", b.lastAmmo, ammo);
                ammo = b.lastAmmo;
                gun.data->ammoCount = ammo;
                gameReloaded = true;
            } else if (ammo < b.lastAmmo) {
                b.spent = single ? 0u : std::min(b.spent + (b.lastAmmo - ammo), capacity - std::min(ammo, capacity));
                detail("Shot: %u rounds and %u fired cases in the gun", ammo, b.spent);
            }
            b.live = ammo;
            b.lastAmmo = ammo;
        }
        st.lastAmmo = gun.data->ammoCount;

        const auto close = [&](const char* how) {
            b.open = false;
            b.held = 0;
            b.toggled = now;
            gun.data->ammoCount = b.live;
            b.lastAmmo = b.live;
            st.lastAmmo = b.live;
            playSound(3);
            buzz(g_settings.gunHand, 2000);
            note("CLOSE (%s): %u rounds, %u fired cases", how, b.live, b.spent);
        };

        // the reload button: a tap opens or closes
        const bool buttonNow = (firing.buttons & buttonMask(g_settings.reloadButton)) != 0;
        const bool buttonBefore = (firing.previousButtons & buttonMask(g_settings.reloadButton)) != 0;
        if (buttonNow && !buttonBefore) {
            st.buttonSince = now;
        }
        if (!buttonNow && buttonBefore && now - st.buttonSince < std::chrono::milliseconds(600) && now - b.toggled > std::chrono::milliseconds(400) &&
            !menuOpen()) {
            if (b.open) {
                close("the button");
            } else {
                b.open = true;
                b.opened = now;
                b.toggled = now;
                gun.data->ammoCount = 0;
                b.lastAmmo = 0;
                playSound(1);
                buzz(g_settings.gunHand, 1200);
                note("OPEN: %u rounds, %u fired cases", b.live, b.spent);
            }
        } else if ((reloadStarted || gameReloaded) && !b.open && b.known) {
            // the game's own reload began (an empty gun reloads by itself): held shut, the hand opens it
            if (now >= b.freezeUntil) {
                note("The game's reload of the closed gun is held still: open it by hand");
            }
            b.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(std::max(g_settings.freezeSeconds, 1.0f) * 1000.0f));
        }
        // a break action's barrels (or a launcher's breech) hang on a hinge: gravity and the gun's own jerks swing them;
        // they fall open freely, and only a pull shut stronger than fHingeHold (a flick) carries them shut
        const bool swinging = hinged && !revolver && !st.leverKnown && b.open && b.known && g_play.openAngle > 0.1f;
        if (!b.driving && !b.pushing && swinging) {
            const float range = g_play.openAngle;
            float theta = b.t * range;
            RE::NiPoint3 heavy = b.restInRoot.translate + RE::NiPoint3{ 0.0f, 10.0f, 0.0f };
            for (const char* name : { "barrels", "breech" }) {
                if (const auto it = st.zoneBoxes.find(name); it != st.zoneBoxes.end()) {
                    heavy = (it->second.first + it->second.second) * 0.5f;
                    break;
                }
            }
            const auto arm = rotateAbout(heavy - g_play.pivot, g_play.axis, theta);
            const float arm2 = std::max(dot(arm, arm), 4.0f);
            b.accel = b.accel * 0.75f + (b.velocity - b.lastVelocity) * (0.25f / dt);
            RE::NiPoint3 accel = b.accel;
            const float a2 = dot(accel, accel);
            if (a2 > 6000.0f * 6000.0f) {
                accel = accel * (6000.0f / std::sqrt(a2));
            }
            const RE::NiPoint3 pull = RE::NiPoint3{ 0.0f, 0.0f, -686.0f } - accel;   // gravity, and the gun moving away under it
            const RE::NiPoint3 pullInGun{ dot(axisOf(root->world, 0), pull), dot(axisOf(root->world, 1), pull), dot(axisOf(root->world, 2), pull) };
            float alpha = dot(cross(arm, pullInGun), g_play.axis) / arm2;
            if (g_settings.hingeHold > 0.0f && alpha < 0.0f && alpha > -g_settings.hingeHold && b.omega > -0.5f) {
                alpha = 0.0f;   // the hinge's stiffness holds them open
            }
            b.omega = (b.omega + alpha * dt) * std::exp(-1.5f * dt);
            theta += b.omega * dt;
            if (theta >= range) {
                theta = range;
                if (b.omega > 0.0f) {
                    if (b.omega > 2.0f) buzz(g_settings.gunHand, 500);
                    b.omega = -b.omega * 0.2f;   // a small bounce at the stop
                }
            }
            if (theta <= 0.03f * range) {
                if (b.omega < -2.5f && now - b.opened > std::chrono::milliseconds(400)) {
                    theta = 0.0f;
                    b.t = 0.0f;
                    b.omega = 0.0f;
                    close("swung shut");
                } else {
                    theta = 0.03f * range;
                    b.omega = std::max(b.omega, 0.0f);
                }
            }
            if (b.open) {
                b.t = theta / range;
            }
        } else if (!b.driving && !b.pushing) {
            b.t = approach(b.t, b.open ? 1.0f : 0.0f, 7.0f * dt);
            b.omega = 0.0f;
        } else {
            b.omega = 0.0f;
        }
        b.lastVelocity = b.velocity;

        // the pose: ours while it is open or moving, and held still while the game plays its own reload
        if (b.t > 0.0f || gunState == reloading || now < b.freezeUntil) {
            mag->local = b.rest;
            if (b.t > 0.0f) {
                const float eased = b.t * b.t * (3.0f - 2.0f * b.t);
                if (!hinged) {
                    turnRows(mag->local.rotate, revolver ? 1 : 0, (revolver ? g_settings.cylinderSwing : g_settings.breakAngle) * 0.01745329f * eased);
                }
            }
            for (auto& [object, local] : b.kids) {
                if (looksLikeObject(object)) {
                    object->local = local;
                }
            }
            refreshSubtree(mag, RE::NiPoint3{});
            if (played) {
                const float eased = (b.driving || swinging) ? b.t : b.t * b.t * (3.0f - 2.0f * b.t);
                posePlayed(root, eased);
            } else if (zoneHinge && b.t > 0.0f) {
                // turned about the hinge drawn in Blender, from where it stands closed
                const float eased = (b.driving || swinging) ? b.t : b.t * b.t * (3.0f - 2.0f * b.t);
                const auto posed = turnedPose(b.restInRoot, st.zoneHinge.axis, st.zoneHinge.pivot, st.zoneHinge.angle * eased, RE::NiPoint3{});
                mag->local = relative(mag->parent->world, compose(root->world, posed));
                refreshSubtree(mag, RE::NiPoint3{});
            }
            b.drawnLocal = mag->local;
            b.drawn = b.t > 0.0f;
        } else {
            b.drawn = false;
        }

        // the gun's speed against the player (a jerk closes it)
        const auto at = root->world.translate - player->GetPosition();
        if (b.prevKnown) {
            b.velocity = b.velocity * 0.5f + (at - b.prevAt) * (0.5f / dt);
        }
        b.prevAt = at;
        b.prevKnown = true;

        const auto barrel = axisOf(mag->world, 1);   // the cylinder's / the barrels' own line, forward
        const auto breechNode = looksLikeObject(child[0]) ? child[0] : mag;
        const auto breechAt = breechNode->world.translate - (revolver ? barrel * (2.0f * scale) : RE::NiPoint3{});
        const bool fullyOpen = b.open && b.t >= (swinging ? 0.6f : 0.95f);   // hanging barrels need not be all the way down
        // zones (Blender): the palm against the opening part, taken back to where the part stands closed
        const auto partZone = [&]() -> const std::pair<RE::NiPoint3, RE::NiPoint3>* {
            for (const char* name : { "cylinder", "barrels", "breech" }) {
                const auto it = st.zoneBoxes.find(name);
                if (it != st.zoneBoxes.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        }();
        const auto palmOnPart = [&](const RE::NiPoint3& palmWorld) {
            const auto inPart = relative(mag->world, pointTransform(palmWorld));
            return compose(b.restInRoot, inPart).translate;
        };
        // the whole hand, not one point: wrist, palm, fingers, fingertips
        const auto fingers = spare.palm - spare.wrist.translate;
        const std::array<RE::NiPoint3, 4> handPoints{ spare.wrist.translate, spare.palm, spare.palm + fingers * 0.8f, spare.palm + fingers * 1.5f };
        const auto handIn = [&](const std::pair<RE::NiPoint3, RE::NiPoint3>& box, float margin, RE::NiPoint3* where) {
            for (const auto& point : handPoints) {
                const auto at = palmOnPart(point);
                if (inBox(box.first, box.second, at, margin)) {
                    if (where) *where = at;
                    return true;
                }
            }
            return false;
        };
        const auto touchesPart = [&](float fallback) {
            if (!spare.valid) {
                return false;
            }
            if (partZone) {
                return handIn(*partZone, g_settings.contactMargin, nullptr);
            }
            return distance(spare.palm, mag->world.translate) < fallback * scale;
        };

        // the latch (zones file: box latch): a closed break action opens only after the free hand pressed it;
        // until then a hand on the barrels just holds the gun (ROCK's two-handed hold)
        const auto latchBox = st.zoneBoxes.find("latch");
        const bool hasLatch = latchBox != st.zoneBoxes.end() && !revolver;
        if (hasLatch && !b.open && spare.valid && handIn(latchBox->second, g_settings.contactMargin, nullptr)) {
            if (now - b.unlatchedAt > std::chrono::milliseconds(1500)) {
                buzz(freeIndex(), 700);
                detail("The latch is pressed");
            }
            b.unlatchedAt = now;
        }
        const bool unlatched = (!hasLatch || now - b.unlatchedAt < std::chrono::milliseconds(1500)) && (!st.leverKnown || b.lever > 0.9f);

        // the handle (zones file "route lever", the Fat Man): pulled back along its way it opens the breech,
        // pushed home it closes it; while it is out the gun does not fire
        bool leverTouch = false;
        if (st.leverKnown && b.leverRestKnown && b.known) {
            auto* bolt = findNamed(root, "WeaponBolt");
            const auto palmInRoot = spare.valid ? relative(root->world, pointTransform(spare.palm)).translate : RE::NiPoint3{};
            const auto handleAt = alongWay(st.leverPts, st.leverAt, b.lever);
            leverTouch = spare.valid && b.held == 0 && distance(palmInRoot, handleAt) < 6.0f;
            const bool grab = spare.valid && spare.grip && !spare.previousGrip;
            if (grab && griphook::owned() && leverTouch && !b.driving) {
                b.leverHeld = true;
                buzz(freeIndex(), 600);
                detail("The free hand takes the handle at %.2f", b.lever);
            }
            if (b.leverHeld && !(spare.valid && spare.grip)) {
                b.leverHeld = false;
                detail("The handle is let go at %.2f", b.lever);
            }
            if (b.leverHeld) {
                float away = 0.0f;
                const float want = nearestOnWay(st.leverPts, st.leverAt, palmInRoot, &away);
                const float before = b.lever;
                b.lever = std::clamp(want, before - 0.12f, before + 0.12f);
                if (b.lever >= 0.95f && before < 0.95f) {
                    buzz(freeIndex(), 900);
                    if (!b.open) {
                        b.open = true;   // the handle all the way back: the breech opens
                        b.opened = now;
                        b.toggled = now;
                        gun.data->ammoCount = 0;
                        b.lastAmmo = 0;
                        playSound(1);
                        buzz(g_settings.gunHand, 1200);
                        note("OPEN (the handle): %u rounds", b.live);
                    }
                }
                if (b.lever <= 0.02f && before > 0.02f) {
                    b.lever = 0.0f;
                    buzz(freeIndex(), 900);
                    playSound(3);
                    note("The handle is home: %u round(s), ready to fire", b.live);
                }
            }
            if (b.leverHeld && b.open && b.lever < 0.5f && !b.driving && b.held == 0) {
                close("the handle pushed forward");
            }
            if (looksLikeObject(bolt) && looksLikeObject(bolt->parent)) {
                if (b.lever > 0.0f) {
                    const auto& fit = st.leverFit;
                    const float turnShare = fit.arc ? std::clamp(b.lever / std::max(fit.arcAt, 0.01f), 0.0f, 1.0f) : 0.0f;
                    const float straightFrom = fit.arc ? fit.arcAt : 0.0f;
                    const RE::NiPoint3 lin = b.lever > straightFrom ? alongWay(st.leverPts, st.leverAt, b.lever) - alongWay(st.leverPts, st.leverAt, straightFrom)
                                                                    : RE::NiPoint3{};
                    const auto posed = fit.arc ? turnedPose(b.leverRestInRoot, fit.axis, fit.pivot, fit.angle * turnShare, lin)
                                               : turnedPose(b.leverRestInRoot, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, RE::NiPoint3{}, 0.0f, lin);
                    bolt->local = relative(bolt->parent->world, compose(root->world, posed));
                    refreshSubtree(bolt, RE::NiPoint3{});
                    b.leverDrawn = true;
                } else if (b.leverDrawn) {
                    bolt->local = b.leverRestLocal;
                    refreshSubtree(bolt, RE::NiPoint3{});
                    b.leverDrawn = false;
                }
            }
        }

        // the ejector rod (zones file: box + route "ejector"): pushed along its way by the hand, it throws out what is in
        const auto ejectBox = st.zoneBoxes.find("ejector");
        const auto ejectWay = st.zoneRoutes.find("ejector");
        if (fullyOpen && spare.valid && ejectBox != st.zoneBoxes.end() && ejectWay != st.zoneRoutes.end() && ejectWay->second.size() >= 2) {
            const auto& box = ejectBox;
            const auto& way = ejectWay;
            RE::NiPoint3 rod{};
            const bool touch = handIn(box->second, 1.5f, &rod);
            if (touch) {
                const auto run = way->second.back() - way->second.front();
                const float length = std::sqrt(dot(run, run));
                if (b.ejectTouch && length > 0.1f) {
                    b.ejectPushed = std::max(0.0f, b.ejectPushed + dot(rod - b.ejectPrev, run * (1.0f / length)));
                    if (b.ejectPushed > 0.6f * length && b.live + b.spent > 0) {
                        note("EJECTOR: pushed %.1f of %.1f: %u rounds back to the pouch, %u fired cases out", b.ejectPushed, length, b.live, b.spent);
                        if (b.live > 0) {
                            hud("Патроны выброшены и вернулись в запас");
                        }
                        b.live = 0;
                        b.spent = 0;
                        b.ejectPushed = 0.0f;
                        playSound(4);
                        buzz(freeIndex(), 1500);
                        buzz(g_settings.gunHand, 1500);
                    }
                }
                b.ejectPrev = rod;
            } else {
                b.ejectPushed = 0.0f;
            }
            b.ejectTouch = touch;
        }

        // muzzle up: everything falls out
        // (held up for a moment and fairly still: a jerk to shut it does not throw them out on the way)
        const bool muzzleUp = barrel.z > (revolver ? 0.5f : 0.3f) && dot(b.velocity, b.velocity) < 90.0f * 90.0f * scale * scale;
        if (!muzzleUp) {
            b.upSince = now;
        }
        if (!single && fullyOpen && b.live + b.spent > 0 && muzzleUp && now - b.upSince > std::chrono::milliseconds(250) &&
            now - b.opened > std::chrono::milliseconds(350)) {
            note("EMPTIED: %u rounds back to the pouch, %u fired cases gone", b.live, b.spent);
            if (b.live > 0) {
                hud("Патроны высыпались и вернулись в запас");
            }
            b.live = 0;
            b.spent = 0;
            playSound(4);
            buzz(g_settings.gunHand, 1500);
        }

        // the free hand
        const bool press = spare.valid && spare.grip && !spare.previousGrip;
        const bool atBelt = spare.valid && inBelt(player, spare.palm);
        const float toGun = spare.valid ? distance(spare.palm, breechAt) : 1e9f;
        if (press && griphook::owned() && !b.open && hinged && unlatched && b.held == 0 && touchesPart(14.0f)) {
            b.driving = true;   // the closed gun taken by its opening part: turning it opens the gun
            b.driveAngle0 = hingeAngle(root, spare.palm);
            b.driveT0 = 0.0f;
            buzz(freeIndex(), 600);
            detail("The free hand takes the closed opening part");
        }
        if (press && griphook::owned() && b.open) {
            if (atBelt && b.held == 0) {
                const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;
                const auto have = inventory > b.live ? inventory - b.live : 0u;
                const auto room = capacity - std::min(capacity, b.live + b.spent);
                const auto take = std::min(have, revolver ? room : std::min(room, 1u));
                if (b.spent > 0 && room == 0) {
                    hud("Сначала вытряхни гильзы: подними ствол вверх");
                } else if (take == 0) {
                    hud(have == 0 ? "Патронов нет" : "Заряжено полностью");
                } else {
                    b.held = take;
                    playSound(4);
                    buzz(freeIndex(), 1200);
                    note("BELT: %u round(s) in the free hand", take);
                }
            } else if (hinged && !atBelt && b.held == 0 && touchesPart(14.0f)) {
                b.driving = true;
                b.driveAngle0 = hingeAngle(root, spare.palm);
                b.driveT0 = b.t;
                buzz(freeIndex(), 800);
                detail("The free hand takes the opening part at %.2f open", b.t);
            } else if (!hinged && !atBelt && b.held == 0 && fullyOpen && toGun < 12.0f * scale) {
                close("the free hand");
            }
        }
        if (b.driving) {
            if (!(spare.valid && spare.grip)) {
                b.driving = false;
                if (b.open && b.t < 0.3f) {
                    close("pushed shut by hand");
                }
            } else {
                float turn = hingeAngle(root, spare.palm) - b.driveAngle0;
                while (turn > 3.14159265f) turn -= 6.2831853f;
                while (turn < -3.14159265f) turn += 6.2831853f;
                const float before = b.t;
                b.t = std::clamp(b.driveT0 + turn / std::max(g_play.openAngle, 0.1f), 0.0f, 1.0f);
                if (!b.open && b.t > 0.15f) {
                    b.open = true;   // turned open by hand
                    b.opened = now;
                    b.toggled = now;
                    gun.data->ammoCount = 0;
                    b.lastAmmo = 0;
                    playSound(1);
                    buzz(g_settings.gunHand, 1200);
                    note("OPEN (by hand): %u rounds, %u fired cases", b.live, b.spent);
                } else if (b.open && b.t <= 0.05f && before > 0.05f) {
                    b.driving = false;
                    close("pushed shut by hand");
                } else if (b.t >= 0.98f && before < 0.98f) {
                    buzz(freeIndex(), 600);
                }
            }
        }
        // the open palm pushes it shut (no grip needed): only a move towards closed carries it
        const bool touching = hinged && b.open && !b.driving && b.held == 0 && spare.valid && !spare.grip && !atBelt &&
                              touchesPart(8.0f);
        if (touching) {
            const float angle = hingeAngle(root, spare.palm);
            if (b.pushing) {
                float turn = angle - b.pushAngle;
                while (turn > 3.14159265f) turn -= 6.2831853f;
                while (turn < -3.14159265f) turn += 6.2831853f;
                if (turn < 0.0f) {
                    b.t = std::clamp(b.t + turn / std::max(g_play.openAngle, 0.1f), 0.0f, 1.0f);
                    b.opened = std::min(b.opened, now);
                    if (b.t <= 0.08f) {
                        b.pushing = false;
                        close("pushed shut by the palm");
                    }
                }
            }
            b.pushAngle = angle;
            b.pushing = b.open;
        } else {
            if (b.pushing && b.open && b.t < 0.5f) {
                close("pushed shut by the palm");
            }
            b.pushing = false;
        }
        if (b.held > 0 && !(spare.valid && spare.grip)) {
            note("The rounds in the hand are put away");
            b.held = 0;
        }
        bool atChamber = toGun < g_settings.breechRadius * scale;
        if (single && spare.valid) {
            atChamber = distance(relative(root->world, pointTransform(spare.palm)).translate, st.breechMouth) < st.breechReach;
        } else if (!st.chambers.empty() && spare.valid) {
            const auto palm = palmOnPart(spare.palm);
            float nearest = 1e9f;
            for (const auto& c : st.chambers) {
                nearest = std::min(nearest, distance(palm, c));
            }
            atChamber = nearest < g_settings.chamberReach;
        }
        if (b.held > 0 && fullyOpen && atChamber) {
            b.live += b.held;
            note("LOAD: %u round(s) in, %u of %u in the gun", b.held, b.live, capacity);
            b.held = 0;
            playSound(0);
            buzz(0, 2000);
            buzz(1, 2000);
            griphook::setOwned(false);
        }
        // a flick of the wrist: a revolver rolled sharply about its barrel, a break action's muzzle snapped up
        const auto forwardNow = axisOf(root->world, 1);
        const auto sideNow = axisOf(root->world, 0);
        float flick = 0.0f;
        if (b.turnKnown && dt > 1e-4f) {
            flick = revolver ? std::fabs(sideNow.z - b.prevSide.z) / dt : (forwardNow.z - b.prevForward.z) / dt;
        }
        b.prevForward = forwardNow;
        b.prevSide = sideNow;
        b.turnKnown = true;
        if (hasLatch && !b.open && !b.driving && unlatched && now - b.unlatchedAt < std::chrono::milliseconds(1500) && b.unlatchedAt != Clock::time_point{} &&
            -flick > g_settings.flickTurn) {
            b.open = true;   // the latch pressed and the muzzle snapped down: the barrels fall open
            b.t = 0.05f;
            b.omega = 4.0f;
            b.opened = now;
            b.toggled = now;
            gun.data->ammoCount = 0;
            b.lastAmmo = 0;
            playSound(1);
            buzz(g_settings.gunHand, 1200);
            note("OPEN (latch and a flick down, %.1f/s): %u rounds, %u fired cases", -flick, b.live, b.spent);
        }
        if (!swinging && fullyOpen && b.held == 0 && !b.driving && now - b.opened > std::chrono::milliseconds(600)) {
            const float jerk = revolver ? std::fabs(dot(b.velocity, axisOf(root->world, 0))) : b.velocity.z;
            if ((jerk > g_settings.flickSpeed * scale || flick > g_settings.flickTurn) && !(barrel.z > 0.3f && b.live + b.spent > 0)) {
                note("Flick: moved %.0f, turned %.1f/s", jerk / std::max(scale, 0.01f), flick);
                close("a flick of the gun");
            }
        }

        // what is seen in it, and the rounds carried in the hand
        const auto show = [&](int index, bool on) {
            if (looksLikeObject(child[index])) {
                setCulled(child[index], !on);
            }
        };
        const std::uint32_t filled = b.live + b.spent;
        // a mini nuke / a missile is a node of the gun's own model (CNuke / CMissile): seen only when it is in or in the hand
        RE::NiAVObject* bigRound = single ? findNamed(root, "CNuke") : nullptr;
        if (single && !looksLikeObject(bigRound)) {
            bigRound = findNamed(root, "CMissile");
        }
        if (single) {
            if (looksLikeObject(bigRound)) {
                setCulled(bigRound, !(b.live > 0 || b.held > 0));
            }
        } else if (revolver) {
            show(4, b.live > 0 || b.held > 0);   // the bullets
            show(3, filled > 0);                 // the cases
            show(2, b.held > 0);                 // the clip they come on
        } else {
            show(0, filled >= 1 || (b.held > 0 && filled == 0));
            show(1, filled >= 2 || (b.held > 0 && filled == 1));
        }
        if (b.held > 0 && spare.valid) {
            const auto y = normalized(axisOf(spare.wrist, 0));   // along the fingers
            auto z = axisOf(spare.wrist, 2);
            z = normalized(z - y * dot(z, y));
            RE::NiTransform pose = identityTransform();
            setRows(pose, cross(y, z), y, z);
            pose.scale = scale;
            pose.translate = spare.palm + y * ((revolver ? 2.0f : 0.0f) * scale);
            if (single) {
                if (looksLikeObject(bigRound)) {
                    pose.translate = spare.palm + y * (6.0f * scale);   // held by its tail, the warhead ahead of the fingers
                    placeNode(bigRound, pose);
                }
            } else if (revolver) {
                placeNode(child[2], pose);
                placeNode(child[4], pose);
            } else {
                placeNode(child[filled == 0 ? 0 : 1], pose);
            }
        }

        // an open gun: the free hand's grip near it or at the belt is this plugin's (no second-hand hold, no ROCK grab)
        if (!spare.grip) {
            const bool mine = spare.valid && ((b.open && (atBelt || distance(spare.palm, root->world.translate) < g_settings.gunRadius)) ||
                                              (!b.open && hinged && unlatched && touchesPart(0.0f)) ||   // closed: only on the opening part, unlatched
                                              leverTouch);
            st.armReason = mine ? (atBelt ? kArmBelt : kArmBlock) : kArmNone;
            griphook::setArmed(mine);
        }
    }

    // ------------------------------------------------------------ what the plugin thinks, for the log

    const char* placeName(int place) noexcept
    {
        switch (place) {
        case kSeated: return "in the gun";
        case kRailAuto: return "sticking out by itself";
        case kRailHand: return "in the well, moved by the hand";
        case kHand: return "drawn in the hand (real one on its way)";
        case kGone: return "no magazine in the gun";
        default: return "?";
        }
    }

    std::string nodeState(RE::NiAVObject* node) noexcept
    {
        if (!looksLikeObject(node)) {
            return "missing";
        }
        int hiddenUp = 0;
        for (auto* at = node->parent; looksLikeObject(at) && hiddenUp < 12; at = at->parent) {
            if (at->flags.flags & 1ull) {
                char text[96];
                std::snprintf(text, sizeof(text), "HIDDEN by parent '%s'", nodeName(at).c_str());
                return text;
            }
            ++hiddenUp;
        }
        return (node->flags.flags & 1ull) ? "HIDDEN" : "shown";
    }

    // One line per thing: the magazine, the slide, the free hand, the real magazines around.
    void writeState(const char* why, const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* magazine, RE::NiAVObject* bolt) noexcept
    {
        const auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        note("[%s] magazine: %s, %.1f out of %.1f%s%s, %u rounds; gun ammoCount %u, chamber %s", why, placeName(st.place), st.s, st.length,
            st.autoHold ? ", held out by the button" : "", st.resting ? ", resting half in" : "", st.rounds, gun.data ? gun.data->ammoCount : 0u,
            st.chamber ? "loaded" : "empty");
        if (looksLikeObject(magazine) && looksLikeObject(root)) {
            const auto at = inGun(root, magazine->world.translate);
            const auto want = st.magRest.translate + st.axis * st.s;
            note("[%s]   magazine node %s, at (%.1f %.1f %.1f) in the gun, %.1f from where it should be", why, nodeState(magazine).c_str(), at.x, at.y,
                at.z, distance(at, want));
        } else {
            note("[%s]   magazine node missing", why);
        }
        if (looksLikeObject(bolt) && looksLikeObject(root)) {
            const auto at = inGun(root, bolt->world.translate);
            note("[%s] slide: pulled %.2f of %.1f%s%s%s%s, node %s at (%.1f %.1f %.1f)", why, st.rackPull, g_settings.rackTravel,
                st.racking ? ", TAKEN by the grip" : "", st.pushing ? ", pushed by the open palm" : "", st.slideLocked ? ", locked open" : "",
                st.boltAction ? ", bolt action" : "", nodeState(bolt).c_str(), at.x, at.y, at.z);
        }
        const bool onMag = looksLikeObject(root) && palmOnMagazine(root, free.palm);
        note("[%s] free hand: %s, grip %s (%s), palm on the magazine %d, wants %d, ROCK holds %08X (on magazine %d, on slide %d, on gun %d)", why,
            free.valid ? "tracked" : "NOT TRACKED", free.grip ? "closed" : "open", griphook::owned() ? "hidden from ROCK" : "seen by ROCK", onMag ? 1 : 0,
            st.armReason, rocklink::heldBy(freeIndex()), g_rock.onMagazine ? 1 : 0, g_rock.onBolt ? 1 : 0, g_rock.onGun ? 1 : 0);
        if (st.handToken || st.handRef) {
            note("[%s] real magazine for the hand: token %d (spare %d, let go %d), ref %08X, ROCK took it %d", why, st.handToken, st.handSpare ? 1 : 0,
                st.handTokenDropped ? 1 : 0, st.handRef, st.grabDone ? 1 : 0);
        }
        for (const auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            note("[%s] real magazine %08X: %u rounds, %s, 3D %s, %.0f from the free hand", why, id, loose.rounds,
                !ref ? "GONE" : (ref->IsDisabled() ? "DISABLED" : "enabled"), looksLikeObject(mag3D) ? nodeState(mag3D).c_str() : "NONE",
                looksLikeObject(mag3D) && free.valid ? distance(mag3D->world.translate, free.palm) : -1.0f);
        }
    }

    // A short fingerprint of the state: when it changes the whole state is written down.
    std::string stateKey() noexcept
    {
        const auto& st = g_state;
        char text[160];
        std::snprintf(text, sizeof(text), "%d|%d|%d|%u|%d|%d|%d|%d|%d|%d|%d|%u|%d", st.place, st.resting ? 1 : 0, st.autoHold ? 1 : 0, st.rounds,
            st.chamber ? 1 : 0, st.racking ? 1 : 0, st.slideLocked ? 1 : 0, st.rackPull > 0.5f ? 1 : 0, st.rackPull > g_settings.rackTravel - 0.5f ? 1 : 0,
            g_rock.onMagazine ? 1 : 0, g_rock.onBolt ? 1 : 0, st.handRef, st.grabDone ? 1 : 0);
        return text;
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
            int kind = gun.weapon && gun.data ? magazineKindOf(player, gun.weapon, magModel) : -1;
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kLaserGun) {
                kind = kLaserCellKind;   // the laser gun has no magazine mod: its cell is the magazine
                magModel = "Weapons\\LaserWeapons\\FusionCell.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kGatlingLaser) {
                kind = kFusionCoreKind;   // the gatling laser: its fusion core is the magazine
                magModel = "Ammo\\FusionCore\\FusionCore.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kMinigun) {
                kind = kMinigunKind;   // the minigun: its drum comes off and a new one goes on
                magModel = "Ammo\\5mm\\5mmAmmo.nif";
            }
            const bool pistol10 = gun.weapon && gun.data && gun.weapon->GetFormID() == k10mmPistol;
            st.managed = gun.weapon && gun.data && (pistol10 || (kind >= 0 && g_settings.allGuns));
            st.equippedAt = now;
            const auto total = gun.data ? gun.data->ammoCount : 0u;
            st.chamber = total > 0;
            st.rounds = total > 0 ? total - 1 : 0;
            st.kind = kind >= 0 ? kind : (capacityOf(gun) > 12 ? 1 : 0);
            st.cellFed = kind == kLaserCellKind || kind == kSyringerKind || kind == kFusionCoreKind || kind == kMinigunKind;   // nothing to rack
            note("Equipped weapon %08X, capacity %u, rounds %u, magazine '%s' (kind %d): %s", gun.weapon ? gun.weapon->GetFormID() : 0u,
                capacityOf(gun), total, magModel.c_str(), kind, st.managed ? "reloaded by hand" : "not handled (no magazine of group 1)");
            if (gun.weapon) {
                loadZoneRoutes(gun.weapon->GetFormID());
            }
        }

        const bool unrecorded = g_settings.autoRecord && gun.weapon && gun.data && gun.data->ammo && !hasRecording(gun.weapon->GetFormID()) && !st.zoneHinge.arc && !st.hasZonesFile;
        if (g_settings.recordAnim || unrecorded || g_rec.active) {
            static Clock::time_point toldAt{};
            if (now - toldAt > std::chrono::seconds(10)) {
                toldAt = now;
                note("IDLE: recording mode (bRecordAnim %d, not recorded yet %d, a take running %d): the game reloads by itself",
                    g_settings.recordAnim ? 1 : 0, unrecorded ? 1 : 0, g_rec.active ? 1 : 0);
            }
            if (unrecorded && !st.recordTold) {
                st.recordTold = true;
                hud("Этот ствол ещё не записан: выстрели хотя бы раз и перезаряди как обычно, мод запишет анимацию");
                note("RECORD: %08X has no recording yet, the next reload is the game's own and is written down", gun.weapon->GetFormID());
            }
            releaseHand();
            griphook::setArmed(false);
            recordReload(player, gun, now);
            return;
        }

        if (!st.managed && !st.breech.checked && g_settings.breech && gun.weapon && gun.data) {
            if (auto* drawnRoot = weaponRoot(player); looksLikeObject(drawnRoot)) {
                st.breech.checked = true;
                bool cylinder = false;
                bool shells = false;
                forEachNode(drawnRoot, [&](RE::NiAVObject* object, int) {
                    const auto name = nodeName(object);
                    cylinder = cylinder || name.find("Cylinder") != std::string::npos;
                    shells = shells || name.find("ShotgunShell") != std::string::npos || name.find("BarrelCatch") != std::string::npos;
                    return true;
                });
                st.breech.mech = cylinder ? 1 : (shells ? 2 : (st.singleShot && st.zoneHinge.arc ? 3 : 0));
                if (st.breech.mech != 0 && looksLikeObject(findNamed(drawnRoot, kMovingParts[0]))) {
                    st.managed = true;
                    note("This gun opens: %s, reloaded by hand",
                        st.breech.mech == 1 ? "a revolver (the cylinder swings out)" : (st.breech.mech == 2 ? "a break action" : "one big round at the breech"));
                } else {
                    st.breech.mech = 0;
                }
            }
        }
        if (!st.managed && st.crankKnown && g_settings.breech && gun.weapon && gun.data) {
            if (auto* crankRoot = weaponRoot(player); looksLikeObject(crankRoot)) {
                updateCrank(player, gun, crankRoot, now);
                return;
            }
        }
        auto* root = st.managed ? weaponRoot(player) : nullptr;
        if (st.managed && st.breech.mech != 0 && looksLikeObject(root)) {
            updateBreech(player, gun, root, now, dt);
            return;
        }
        if (!st.managed || !looksLikeObject(root)) {
            if (st.managed && !st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
                st.dumped = true;   // why nothing happens: the drawn gun has no WeaponMagazine node where it is looked for
                auto* fp = player->firstPerson3D.get();
                note("IDLE: the gun is reloaded by hand but its model is not found (first person 3D %s, third person %s); nodes named Weapon*:",
                    looksLikeObject(fp) ? "present" : "MISSING", looksLikeObject(player->Get3D()) ? "present" : "MISSING");
                int shown = 0;
                RE::NiAVObject* tops[2]{ fp, player->Get3D() };
                for (auto* top : tops) {
                    forEachNode(top, [&](RE::NiAVObject* object, int depth) {
                        const auto name = nodeName(object);
                        if (shown < 40 && name.rfind("Weapon", 0) == 0) {
                            ++shown;
                            note("  %s (depth %d, culled %d)", name.c_str(), depth, (object->flags.flags & 1ull) ? 1 : 0);
                        }
                        return depth < 40;
                    });
                }
            }
            if (st.managed) {
                putAway("the gun is not drawn");
            } else {
                releaseHand();
                griphook::setArmed(false);
            }
            looseMagazines(nullptr, nullptr);
            hideRound();
            // a gun this plugin does not reload yet: ROCK is only asked what parts it has (for the log)
            rocklink::Frame link{};
            auto* otherRoot = !st.managed && gun.weapon && gun.data ? weaponRoot(player) : nullptr;
            link.gun = looksLikeObject(otherRoot);
            if (link.gun && !st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
                st.dumped = true;   // the nodes of guns not reloaded by hand yet: what the next mechanisms are built from
                logNote("Weapon node tree (not handled yet):");
                dumpTree(otherRoot);
            }
            link.freeHand = freeIndex();
            link.magazine.node = kMovingParts[0];
            link.bolt.node = kMovingParts[1];
            g_rock = rocklink::frame(link);
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
        if (!st.magNodeChecked) {
            st.magNodeChecked = true;
            // pipe guns hang the magazine on another node (WeaponExtra3 > P-Mag); WeaponMagazine is left empty there
            bool drawn = false;
            if (auto* wm = findNamed(root, "WeaponMagazine")) {
                forEachNode(wm, [&](RE::NiAVObject* object, int) {
                    drawn = drawn || !object->IsNode();
                    return !drawn;
                });
            }
            if (!drawn) {
                if (auto* pmag = findNamed(root, "P-Mag"); looksLikeObject(pmag) && looksLikeObject(pmag->parent)) {
                    const auto name = nodeName(pmag->parent);
                    if (!name.empty() && name != "WeaponMagazine") {
                        st.magNodeName = name;
                    }
                }
            }
            note("Magazine node: '%s'%s", st.magNodeName.c_str(), drawn ? "" : " (WeaponMagazine has nothing drawn under it)");
        }
        std::array<RE::NiAVObject*, 5> parts{};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i] = findNamed(root, kMovingParts[i]);
        }
        if (st.magNodeName != kMovingParts[0]) {
            parts[0] = findNamed(root, st.magNodeName.c_str());
            for (std::size_t i = 1; i < parts.size(); ++i) {
                if (st.magNodeName == kMovingParts[i]) parts[i] = nullptr;
            }
        }
        auto* magazine = parts[0];
        auto* bolt = st.cellFed ? nullptr : parts[1];   // an energy gun's 'bolt' is not worked by hand
        if (st.hasBolt != looksLikeObject(bolt)) {
            st.hasBolt = looksLikeObject(bolt);
            note("Slide / bolt node: %s", st.hasBolt ? "found" : "none (rounds are chambered by the magazine)");
        }
        // an empty gun: the game would play its own reload by itself; the parts stay put (the slide stays open)
        const bool frozen = g_settings.blockReload && (gunState == reloading || now < st.freezeUntil || pressed || st.slideLocked || st.lastAmmo == 0);
        const bool quiet = !frozen && st.place == kSeated && st.s <= 0.0f && gunState == static_cast<std::uint32_t>(g_settings.idleGunState) &&
                           !st.racking && st.rackPull <= 0.0f && !st.slideLocked && now - st.lastShot > std::chrono::milliseconds(600) &&
                           now - st.equippedAt > std::chrono::milliseconds(700) && now - st.lastDrive > std::chrono::milliseconds(700);
        if (quiet && gun.data->ammoCount > 0 && !(g_settings.core2 && st.idleKnown)) {
            // core2: taken once, with the gun at rest after it is drawn; later the game's own reload may have moved the parts
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
        useRockBoxes(root, bolt, quiet && gun.data->ammoCount > 0);
        if (g_settings.core2 && !st.zonesEarly && st.railKnown && gun.weapon) {
            // the zones file at once (the hand finds the parts from the start); read again after ROCK's boxes come
            st.zonesEarly = true;
            zonesFor(gun.weapon->GetFormID(), false);
        }
        if (g_settings.core2 && !st.zonesChecked && st.magBoxKnown && (st.slideBoxKnown || !looksLikeObject(bolt))) {
            st.zonesChecked = true;
            zonesFor(gun.weapon->GetFormID());
        }
        if (g_settings.core2 && st.idleKnown && looksLikeObject(magazine) && st.place == kSeated) {
            magazine->local = st.idle[0];   // seated: always where it sits at rest (the game's reload moves it away)
            refreshSubtree(magazine, RE::NiPoint3{});
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
                if (st.boltAction && st.place == kSeated) {
                    // a bolt action: the fired case stays in the chamber until the bolt is worked
                    st.rounds = ammo;
                    st.chamber = false;
                    ammo = 0;
                    gun.data->ammoCount = 0;
                } else if (st.place == kSeated) {
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
        if (now < st.stopReloadUntil && gunState == reloading) {
            static int cut = 0;
            player->gunState = static_cast<RE::GUN_STATE>(g_settings.idleGunState);
            if (++cut % 30 == 1) {
                note("The game's reload of an empty gun is cut short (gunState %u -> %d)", gunState, g_settings.idleGunState);
            }
        }
        if (pressed && g_settings.blockReload) {
            st.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
            const bool byButton = now - st.buttonEdge < std::chrono::milliseconds(700);
            const bool justEmptied = ammo == 0 && now - st.lastShot < std::chrono::milliseconds(2500);
            if (!byButton && (triggerHeld || justEmptied)) {
                // the game reloads by itself when the trigger is pulled on an empty gun: a dry click, not a reload
                logNote("The game's own reload of an empty gun: ignored");
                if (g_settings.stopEmptyReload) {
                    st.stopReloadUntil = now + std::chrono::milliseconds(1500);
                }
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
        const bool rockArm = rockOn() && (st.armReason == kArmRail || st.armReason == kArmSlide);   // ROCK sees this press too
        const bool rockTook = g_rock.onMagazine && !st.rockWasOnMagazine && free.valid && free.grip && st.place == kRailAuto;
        st.rockWasOnMagazine = g_rock.onMagazine;
        if (rockTook && !(press && st.armReason == kArmRail)) {
            logNote("ROCK closed the free hand on the magazine");
            takeFromRail(root, free);
        } else if (press && (griphook::owned() || rockArm)) {
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
                    st.handoverAt = now + std::chrono::milliseconds(60);
                    st.forceTargets = now + std::chrono::milliseconds(50);   // ROCK learns of the magazine before it sees the grip
                    st.handoverDue = g_settings.rockBeltHandover && !realHand();
                    if (realHand()) {
                        st.handToken = spawnLoose(pose, rounds, "from the belt into the hand");
                        st.handTokenDropped = false;
                        griphook::setOwned(true);
                    }
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
                   distance(free.palm, root->world.translate) > g_settings.gunRadius && st.armReason != kArmSlide && !g_rock.onBolt && !g_rock.onGun &&
                   now - st.lastBelt > std::chrono::milliseconds(700)) {
            // the gun still has its magazine: a spare one is a real object for ROCK to hold
            const auto rounds = freshMagazineRounds(gun, inventory);
            st.lastBelt = now;
            if (rounds > 0 && g_settings.core2) {
                if (!st.handToken && !st.handRef) {
                    st.handToken = spawnLoose(holdPose(free, magScale), rounds, "a spare from the belt");
                    st.handTokenDropped = false;
                    st.handSpare = true;
                    playSound(4);
                    buzz(freeIndex(), 1200);
                }
            } else if (rounds > 0) {
                spawnLoose(holdPose(free, magScale), rounds, "a spare from the belt");
            }
        }

        // the magazine's way
        switch (st.place) {
        case kRailAuto:
            if (g_settings.core2 && free.valid && !free.grip && st.prevHandKnown && palmOnMagazine(root, free.palm)) {
                // the open palm pushes it back in (only inwards: a palm cannot pull)
                const auto moved = inGun(root, free.wand.translate) - st.prevHandLocal;
                const float along = dot(moved, st.axis);
                if (along < 0.0f && dot(moved, moved) < 36.0f) {
                    st.s = std::max(0.0f, st.s + along);
                    st.autoHold = false;
                    st.resting = true;
                    if (st.s <= g_settings.seatAt) {
                        seat(gun);
                        note("CORE2: the palm pushed the magazine home");
                        break;
                    }
                }
            }
            if (st.resting) {
                break;   // half in the well: it stays where it was left
            }
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
                    } else if (g_settings.core2 && st.s < st.length - 0.5f) {
                        st.place = kRailAuto;   // let go half in: it stays in the well, a push of the palm seats it
                        st.resting = true;
                        st.autoHold = false;
                        releaseHand();
                        note("CORE2: the magazine is let go %.1f out: it stays in the well", st.s);
                    } else {
                        startDrop(railPose(root, st.s), st.velocityValid ? st.velocity : RE::NiPoint3{}, "let go in the well");
                    }
                    break;
                }
                const auto carried = compose(free.wand, st.magInWand);
                const float target = st.driveS0 + (alongRail(root, carried.translate) - st.driveAlong0);
                // core2: caught low (the gun's collision stops a real magazine below the mouth), it leaves again only when
                // pulled 1.5 further out than it has been since it was caught
                if (g_settings.core2) {
                    st.exitAt = std::min(st.exitAt > 0.0f ? st.exitAt : 1e9f, std::max(st.length + 2.0f, st.s + 1.5f));
                }
                const float limit = g_settings.core2 ? st.exitAt : st.length + g_settings.captureAhead + 1.0f;
                if (target > limit) {
                    // clear of the well: from here it goes with the hand
                    if (g_settings.holdPoseOnExit && !g_rock.onMagazine) {
                        startBlend(st.lastDrawnValid ? st.lastDrawn : railPose(root, st.s), 0.20f);
                        st.magInWand = relative(free.wand, holdPose(free, gunScale(root)));
                    } else {
                        // it stays in the hand the way it was taken: no jump, and ROCK's hand stays on it
                        st.magInWand = relative(free.wand, st.lastDrawnValid ? st.lastDrawn : railPose(root, st.s));
                    }
                    st.place = kHand;
                    st.mustLeave = true;
                    unpinHand();
                    playSound(1);
                    note("OUT: the magazine left the well in the free hand (%u rounds)", st.rounds);
                    st.forceTargets = now + std::chrono::milliseconds(1500);   // ROCK is told the gun's magazine is no longer to be held
                    if (realHand()) {
                        // drawn at the hand only until the real one is there; then ROCK holds the real one
                        st.handToken = spawnLoose(compose(free.wand, st.magInWand), st.rounds, "out of the well into the hand");
                        st.handTokenDropped = false;
                        st.handoverDue = false;
                        griphook::setOwned(true);
                    }
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
                if (st.handToken) {
                    setFresh(st.handToken, target);
                }
                if (st.handToken && (!free.valid || !free.grip)) {
                    // let go before the real one came: it appears where the hand is and falls (or goes back to the pouch)
                    st.handTokenDropped = true;
                    st.handStash = inBelt(player, free.palm) || overShoulder(player, free.palm);
                    st.place = kGone;
                    st.rounds = 0;
                    note("The magazine is let go before the real one came (%s)", st.handStash ? "put away" : "it falls");
                    break;
                }
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
                const bool upright = -dot(wayOutOf(target), axisWorld(root)) < -0.5f;
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
                // in the hand and held by ROCK: ROCK has already put the node where it was told a frame ago and has its
                // hand on that very pose; placing it again here would give the magazine a second, slightly different, hold
                bool leave = false;
                if (st.place == kHand && g_rock.onMagazine) {
                    const auto apart = relative(drawn, magazine->world).translate;
                    const float off = std::sqrt(dot(apart, apart));
                    leave = g_settings.rockPlacesHeld && off < 25.0f;
                    static int tick = 0;
                    if (++tick % 90 == 0) {
                        detail("Held magazine: ROCK has it (%.2f, %.2f, %.2f) from where the controller puts it (%.2f apart)%s", apart.x, apart.y, apart.z,
                            off, leave ? "" : ": too far, placed by the plugin");
                    }
                }
                if (!leave) {
                    placeNode(magazine, drawn);
                } else {
                    const auto& w = magazine->world;
                    carryBounds(magazine, w.translate + (axisOf(w, 0) * st.sideInNode.x + axisOf(w, 1) * st.sideInNode.y +
                                                         axisOf(w, 2) * ((st.topAt + st.bottomAt) * 0.5f)) * w.scale);
                }
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
        st.prevHandLocal = inGun(root, free.wand.translate);
        st.prevHandKnown = free.valid;
        drawRound(gun, root);
        if (g_settings.core2) {
            realMagazineInHand(player);
        }

        // the free hand stays on what it holds: the magazine in the well, or the slide
        const bool rockHolds = g_rock.onMagazine || g_rock.onBolt;   // ROCK draws the hand and the fingers on the part itself
        if (rockHolds) {
            st.rackPinned = false;
            unpinHand();
        } else if (st.racking && looksLikeObject(bolt) && free.valid) {
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
        poseFingers(!rockHolds && (st.place == kRailHand || st.place == kHand || st.racking));

        // a magazine from the belt: once it is in the hand the grip is shown again, so that ROCK closes the hand on it
        if (st.handoverDue && now >= st.handoverAt) {
            st.handoverDue = false;
            if (rockOn() && st.place == kHand && free.grip && griphook::owned()) {
                griphook::setOwned(false);
                detail("Belt magazine: the grip is shown to ROCK");
            }
        }

        looseMagazines(&gun, root);

        // whose the free hand's next grip press will be
        if (!free.grip) {
            int reason = kArmNone;
            if (free.valid) {
                // a magazine sticking out comes first: the slide is not taken until it is dealt with
                float toSlide = looksLikeObject(bolt) && st.place != kRailAuto ? distance(free.palm, slideGrabPoint(root, bolt)) : 1e9f;
                float toMag = st.place == kRailAuto && st.railKnown ? distance(free.palm, grabPoint(root)) : 1e9f;
                if (g_settings.core2) {
                    // touching, not near a point: the box of the part as it stands
                    toMag = st.place == kRailAuto && st.railKnown && palmOnMagazine(root, free.palm) ? 0.0f : 1e9f;
                    if (toMag > 0.0f && looksLikeObject(bolt) && st.slideBoxKnown) {
                        const float pull = st.rackPull;
                        const float pullU = pull / std::max(g_settings.rackTravel, 0.1f);
                        const auto palmIn = inGun(root, free.palm);
                        const auto shift = hasRoute() ? palmIn - routeInverse(pullU, palmIn)
                                         : g_settings.recordedBolt && g_play.parsed && g_play.boltKnown && g_play.weapon == gun.weapon->GetFormID()
                                               ? boltWay(pull / std::max(g_settings.rackTravel, 0.1f)).translate - boltWay(0.0f).translate
                                               : RE::NiPoint3{ 0.0f, -pull, 0.0f };
                        toSlide = inBox(st.slideLo, st.slideHi, inGun(root, free.palm) - shift, g_settings.contactMargin) ? 0.5f : 1e9f;
                    }
                }
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
            // at the magazine and at the slide ROCK must see the press (it takes the part); elsewhere it is hidden as before
            const bool leaveToRock = rockOn() && (reason == kArmRail || reason == kArmSlide);
            griphook::setArmed(reason != kArmNone && !leaveToRock);
        }

        // ROCK: what the free hand may take, and where the parts have been moved
        {
            rocklink::Frame link{};
            link.gun = true;
            link.freeHand = freeIndex();
            link.exclusive = st.place != kSeated || st.armReason == kArmSlide || st.racking;
            const bool justSeated = now - st.seatedAt < std::chrono::milliseconds(700);   // the hand must not jump from the magazine to the slide
            link.keepTargets = free.grip && !justSeated && now > st.forceTargets && !(g_settings.core2 && st.handRef && !st.grabDone);
            link.chamber = st.chamber ? 2 : 1;
            const bool magOut = draw && (st.place == kRailAuto || st.place == kRailHand || st.place == kHand) && looksLikeObject(magazine);
            link.magazine.node = st.magNodeName.c_str();
            link.magazine.grab = magOut && st.railKnown;
            link.magazine.point[0] = st.sideInNode.x;
            link.magazine.point[1] = st.sideInNode.y;
            link.magazine.point[2] = st.bottomAt + g_settings.grabUp;
            link.magazine.radius = g_settings.magGrabRadius;
            if (magOut && looksLikeObject(magazine->parent)) {
                // ROCK uses this pose a frame later, against the gun as it stands then: with a magazine carried in the
                // hand both are moved ahead by one frame's travel, or the hand ROCK draws on it trails and shakes
                auto target = drawn;
                auto parentWorld = magazine->parent->world;
                if (st.place == kHand && st.prevDriveKnown) {
                    const auto magStep = drawn.translate - st.prevDrawnAt;
                    const auto parentStep = parentWorld.translate - st.prevParentAt;
                    if (dot(magStep, magStep) < 36.0f && dot(parentStep, parentStep) < 36.0f) {
                        target.translate += magStep;
                        parentWorld.translate += parentStep;
                    }
                }
                st.prevDrawnAt = drawn.translate;
                st.prevParentAt = magazine->parent->world.translate;
                st.prevDriveKnown = true;
                link.magazine.drive = true;
                link.magazine.parentLocal = toLinkPose(relative(parentWorld, target));
            } else {
                st.prevDriveKnown = false;
            }
            if (looksLikeObject(bolt)) {
                link.bolt.node = kMovingParts[1];
                link.bolt.grab = st.slidePointKnown && !justSeated && st.place != kRailAuto;   // a magazine sticking out comes first
                link.bolt.point[0] = st.slidePointInBolt.x;
                link.bolt.point[1] = st.slidePointInBolt.y;
                link.bolt.point[2] = st.slidePointInBolt.z;
                link.bolt.radius = g_settings.slideGrabRadius;
                if ((st.racking || st.rackPull > 0.0f) && looksLikeObject(bolt->parent)) {
                    link.bolt.drive = true;
                    link.bolt.parentLocal = toLinkPose(relative(bolt->parent->world, bolt->world));
                }
            }
            if (link.magazine.drive || link.bolt.drive) {
                st.lastDrive = now;
            }
            g_rock = rocklink::frame(link);
        }

        // the log: every change of state, and a numbered mark when the free hand clicks its stick
        {
            static std::string lastKey;
            static int marks = 0;
            const bool markNow = free.valid && (free.buttons & buttonMask(g_settings.markButton)) != 0 &&
                                 (free.previousButtons & buttonMask(g_settings.markButton)) == 0;
            if (markNow) {
                ++marks;
                char tag[32];
                std::snprintf(tag, sizeof(tag), "MARK %d", marks);
                logNote("==================================================================");
                writeState(tag, gun, root, magazine, bolt);
                char text[96];
                std::snprintf(text, sizeof(text), "Метка %d записана в лог", marks);
                hud(text);
                buzz(freeIndex(), 3000);
            } else if (g_settings.traceState) {
                auto key = stateKey();
                if (key != lastKey) {
                    lastKey = std::move(key);
                    writeState("state", gun, root, magazine, bolt);
                }
            }
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

    // the frame update with a clock on it: a frame that takes long is written to the log (to find hitches)
    void timedUpdate() noexcept
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point lastFlush = clock::now();
        static clock::time_point lastSlowNote{};
        static int slowFrames = 0;
        static double worst = 0.0;
        const auto start = clock::now();
        update();
        const auto end = clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        if (ms > 2.0) {
            ++slowFrames;
            worst = (std::max)(worst, ms);
            if (end - lastSlowNote > std::chrono::seconds(5)) {
                lastSlowNote = end;
                char text[128];
                std::snprintf(text, sizeof(text), "SLOW: ManualReloadF frame took %.1f ms (%d slow frames, worst %.1f ms)", ms, slowFrames, worst);
                logNote(text);
                slowFrames = 0;
                worst = 0.0;
            }
        }
        if (end - lastFlush > std::chrono::seconds(2)) {
            lastFlush = end;
            manualreload::runtime::flushLog();
        }
    }

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        static std::uint64_t frames = 0;
        if (++frames % 5400 == 1) {
            logNote("frame update is running");
        }
        __try {
            timedUpdate();
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
        rocklink::connect(&hookLog, g_settings.rockParts);
        g_rock = {};
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
