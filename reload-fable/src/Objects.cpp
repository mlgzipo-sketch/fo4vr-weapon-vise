#include "Objects.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <unordered_set>
#include <vector>

#include "Runtime.h"
#include "Angles.h"
#include "GripHook.h"
#include "Gun.h"
#include "Hands.h"
#include "Log.h"
#include "NiMath.h"
#include "Nodes.h"
#include "Palm.h"
#include "Paths.h"
#include "Rail.h"
#include "RockLink.h"
#include "SafeMemory.h"
#include "ScriptBridge.h"
#include "Settings.h"
#include "State.h"
#include "Zones.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // guns whose loose magazine ROCK would not take (it hung in the air): their magazine is drawn in the hand instead
    std::unordered_set<std::uint32_t> g_noRealHandGuns;

    bool g_noRealHandNow{ false };

    bool realHand() noexcept { return (g_settings.realInHand || g_settings.core2) && !g_noRealHandNow; }

    // ------------------------------------------------------------ magazines as real objects

    std::array<float, 6> poseForScript(const RE::NiTransform& loose) noexcept
    {
        const auto angles = anglesFor(loose.rotate);
        return { loose.translate.x, loose.translate.y, loose.translate.z, angles[0], angles[1], angles[2] };
    }

    std::int32_t g_nextCasingToken = kCasingTokenBase;

    std::deque<Casing> g_casings;

    RE::TESAmmo* g_casingModelFor{ nullptr };

    bool g_casingModelReady{ false };

    bool prepareCasingModel(const Gun& gun) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        if (g_casingModelFor == ammo) {
            return g_casingModelReady;
        }
        g_casingModelFor = ammo;
        g_casingModelReady = false;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kCasingForm, "ManualReloadF.esp") : nullptr;
        if (!form || !ammo || !readable(ammo, sizeof(RE::TESAmmo))) {
            note("Casing: %s", !form ? "MRF_MagCasing was not found in ManualReloadF.esp" : "the gun has no ammo");
            return false;
        }
        const char* m = ammo->shellCasing.GetModel();
        if (!m || !*m) {
            note("Casing: ammo %08X has no case model: nothing is thrown out", ammo->GetFormID());
            return false;
        }
        auto* misc = form->As<RE::TESObjectMISC>();
        if (!misc) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(m);
        g_casingModelReady = true;
        note("Casing: model '%s' (ammo %08X)", m, ammo->GetFormID());
        return true;
    }

    // `count` cases (or unfired rounds) out of the gun at `at`, thrown along `way` (world), a little apart

    void ejectCasings(const Gun& gun, const RE::NiAVObject* root, const RE::NiPoint3& at, const RE::NiPoint3& way, std::uint32_t count, const char* why) noexcept
    {
        if (!g_settings.casings || count == 0 || !looksLikeObject(root) || !prepareCasingModel(gun)) {
            return;
        }
        count = std::min(count, 8u);
        const auto dir = normalized(way);
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::NiTransform pose = root->world;
            const float spread = static_cast<float>(i) - 0.5f * static_cast<float>(count - 1);
            // well apart, or the physics throws two cases born inside each other far away
            pose.translate = at + axisOf(root->world, 1) * (spread * 2.0f * gunScale(root)) + axisOf(root->world, 2) * (0.6f * static_cast<float>(i % 2) * gunScale(root));
            PendingCommand command;
            command.type = kCasing;
            command.kind = kCasingKind;
            command.token = g_nextCasingToken--;
            if (g_nextCasingToken < -900000) {
                g_nextCasingToken = kCasingTokenBase;
            }
            command.pose = poseForScript(pose);
            const float wobble = 0.15f * spread;
            command.impulse = { dir.x + wobble, dir.y, dir.z, g_settings.casingImpulse };
            pushCommand(command);
        }
        note("CASINGS: %u out (%s)", count, why);
    }

    BeltRound g_beltRound;

    int g_roundOrphans = 0;   // asked for and given up before they came: deleted when they come

    void dropHandRound(const char* why) noexcept
    {
        if (g_beltRound.ref) {
            if (rocklink::heldBy(freeIndex()) == g_beltRound.ref) {
                rocklink::releaseHeld(freeIndex(), g_beltRound.ref);
            }
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_beltRound.ref;
            pushCommand(command);
        } else if (g_beltRound.asked) {
            ++g_roundOrphans;
        }
        if (g_beltRound.asked) {
            note("ROUND: the real round %08X is gone (%s)", g_beltRound.ref, why);
        }
        g_beltRound = BeltRound{};
    }

    std::vector<LooseRound> g_looseRounds;

    void deleteRef(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
    }

    void keepHandRound(const char* why) noexcept
    {
        if (!(g_beltRound.ref && g_beltRound.held)) {
            dropHandRound(why);
            return;
        }
        g_looseRounds.push_back(LooseRound{ g_beltRound.ref, g_beltRound.weapon, std::max(g_beltRound.rounds, 1u), Clock::now() });
        note("ROUND: the real round %08X stays (%s): %u round(s) for %08X", g_beltRound.ref, why, g_beltRound.rounds, g_beltRound.weapon);
        while (g_looseRounds.size() > 6) {
            deleteRef(g_looseRounds.front().ref);
            g_looseRounds.erase(g_looseRounds.begin());
        }
        g_beltRound = BeltRound{};
    }

    std::uint32_t looseRoundsFor(std::uint32_t weapon) noexcept
    {
        std::uint32_t n = 0;
        for (const auto& r : g_looseRounds) {
            n += r.weapon == weapon ? r.rounds : 0u;
        }
        return n;
    }

    void tidyLooseRounds(Clock::time_point now) noexcept
    {
        const auto held = rocklink::heldBy(freeIndex());
        for (auto it = g_looseRounds.begin(); it != g_looseRounds.end();) {
            if (it->ref != held && now - it->born > std::chrono::seconds(180)) {
                deleteRef(it->ref);
                it = g_looseRounds.erase(it);
            } else {
                ++it;
            }
        }
    }

    // the model of MRF_MagHeldRound: a missile, a mini nuke, a cell: the ammo's own model; a shell: its case

    bool prepareHeldRoundModel(const Gun& gun, bool big) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kHeldRoundForm, "ManualReloadF.esp") : nullptr;
        auto* misc = form ? form->As<RE::TESObjectMISC>() : nullptr;
        if (!misc || !ammo || !readable(ammo, sizeof(RE::TESAmmo))) {
            note("ROUND: %s", !misc ? "MRF_MagHeldRound is not in ManualReloadF.esp (old plugin?)" : "the gun has no ammo");
            return false;
        }
        const char* casing = ammo->shellCasing.GetModel();
        const char* world = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel();
        const char* model = big ? (world && *world ? world : casing) : (casing && *casing ? casing : world);
        if (!model || !*model) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(model);
        detail("ROUND: the round in the hand is '%s'", model);
        return true;
    }

    void tidyCasings(Clock::time_point now) noexcept
    {
        while (!g_casings.empty() && (static_cast<int>(g_casings.size()) > std::max(1, g_settings.casingMax) ||
                                      now - g_casings.front().born > std::chrono::milliseconds(static_cast<int>(g_settings.casingSeconds * 1000.0f)))) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_casings.front().ref;
            pushCommand(command);
            g_casings.pop_front();
        }
    }

    // A real magazine where the gun's magazine node would stand at `node`.

    std::int32_t spawnLoose(const RE::NiTransform& node, std::uint32_t rounds, const char* why) noexcept
    {
        const auto loose = compose(node, g_state.looseInNode);
        const auto token = g_nextToken++;
        g_pending[token] = Pending{ rounds, g_state.kind, loose.rotate };
        PendingCommand command;
        command.type = kSpawn;
        command.kind = g_state.kind;
        command.token = token;
        command.pose = poseForScript(loose);
        command.scale = loose.scale > 0.05f && loose.scale < 20.0f ? loose.scale : 1.0f;
        {
            std::scoped_lock guard(g_shared.lock);
            g_shared.fresh[token] = command.pose;
        }
        pushCommand(command);
        note("Spawn magazine kind %d with %u rounds (%s) at (%.0f, %.0f, %.0f)", g_state.kind, rounds, why, loose.translate.x, loose.translate.y, loose.translate.z);
        return token;
    }

    void setFresh(std::int32_t token, const RE::NiTransform& node) noexcept
    {
        const auto pose = poseForScript(compose(node, g_state.looseInNode));
        std::scoped_lock guard(g_shared.lock);
        if (const auto it = g_shared.fresh.find(token); it != g_shared.fresh.end()) {
            it->second = pose;
        }
    }

    void deleteMagazine(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
        g_loose.erase(ref);
    }

    RoundState g_round;

    void hideRound() noexcept
    {
        if (g_round.ref) {
            if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                setCulled(ref->Get3D(), true);
            }
        }
    }

    void takeRound(std::uint32_t ref) noexcept
    {
        if (g_round.ref && g_round.ref != ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = ref;
        g_round.asked = false;
        g_round.missing = false;
        note("Round: object %08X is here", ref);
    }

    void dropRound() noexcept
    {
        if (g_round.ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = 0;
        g_round.asked = false;
    }

    // The model of the drawn round: sRoundModel, else the casing of the gun's ammo, else its world model.

    bool prepareRoundModel(const Gun& gun) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        if (g_round.modelReady && g_round.modelFor == ammo) {
            return true;
        }
        g_round.modelFor = ammo;
        g_round.modelReady = false;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kRoundForm, "ManualReloadF.esp") : nullptr;
        auto* stat = form ? form->As<RE::TESObjectSTAT>() : nullptr;
        if (!stat) {
            note("Round: MRF_Round (%03X) was not found in ManualReloadF.esp", kRoundForm);
            return false;
        }
        std::string casing;
        std::string world;
        if (ammo && readable(ammo, sizeof(RE::TESAmmo))) {
            if (const char* m = ammo->shellCasing.GetModel(); m && *m) casing = m;
            if (const char* m = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel(); m && *m) world = m;
        }
        std::string model = !g_settings.roundModel.empty() ? g_settings.roundModel : !casing.empty() ? casing : world;
        note("Round: ammo %08X casing '%s', world model '%s' -> using '%s'", ammo ? ammo->GetFormID() : 0u, casing.c_str(), world.c_str(), model.c_str());
        if (model.empty()) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*stat)).SetModel(model.c_str());
        g_round.modelReady = true;
        if (g_round.ref) {
            dropRound();   // made with another model: make it again
        }
        return true;
    }

    void drawRound(const Gun& gun, RE::NiAVObject* root) noexcept
    {
        auto& st = g_state;
        if (!g_settings.showRound || !looksLikeObject(root) || !st.railKnown) {
            if (g_round.ref) {
                if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                    setCulled(ref->Get3D(), true);
                }
            }
            return;
        }
        const bool open = st.slideLocked || st.rackPull >= g_settings.rackTravel * std::clamp(g_settings.roundOpenAt, 0.05f, 1.0f);
        const bool inChamber = st.chamber && !st.rackCycled;
        const bool onMagazine = !inChamber && st.place == kSeated && st.rounds > 0;
        const bool visible = open && (inChamber || onMagazine);

        if (!g_round.ref) {
            const auto now = Clock::now();
            if (!g_round.asked || now - g_round.askedAt > std::chrono::seconds(5)) {
                if (prepareRoundModel(gun)) {
                    PendingCommand command;
                    command.type = kRound;
                    pushCommand(command);
                    g_round.asked = true;
                    g_round.askedAt = now;
                }
            }
            return;
        }
        auto* ref = lookupRef(g_round.ref);
        auto* model = ref ? ref->Get3D() : nullptr;
        if (!looksLikeObject(model)) {
            const auto now = Clock::now();
            if (!g_round.missing) {
                g_round.missing = true;
                g_round.missingSince = now;
            } else if (now - g_round.missingSince > std::chrono::seconds(3)) {
                note("Round: object %08X has no 3D, made again", g_round.ref);
                dropRound();
                g_round.missing = false;
            }
            return;
        }
        g_round.missing = false;
        if (!visible) {
            setCulled(model, true);
            return;
        }
        // the magazine's top in the gun's frame, then up / forward to the chamber
        const float scale = gunScale(root);
        auto top = compose(root->world, st.magRest);
        const auto up = axisOf(top, 2);
        RE::NiPoint3 at = top.translate + up * ((st.topAt + g_settings.roundUp) * top.scale);
        if (inChamber) {
            at += axisOf(root->world, 2) * (g_settings.chamberUp * scale) + axisOf(root->world, 1) * (g_settings.chamberForward * scale);
        }
        const float d = 0.01745329f;
        Mat turn = axisRotation(0, g_settings.roundRotX * d);
        turn = mul(turn, axisRotation(1, g_settings.roundRotY * d));
        turn = mul(turn, axisRotation(2, g_settings.roundRotZ * d));
        RE::NiTransform pose = identityTransform();
        pose.rotate = fromMat(mul(turn, toMat(root->world.rotate)));
        pose.translate = at;
        pose.scale = scale * (g_settings.roundScale > 0.0f ? g_settings.roundScale : 1.0f);
        setCulled(model, false);
        placeNode(model, pose);
        if (!g_round.logged) {
            g_round.logged = true;
            note("Round: drawn %s, bound radius %.2f", inChamber ? "in the chamber" : "on the magazine", model->worldBound.fRadius);
        }
    }

    void takeScriptNews() noexcept
    {
        std::vector<NewMag> mags;
        {
            std::scoped_lock guard(g_shared.lock);
            mags.swap(g_shared.newMags);
        }
        for (const auto& mag : mags) {
            if (mag.token == kRoundToken) {
                takeRound(mag.ref);
                continue;
            }
            if (mag.token == kPoseToken) {
                if (g_roundOrphans > 0 || !g_beltRound.asked || g_beltRound.ref) {
                    g_roundOrphans = std::max(0, g_roundOrphans - 1);
                    PendingCommand command;
                    command.type = kDelete;
                    command.ref = mag.ref;
                    pushCommand(command);
                    detail("ROUND: %08X came too late: deleted", mag.ref);
                    continue;
                }
                g_beltRound.ref = mag.ref;
                g_beltRound.grabAt = Clock::time_point{};   // asked for at once
                note("ROUND: real round %08X is here (%.2f s)", mag.ref, std::chrono::duration<float>(Clock::now() - g_beltRound.askedAt).count());
                continue;
            }
            if (mag.token <= kCasingTokenBase) {
                g_casings.push_back(Casing{ mag.ref, Clock::now() });
                continue;
            }
            Loose loose;
            loose.born = Clock::now();
            if (const auto it = g_pending.find(mag.token); it != g_pending.end()) {
                loose.rounds = it->second.rounds;
                loose.kind = it->second.kind;
                auto* ref = lookupRef(mag.ref);
                learnConvention(ref, ref ? ref->Get3D() : nullptr);
                g_pending.erase(it);
            }
            {
                std::scoped_lock guard(g_shared.lock);
                g_shared.fresh.erase(mag.token);
            }
            if (g_state.dropActive && g_state.dropToken == mag.token) {
                g_state.dropActive = false;
            }
            g_loose[mag.ref] = loose;
            note("Magazine %08X is out: %u rounds", mag.ref, loose.rounds);
            auto& st = g_state;
            if (st.handToken != 0 && mag.token == st.handToken) {
                st.handToken = 0;
                if (st.handTokenDropped) {
                    st.handSpare = false;
                    if (st.handStash) {
                        deleteMagazine(mag.ref);
                        hud("Magazine stowed, rounds returned to inventory");
                    }
                } else if (st.place == kHand || st.handSpare) {
                    // the real one is in the palm: the gun's own magazine is hidden, ROCK closes the hand on the real one
                    if (st.place == kHand) {
                        st.place = kGone;
                        st.rounds = 0;
                    }
                    st.handSpare = false;
                    st.velocityValid = false;
                    griphook::setOwned(false);
                    note("HAND: magazine %08X is now held as a real object (ROCK)", mag.ref);
                    if (g_settings.core2) {
                        st.handRef = mag.ref;
                        st.handRefAt = Clock::now();
                        st.grabTriedAt = st.handRefAt;
                        st.grabTries = 1;
                        st.grabDone = false;
                        const auto& palm = g_hands[freeIndex()].palm;
                        const float point[3]{ palm.x, palm.y, palm.z };
                        auto* ref = lookupRef(mag.ref);
                        auto* mag3D = ref ? ref->Get3D() : nullptr;
                        if (const auto held = rocklink::heldBy(freeIndex()); held != 0 && held != mag.ref) {
                            rocklink::releaseHeld(freeIndex(), held);
                        }
                        if (looksLikeObject(mag3D)) {
                            const auto c = mag3D->worldBound.center;
                            const float middle[3]{ c.x, c.y, c.z };
                            rocklink::forceGrab(freeIndex(), mag.ref, middle);
                        } else {
                            rocklink::forceGrab(freeIndex(), mag.ref, point);
                        }
                    }
                } else {
                    deleteMagazine(mag.ref);   // it was already pushed into the well
                }
            }
        }
    }
}
