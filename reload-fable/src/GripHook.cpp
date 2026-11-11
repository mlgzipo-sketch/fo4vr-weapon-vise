// Hides the free hand's grip from every other reader of the controller while ManualReloadF owns that press.
// ROCK takes the pistol with the free hand whenever that hand's grip is down next to the firing grip, and
// its API is closed to this build. So IVRSystem::GetControllerState and GetControllerStateWithPose are
// hooked (the functions, not a table slot, so readers that saved the address still pass through).
// Kept apart from the rest of the plugin because it needs Windows.h.
#include <Windows.h>

#include <MinHook.h>

#include "api/openvr.h"

#include "GripHook.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
griphook::Log g_log{ nullptr };
std::atomic<bool> g_hide{ true };

void note(const char* format, ...) noexcept
{
    char text[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (g_log) {
        g_log(text);
    }
}

bool readable(const void* address, std::size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!address || VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<std::uintptr_t>(address) + size <= end;
}

using State_t = bool (*)(void*, vr::TrackedDeviceIndex_t, vr::VRControllerState_t*, std::uint32_t);
using StatePose_t = bool (*)(void*, vr::ETrackingUniverseOrigin, vr::TrackedDeviceIndex_t, vr::VRControllerState_t*, std::uint32_t, vr::TrackedDevicePose_t*);

struct Version { const char* name; int stateIndex; int poseIndex; };
// slots of GetControllerState / GetControllerStateWithPose in the two interface versions used here
// (the game and this plugin: 017; ROCK: 019), counted from the OpenVR headers of those versions
constexpr std::array<Version, 2> kVersions{ { { "IVRSystem_017", 33, 34 }, { "IVRSystem_019", 34, 35 } } };

std::array<State_t, 2>     g_origState{};
std::array<StatePose_t, 2> g_origPose{};
std::array<void*, 2>       g_self{};
std::array<void*, 2>       g_stateTarget{};
std::array<void*, 2>       g_poseTarget{};
bool                       g_tried{ false };
bool                       g_installed{ false };

std::atomic<bool>          g_armed{ false };    // the next grip press of the free hand is ours
std::atomic<bool>          g_owned{ false };    // this press is ours: hidden from everyone else
std::atomic<bool>          g_previous{ false };
std::atomic<std::uint32_t> g_device{ vr::k_unTrackedDeviceIndexInvalid };
std::atomic<bool>          g_axis2IsGrip{ false };
std::atomic<std::uint64_t> g_gripMask{ 1ull << 2 };
std::atomic<std::uint32_t> g_hidden{ 0 };       // reads answered with the grip hidden (for the log)

// One place decides whose press it is, whoever reads the controller first in a frame.
void arbitrate(bool raw) noexcept
{
    const bool previous = g_previous.exchange(raw);
    if (!raw) {
        g_owned.store(false);
    } else if (!previous && g_armed.load()) {
        g_owned.store(true);
    }
}

void filter(vr::TrackedDeviceIndex_t device, vr::VRControllerState_t& state) noexcept
{
    if (device != g_device.load()) {
        return;
    }
    const auto mask = g_gripMask.load();
    arbitrate((state.ulButtonPressed & mask) != 0);
    if (!g_owned.load() || !g_hide.load()) {
        return;
    }
    auto hide = mask;
    if (g_axis2IsGrip.load()) {
        hide |= 1ull << 34;
        state.rAxis[2].x = 0.0f;
        state.rAxis[2].y = 0.0f;
    }
    state.ulButtonPressed &= ~hide;
    state.ulButtonTouched &= ~hide;
    g_hidden.fetch_add(1);
}

template <int N>
bool detourState(void* self, vr::TrackedDeviceIndex_t device, vr::VRControllerState_t* state, std::uint32_t size)
{
    const bool ok = g_origState[N](self, device, state, size);
    if (ok && state && size >= sizeof(vr::VRControllerState_t)) {
        filter(device, *state);
    }
    return ok;
}

template <int N>
bool detourPose(void* self, vr::ETrackingUniverseOrigin origin, vr::TrackedDeviceIndex_t device, vr::VRControllerState_t* state, std::uint32_t size, vr::TrackedDevicePose_t* pose)
{
    const bool ok = g_origPose[N](self, origin, device, state, size, pose);
    if (ok && state && size >= sizeof(vr::VRControllerState_t)) {
        filter(device, *state);
    }
    return ok;
}

HMODULE moduleOf(const void* address) noexcept
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

std::string moduleName(HMODULE module) noexcept
{
    char path[MAX_PATH]{};
    if (!module || GetModuleFileNameA(module, path, MAX_PATH) == 0) {
        return "?";
    }
    const char* name = std::strrchr(path, '\\');
    return name ? name + 1 : path;
}

// A table slot another mod has already redirected: what it held is read from the module's file.
void* originalFromDisk(HMODULE module, std::uintptr_t slotRva) noexcept
{
    try {
        wchar_t path[1024]{};
        if (GetModuleFileNameW(module, path, 1024) == 0) {
            return nullptr;
        }
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        IMAGE_DOS_HEADER dos{};
        file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
        if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE) {
            return nullptr;
        }
        IMAGE_NT_HEADERS64 nt{};
        file.seekg(dos.e_lfanew);
        file.read(reinterpret_cast<char*>(&nt), sizeof(nt));
        if (!file || nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return nullptr;
        }
        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        file.seekg(dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader);
        file.read(reinterpret_cast<char*>(sections.data()), static_cast<std::streamsize>(sections.size() * sizeof(IMAGE_SECTION_HEADER)));
        if (!file) {
            return nullptr;
        }
        std::uint64_t value = 0;
        bool found = false;
        for (const auto& section : sections) {
            if (slotRva >= section.VirtualAddress && slotRva + 8 <= static_cast<std::uintptr_t>(section.VirtualAddress) + section.SizeOfRawData) {
                file.seekg(static_cast<std::streamoff>(slotRva - section.VirtualAddress + section.PointerToRawData));
                file.read(reinterpret_cast<char*>(&value), sizeof(value));
                found = static_cast<bool>(file);
                break;
            }
        }
        if (!found || value < nt.OptionalHeader.ImageBase) {
            return nullptr;
        }
        const std::uint64_t rva = value - nt.OptionalHeader.ImageBase;
        for (const auto& section : sections) {
            if (rva >= section.VirtualAddress && rva < static_cast<std::uint64_t>(section.VirtualAddress) + section.Misc.VirtualSize &&
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
                return reinterpret_cast<std::uint8_t*>(module) + rva;
            }
        }
    } catch (...) {
    }
    return nullptr;
}

