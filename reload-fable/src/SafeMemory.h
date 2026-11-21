// ManualReloadF: safe reading of game memory and walking node trees (a bad pointer must never crash the game)
#pragma once

#include <RE/Fallout.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mrf
{
    // bumped once a frame: readable regions found during a frame are trusted until the next one
    extern std::atomic<std::uint32_t> g_memoryEpoch;

    bool readableRegion(const void* address, std::uintptr_t* regionEnd) noexcept;
    bool readable(const void* address, std::size_t size) noexcept;
    // readable, aligned, and its vtable is in the game's .rdata
    bool looksLikeObject(const void* object) noexcept;
    std::string nodeName(const RE::NiAVObject* object) noexcept;

    template <class F>
    void forEachNode(RE::NiAVObject* object, F&& visit, int depth = 0) noexcept
    {
        if (!looksLikeObject(object) || depth > 40) {
            return;
        }
        if (!visit(object, depth)) {
            return;
        }
        auto* node = object->IsNode();
        if (!node) {
            return;
        }
        auto& children = node->GetRuntimeData().children;
        const auto count = children.size();
        if (count > 256) {
            return;
        }
        for (std::uint16_t index = 0; index < count; ++index) {
            forEachNode(children[index].get(), visit, depth + 1);
        }
    }

    RE::NiAVObject* findNamed(RE::NiAVObject* root, const char* name) noexcept;
    void setCulled(RE::NiAVObject* object, bool culled) noexcept;
}
