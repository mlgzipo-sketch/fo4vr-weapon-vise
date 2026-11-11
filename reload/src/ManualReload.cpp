// ManualReload: hand reloading for Fallout 4 VR (Into the Radius style).
//
// Build 0.0.1 is a probe. It answers the two questions the whole mod depends on:
//   1. Can a plugin set the number of rounds loaded in the gun?
//      (EquippedWeaponData::ammoCount of the player's equipped weapon)
//   2. Can a plugin stop the game's own reload in VR?
//      (Actor::ReloadWeapon, vtable slot 0xEF, patched on the player's vtable)
// and it writes down the weapon's node tree (WeaponMagazine, WeaponBolt, ...).
//
// Test in game: the reload button no longer refills the gun. Instead it toggles
// "magazine out" (magazine hidden, only the chambered round left) and
// "magazine in" (magazine shown, gun loaded from the inventory).
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

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
        bool blockReload{ true };
        bool toggleMagazine{ true };
        int  dumpDepth{ 12 };
        // raw gunState the VR game reports while reloading (flat FO4 enum says 4, VR log shows 8)
        int  reloadGunState{ 8 };
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
                if (key == "bBlockReload") {
                    g_settings.blockReload = value != 0.0f;
                } else if (key == "bToggleMagazine") {
                    g_settings.toggleMagazine = value != 0.0f;
                } else if (key == "iReloadGunState") {
                    g_settings.reloadGunState = static_cast<int>(value);
                } else if (key == "iDumpDepth") {
                    g_settings.dumpDepth = static_cast<int>(value);
                }
            }
        } catch (...) {
        }
        char message[160]{};
        std::snprintf(message, sizeof(message), "Settings: bBlockReload=%d bToggleMagazine=%d iReloadGunState=%d iDumpDepth=%d",
            g_settings.blockReload ? 1 : 0, g_settings.toggleMagazine ? 1 : 0, g_settings.reloadGunState, g_settings.dumpDepth);
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

    // ------------------------------------------------------------ state

    struct State
    {
        RE::TESObjectWEAP* weapon{ nullptr };
        std::uint32_t      lastAmmo{ 0xFFFFFFFF };
        std::uint32_t      lastInventory{ 0xFFFFFFFF };
        std::uint32_t      lastGunState{ 0xFFFFFFFF };
        bool               magazineOut{ false };
        bool               dumped{ false };
        Clock::time_point  lastToggle{};
        Clock::time_point  equippedAt{};
    };
    State g_state;

    std::atomic<int> g_reloadCalls{ 0 };
    std::atomic<int> g_reloadBlocked{ 0 };
    std::atomic<bool> g_toggleRequested{ false };

    void toggleMagazine(RE::PlayerCharacter* player, const Gun& gun, const char* why) noexcept
    {
        if (!g_settings.toggleMagazine || !gun.data) {
            return;
        }
        const auto now = Clock::now();
        if (now - g_state.lastToggle < std::chrono::milliseconds(1500)) {
            return;
        }
        g_state.lastToggle = now;
        auto* root = weaponRoot(player);
        auto* magazine = findNamed(root, "WeaponMagazine");
        const auto before = gun.data->ammoCount;
        const auto inventory = inventoryCount(player, gun.data->ammo);
        const auto capacity = capacityOf(gun);
        if (!g_state.magazineOut) {
            // magazine out: only the round in the chamber stays
            gun.data->ammoCount = before > 0 ? 1u : 0u;
            setCulled(magazine, true);
            g_state.magazineOut = true;
        } else {
            const std::uint32_t chamber = before > 0 ? 1u : 0u;
            std::uint32_t loaded = chamber + capacity;
            if (loaded > inventory) {
                loaded = inventory;
            }
            gun.data->ammoCount = loaded;
            setCulled(magazine, false);
            g_state.magazineOut = false;
        }
        char message[220]{};
        std::snprintf(message, sizeof(message), "TOGGLE (%s): magazine %s, ammoCount %u -> %u (capacity %u, inventory %u), WeaponMagazine node %s",
            why, g_state.magazineOut ? "OUT" : "IN", before, gun.data->ammoCount, capacity, inventory, magazine ? "found" : "NOT found");
        logNote(message);
    }

    // ------------------------------------------------------------ reload hook

    using ReloadWeapon_t = bool (*)(void*, void*, std::uintptr_t);
    ReloadWeapon_t g_originalReload{ nullptr };

    bool onReloadWeapon(void* actor, void* weaponInstance, std::uintptr_t equipIndex) noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (actor != player || !player) {
            return g_originalReload(actor, weaponInstance, equipIndex);
        }
        ++g_reloadCalls;
        // Sanity check that slot 0xEF really is ReloadWeapon: its first argument is
        // a weapon instance whose object is the gun the player holds.
        const auto gun = equippedGun(player);
        bool matches = false;
        if (weaponInstance && readable(weaponInstance, sizeof(RE::BGSObjectInstance))) {
            matches = gun.weapon && static_cast<RE::BGSObjectInstance*>(weaponInstance)->object == gun.weapon;
        }
        char message[200]{};
        std::snprintf(message, sizeof(message), "ReloadWeapon called on the player: weapon %s, ammoCount %u, %s",
            matches ? "matches the held gun" : "DOES NOT match (wrong slot?)",
            gun.data ? gun.data->ammoCount : 0u,
            (matches && g_settings.blockReload) ? "BLOCKED" : "passed to the game");
        logNote(message);
        if (matches && g_settings.blockReload) {
            ++g_reloadBlocked;
            g_toggleRequested = true;
            return false;
        }
        return g_originalReload(actor, weaponInstance, equipIndex);
    }

    void installReloadHook(RE::PlayerCharacter* player) noexcept
    {
        static bool done = false;
        if (done || !player) {
            return;
        }
        done = true;
        constexpr std::size_t slot = 0xEF;
        auto* vtable = *reinterpret_cast<std::uintptr_t**>(player);
        auto* entry = vtable + slot;
        if (!readable(entry, sizeof(std::uintptr_t))) {
            logWarning("Player vtable not readable; reload hook not installed");
            return;
        }
        std::uint32_t oldProtect = 0;
        if (!REX::W32::VirtualProtect(entry, sizeof(std::uintptr_t), 0x40u /* PAGE_EXECUTE_READWRITE */, &oldProtect)) {
            logWarning("VirtualProtect failed; reload hook not installed");
            return;
        }
        g_originalReload = reinterpret_cast<ReloadWeapon_t>(*entry);
        *entry = reinterpret_cast<std::uintptr_t>(&onReloadWeapon);
        REX::W32::VirtualProtect(entry, sizeof(std::uintptr_t), oldProtect, &oldProtect);
        char message[160]{};
        std::snprintf(message, sizeof(message), "Reload hook installed on player vtable slot 0x%zX (original at exe+0x%llX)", slot,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_originalReload) - REL::Module::get().base()));
        logNote(message);
    }

    // ------------------------------------------------------------ frame

    void update() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D()) {
            return;
        }
        installReloadHook(player);
        const auto gun = equippedGun(player);
        const auto now = Clock::now();

        if (gun.weapon != g_state.weapon) {
            g_state.weapon = gun.weapon;
            g_state.dumped = false;
            g_state.magazineOut = false;
            g_state.equippedAt = now;
            g_state.lastAmmo = 0xFFFFFFFF;
            char message[200]{};
            std::snprintf(message, sizeof(message), "Equipped weapon %08X, ammo %08X, capacity %u",
                gun.weapon ? gun.weapon->GetFormID() : 0u,
                (gun.data && gun.data->ammo) ? gun.data->ammo->GetFormID() : 0u,
                capacityOf(gun));
            logNote(message);
        }
        if (!gun.weapon || !gun.data) {
            return;
        }

        // the 3D appears a moment after equipping
        if (!g_state.dumped && now - g_state.equippedAt > std::chrono::seconds(2)) {
            g_state.dumped = true;
            if (auto* root = weaponRoot(player)) {
                logNote("Weapon node tree:");
                dumpTree(root);
            } else {
                logNote("No WeaponMagazine node found on the player (first or third person)");
            }
        }

        auto ammo = gun.data->ammoCount;
        const auto inventory = inventoryCount(player, gun.data->ammo);
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        if (ammo != g_state.lastAmmo || inventory != g_state.lastInventory || gunState != g_state.lastGunState) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "ammoCount %u, ammo in inventory %u, gunState %u%s",
                ammo, inventory, gunState, gunState == reloading ? " (reloading)" : "");
            logInfo(message);
        }
        // VR refills the gun without going through the player's ReloadWeapon vfunc,
        // so any rise of the count that we did not make is the game's reload: undo it.
        if (g_settings.blockReload && g_state.lastAmmo != 0xFFFFFFFF && ammo > g_state.lastAmmo) {
            char message[120]{};
            std::snprintf(message, sizeof(message), "Game refill %u -> %u reverted", g_state.lastAmmo, ammo);
            logNote(message);
            gun.data->ammoCount = g_state.lastAmmo;
            ammo = g_state.lastAmmo;
        }
        const bool reloadStarted = gunState == reloading && g_state.lastGunState != reloading && g_state.lastGunState != 0xFFFFFFFF;
        g_state.lastInventory = inventory;
        g_state.lastGunState = gunState;

        if (reloadStarted && g_settings.blockReload) {
            logNote("Reload animation started (gunState = reloading)");
            toggleMagazine(player, gun, "reload button");
        }
        if (g_toggleRequested.exchange(false)) {
            toggleMagazine(player, gun, "ReloadWeapon");
        }
        g_state.lastAmmo = gun.data->ammoCount;
        if (g_state.magazineOut) {
            // the game may redraw the gun; keep the magazine hidden
            setCulled(findNamed(weaponRoot(player), "WeaponMagazine"), true);
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
    }
}
