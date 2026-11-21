// ManualReloadF: core2: the open palm pushes a part along its way (slide, bolt, magazine)
#pragma once

#include <RE/Fallout.h>
#include "State.h"

namespace mrf
{
    // ------------------------------------------------------------ core2: the palm on a part
    bool inBox(const RE::NiPoint3& lo, const RE::NiPoint3& hi, const RE::NiPoint3& p, float margin) noexcept;

    RE::NiPoint3 inGun(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept;

    // The point of the magazine the well catches (zones file "point magtop"; else its top), or the same point at
    // the other end (a cell goes in either way round), for a magazine node standing at `node`.
    RE::NiPoint3 catchPointOf(const RE::NiTransform& node, bool otherEnd) noexcept;

    // The catch, as drawn: the magazine's top (either end, for a cell) inside the well's cylinder, which runs
    // from its mouth out along the way out as far as `ahead` (and `below` into the gun). `along` is how far out
    // past the mouth that end is, `radial` how far off the cylinder's line.
    bool inWellZone(const RE::NiAVObject* root, const RE::NiTransform& node, float sideMax, float ahead, float below, float* along, float* radial) noexcept;

    // How far out of its seat a magazine node stands (the rail's own measure), whatever its top.
    float seatAlong(const RE::NiAVObject* root, const RE::NiTransform& node) noexcept;

    // The magazine is touched: the palm is in its box as it stands now (moved out along the well by s).
    bool palmOnMagazine(const RE::NiAVObject* root, const RE::NiPoint3& palm) noexcept;
}
