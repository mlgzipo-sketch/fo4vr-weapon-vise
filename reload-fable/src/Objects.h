// ManualReloadF: Magazines, rounds and fired cases as real objects in the world (spawned by the Papyrus script)
#pragma once

#include <RE/Fallout.h>
#include <array>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>
#include "State.h"
#include "Gun.h"

namespace mrf
{
    // guns whose loose magazine ROCK would not take (it hung in the air): their magazine is drawn in the hand instead
    extern std::unordered_set<std::uint32_t> g_noRealHandGuns;

    extern bool g_noRealHandNow;

    bool realHand() noexcept;

    // ------------------------------------------------------------ magazines as real objects
    std::array<float, 6> poseForScript(const RE::NiTransform& loose) noexcept;

    // ------------------------------------------------------------ cases thrown out of the gun
    inline constexpr RE::TESFormID kCasingForm = 0x82B;   // MRF_MagCasing (index 30 of MRF_Mags)
    inline constexpr std::int32_t kCasingKind = 30;

    inline constexpr std::int32_t kCasingTokenBase = -1000;

    extern std::int32_t g_nextCasingToken;

    struct Casing
    {
        std::uint32_t     ref{ 0 };
        Clock::time_point born{};
    };

    extern std::deque<Casing> g_casings;

    extern RE::TESAmmo* g_casingModelFor;

    extern bool g_casingModelReady;

    bool prepareCasingModel(const Gun& gun) noexcept;

    // `count` cases (or unfired rounds) out of the gun at `at`, thrown along `way` (world), a little apart
    void ejectCasings(const Gun& gun, const RE::NiAVObject* root, const RE::NiPoint3& at, const RE::NiPoint3& way, std::uint32_t count, const char* why) noexcept;

    // A round from the belt (a shell, a revolver's clip, a missile, a cell, a capacitor) as a real object that ROCK
    // holds in the free hand: the fingers close on it by themselves. Drawn in the hand until ROCK has it.
    inline constexpr std::int32_t kPoseToken = -8;

    inline constexpr RE::TESFormID kHeldRoundForm = 0x82D;   // MRF_MagHeldRound (index 32 of MRF_Mags), its model set here
    inline constexpr std::int32_t kSpeedloaderKind = 31;     // MRF_MagSpeedloader: the game's .45 speedloader, six rounds on it
    inline constexpr std::int32_t kHeldRoundKind = 32;

    struct BeltRound
    {
        bool              wanted{ false };   // the belt gave rounds: one object is to be made
        bool              asked{ false };
        std::uint32_t     ref{ 0 };
        std::uint32_t     weapon{ 0 };
        Clock::time_point askedAt{};
        Clock::time_point grabAt{};
        int               grabTries{ 0 };
        bool              held{ false };     // ROCK has had it in the hand
        std::uint32_t     rounds{ 0 };       // how many it stands for (a revolver's clip: all six)
    };

    extern BeltRound g_beltRound;

    extern int g_roundOrphans;   // asked for and given up before they came: deleted when they come

    void dropHandRound(const char* why) noexcept;

    // A real round let go away from the belt (or kept in the hand when the gun is put away): it lies there,
    // the hand takes it again (ROCK), however it likes, and it goes in like one from the belt.
    struct LooseRound
    {
        std::uint32_t     ref{ 0 };
        std::uint32_t     weapon{ 0 };
        std::uint32_t     rounds{ 0 };
        Clock::time_point born{};
    };

    extern std::vector<LooseRound> g_looseRounds;

    void deleteRef(std::uint32_t ref) noexcept;

    void keepHandRound(const char* why) noexcept;

    std::uint32_t looseRoundsFor(std::uint32_t weapon) noexcept;

    void tidyLooseRounds(Clock::time_point now) noexcept;

    // the model of MRF_MagHeldRound: a missile, a mini nuke, a cell: the ammo's own model; a shell: its case
    bool prepareHeldRoundModel(const Gun& gun, bool big) noexcept;

    void tidyCasings(Clock::time_point now) noexcept;

    // A real magazine where the gun's magazine node would stand at `node`.
    std::int32_t spawnLoose(const RE::NiTransform& node, std::uint32_t rounds, const char* why) noexcept;

    void setFresh(std::int32_t token, const RE::NiTransform& node) noexcept;

    void deleteMagazine(std::uint32_t ref) noexcept;

    // ------------------------------------------------------------ the round seen through the open slide
    // A static object of this plugin (MRF_Round) whose model is set from the gun's ammo (its casing) before it is
    // made. The plugin draws its 3D every frame: in the chamber while one is loaded, else on top of the magazine
    // while it has rounds, and hidden when the slide is closed or there is nothing to see.
    inline constexpr std::int32_t kRoundToken = -7;

    inline constexpr RE::TESFormID kRoundForm = 0x80E;

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

    extern RoundState g_round;

    void hideRound() noexcept;

    void takeRound(std::uint32_t ref) noexcept;

    void dropRound() noexcept;

    // The model of the drawn round: sRoundModel, else the casing of the gun's ammo, else its world model.
    bool prepareRoundModel(const Gun& gun) noexcept;

    void drawRound(const Gun& gun, RE::NiAVObject* root) noexcept;

    void takeScriptNews() noexcept;
}
