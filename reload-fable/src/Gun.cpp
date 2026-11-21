#include "Gun.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "SafeMemory.h"
#include "Settings.h"
#include "Log.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

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
}