// The function behind a table slot of an interface object, even when the slot was redirected.
void* resolve(void* self, int index, const char* what) noexcept
{
    if (!self || !readable(self, sizeof(void*))) {
        return nullptr;
    }
    auto** table = *static_cast<void***>(self);
    if (!table || !readable(table + index, sizeof(void*))) {
        return nullptr;
    }
    void* target = table[index];
    const auto home = moduleOf(table);
    const auto owner = moduleOf(target);
    if (!home) {
        note("Grip hook: %s table is not inside a module; skipped", what);
        return nullptr;
    }
    if (owner != home) {
        const auto slotRva = reinterpret_cast<std::uintptr_t>(table + index) - reinterpret_cast<std::uintptr_t>(home);
        void* original = originalFromDisk(home, slotRva);
        note("Grip hook: %s slot is redirected to %s; original in %s %s", what, moduleName(owner).c_str(),
            moduleName(home).c_str(), original ? "found" : "NOT found");
        return original;
    }
    return target;
}

template <int N>
void hookVersion(void* (*getInterface)(const char*, vr::EVRInitError*)) noexcept
{
    const auto& version = kVersions[N];
    vr::EVRInitError error = vr::VRInitError_None;
    void* self = getInterface(version.name, &error);
    if (!self) {
        note("Grip hook: %s not offered (error %d)", version.name, static_cast<int>(error));
        return;
    }
    g_self[N] = self;
    g_stateTarget[N] = resolve(self, version.stateIndex, version.name);
    g_poseTarget[N] = resolve(self, version.poseIndex, version.name);
    for (int earlier = 0; earlier < N; ++earlier) {   // two versions served by one function: one hook
        if (g_stateTarget[N] && g_stateTarget[N] == g_stateTarget[earlier]) {
            g_origState[N] = g_origState[earlier];
            g_stateTarget[N] = nullptr;
        }
        if (g_poseTarget[N] && g_poseTarget[N] == g_poseTarget[earlier]) {
            g_origPose[N] = g_origPose[earlier];
            g_poseTarget[N] = nullptr;
        }
    }
    int made = 0;
    if (g_stateTarget[N]) {
        void* original = nullptr;
        if (MH_CreateHook(g_stateTarget[N], reinterpret_cast<void*>(&detourState<N>), &original) == MH_OK && original) {
            g_origState[N] = reinterpret_cast<State_t>(original);   // known before the hook goes live
            if (MH_EnableHook(g_stateTarget[N]) == MH_OK) {
                ++made;
            }
        }
    }
    if (g_poseTarget[N]) {
        void* original = nullptr;
        if (MH_CreateHook(g_poseTarget[N], reinterpret_cast<void*>(&detourPose<N>), &original) == MH_OK && original) {
            g_origPose[N] = reinterpret_cast<StatePose_t>(original);
            if (MH_EnableHook(g_poseTarget[N]) == MH_OK) {
                ++made;
            }
        }
    }
    note("Grip hook: %s in %s: %d new hooks (state %s, pose %s)", version.name, moduleName(moduleOf(*static_cast<void***>(self))).c_str(),
        made, g_origState[N] ? "ok" : "no", g_origPose[N] ? "ok" : "no");
    if (g_origState[N] || g_origPose[N]) {
        g_installed = true;
    }
}

}

