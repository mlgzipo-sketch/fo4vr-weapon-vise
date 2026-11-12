#include "RockLink.h"

#include <Windows.h>

#include "ROCK/Animation.h"
#include "ROCK/Client.h"
#include "ROCK/Hands.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>

#pragma comment(lib, "version.lib")

namespace rocklink
{
    namespace
    {
        namespace api = rock::api;

        LogFn g_log{ nullptr };
        bool g_tried{ false };
        api::Client g_client;
        const api::hands::Api* g_hands{ nullptr };
        const api::animation::Api* g_anim{ nullptr };
        bool g_haveGrip{ false };
        api::Transform g_grip[api::hands::kFingerLocalTransformCount]{};
        bool g_holding[2]{};
        int g_failures{ 0 };
        constexpr auto kGripFile = "Data/F4SE/Plugins/ManualReloadF_grip.bin";

        void say(const char* format, auto... args) noexcept
        {
            if (!g_log) return;
            char text[256]{};
            std::snprintf(text, sizeof(text), format, args...);
            g_log(text);
        }

        api::Hand handOf(int index) noexcept { return index == 0 ? api::Hand::Right : api::Hand::Left; }

        // The file version of the loaded ROCK.dll (major, minor), 0.0 when unknown.
        bool versionAtLeastOne(HMODULE module, unsigned& major, unsigned& minor) noexcept
        {
            major = minor = 0;
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &handle);
            if (!size) return false;
            std::string buffer(size, '\0');
            if (!GetFileVersionInfoW(path, 0, size, buffer.data())) return false;
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return false;
            major = HIWORD(info->dwFileVersionMS);
            minor = LOWORD(info->dwFileVersionMS);
            return major >= 1;
        }

        void loadGrip() noexcept
        {
            std::ifstream file(kGripFile, std::ios::binary);
            api::Transform grip[api::hands::kFingerLocalTransformCount]{};
            if (file.read(reinterpret_cast<char*>(grip), sizeof(grip))) {
                std::memcpy(g_grip, grip, sizeof(grip));
                g_haveGrip = true;
                say("ROCK: grip pose loaded from %s", kGripFile);
            }
        }
    }

    bool connect(LogFn logFn, bool enabled) noexcept
    {
        g_log = logFn;
        if (connected() || g_tried) return connected();   // one try per refusal reason that cannot change; retried on load otherwise
        if (!enabled) {
            say("ROCK: API not used (bRockApi=0)");
            g_tried = true;
            return false;
        }
        const auto module = GetModuleHandleW(L"ROCK.dll");
        if (!module) {
            say("ROCK: ROCK.dll is not loaded; fingers stay with FRIK");
            return false;
        }
        unsigned major = 0, minor = 0;
        if (!versionAtLeastOne(module, major, minor)) {
            say("ROCK: ROCK.dll version %u.%u is older than 1.0; its API is not asked (fingers stay with FRIK)", major, minor);
            g_tried = true;
            return false;
        }
        const auto query = reinterpret_cast<api::QueryInterfaceV1>(GetProcAddress(module, api::kQueryExportName));
        if (!query) {
            say("ROCK: %s not exported", api::kQueryExportName);
            return false;
        }
        auto status = g_client.connect(query, "ManualReloadF");
        if (status != api::Status::Ok) {
            say("ROCK %u.%u: connect failed (%u)", major, minor, static_cast<unsigned>(status));
            return false;
        }
        const auto handsStatus = g_client.acquire(static_cast<std::uint32_t>(api::hands::Permission::Read), g_hands);
        const auto animStatus = g_client.acquire(static_cast<std::uint32_t>(api::animation::Permission::Write), g_anim);
        const char* version = "?";
        if (g_client.core() && g_client.core()->getModVersion) {
            g_client.core()->getModVersion(g_client.owner(), &version);
        }
        say("ROCK %s connected: hands %u, animation %u", version ? version : "?", static_cast<unsigned>(handsStatus), static_cast<unsigned>(animStatus));
        loadGrip();
        return connected();
    }

    bool connected() noexcept { return g_hands && g_anim; }
    bool hasGrip() noexcept { return g_haveGrip; }

    bool captureGrip(int index) noexcept
    {
        if (!g_hands) return false;
        api::hands::PresentedHandPose pose{};
        const auto status = g_hands->getPresentedHandPose(g_client.owner(), handOf(index), &pose);
        constexpr std::uint32_t all = (1u << api::hands::kFingerLocalTransformCount) - 1;
        if (status != api::Status::Ok || !(pose.flags & static_cast<std::uint32_t>(api::hands::PresentedHandPoseFlag::FingerLocalsValid)) ||
            (pose.fingerLocalTransformMask & all) != all) {
            return false;
        }
        std::memcpy(g_grip, pose.fingerLocalTransforms, sizeof(g_grip));
        g_haveGrip = true;
        std::ofstream file(kGripFile, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(g_grip), sizeof(g_grip));
        say("ROCK: grip pose taken from the %s hand holding a magazine (saved)", index == 0 ? "right" : "left");
        return true;
    }

    void holdFingers(int index, bool on) noexcept
    {
        if (!g_anim || index < 0 || index > 1) return;
        if (!on || !g_haveGrip) {
            if (g_holding[index]) {
                g_anim->clearHandVisualAuthority(g_client.owner(), handOf(index));
                g_holding[index] = false;
            }
            return;
        }
        api::animation::HandVisualAuthorityRequest request{};
        request.hand = handOf(index);
        request.flags = static_cast<std::uint32_t>(api::animation::HandVisualAuthorityFlag::FingerLocalTransforms);
        request.priority = 500;
        request.fingerLocalTransformMask = (1u << api::animation::kFingerLocalTransformCount) - 1;
        std::memcpy(request.fingerLocalTransforms, g_grip, sizeof(g_grip));
        request.leaseFrames = 10;
        api::Sample sample{};
        if (g_anim->getSample(g_client.owner(), &sample) == api::Status::Ok) {
            request.worldGeneration = sample.worldGeneration;
            request.skeletonGeneration = sample.skeletonGeneration;
            request.providerGeneration = sample.providerGeneration;
        }
        const auto status = g_anim->setHandVisualAuthority(g_client.owner(), &request);
        if (status == api::Status::Ok) {
            g_holding[index] = true;
        } else if (g_failures++ < 5) {
            say("ROCK: finger pose refused (%u)", static_cast<unsigned>(status));
        }
    }
}
