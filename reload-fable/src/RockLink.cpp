// See RockLink.h. Everything ROCK answers is written to the log the first time and whenever it changes, because
// this link was written from ROCK's public headers and documents (src/ROCK/SOURCE.txt), not against a running game.
#include <Windows.h>

#include "RockLink.h"

#include "ROCK/Client.h"
#include "ROCK/Weapon.h"
#include "ROCK/WeaponParts.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace rocklink
{
    namespace
    {
        namespace api = rock::api;
        namespace parts = rock::api::weaponparts;

        constexpr std::uint32_t kSlot = 0;
        constexpr std::uint32_t kGroupMagazine = 1;
        constexpr std::uint32_t kGroupBolt = 2;

        LogFn g_log{ nullptr };
        bool g_refused{ false };
        api::Client g_client;
        const api::weapon::Api* g_weapon{ nullptr };
        const parts::Api* g_parts{ nullptr };

        std::uint64_t g_key{ 0 };
        std::uint64_t g_frames{ 0 };
        bool g_registered{ false };
        bool g_driving{ false };
        std::string g_config;            // what ROCK was last told about the targets
        std::uint64_t g_magazineKey{ 0 };
        std::uint64_t g_boltKey{ 0 };
        std::string g_namesFor;          // the node names the keys were looked up for
        bool g_dumped{ false };
        int g_dumpTries{ 0 };
        struct Box { bool known{ false }; float lo[3]{}; float hi[3]{}; };
        std::uint32_t g_boltBody{ parts::kNoBody };
        bool g_handleKnown{ false };
        float g_handle[3]{};
        Box g_boxes[2];                  // 0 the magazine, 1 the slide / bolt (the biggest of its colliders)
        std::string g_lastGrip;
        std::string g_lastDrive;
        std::uint32_t g_lastStatus[8]{ 999, 999, 999, 999, 999, 999, 999, 999 };

        void say(const char* format, ...) noexcept
        {
            if (!g_log) {
                return;
            }
            char text[400]{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(text, sizeof(text), format, args);
            va_end(args);
            g_log(text);
        }

        // A call's answer is logged when it differs from the last answer of that call.
        bool answer(int which, const char* what, api::Status status) noexcept
        {
            const auto value = static_cast<std::uint32_t>(status);
            if (g_lastStatus[which] != value) {
                g_lastStatus[which] = value;
                say("ROCK: %s -> %u%s", what, value, value == 0 ? " (ok)" : "");
            }
            return status == api::Status::Ok;
        }

        api::Hand handOf(int index) noexcept { return index == 0 ? api::Hand::Right : api::Hand::Left; }

        bool versionAtLeastOne(HMODULE module, unsigned& major, unsigned& minor) noexcept
        {
            major = minor = 0;
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(module, path, MAX_PATH)) {
                return false;
            }
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &handle);
            if (!size) {
                return false;
            }
            std::string buffer(size, '\0');
            if (!GetFileVersionInfoW(path, 0, size, buffer.data())) {
                return false;
            }
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
                return false;
            }
            major = HIWORD(info->dwFileVersionMS);
            minor = LOWORD(info->dwFileVersionMS);
            return major >= 1;
        }

        api::Transform toApi(const Pose& pose) noexcept
        {
            api::Transform out{};
            std::memcpy(out.rotate, pose.rotate, sizeof(out.rotate));
            std::memcpy(out.translate, pose.translate, sizeof(out.translate));
            out.scale = pose.scale;
            return out;
        }

        void copyName(char (&out)[64], const char* name) noexcept
        {
            std::memset(out, 0, sizeof(out));
            if (name) {
                const auto length = std::strlen(name);
                std::memcpy(out, name, length < sizeof(out) - 1 ? length : sizeof(out) - 1);
            }
        }

        // The source keys of the two nodes, for the grab points.
        void findKeys(const Frame& f) noexcept
        {
            g_magazineKey = g_boltKey = 0;
            std::uint32_t total = 0;
            auto status = g_parts->copySources(g_client.owner(), kSlot, g_key, nullptr, 0, &total);
            if (status != api::Status::Ok || total == 0) {
                say("ROCK: the weapon's nodes are not listed yet (%u, %u nodes)", static_cast<unsigned>(status), total);
                return;
            }
            if (total > parts::kMaxSources) {
                total = parts::kMaxSources;
            }
            std::vector<parts::Source> sources(total);
            std::uint32_t count = 0;
            status = g_parts->copySources(g_client.owner(), kSlot, g_key, sources.data(), total, &count);
            if (status != api::Status::Ok) {
                say("ROCK: the weapon's nodes could not be copied (%u)", static_cast<unsigned>(status));
                return;
            }
            int magazines = 0;
            int bolts = 0;
            for (std::uint32_t i = 0; i < count && i < total; ++i) {
                const auto& source = sources[i];
                if (f.magazine.node && std::strncmp(source.name, f.magazine.node, sizeof(source.name)) == 0) {
                    if (magazines++ == 0) g_magazineKey = source.sourceKey;
                }
                if (f.bolt.node && std::strncmp(source.name, f.bolt.node, sizeof(source.name)) == 0) {
                    if (bolts++ == 0) g_boltKey = source.sourceKey;
                }
            }
            say("ROCK: %u nodes in the weapon; '%s' x%d (key %016llX), '%s' x%d (key %016llX)", count, f.magazine.node ? f.magazine.node : "-",
                magazines, g_magazineKey, f.bolt.node ? f.bolt.node : "-", bolts, g_boltKey);
        }

        // A bolt with a handle out of one side of the gun (pipe guns, combat rifle and shotgun, SMG): the handle's
        // middle, from the points of the bolt's collider that stick out. A pistol slide has none.
        void findHandle() noexcept
        {
            g_handleKnown = false;
            const auto& b = g_boxes[1];
            if (!b.known) {
                return;
            }
            const float size[3]{ b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2] };
            const float middle[3]{ (b.lo[0] + b.hi[0]) * 0.5f, (b.lo[1] + b.hi[1]) * 0.5f, (b.lo[2] + b.hi[2]) * 0.5f };
            const float biggest = size[0] > size[1] ? (size[0] > size[2] ? size[0] : size[2]) : (size[1] > size[2] ? size[1] : size[2]);
            const float lean = -b.lo[0] - b.hi[0];   // above 0: it reaches further to the gun's left (-x)
            if (biggest < 4.5f) {
                std::memcpy(g_handle, middle, sizeof(g_handle));
                g_handleKnown = true;
                say("ROCK: the bolt is a small handle: taken at its middle (%.1f %.1f %.1f)", g_handle[0], g_handle[1], g_handle[2]);
                return;
            }
            if (std::fabs(lean) < 1.0f) {
                return;
            }
            const float edge = lean > 0.0f ? b.lo[0] : b.hi[0];
            g_handle[0] = edge + (lean > 0.0f ? 0.7f : -0.7f);
            g_handle[1] = middle[1];
            g_handle[2] = middle[2];
            const char* how = "the box";
            std::vector<api::Point3> points(parts::kMaxEvidencePoints);
            std::uint32_t count = 0;
            if (g_boltBody != parts::kNoBody &&
                g_parts->copyEvidencePoints(g_client.owner(), kSlot, g_boltBody, points.data(), parts::kMaxEvidencePoints, &count) == api::Status::Ok) {
                float sum[3]{};
                int used = 0;
                bool inside = count > 0;
                for (std::uint32_t i = 0; i < count && i < parts::kMaxEvidencePoints; ++i) {
                    const float p[3]{ points[i].x, points[i].y, points[i].z };
                    for (int k = 0; k < 3; ++k) {
                        inside = inside && p[k] > b.lo[k] - 0.6f && p[k] < b.hi[k] + 0.6f;
                    }
                    if (std::fabs(p[0] - edge) < 1.3f) {
                        for (int k = 0; k < 3; ++k) sum[k] += p[k];
                        ++used;
                    }
                }
                if (inside && used >= 2) {
                    for (int k = 0; k < 3; ++k) g_handle[k] = sum[k] / static_cast<float>(used);
                    how = "the collider's points";
                } else {
                    say("ROCK: the bolt collider's %u points were not usable (inside the box %d, at the edge %d)", count, inside ? 1 : 0, used);
                }
            }
            g_handleKnown = true;
            say("ROCK: the bolt's handle sticks out to the %s: taken at (%.1f %.1f %.1f), from %s", lean > 0.0f ? "left" : "right", g_handle[0], g_handle[1],
                g_handle[2], how);
        }

        // What ROCK knows of the weapon's parts: the base for holding other guns right.
        bool dumpEvidence() noexcept
        {
            std::vector<parts::EvidenceDetail> details(parts::kMaxEvidenceDetails);
            std::uint32_t count = 0;
            const auto status = g_parts->copyEvidenceDetails(g_client.owner(), kSlot, details.data(), parts::kMaxEvidenceDetails, &count);
            if (status != api::Status::Ok || count == 0) {
                return false;
            }
            say("ROCK parts of this weapon (%u colliders): kind (7 magazine, 8 magwell, 9 bolt, 10 slide, 11 charging handle), roles, node, box", count);
            float best[2]{ 0.0f, 0.0f };
            for (std::uint32_t i = 0; i < count && i < parts::kMaxEvidenceDetails; ++i) {
                const auto& d = details[i];
                const int which = d.partKind == 7 ? 0 : ((d.partKind >= 9 && d.partKind <= 11) ? 1 : -1);
                if (which >= 0 && d.localBoundsGame.valid) {
                    const auto& b = d.localBoundsGame;
                    const float volume = (b.max.x - b.min.x) * (b.max.y - b.min.y) * (b.max.z - b.min.z);
                    if (volume > best[which]) {
                        best[which] = volume;
                        if (which == 1) g_boltBody = d.bodyId;
                        g_boxes[which] = { true, { b.min.x, b.min.y, b.min.z }, { b.max.x, b.max.y, b.max.z } };
                    }
                }
                char name[65]{};
                std::memcpy(name, d.sourceName, 64);
                say("  part %u: kind %u action %u reload %u support %u socket %u body %u node '%s' box (%.1f %.1f %.1f)..(%.1f %.1f %.1f)%s", i, d.partKind,
                    d.actionRole, d.reloadRole, d.supportRole, d.socketRole, d.bodyId, name, d.localBoundsGame.min.x, d.localBoundsGame.min.y,
                    d.localBoundsGame.min.z, d.localBoundsGame.max.x, d.localBoundsGame.max.y, d.localBoundsGame.max.z,
                    d.localBoundsGame.valid ? "" : " (no box)");
            }
            findHandle();
            return true;
        }

        void forget() noexcept
        {
            if (g_registered && g_parts) {
                g_parts->clearGrabPoints(g_client.owner(), kSlot);
                g_parts->clearPartTargets(g_client.owner(), kSlot);
                say("ROCK: the gun's parts are given back");
            }
            if (g_driving && g_parts) {
                g_parts->clearDriveTargets(g_client.owner(), kSlot);
            }
            g_registered = false;
            g_driving = false;
            g_config.clear();
            g_key = 0;
            g_dumped = false;
            g_dumpTries = 0;
            g_boxes[0] = g_boxes[1] = {};
            g_handleKnown = false;
            g_boltBody = parts::kNoBody;
        }

        void setTargets(const Frame& f) noexcept
        {
            char text[256]{};
            std::snprintf(text, sizeof(text), "%016llX x%d m%d(%.2f %.2f %.2f r%.1f k%016llX) b%d(%.2f %.2f %.2f r%.1f k%016llX) h%d", g_key,
                f.exclusive ? 1 : 0, f.magazine.grab ? 1 : 0, f.magazine.point[0], f.magazine.point[1], f.magazine.point[2], f.magazine.radius,
                g_magazineKey, f.bolt.grab ? 1 : 0, f.bolt.point[0], f.bolt.point[1], f.bolt.point[2], f.bolt.radius, g_boltKey, f.freeHand);
            if (g_config == text || (f.keepTargets && g_registered)) {
                return;
            }
            g_config = text;
            parts::Target targets[2]{};
            parts::GrabPoint points[2]{};
            std::uint32_t targetCount = 0;
            std::uint32_t pointCount = 0;
            const auto add = [&](const Part& part, std::uint32_t group, std::uint64_t key) {
                if (!part.grab || !part.node) {
                    return;
                }
                auto& target = targets[targetCount++];
                target.flags = static_cast<std::uint32_t>(parts::TargetFlag::MatchSourceName) |
                               (f.exclusive ? 0u : static_cast<std::uint32_t>(parts::TargetFlag::NonExclusive));
                target.grabMode = parts::GrabMode::AttachOnly;
                target.weaponGenerationKey = g_key;
                target.groupId = group;
                target.priority = 100;
                copyName(target.sourceName, part.node);
                if (key) {
                    auto& point = points[pointCount++];
                    point.pointId = group;
                    point.groupId = group;
                    point.weaponGenerationKey = g_key;
                    point.sourceKey = key;
                    point.handMask = f.freeHand == 0 ? 2u : 1u;   // left 1, right 2
                    point.radiusGame = part.radius;
                    point.pointInSource = { part.point[0], part.point[1], part.point[2] };
                }
            };
            add(f.magazine, kGroupMagazine, g_magazineKey);
            add(f.bolt, kGroupBolt, g_boltKey);
            if (targetCount == 0) {
                g_parts->clearGrabPoints(g_client.owner(), kSlot);
                const auto status = g_parts->clearPartTargets(g_client.owner(), kSlot);
                say("ROCK: nothing of the gun is ours to take now (%u)", static_cast<unsigned>(status));
                g_registered = false;
                return;
            }
            const auto status = g_parts->setPartTargets(g_client.owner(), kSlot, targets, targetCount);
            api::Status pointStatus = api::Status::NotReady;
            if (status == api::Status::Ok) {
                g_registered = true;
                pointStatus = pointCount ? g_parts->setGrabPoints(g_client.owner(), kSlot, points, pointCount) :
                                           g_parts->clearGrabPoints(g_client.owner(), kSlot);
            }
            say("ROCK: the free hand may take%s%s%s: targets %u, %u grab points %u", f.magazine.grab ? " the magazine" : "", f.bolt.grab ? " the slide" : "",
                f.exclusive ? " and nothing else on the gun" : "", static_cast<unsigned>(status), pointCount, static_cast<unsigned>(pointStatus));
        }

        void setDrives(const Frame& f) noexcept
        {
            parts::DriveTarget drives[2]{};
            std::uint32_t count = 0;
            const auto add = [&](const Part& part, std::uint32_t group) {
                if (!part.drive || !part.node) {
                    return;
                }
                auto& drive = drives[count++];
                drive.flags = static_cast<std::uint32_t>(parts::TargetFlag::MatchSourceName);
                drive.driveSpace = parts::DriveSpace::SourceParentLocal;
                drive.weaponGenerationKey = g_key;
                drive.groupId = group;
                drive.priority = 100;
                drive.leaseFrames = 3;
                drive.targetTransform = toApi(part.parentLocal);
                copyName(drive.sourceName, part.node);
            };
            add(f.magazine, kGroupMagazine);
            add(f.bolt, kGroupBolt);
            if (count == 0) {
                if (g_driving) {
                    g_driving = false;
                    answer(2, "parts let go (clearDriveTargets)", g_parts->clearDriveTargets(g_client.owner(), kSlot));
                    g_lastStatus[1] = 999;
                }
                return;
            }
            g_driving = true;
            answer(1, "parts moved by the plugin (setDriveTargets)", g_parts->setDriveTargets(g_client.owner(), kSlot, drives, count));
            if (g_frames % 45 == 0) {
                parts::DriveResult results[4]{};
                std::uint32_t got = 0;
                if (g_parts->copyDriveResults(g_client.owner(), kSlot, results, 4, &got) == api::Status::Ok) {
                    char text[160]{};
                    int at = std::snprintf(text, sizeof(text), "%u:", got);
                    for (std::uint32_t i = 0; i < got && i < 4 && at > 0 && at < 140; ++i) {
                        at += std::snprintf(text + at, sizeof(text) - at, " g%u=%u", results[i].groupId, static_cast<unsigned>(results[i].result));
                    }
                    if (g_lastDrive != text) {
                        g_lastDrive = text;
                        say("ROCK: drive results %s (1 applied, 2 unresolved, 4 no parent, 5 lost priority)", text);
                    }
                }
            }
        }
    }

    bool connect(LogFn logFn, bool enabled) noexcept
    {
        g_log = logFn;
        if (connected() || g_refused) {
            return connected();
        }
        if (!enabled) {
            say("ROCK: its API is not used (bRockParts=0)");
            g_refused = true;
            return false;
        }
        const auto module = GetModuleHandleW(L"ROCK.dll");
        if (!module) {
            say("ROCK: ROCK.dll is not loaded; the hand is held by this plugin alone");
            return false;
        }
        unsigned major = 0, minor = 0;
        if (!versionAtLeastOne(module, major, minor)) {
            say("ROCK: ROCK.dll is version %u.%u, older than 1.0; its API is not asked", major, minor);
            g_refused = true;
            return false;
        }
        const auto query = reinterpret_cast<api::QueryInterfaceV1>(GetProcAddress(module, api::kQueryExportName));
        if (!query) {
            say("ROCK %u.%u: %s is not exported", major, minor, api::kQueryExportName);
            g_refused = true;
            return false;
        }
        if (g_client.owner() == 0) {
            const auto status = g_client.connect(query, "ManualReloadF");
            if (status != api::Status::Ok) {
                say("ROCK %u.%u: registration failed (%u)", major, minor, static_cast<unsigned>(status));
                return false;
            }
        }
        const auto weaponStatus = g_client.acquire(3, g_weapon);
        const auto partsStatus = g_client.acquire(3, g_parts);
        const char* version = nullptr;
        if (g_client.core() && g_client.core()->getModVersion) {
            g_client.core()->getModVersion(g_client.owner(), &version);
        }
        say("ROCK %s (file %u.%u) connected: weapon %u, weapon parts %u (0 = ok)", version ? version : "?", major, minor,
            static_cast<unsigned>(weaponStatus), static_cast<unsigned>(partsStatus));
        if (!connected()) {
            g_refused = true;
        }
        return connected();
    }

    bool connected() noexcept { return g_weapon && g_parts; }

    Result frame(const Frame& f) noexcept
    {
        Result result{};
        if (!connected()) {
            return result;
        }
        ++g_frames;
        if (!f.gun) {
            forget();
            return result;
        }
        std::uint64_t key = 0;
        const auto keyStatus = g_weapon->getInteractionKey(g_client.owner(), kSlot, &key);
        if (!answer(0, "the weapon's key (getInteractionKey)", keyStatus) || key == 0) {
            if (g_key != 0) {
                forget();
            }
            return result;
        }
        result.active = true;
        std::string names = std::string(f.magazine.node ? f.magazine.node : "") + "|" + (f.bolt.node ? f.bolt.node : "");
        if (key != g_key || names != g_namesFor) {
            say("ROCK: weapon key %016llX (was %016llX)", key, g_key);
            g_key = key;
            g_namesFor = names;
            g_config.clear();
            g_registered = false;
            g_dumped = false;
            g_dumpTries = 0;
            g_boxes[0] = g_boxes[1] = {};
            g_handleKnown = false;
            g_boltBody = parts::kNoBody;
            for (auto& status : g_lastStatus) {
                status = 999;
            }
            g_lastStatus[0] = 0;
            findKeys(f);
        } else if ((g_magazineKey == 0 || g_boltKey == 0) && g_frames % 90 == 0 && g_dumpTries < 12) {
            findKeys(f);
            g_config.clear();
        }
        if (!g_dumped && g_frames % 90 == 0 && g_dumpTries < 12) {
            ++g_dumpTries;
            g_dumped = dumpEvidence();
            if (!g_dumped && g_dumpTries == 12) {
                say("ROCK: no colliders were reported for this weapon");
            }
        }

        setTargets(f);
        setDrives(f);
        if (f.chamber != 0 && g_weapon->setChamberMarker) {
            answer(3, "chamber mark beside the ammo counter (setChamberMarker)",
                g_weapon->setChamberMarker(g_client.owner(), kSlot,
                    f.chamber == 2 ? api::weapon::ChamberMarker::Chambered : api::weapon::ChamberMarker::Empty, 3));
        }

        parts::GripState grip{};
        const auto gripStatus = g_parts->getGripState(g_client.owner(), handOf(f.freeHand), &grip);
        if (answer(4, "what the free hand holds (getGripState)", gripStatus) && grip.active) {
            const bool ours = grip.targetOwner == g_client.owner();
            result.onMagazine = ours && grip.targetGroupId == kGroupMagazine;
            result.onBolt = ours && grip.targetGroupId == kGroupBolt;
            result.onGun = !result.onMagazine && !result.onBolt;
        }
        char name[65]{};
        std::memcpy(name, grip.sourceName, 64);
        char text[200]{};
        std::snprintf(text, sizeof(text), "active %u kind %u attachOnly %u ours %d group %u part kind %u body %u node '%s'", grip.active,
            static_cast<unsigned>(grip.gripKind), grip.attachOnly, grip.targetOwner == g_client.owner() ? 1 : 0, grip.targetGroupId, grip.partKind,
            grip.bodyId, name);
        if (g_lastGrip != text) {
            g_lastGrip = text;
            say("ROCK: free hand grip: %s", text);
        }
        return result;
    }

    bool partBox(int which, float lo[3], float hi[3]) noexcept
    {
        if (which < 0 || which > 1 || !g_boxes[which].known) {
            return false;
        }
        std::memcpy(lo, g_boxes[which].lo, sizeof(g_boxes[which].lo));
        std::memcpy(hi, g_boxes[which].hi, sizeof(g_boxes[which].hi));
        return true;
    }

    bool boltHandle(float point[3]) noexcept
    {
        if (!g_handleKnown) {
            return false;
        }
        std::memcpy(point, g_handle, sizeof(g_handle));
        return true;
    }
}
