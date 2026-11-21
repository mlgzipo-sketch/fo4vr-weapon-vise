// ManualReloadF: settings read from Data/F4SE/Plugins/ManualReloadF.ini
#pragma once

#include <string>

namespace mrf
{
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
        bool  holdAsInGun{ true };      // a magazine from the belt comes turned as it goes into the gun held now (no turn of the hand at the well)
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
        bool  realRounds{ true };       // rounds from the belt (shells, a revolver's clip, a missile, a cell) are real objects ROCK holds
        bool  casings{ true };          // racking, a break action or a revolver emptied: cases (and unfired rounds) fly out as objects
        float casingImpulse{ 0.4f };    // how hard they are thrown (Havok impulse)
        float casingSeconds{ 40.0f };   // they lie this long, then are removed
        int   casingMax{ 24 };
        std::string roundModel;         // the model of the drawn round (empty: the gun's ammo casing)
    };
    extern Settings g_settings;

    // reads the ini into g_settings (missing keys keep their defaults)
    void loadSettings() noexcept;
}
