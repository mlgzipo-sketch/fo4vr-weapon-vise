#include "Hands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Runtime.h"
#include "GripHook.h"
#include "Settings.h"
#include "Log.h"
#include "NiMath.h"
#include "SafeMemory.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

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

    std::array<Hand, 2> g_hands;

    bool g_pinned{ false };

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
        hand.axes = {};
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if (griphook::rawState(device, &state, sizeof(state)) || (g_vr && g_vr->GetControllerState(device, &state, sizeof(state)))) {
                hand.buttons = state.ulButtonPressed;
                for (int a = 0; a < 5; ++a) {
                    hand.axes[a] = RE::NiPoint3{ state.rAxis[a].x, state.rAxis[a].y, 0.0f };
                }
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
        // While we pin the drawn hand onto a part, the arm chain reports our own pin back, and a
        // grab that follows the palm then chases itself (the crank spinning, the gauss bolt
        // shaking).  So while pinned, the palm and the wrist come from the controller, offset as
        // they were the last time the hand was free.
        const bool pinnedHere = g_pinned && index == freeIndex();
        if (pinnedHere && hand.inWandKnown) {
            hand.wrist = compose(hand.wand, hand.wristInWand);
            hand.palm = compose(hand.wand, pointTransform(hand.palmInWand)).translate;
        } else if (!pinnedHere) {
            hand.wristInWand = relative(hand.wand, hand.wrist);
            hand.palmInWand = relative(hand.wand, pointTransform(hand.palm)).translate;
            hand.inWandKnown = true;
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

    // The hand on a turning part (a crank, a handle): the palm stays on the point of the part it took, the wrist
    // keeps the turn of the controller (so it is not wrung round, and does not swing out on a wider circle).

    RE::NiTransform handOnPart(const RE::NiTransform& wristNow, const RE::NiTransform& part, const RE::NiPoint3& gripInPart, const RE::NiPoint3& palmInWrist) noexcept
    {
        auto target = wristNow;
        target.scale = 1.0f;
        const auto grip = compose(part, pointTransform(gripInPart)).translate;
        const auto palmOffset = compose(target, pointTransform(palmInWrist)).translate - target.translate;
        target.translate = grip - palmOffset;
        return target;
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
}