namespace griphook
{
    void install(Log log, bool hide) noexcept
    {
        if (g_tried) {
            return;
        }
        g_tried = true;
        g_log = log;
        g_hide.store(hide);
        if (!hide) {
            note("Grip hook: off (bHideGrip=0)");
            return;
        }
        using GetInterface_t = void* (*)(const char*, vr::EVRInitError*);
        const auto module = GetModuleHandleW(L"openvr_api.dll");
        const auto get = module ? reinterpret_cast<GetInterface_t>(reinterpret_cast<void*>(GetProcAddress(module, "VR_GetGenericInterface"))) : nullptr;
        if (!get) {
            note("Grip hook: openvr_api.dll not found");
            return;
        }
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            note("Grip hook: MinHook failed to start (%d)", static_cast<int>(status));
            return;
        }
        hookVersion<0>(get);
        hookVersion<1>(get);
        if (!g_installed) {
            note("Grip hook: nothing hooked; other mods will see the free hand's grip while it holds a magazine");
        }
    }

    bool installed() noexcept { return g_installed; }
    void arbitrate(bool rawGrip) noexcept { ::arbitrate(rawGrip); }
    void setArmed(bool armed) noexcept { g_armed.store(armed); }
    bool owned() noexcept { return g_owned.load(); }
    void setOwned(bool value) noexcept { g_owned.store(value); }
    void setDevice(std::uint32_t device) noexcept { g_device.store(device); }
    void setGripMask(std::uint64_t mask) noexcept { g_gripMask.store(mask); }
    void setAxis2IsGrip(bool value) noexcept { g_axis2IsGrip.store(value); }
    std::uint32_t hiddenCount() noexcept { return g_hidden.load(); }

    bool rawState(std::uint32_t device, void* state, std::uint32_t size) noexcept
    {
        if (!g_origState[0] || !g_self[0] || !state) {
            return false;
        }
        return g_origState[0](g_self[0], device, static_cast<vr::VRControllerState_t*>(state), size);
    }
}
