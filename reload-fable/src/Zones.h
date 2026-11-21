// ManualReloadF: core2: the zones file of a gun (Data/F4SE/Plugins/ManualReloadF_zones/<FORMID>.txt), drawn in Blender or the in-game editor
#pragma once

#include <RE/Fallout.h>
#include <string>
#include "State.h"

namespace mrf
{
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
    // a turn that is a turn: three unit axes at right angles, right-handed (a broken hold line is not used)
    bool squareRotation(const RE::NiTransform& t) noexcept;

    void loadZoneRoutes(std::uint32_t weapon) noexcept;

    void zonesFor(std::uint32_t weapon, bool mayWrite = true) noexcept;
}
