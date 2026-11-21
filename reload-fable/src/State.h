// ManualReloadF: what the plugin knows about the drawn gun and its magazine (one State, reset on every equip)
#pragma once

#include <RE/Fallout.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Gun.h"
#include "Paths.h"
#include "RockLink.h"
#include "Runtime.h"

namespace mrf
{
    using Clock = std::chrono::steady_clock;

    // ------------------------------------------------------------ state

    inline constexpr std::uint32_t k10mmPistol = 0x00004822;   // Fallout4.esm

    inline constexpr float kGravity = 686.0f;                   // game units per second squared

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

    // Guns that open instead of taking a magazine: a revolver's cylinder swings out, a break-action gun folds.
    struct Breech
    {
        int               mech{ 0 };        // 0 none, 1 revolver, 2 break action, 3 one big round at a breech (Fat Man, missile launcher),
                                            // 4 a cover and one cell (alien blaster), 5 three capacitors in three sockets (gamma gun)
        std::uint32_t     caps{ 0 };        // mech 5: capacitors in
        RE::NiTransform   cellRest{};       // mech 4: the cell node as it stands in the gun
        bool              cellRestKnown{ false };
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
        bool              leverArmed{ false };   // the handle went out far enough for its return to count
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
        bool               railPinned{ false };            // the hand is held on the magazine on the rail..
        RE::NiPoint3       gripInMag{}, palmInWristMag{};  // ..by the palm's point on it, turned as the controller is
        float              driveS0{ 0.0f };
        float              driveAlong0{ 0.0f };
        bool               mustLeave{ false };             // just pulled out: has to clear the well before it is caught again
        RE::NiTransform    blendFrom{};
        Clock::time_point  blendStart{};
        float              blendSeconds{ 0.0f };
        RE::NiTransform    alignOffset{};                  // caught: the magazine as it was taken, in the rail's pose at the catch
        float              alignFromS{ 0.0f };             // how far out it was caught: it turns onto the rail as it goes in from there
        bool               alignActive{ false };
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
        bool               breechZone{ false };            // ..a cylinder as drawn: way out, radius, length out of the mouth
        RE::NiPoint3       breechAxis{};
        float              breechSide{ 0.0f };
        float              breechAhead{ 0.0f };
        bool               leverKnown{ false };            // zones file "route lever": the handle (WeaponBolt) that opens the breech
        std::vector<RE::NiPoint3> leverPts;
        std::vector<float> leverAt;
        ArcFit             leverFit{};                     // ..turned first, then pulled straight
        std::string        leverNodeName;                  // the node the handle hangs on (found once: WeaponBolt, WeaponExtra1..3)
        bool               leverNodeChecked{ false };
        float              lever{ 0.0f };                  // magazine guns (the gatling laser): 0 home .. 1 pulled out
        bool               leverHeld{ false }, leverRestKnown{ false }, leverDrawn{ false }, leverTouch{ false }, leverArmed{ false };
        RE::NiTransform    leverRestLocal{}, leverRestInRoot{};
        bool               leverPinned{ false }, crankPinned{ false };
        // parts that hang on the magazine node but belong to the gun (the cryolator's drum): they stay put
        std::vector<std::pair<std::string, RE::NiTransform>> gunPartsOnMag;   // name, rest pose in the gun
        bool               gunPartsChecked{ false };
        std::vector<std::string> gunPartsHidden;                               // magazine branches culled instead of the whole node
        RE::NiTransform    wristInLever{}, wristInCrank{};
        RE::NiPoint3       gripInLever{}, gripInCrank{}, palmInWristLever{}, palmInWristCrank{};   // the palm's point on the part, and in the wrist
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
        RE::NiPoint3       zoneMouth{};                    // zones file: the well's mouth (the cylinder's origin), in the gun
        bool               zoneMouthKnown{ false };
        RE::NiPoint3       catchPointInNode{};             // zones file "point magtop": the point of the magazine the well catches
        bool               catchPointKnown{ false };
        RE::NiPoint3       holdPointInGun{};               // zones file "point maghold": where the palm holds the magazine (on it, seated, gun frame)
        bool               holdPointKnown{ false };
        RE::NiTransform    roundHold{};                    // zones file "roundhold": a round (shell, clip, rocket) in the hand as it was shown
        bool               roundHoldKnown{ false };
        bool               roundAdjusting{ false };        // the round hangs in the air until the hand takes it as it should be held
        RE::NiTransform    roundFrozen{};
        RE::NiTransform    holdInWand{};                   // zones file "hold": the magazine in the hand as it was shown (stick click with one held)
        bool               holdInWandKnown{ false };
        bool               gameBoltAction{ true };
        bool               spentInChamber{ false };        // a bolt action fired: its case waits for the bolt         // the game's own flag for the gun as built (a pipe pistol is not a bolt action)
        float              zoneCatchAhead{ -1.0f };                 // core2: out this far along the well and the magazine has left it
    };

    extern State g_state;

    extern RE::NiPoint3 g_liveTop;   // the top of the magazine in the hand, last measured (shown with the zones)
    extern std::chrono::steady_clock::time_point g_liveTopAt;

    extern rocklink::Result g_rock;   // what ROCK answered last frame

    bool rockOn() noexcept;

    inline constexpr std::array<const char*, 5> kMovingParts{ "WeaponMagazine", "WeaponBolt", "WeaponExtra1", "WeaponExtra2", "WeaponExtra3" };

    extern std::unordered_map<std::uint32_t, Loose> g_loose;     // real magazines lying around or held through ROCK
    extern std::unordered_map<std::int32_t, Pending> g_pending;  // spawn token -> what it will be
    extern std::int32_t g_nextToken;

    std::uint32_t looseRounds() noexcept;

    void setAmmo(const Gun& gun, std::uint32_t value) noexcept;

    // The game fires while ammoCount > 0, so a gun with an empty chamber must read 0.
    void syncAmmo(const Gun& gun) noexcept;
}
