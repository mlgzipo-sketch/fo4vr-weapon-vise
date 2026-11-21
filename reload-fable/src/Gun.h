// ManualReloadF: the equipped gun: which weapon, which magazine model (kind), its 3D
#pragma once

#include <RE/Fallout.h>
#include <array>
#include <cstdint>
#include <string>
#include "Runtime.h"

namespace mrf
{
    // ------------------------------------------------------------ the gun

    struct Gun
    {
        RE::TESObjectWEAP*      weapon{ nullptr };
        RE::EquippedWeaponData* data{ nullptr };
        RE::TESObjectWEAP::InstanceData* instance{ nullptr };
    };

    Gun equippedGun(RE::PlayerCharacter* player) noexcept;

    std::uint32_t inventoryCount(RE::PlayerCharacter* player, const RE::TESForm* object) noexcept;

    // Guns with a detachable magazine and a slide / bolt / charging handle (group 1). A gun is handled when the
    // magazine mod it wears has one of these models; the index is the magazine's kind (MRF_Mags order in the ESP).
    inline constexpr std::uint32_t kLaserGun = 0x0009983B;

    inline constexpr std::uint32_t kGatlingLaser = 0x000E27BC;

    inline constexpr int kLaserCellKind = 19;     // MRF_MagLaserCell
    inline constexpr int kSyringerKind = 20;      // MRF_MagSyringer
    inline constexpr int kFusionCoreKind = 21;    // MRF_MagFusionCore
    inline constexpr std::uint32_t kMinigun = 0x0001F669;

    inline constexpr int kMinigunKind = 22;       // MRF_MagMinigun: the drum cut out of the minigun's own model (Meshes\\ManualReloadF\\MinigunDrum.nif)
    inline constexpr std::uint32_t kInstituteLaser = 0x001633CC;   // its cell is the laser gun's fusion cell
    inline constexpr std::uint32_t kCryolator = 0x00171B2B;

    inline constexpr std::uint32_t kAlienBlaster = 0x000FF995;   // its cover (WeaponExtra1) swings open, one cell (WeaponMagazine) feeds it
    inline constexpr std::uint32_t kGammaGun = 0x000DDB7C;       // three capacitors (WeaponMagazineChild1-3), each into its own socket
    inline constexpr int kPlasmaKind = 23;        // MRF_MagPlasma
    inline constexpr int kGaussFirstKind = 24;    // MRF_MagGaussSmall .. MRF_MagGaussXL (24..26)
    inline constexpr int kFlamerFirstKind = 27;   // MRF_MagFlamer, MRF_MagFlamerLarge (27, 28)
    inline constexpr int kCryoKind = 29;          // MRF_MagCryo
    inline constexpr std::array<const char*, 30> kMagModels{
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
        "weapons\\plasma\\plasmacell.nif",
        "weapons\\gaussrifle\\gaussriflemagsmall.nif", "weapons\\gaussrifle\\gaussriflemaglarge.nif", "weapons\\gaussrifle\\gaussriflemagextralarge.nif",
        "weapons\\flamer\\flamerpropellenttank.nif", "weapons\\flamer\\flamerpropellanttanklarge.nif",
        "ammo\\cryoammo\\cryocell.nif",
    };

    // The middle of each magazine model on its own (its geometry's box, measured from the nifs of MRF_Mags):
    // a gun can wear the same model shifted off its magazine node (the Thompson's drum sits 13 behind it),
    // and a real magazine made at the node's pose would then stand that far from the hand.
    inline const std::array<RE::NiPoint3, 30> kLooseMiddle{
        RE::NiPoint3{ -0.03f, 0.50f, 0.43f },
        RE::NiPoint3{ -0.03f, -1.61f, -3.63f },
        RE::NiPoint3{ -6.59f, 0.00f, 0.00f },
        RE::NiPoint3{ -5.17f, -0.53f, -4.50f },
        RE::NiPoint3{ 0.04f, 0.17f, -3.32f },
        RE::NiPoint3{ 0.04f, 1.07f, -6.59f },
        RE::NiPoint3{ 0.09f, 0.59f, -1.46f },
        RE::NiPoint3{ 0.13f, 0.59f, -3.21f },
        RE::NiPoint3{ 0.00f, 0.01f, 0.09f },
        RE::NiPoint3{ 0.00f, -0.33f, -1.55f },
        RE::NiPoint3{ -1.32f, 0.00f, -0.07f },
        RE::NiPoint3{ -3.23f, 0.00f, -0.07f },
        RE::NiPoint3{ 1.93f, 1.29f, -4.33f },
        RE::NiPoint3{ 0.14f, 0.09f, -3.61f },
        RE::NiPoint3{ 0.01f, 0.14f, -4.83f },
        RE::NiPoint3{ 0.14f, 0.42f, -3.35f },
        RE::NiPoint3{ 0.04f, -0.07f, 0.35f },
        RE::NiPoint3{ 0.04f, -0.07f, -1.47f },
        RE::NiPoint3{ 0.04f, 0.82f, -4.72f },
        RE::NiPoint3{ 0.00f, -2.85f, 0.00f },
        RE::NiPoint3{ -1.17f, 0.28f, 0.36f },
        RE::NiPoint3{ 0.00f, -0.06f, 0.00f },
        RE::NiPoint3{ 0.04f, 11.30f, 0.00f },
        RE::NiPoint3{ 0.04f, 0.33f, 1.15f },
        RE::NiPoint3{ 0.03f, -2.92f, 0.08f },
        RE::NiPoint3{ 0.03f, -0.06f, 0.08f },
        RE::NiPoint3{ -2.90f, -2.12f, -2.15f },
        RE::NiPoint3{ 0.01f, 6.21f, -0.02f },
        RE::NiPoint3{ 0.00f, 9.09f, 0.00f },
        RE::NiPoint3{ 0.00f, 0.00f, 2.85f }
    };

    // The magazine the equipped gun wears: its magazine mod's model, looked up in kMagModels (-1: none of ours).
    int magazineKindOf(RE::PlayerCharacter* player, const RE::TESForm* weapon, std::string& model) noexcept;

    std::uint32_t capacityOf(const Gun& gun) noexcept;

    RE::NiAVObject* weaponRoot(RE::PlayerCharacter* player) noexcept;

    void dumpTree(RE::NiAVObject* root) noexcept;
}
