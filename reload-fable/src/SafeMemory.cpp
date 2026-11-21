#include "SafeMemory.h"

#include <algorithm>
#include <array>

namespace mrf
{

    // Readable committed regions seen this frame: walking the gun's node tree asked Windows (VirtualQuery) thousands
    // of times a frame, which took 10-20 ms (the hitches). A region found readable is trusted until the next frame.
    std::atomic<std::uint32_t> g_memoryEpoch{ 1 };

    struct ReadableRegion
    {
        std::uintptr_t start{ 0 }, end{ 0 };
    };

    bool readableRegion(const void* address, std::uintptr_t* regionEnd) noexcept
    {
        thread_local std::array<ReadableRegion, 16> regions{};
        thread_local std::uint32_t epoch = 0;
        thread_local std::size_t next = 0;
        const auto now = g_memoryEpoch.load(std::memory_order_relaxed);
        if (epoch != now) {
            epoch = now;
            regions.fill(ReadableRegion{});
        }
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        for (const auto& r : regions) {
            if (at >= r.start && at < r.end) {
                *regionEnd = r.end;
                return true;
            }
        }
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != 0x1000u /* MEM_COMMIT */) {
            return false;
        }
        constexpr std::uint32_t mask = 0x02u | 0x04u | 0x08u | 0x20u | 0x40u;
        if ((info.protect & mask) == 0 || (info.protect & 0x100u) != 0) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(info.baseAddress);
        const auto end = start + info.regionSize;
        regions[next] = ReadableRegion{ start, end };
        next = (next + 1) % regions.size();
        *regionEnd = end;
        return true;
    }

    bool readable(const void* address, std::size_t size) noexcept
    {
        if (!address) {
            return false;
        }
        std::uintptr_t end = 0;
        if (!readableRegion(address, &end)) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        if (start + size <= end) {
            return true;
        }
        return readable(reinterpret_cast<const void*>(end), start + size - end);   // runs on into the next region
    }

    bool looksLikeObject(const void* object) noexcept
    {
        if (!object || (reinterpret_cast<std::uintptr_t>(object) & 7) != 0 || !readable(object, sizeof(RE::NiAVObject))) {
            return false;
        }
        static const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
        const auto vtable = *static_cast<const std::uintptr_t*>(object);
        return vtable >= rdata.address() && vtable < rdata.address() + rdata.size();
    }

    std::string nodeName(const RE::NiAVObject* object) noexcept
    {
        if (!looksLikeObject(object)) {
            return {};
        }
        const char* text = object->GetName().data();
        if (!text || !readable(text, 1)) {
            return {};
        }
        std::uintptr_t end = 0;
        if (!readableRegion(text, &end)) {
            return {};
        }
        const std::size_t room = static_cast<std::size_t>(std::min<std::uintptr_t>(end - reinterpret_cast<std::uintptr_t>(text), 80));
        std::size_t size = 0;
        while (size < room && text[size] != '\0') {
            ++size;
        }
        return { text, size };
    }


    RE::NiAVObject* findNamed(RE::NiAVObject* root, const char* name) noexcept
    {
        RE::NiAVObject* found = nullptr;
        forEachNode(root, [&](RE::NiAVObject* object, int) {
            if (found) {
                return false;
            }
            if (nodeName(object) == name) {
                found = object;
                return false;
            }
            return true;
        });
        return found;
    }

    void setCulled(RE::NiAVObject* object, bool culled) noexcept
    {
        if (!looksLikeObject(object)) {
            return;
        }
        if (culled) {
            object->flags.flags |= 1ull;
        } else {
            object->flags.flags &= ~1ull;
        }
    }
}
