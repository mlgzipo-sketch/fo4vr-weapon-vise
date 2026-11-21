#include "Settings.h"

#include "Log.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace mrf
{
    Settings g_settings;

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
            { "fHoldUp", 2, &s.holdUp }, { "bHoldAsInGun", 0, &s.holdAsInGun }, { "fHoldSide", 2, &s.holdSide }, { "fHoldForward", 2, &s.holdForward },
            { "fHoldRotX", 2, &s.holdRotX }, { "fHoldRotY", 2, &s.holdRotY }, { "fHoldRotZ", 2, &s.holdRotZ },
            { "iHoldFlip", 1, &s.holdFlip }, { "fFingerCurl", 2, &s.fingerCurl }, { "fFreezeSeconds", 2, &s.freezeSeconds },
            { "fSlideGrabRadius", 2, &s.slideGrabRadius }, { "fSlideGrabAt", 2, &s.slideGrabAt }, { "fSlideGrabHigh", 2, &s.slideGrabHigh }, { "fRackTravel", 2, &s.rackTravel }, { "fSlideLockAt", 2, &s.slideLockAt },
            { "fHandRadius", 2, &s.handRadius }, { "fLooseLifetime", 2, &s.looseLifetime }, { "iMaxLoose", 1, &s.maxLoose },
            { "bAllGuns", 0, &s.allGuns }, { "bRockParts", 0, &s.rockParts }, { "bRockBeltHandover", 0, &s.rockBeltHandover },
            { "bHoldPoseOnExit", 0, &s.holdPoseOnExit }, { "bBreech", 0, &s.breech }, { "bRecordAnim", 0, &s.recordAnim }, { "bAutoRecord", 0, &s.autoRecord }, { "bRecordedBolt", 0, &s.recordedBolt }, { "bRealInHand", 0, &s.realInHand }, { "bCore2", 0, &s.core2 }, { "fContactMargin", 2, &s.contactMargin }, { "iMarkButton", 1, &s.markButton }, { "bStopEmptyReload", 0, &s.stopEmptyReload }, { "fCatchSide", 2, &s.catchSide }, { "fCatchAhead", 2, &s.catchAhead }, { "fPushFactor", 2, &s.pushFactor }, { "bTraceState", 0, &s.traceState }, { "fCylinderSwing", 2, &s.cylinderSwing },
            { "fBreakAngle", 2, &s.breakAngle }, { "fFlickSpeed", 2, &s.flickSpeed }, { "fFlickTurn", 2, &s.flickTurn }, { "fHingeHold", 2, &s.hingeHold }, { "fBreechRadius", 2, &s.breechRadius }, { "fChamberReach", 2, &s.chamberReach }, { "bRockPlacesHeld", 0, &s.rockPlacesHeld }, { "bNativeSound", 0, &s.nativeSound }, { "iSoundFlags", 1, &s.soundFlags }, { "bShowRound", 0, &s.showRound }, { "fRoundOpenAt", 2, &s.roundOpenAt }, { "fChamberUp", 2, &s.chamberUp },
            { "fChamberForward", 2, &s.chamberForward }, { "fRoundUp", 2, &s.roundUp }, { "fRoundRotX", 2, &s.roundRotX },
            { "fRoundRotY", 2, &s.roundRotY }, { "fRoundRotZ", 2, &s.roundRotZ }, { "fRoundScale", 2, &s.roundScale }, { "bCasings", 0, &s.casings }, { "bRealRounds", 0, &s.realRounds }, { "fCasingImpulse", 2, &s.casingImpulse }, { "fCasingSeconds", 2, &s.casingSeconds }, { "iCasingMax", 1, &s.casingMax },
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
}
