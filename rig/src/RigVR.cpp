// RigVR: a load-bearing rig for Fallout 4 VR (Into the Radius style).
//
// A mannequin stands in your settlement and wears the rig. Pouches are real items: take one in your hand (from the
// ground or out of the inventory), bring it to the rig on the mannequin, the places it may hang light up, let go and
// it hangs there. Grip a pouch on the mannequin with an empty hand and it is in your hand again. Take the mannequin by
// both shoulders to put the rig on (and the same at the mannequin to hang it back); on you nothing is moved, only used.
//
// How it is built:
//   - the rig is described by a rig file (Data\F4SE\Plugins\RigVR\Rigs\RigBase.txt, written from Blender): its models,
//     the skeleton bones they follow, and the areas (triangles) pouches may hang on.
//   - everything drawn is a model loaded by this plugin and attached to a node of the game: under the mannequin's 3D,
//     or under the bones of your body (Pelvis, SPINE2), with a fixed local transform. The engine moves them with the
//     mannequin / your body; nothing is placed by hand each frame.
//   - what a pouch holds is a reservation of items that really are in your inventory.
//   - the rig (which pouch hangs where, what it holds) is kept by the quest script in the save.
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using rigvr::runtime::logInfo;
    using rigvr::runtime::logNote;
    using rigvr::runtime::logWarning;

    void note(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logNote(text);
    }

    void detail(const char* format, ...) noexcept
    {
        char text[512]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        logInfo(text);
    }

    // ------------------------------------------------------------ pouch kinds

    constexpr int kMaxPouches = 12;
    constexpr int kKinds = 6;         // pouch kinds 1..6
    constexpr int kRigItem = 7;       // the rig itself as an item
    constexpr int kItemsPerPouch = 4;

    enum Kind : int
    {
        kEmpty = 0,
        kMagPistol = 1,
        kMagRifle = 2,
        kMagUniversal = 3,
        kHolster = 4,
        kDump = 5,
        kSling = 6,       // carbine / big gun holder
    };

    const char* const kKindNames[kKinds + 1]{ "", "MagPistol", "MagRifle", "MagUniversal", "Holster", "Dump", "Sling" };

    bool isMag(int kind) noexcept { return kind == kMagPistol || kind == kMagRifle || kind == kMagUniversal; }

    const char* kindTitle(int kind) noexcept
    {
        switch (kind) {
        case kMagPistol: return "Подсумок для пистолетных магазинов";
        case kMagRifle: return "Подсумок для винтовочных магазинов";
        case kMagUniversal: return "Универсальный подсумок";
        case kHolster: return "Кобура";
        case kDump: return "Сумка";
        case kSling: return "Держатель карабина";
        case kRigItem: return "Разгрузка";
        default: return "";
        }
    }

    int kindByName(const std::string& name) noexcept
    {
        for (int k = 1; k <= kKinds; ++k) {
            if (name == kKindNames[k]) {
                return k;
            }
        }
        return kEmpty;
    }

    std::string pouchModel(int kind) { return std::string("RigVR\\Custom\\Pouch") + kKindNames[kind] + ".nif"; }

    // how close the palm must be to a pouch's middle to use / take it
    float reachOf(int kind) noexcept
    {
        switch (kind) {
        case kSling: return 18.0f;
        case kHolster: return 10.0f;
        case kDump: return 11.0f;
        default: return 8.0f;
        }
    }

    // how far the middle of a pouch is out from where it hangs (its back)
    float depthOf(int kind) noexcept
    {
        switch (kind) {
        case kSling: return 6.0f;
        case kDump: return 5.0f;
        default: return 3.0f;
        }
    }

    int capacityOf(int kind) noexcept
    {
        switch (kind) {
        case kHolster: return 1;
        case kSling: return 1;
        case kDump: return 12;
        default: return 0;
        }
    }

    // ------------------------------------------------------------ settings

    struct Settings
    {
        int   gunHand{ 0 };            // 0 = right hand holds the gun
        int   gripButton{ 2 };
        bool  haptics{ true };
        bool  messages{ true };
        float holdRadius{ 16.0f };     // a pouch item this close to a gripping palm is held by it
        float snapRadius{ 20.0f };     // a held pouch this close to an area hangs there when let go
        float showAreas{ 120.0f };     // the areas light up with a pouch in the hand this close to the mannequin
        float mannequinReach{ 150.0f };// the mannequin is used within this distance
        float wearRadius{ 14.0f };     // both hands this close to the mannequin's shoulders to put the rig on / hang it back
        float wearWindow{ 0.6f };      // ..both grips pressed within this many seconds
        float packSeconds{ 1.5f };     // hold the grip on the mannequin's head this long to fold it up
        float storeRadius{ 16.0f };    // an item let go this close to the palm over a pouch goes in
        float mouthRadius{ 12.0f };    // a chem / food from a pouch this close to the mouth is used
        float holsterCooldown{ 1.0f };
        float upSnapDegrees{ 25.0f };  // a pouch held this close to upright hangs straight
        float areaSmooth{ 7.0f };      // the faces of an area this close to where a pouch hangs set which way it faces
        float shoulderX{ 14.7f };      // the mannequin's shoulders in its own space (right, forward, up)
        float shoulderY{ -3.4f };
        float shoulderZ{ 105.0f };
        float headZ{ 116.0f };
        float bodyScale{ 1.15f };     // the rig on you is this much bigger than on the mannequin (sits over armour)
        float bodyScaleZ{ 85.0f };    // ..around this height
        float holsterGun[3]{ 0.0f, 3.0f, 11.0f };   // where a holstered gun's grip is in the holster (right, out, up)
        float slingGun[3]{ 0.0f, 6.0f, 9.0f };      // ..and in the carbine holder
    };
    Settings g_settings;

    void loadSettings() noexcept
    {
        struct Entry { const char* key; int type; void* value; };
        auto& s = g_settings;
        const Entry entries[]{
            { "iGunHand", 1, &s.gunHand }, { "iGripButton", 1, &s.gripButton }, { "bHaptics", 0, &s.haptics },
            { "bMessages", 0, &s.messages }, { "fHoldRadius", 2, &s.holdRadius }, { "fSnapRadius", 2, &s.snapRadius },
            { "fShowAreas", 2, &s.showAreas }, { "fMannequinReach", 2, &s.mannequinReach }, { "fWearRadius", 2, &s.wearRadius },
            { "fWearWindow", 2, &s.wearWindow }, { "fPackSeconds", 2, &s.packSeconds }, { "fStoreRadius", 2, &s.storeRadius },
            { "fMouthRadius", 2, &s.mouthRadius }, { "fHolsterCooldown", 2, &s.holsterCooldown }, { "fUpSnapDegrees", 2, &s.upSnapDegrees }, { "fAreaSmooth", 2, &s.areaSmooth },
            { "fShoulderX", 2, &s.shoulderX }, { "fShoulderY", 2, &s.shoulderY }, { "fShoulderZ", 2, &s.shoulderZ }, { "fHeadZ", 2, &s.headZ }, { "fBodyScale", 2, &s.bodyScale }, { "fBodyScaleZ", 2, &s.bodyScaleZ },
        };
        try {
            std::ifstream file("Data/F4SE/Plugins/RigVR.ini");
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
                if (key == "fHolsterGun" || key == "fSlingGun") {
                    float* out = key == "fHolsterGun" ? s.holsterGun : s.slingGun;
                    const char* at = line.c_str() + equals + 1;
                    for (int i = 0; i < 3; ++i) {
                        char* end = nullptr;
                        const float v = std::strtof(at, &end);
                        if (end == at) break;
                        out[i] = v;
                        at = end;
                        while (*at == ',' || *at == ' ') ++at;
                    }
                    continue;
                }
                const float value = std::strtof(line.c_str() + equals + 1, nullptr);
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
        note("Settings: gun hand %d, hold %.0f, snap %.0f, wear %.0f", s.gunHand, s.holdRadius, s.snapRadius, s.wearRadius);
    }

    // ------------------------------------------------------------ the rig file

    struct Area
    {
        std::string name;
        std::string bone;
        std::string highlight;   // model shown while a pouch is brought to the rig
        std::vector<std::array<RE::NiPoint3, 3>> tris;
        std::uint32_t kinds{ 0xFFFFFFFFu };   // bit per pouch kind that may hang here
    };

    struct Preset
    {
        int kind{ kEmpty };
        int area{ -1 };
        RE::NiTransform pose{};
    };

    struct RigDef
    {
        bool loaded{ false };
        std::map<std::string, RE::NiTransform> binds;
        std::vector<std::pair<std::string, std::string>> models;   // model path, bone
        std::vector<Area> areas;
        std::vector<Preset> presets;
    };
    RigDef g_def;

    void loadRigFile() noexcept
    {
        g_def = RigDef{};
        try {
            std::ifstream file("Data/F4SE/Plugins/RigVR/Rigs/RigBase.txt");
            std::string line;
            int lineNo = 0;
            while (file && std::getline(file, line)) {
                ++lineNo;
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                std::istringstream in(line);
                std::string word;
                in >> word;
                if (word == "bind" || word == "preset") {
                    std::string a, b;
                    in >> a;
                    if (word == "preset") in >> b;
                    float v[12]{};
                    for (auto& x : v) in >> x;
                    if (!in) {
                        note("Rig file line %d: not read", lineNo);
                        continue;
                    }
                    RE::NiTransform t;
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j)
                            t.rotate.entry[i][j] = v[i * 3 + j];
                    t.translate = { v[9], v[10], v[11] };
                    t.scale = 1.0f;
                    if (word == "bind") {
                        g_def.binds[a] = t;
                    } else {
                        Preset p;
                        p.kind = kindByName(a);
                        p.pose = t;
                        for (std::size_t i = 0; i < g_def.areas.size(); ++i) {
                            if (g_def.areas[i].name == b) p.area = static_cast<int>(i);
                        }
                        if (p.kind != kEmpty && p.area >= 0) {
                            g_def.presets.push_back(p);
                        } else {
                            note("Rig file line %d: preset %s on %s not understood", lineNo, a.c_str(), b.c_str());
                        }
                    }
                } else if (word == "model") {
                    std::string path, bone;
                    in >> path >> bone;
                    g_def.models.emplace_back(path, bone);
                } else if (word == "area") {
                    Area a;
                    std::string kinds;
                    in >> a.name >> a.bone >> kinds;
                    if (!kinds.empty() && kinds != "*") {
                        a.kinds = 0;
                        std::istringstream list(kinds);
                        std::string k;
                        while (std::getline(list, k, ',')) {
                            const int kind = kindByName(k);
                            if (kind) a.kinds |= 1u << kind;
                        }
                    }
                    g_def.areas.push_back(a);
                } else if (word == "tri" && !g_def.areas.empty()) {
                    std::array<RE::NiPoint3, 3> t;
                    for (auto& p : t) in >> p.x >> p.y >> p.z;
                    if (in) g_def.areas.back().tris.push_back(t);
                } else if (word == "highlight") {
                    std::string name, path;
                    in >> name >> path;
                    for (auto& a : g_def.areas) {
                        if (a.name == name) a.highlight = path;
                    }
                }
            }
        } catch (...) {
        }
        g_def.loaded = !g_def.models.empty() && !g_def.areas.empty();
        std::size_t tris = 0;
        for (const auto& a : g_def.areas) tris += a.tris.size();
        note("Rig file: %s; %zu models, %zu areas (%zu triangles), %zu binds, %zu preset pouches", g_def.loaded ? "loaded" : "MISSING or empty",
            g_def.models.size(), g_def.areas.size(), tris, g_def.binds.size(), g_def.presets.size());
    }

    // ------------------------------------------------------------ memory safety

    bool readable(const void* address, std::size_t size) noexcept
    {
        if (!address) {
            return false;
        }
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != 0x1000u) {
            return false;
        }
        constexpr std::uint32_t mask = 0x02u | 0x04u | 0x08u | 0x20u | 0x40u;
        if ((info.protect & mask) == 0 || (info.protect & 0x100u) != 0) {
            return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(address);
        const auto end = reinterpret_cast<std::uintptr_t>(info.baseAddress) + info.regionSize;
        return start + size <= end;
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


    // ------------------------------------------------------------ math (the game's row convention: rows are the local axes)

    float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float dot(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

    RE::NiPoint3 cross(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    float length(const RE::NiPoint3& a) noexcept { return std::sqrt(dot(a, a)); }

    RE::NiPoint3 normalized(const RE::NiPoint3& a) noexcept
    {
        const float l = length(a);
        return l > 1.0e-6f ? a * (1.0f / l) : RE::NiPoint3{};
    }

    RE::NiPoint3 row(const RE::NiMatrix3& m, int i) noexcept { return { m.entry[i][0], m.entry[i][1], m.entry[i][2] }; }

    void setRow(RE::NiMatrix3& m, int i, const RE::NiPoint3& v) noexcept
    {
        m.entry[i][0] = v.x;
        m.entry[i][1] = v.y;
        m.entry[i][2] = v.z;
    }

    RE::NiTransform identity() noexcept
    {
        RE::NiTransform t;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t.rotate.entry[i][j] = i == j ? 1.0f : 0.0f;
        t.translate = {};
        t.scale = 1.0f;
        return t;
    }

    RE::NiTransform compose(const RE::NiTransform& parent, const RE::NiTransform& local) noexcept
    {
        RE::NiTransform world{};
        world.rotate = local.rotate * parent.rotate;
        world.translate = parent.translate + parent.rotate.Transpose() * local.translate * parent.scale;
        world.scale = local.scale * parent.scale;
        return world;
    }

    RE::NiTransform relative(const RE::NiTransform& parent, const RE::NiTransform& world) noexcept
    {
        const float scale = parent.scale > 0.0f ? parent.scale : 1.0f;
        RE::NiTransform local{};
        local.rotate = world.rotate * parent.rotate.Transpose();
        local.translate = parent.rotate * (world.translate - parent.translate) * (1.0f / scale);
        local.scale = world.scale / scale;
        return local;
    }

    RE::NiPoint3 toLocal(const RE::NiTransform& parent, const RE::NiPoint3& world) noexcept
    {
        const float scale = parent.scale > 0.0f ? parent.scale : 1.0f;
        return parent.rotate * (world - parent.translate) * (1.0f / scale);
    }

    RE::NiPoint3 toWorld(const RE::NiTransform& parent, const RE::NiPoint3& local) noexcept
    {
        return parent.translate + parent.rotate.Transpose() * local * parent.scale;
    }

    constexpr float kDegrees = 3.14159265f / 180.0f;

    // The point of a triangle closest to p (Ericson, Real-Time Collision Detection 5.1.5).
    RE::NiPoint3 closestOnTriangle(const RE::NiPoint3& p, const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c) noexcept
    {
        const auto ab = b - a, ac = c - a, ap = p - a;
        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return a;
        const auto bp = p - b;
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) return b;
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
        const auto cp = p - c;
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) return c;
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        const float denom = 1.0f / (va + vb + vc);
        return a + ab * (vb * denom) + ac * (vc * denom);
    }


    // ------------------------------------------------------------ models attached to game nodes
    //
    // A model is loaded once per file by the game's model database, which keeps one node per file. A second copy of
    // the same pouch needs its own file: Meshes\RigVR\Inst\<name>_<n>.nif, copied from the model when first needed.

    struct Instance
    {
        std::string base;                  // the model it shows
        RE::NiPointer<RE::NiNode> node;
        bool used{ false };                // wanted this frame
    };
    std::vector<std::unique_ptr<Instance>> g_instances;
    std::map<std::string, bool> g_missingModels;

    RE::NiNode* loadModel(const std::string& path) noexcept
    {
        RE::BSModelDB::DBTraits::ArgsType args{};
        args.lodFadeMult = RE::ENUM_LOD_MULT::kObjects;
        args.loadLevel = 0;
        args.prepareAfterLoad = 1;
        args.useErrorMarker = 0;
        args.performProcess = 1;
        args.createFadeNode = 0;
        args.loadTextures = 1;
        for (const std::string& name : { path, std::string("Meshes\\") + path }) {
            RE::NiPointer<RE::NiNode> node;
            const auto error = RE::BSModelDB::Demand(name.c_str(), &node, args);
            if (node && looksLikeObject(node.get())) {
                detail("Model %s loaded (as %s)", path.c_str(), name.c_str());
                return node.get();
            }
            detail("Model %s: error %d", name.c_str(), static_cast<int>(error));
        }
        return nullptr;
    }

    std::string instancePath(const std::string& base, int n) noexcept
    {
        if (n == 0) {
            return base;
        }
        try {
            std::string stem = std::filesystem::path(base).stem().string();
            const std::string rel = "RigVR\\Inst\\" + stem + "_" + std::to_string(n) + ".nif";
            std::string from = "Data/Meshes/" + base;
            std::string to = "Data/Meshes/" + rel;
            std::replace(from.begin(), from.end(), '\\', '/');
            std::replace(to.begin(), to.end(), '\\', '/');
            std::filesystem::create_directories(std::filesystem::path(to).parent_path());
            if (!std::filesystem::exists(to) || std::filesystem::file_size(to) != std::filesystem::file_size(from) ||
                std::filesystem::last_write_time(to) < std::filesystem::last_write_time(from)) {
                std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing);
            }
            return rel;
        } catch (...) {
            note("Could not copy %s for a second instance", base.c_str());
            return {};
        }
    }

    // The game only finds loose files that were there when it started: the copies are made at plugin load.
    void prepareInstances() noexcept
    {
        int made = 0;
        for (int k = 1; k <= kKinds; ++k) {
            for (int n = 1; n <= 5; ++n) {
                made += instancePath(pouchModel(k), n).empty() ? 0 : 1;
            }
        }
        note("Pouch model copies ready: %d", made);
    }

    // A free instance of the model (made if needed). Null if the model cannot be loaded.
    Instance* acquire(const std::string& base) noexcept
    {
        if (base.empty() || g_missingModels.contains(base)) {
            return nullptr;
        }
        int count = 0;
        for (auto& inst : g_instances) {
            if (inst->base != base) continue;
            ++count;
            if (!inst->used) {
                inst->used = true;
                return inst.get();
            }
        }
        const auto path = instancePath(base, count);
        if (path.empty() || g_missingModels.contains(path)) {
            return nullptr;
        }
        auto* node = loadModel(path);
        if (!node) {
            g_missingModels[path] = true;
            if (count == 0) {
                g_missingModels[base] = true;
            }
            note("Model %s could not be loaded", path.c_str());
            return nullptr;
        }
        auto inst = std::make_unique<Instance>();
        inst->base = base;
        inst->node.reset(node);
        inst->used = true;
        g_instances.push_back(std::move(inst));
        note("Model %s instance %d ready", base.c_str(), count);
        return g_instances.back().get();
    }

    RE::NiBound recomputeBound(RE::NiAVObject* object, int depth = 0) noexcept
    {
        RE::NiBound out{};
        out.center = object->world.translate;
        out.fRadius = 0.0f;
        if (auto* geometry = object->IsGeometry()) {
            const auto& model = geometry->GetRuntimeData().modelBound;
            if (model.fRadius > 0.0f && model.fRadius < 5000.0f) {
                out.center = toWorld(object->world, model.center);
                out.fRadius = model.fRadius * object->world.scale;
            } else {
                out.fRadius = 50.0f;
            }
        } else if (auto* node = object->IsNode(); node && depth < 40) {
            auto& children = node->GetRuntimeData().children;
            const auto count = children.size();
            std::vector<RE::NiBound> parts;
            for (std::uint16_t i = 0; i < count && i < 256; ++i) {
                auto* child = children[i].get();
                if (!looksLikeObject(child)) continue;
                const auto b = recomputeBound(child, depth + 1);
                if (b.fRadius > 0.0f) parts.push_back(b);
            }
            if (!parts.empty()) {
                RE::NiPoint3 c{};
                for (const auto& b : parts) c += b.center;
                c = c * (1.0f / static_cast<float>(parts.size()));
                float r = 0.0f;
                for (const auto& b : parts) r = std::max(r, distance(c, b.center) + b.fRadius);
                out.center = c;
                out.fRadius = r;
            }
        }
        object->worldBound = out;
        return out;
    }

    // Hangs the instance under `parent` at `local` (moved from wherever it was) and brings its world data up to date
    // for this frame; from then on the engine carries it along with the parent.
    void show(Instance* inst, RE::NiNode* parent, const RE::NiTransform& local) noexcept
    {
        if (!inst || !looksLikeObject(parent)) {
            return;
        }
        auto* node = inst->node.get();
        if (!looksLikeObject(node)) {
            return;
        }
        bool fresh = false;
        if (node->parent != parent) {
            if (auto* old = node->parent; looksLikeObject(old)) {
                old->DetachChild(node);
            }
            parent->AttachChild(node, true);
            fresh = true;
            detail("Model %s attached under %s", inst->base.c_str(), parent->name.c_str());
        }
        setCulled(node, false);
        node->local = local;
        node->world = compose(parent->world, local);
        forEachNode(node, [](RE::NiAVObject* object, int depth) {
            if (depth > 0) {
                if (auto* up = object->parent; looksLikeObject(up)) {
                    object->world = compose(up->world, object->local);
                }
            }
            return true;
        });
        (void)fresh;
        const auto bound = recomputeBound(node);
        // the parent's bound must reach the model, or the renderer culls it with the parent
        RE::NiAVObject* up = parent;
        for (int i = 0; i < 6 && looksLikeObject(up); ++i) {
            auto& b = up->worldBound;
            const float needed = distance(b.center, bound.center) + bound.fRadius;
            if (b.fRadius > 0.0f && b.fRadius < needed && needed < 20000.0f) {
                b.fRadius = needed;
            }
            up = up->parent;
        }
    }

    // A gun put away is seen as a copy of the gun's own node as it was in the hand (with all its mods): one copy per
    // holster / carbine holder, kept while the gun is there.
    struct GunVisual
    {
        Instance* inst{ nullptr };
        std::uint32_t form{ 0 };
        RE::NiTransform inPouch{};   // the gun in the pouch's own space
    };
    std::array<GunVisual, 16> g_gunVisuals{};

    // the gun in the hand now (or last seen there): a copy of its node, its place in the world and the middle of it
    struct GunSnapshot
    {
        RE::NiPointer<RE::NiNode> copy;
        std::uint32_t form{ 0 };
        RE::NiAVObject* source{ nullptr };   // the model under the Weapon node the copy was made from
        RE::NiTransform world{};
        RE::NiPoint3 middle{};
        bool seen{ false };                  // in the hand this frame
    } g_gunSnapshot;

    RE::NiNode* cloneNode(RE::NiNode* source) noexcept
    {
        if (!looksLikeObject(source)) {
            return nullptr;
        }
        RE::NiCloningProcess process{};
        process.copyType = RE::NiCloningProcess::CopyType::kCopyExact;
        process.appendChar = '$';
        process.scale = { 1.0f, 1.0f, 1.0f };
        auto* object = source->CreateClone(process);
        if (!object) {
            return nullptr;
        }
        source->ProcessClone(process);
        auto* av = static_cast<RE::NiAVObject*>(object);
        return looksLikeObject(av) ? av->IsNode() : nullptr;
    }

    void beginFrameModels() noexcept
    {
        for (auto& inst : g_instances) inst->used = false;
    }

    // Models not shown this frame are taken off their parent.
    void endFrameModels() noexcept
    {
        for (auto& inst : g_instances) {
            auto* node = inst->node.get();
            if (!inst->used && looksLikeObject(node) && node->parent) {
                auto* parent = node->parent;
                if (looksLikeObject(parent)) {
                    parent->DetachChild(node);
                }
                node->parent = nullptr;
            }
        }
    }

    void detachAllModels() noexcept
    {
        beginFrameModels();
        endFrameModels();
    }

    // ------------------------------------------------------------ game helpers

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept
    {
        if (!id) {
            return nullptr;
        }
        auto* form = RE::TESForm::GetFormByID(id);
        if (!form || !readable(form, sizeof(RE::TESObjectREFR))) {
            return nullptr;
        }
        return form->As<RE::TESObjectREFR>();
    }

    RE::TESForm* lookupForm(std::uint32_t id) noexcept
    {
        return id ? RE::TESForm::GetFormByID(id) : nullptr;
    }

    std::uint32_t inventoryCount(RE::PlayerCharacter* player, std::uint32_t formID) noexcept
    {
        auto* object = lookupForm(formID);
        auto* list = player ? player->inventoryList : nullptr;
        if (!object || !list || !readable(list, sizeof(RE::BGSInventoryList))) {
            return 0;
        }
        std::uint32_t total = 0;
        for (auto& item : list->data) {
            if (item.object != object) continue;
            for (auto* stack = item.stackData.get(); stack; stack = stack->nextStack.get()) {
                total += stack->count;
            }
        }
        return total;
    }

    // The gun the player holds (not fists, grenades or mines): its base form.
    RE::TESObjectWEAP* equippedGun(RE::PlayerCharacter* player) noexcept
    {
        auto* process = player ? player->currentProcess : nullptr;
        if (!process || !readable(process, sizeof(RE::AIProcess))) {
            return nullptr;
        }
        auto* middle = process->middleHigh;
        if (!middle || !readable(middle, sizeof(RE::MiddleHighProcessData))) {
            return nullptr;
        }
        auto& items = middle->equippedItems;
        if (items.size() > 16) {
            return nullptr;
        }
        for (auto& item : items) {
            auto* form = item.item.object;
            if (!form || !readable(form, sizeof(RE::TESForm)) || form->GetFormType() != RE::ENUM_FORM_ID::kWEAP) continue;
            auto* weapon = static_cast<RE::TESObjectWEAP*>(form);
            if (weapon->weaponData.type.get() == RE::WEAPON_TYPE::kGun) {
                return weapon;
            }
        }
        return nullptr;
    }

    // Can this item go into the bag?
    bool bagAccepts(RE::TESForm* form) noexcept
    {
        if (!form) {
            return false;
        }
        const auto kind = form->GetFormType();
        return kind == RE::ENUM_FORM_ID::kMISC || kind == RE::ENUM_FORM_ID::kALCH || kind == RE::ENUM_FORM_ID::kAMMO ||
               kind == RE::ENUM_FORM_ID::kBOOK || kind == RE::ENUM_FORM_ID::kNOTE || kind == RE::ENUM_FORM_ID::kKEYM ||
               kind == RE::ENUM_FORM_ID::kINGR || kind == RE::ENUM_FORM_ID::kARMO ||
               (kind == RE::ENUM_FORM_ID::kWEAP && static_cast<RE::TESObjectWEAP*>(form)->weaponData.type.get() != RE::WEAPON_TYPE::kGun);
    }

    // ------------------------------------------------------------ controllers and hands

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

    struct Hand
    {
        bool         valid{ false };
        RE::NiPoint3 palm{};
        bool         grip{ false };
        bool         previousGrip{ false };
        bool pressed() const noexcept { return grip && !previousGrip; }
        bool released() const noexcept { return !grip && previousGrip; }
    };
    std::array<Hand, 2> g_hands;   // 0 right, 1 left

    FRIKApiV2::Hand frikHand(int index) noexcept { return index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left; }

    void readHand(int index) noexcept
    {
        auto& hand = g_hands[index];
        hand.previousGrip = hand.grip;
        hand.valid = false;
        hand.grip = false;
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid && g_vr) {
            vr::VRControllerState_t state{};
            if (g_vr->GetControllerState(device, &state, sizeof(state))) {
                hand.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
            }
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
        hand.palm = wand.translate;
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            const float length = distance(tip, wrist);
            if (length < 40.0f && length > 0.1f) {
                hand.palm = (tip + wrist) * 0.5f;
            }
        }
        hand.valid = true;
    }

    void hud(const char* text) noexcept
    {
        if (g_settings.messages && text) {
            RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false, false);
        }
    }

    bool menuOpen() noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (frik && frik->isWristPipboyOpen && frik->isWristPipboyOpen()) {
            return true;
        }
        if (frik && frik->isConfigOpen && frik->isConfigOpen()) {
            return true;
        }
        const auto* ui = RE::UI::GetSingleton();
        return ui && ui->menuMode != 0;
    }

    bool bone(const char* name, RE::NiPoint3& out) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->getBoneWorldTransform) {
            return false;
        }
        RE::NiTransform t;
        if (!frik->getBoneWorldTransform(name, &t)) {
            return false;
        }
        out = t.translate;
        return true;
    }

    // ------------------------------------------------------------ talking to the script

    enum Command : std::int32_t
    {
        kNone = 0,
        kDelete = 2,        // ref: a pouch item hung on the rig is gone from the world
        kTakeItem = 5,      // form: one less in the inventory
        kGiveItem = 6,      // form: one more in the inventory
        kStoreRef = 7,      // ref: picked up into the inventory
        kDropToHand = 8,    // form, pose, token: out of the inventory into the hand
        kConsume = 9,       // ref: used (a chem is injected, food eaten)
        kEquip = 10,        // form
        kUnequip = 11,      // form
        kSound = 12,        // kind: 0 snap, 1 pick up, 2 focus, 3 put in
        kSave = 13,
        kPack = 14,         // ref: the mannequin is folded back into the kit
    };

    struct PendingCommand
    {
        std::int32_t type{ kNone };
        std::int32_t kind{ 0 };
        std::uint32_t ref{ 0 };
        std::uint32_t form{ 0 };
        std::int32_t token{ 0 };
        std::array<float, 6> pose{};
    };

    struct NewRef
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
    };

    struct Shared
    {
        std::mutex lock;
        std::deque<PendingCommand> commands;
        PendingCommand current;
        std::vector<NewRef> newRefs;
        std::vector<std::int32_t> incoming;   // the saved rig, pushed by the script after a load
        bool stateArrived{ false };
        std::uint32_t mannequinArrived{ 0 };
        bool mannequinChanged{ false };
        std::array<std::uint32_t, 1 + kRigItem> forms{};   // 0 kit, 1..6 pouch items, 7 the rig item
        std::string hudText;
    } g_shared;

    void queueHud(const char* text) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.hudText = text ? text : "";
    }

    void push(const PendingCommand& command) noexcept
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.commands.push_back(command);
    }

    void pushSimple(std::int32_t type, std::uint32_t ref = 0, std::uint32_t form = 0, std::int32_t kind = 0) noexcept
    {
        PendingCommand c;
        c.type = type;
        c.ref = ref;
        c.form = form;
        c.kind = kind;
        push(c);
    }

    void sound(int index) noexcept { pushSimple(kSound, 0, 0, index); }

    std::int32_t PollCommand(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        if (g_shared.commands.empty()) {
            g_shared.current = {};
            return 0;
        }
        g_shared.current = g_shared.commands.front();
        g_shared.commands.pop_front();
        return g_shared.current.type;
    }

    std::int32_t CommandKind(std::monostate) { std::scoped_lock guard(g_shared.lock); return g_shared.current.kind; }
    std::int32_t CommandRefID(std::monostate) { std::scoped_lock guard(g_shared.lock); return static_cast<std::int32_t>(g_shared.current.ref); }
    std::int32_t CommandFormID(std::monostate) { std::scoped_lock guard(g_shared.lock); return static_cast<std::int32_t>(g_shared.current.form); }
    std::int32_t CommandToken(std::monostate) { std::scoped_lock guard(g_shared.lock); return g_shared.current.token; }

    std::vector<float> CommandPose(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        return { g_shared.current.pose.begin(), g_shared.current.pose.end() };
    }

    void AddRef(std::monostate, RE::TESObjectREFR* ref, std::int32_t token)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.newRefs.push_back(NewRef{ ref ? ref->GetFormID() : 0u, token });
    }

    void SetForm(std::monostate, std::int32_t index, std::int32_t formID)
    {
        if (index >= 0 && index < static_cast<std::int32_t>(g_shared.forms.size())) {
            std::scoped_lock guard(g_shared.lock);
            g_shared.forms[index] = static_cast<std::uint32_t>(formID);
        }
    }

    void SetMannequin(std::monostate, RE::TESObjectREFR* ref)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.mannequinArrived = ref ? ref->GetFormID() : 0u;
        g_shared.mannequinChanged = true;
    }

    void ClearState(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.incoming.clear();
        g_shared.stateArrived = false;
    }

    void PushState(std::monostate, std::int32_t value)
    {
        std::scoped_lock guard(g_shared.lock);
        if (g_shared.incoming.size() < 1024) {
            g_shared.incoming.push_back(value);
        }
    }

    void StateDone(std::monostate)
    {
        std::scoped_lock guard(g_shared.lock);
        g_shared.stateArrived = true;
    }

    std::vector<std::int32_t> GetState(std::monostate);

    void Notify(std::monostate, std::int32_t message)
    {
        note("Script: %d", message);
        switch (message) {
        case 2: queueHud("Не получилось достать предмет"); break;
        case 3: queueHud("Не получилось поставить манекен"); break;
        case 20: queueHud("Разгрузка: в инвентаре (Разное) манекен. Выбрось его из Пип-Боя, чтобы поставить; разгрузка с подсумками уже на нём"); break;
        default: break;
        }
    }

    bool registerNatives(RE::BSScript::IVirtualMachine* vm)
    {
        if (!vm) {
            return false;
        }
        constexpr auto kScript = "RigVR";
        vm->BindNativeMethod(kScript, "PollCommand", PollCommand, true);
        vm->BindNativeMethod(kScript, "CommandKind", CommandKind, true);
        vm->BindNativeMethod(kScript, "CommandRefID", CommandRefID, true);
        vm->BindNativeMethod(kScript, "CommandFormID", CommandFormID, true);
        vm->BindNativeMethod(kScript, "CommandToken", CommandToken, true);
        vm->BindNativeMethod(kScript, "CommandPose", CommandPose, true);
        vm->BindNativeMethod(kScript, "AddRef", AddRef, true);
        vm->BindNativeMethod(kScript, "SetForm", SetForm, true);
        vm->BindNativeMethod(kScript, "SetMannequin", SetMannequin, true);
        vm->BindNativeMethod(kScript, "ClearState", ClearState, true);
        vm->BindNativeMethod(kScript, "PushState", PushState, true);
        vm->BindNativeMethod(kScript, "StateDone", StateDone, true);
        vm->BindNativeMethod(kScript, "GetState", GetState, true);
        vm->BindNativeMethod(kScript, "Notify", Notify, true);
        logNote("Papyrus functions of script RigVR registered");
        return true;
    }

    // ------------------------------------------------------------ the rig (kept in the save)

    struct Stack
    {
        std::uint32_t form{ 0 };
        std::int32_t  count{ 0 };
    };

    struct Pouch
    {
        int kind{ kEmpty };
        int area{ -1 };
        RE::NiTransform pose{};   // in body space: where its back hangs, rows = its axes (+Y out of the rig, +Z up)
        std::array<Stack, kItemsPerPouch> items{};

        int total() const noexcept
        {
            int n = 0;
            for (const auto& s : items) n += s.form ? s.count : 0;
            return n;
        }
    };

    struct Rig
    {
        bool worn{ false };
        bool rigOn{ true };            // the rig is on the mannequin / on you (false: it is an item somewhere)
        std::uint32_t mannequin{ 0 };
        std::vector<Pouch> pouches;
    };

    std::mutex g_rigLock;   // GetState runs on a script thread
    Rig g_rig;
    bool g_rigLoaded{ false };
    bool g_dirty{ false };
    Clock::time_point g_lastSave{};

    constexpr std::int32_t kStateVersion = 2;

    std::int32_t bits(float f) noexcept { return std::bit_cast<std::int32_t>(f); }
    float unbits(std::int32_t i) noexcept
    {
        const float f = std::bit_cast<float>(i);
        return std::isfinite(f) ? f : 0.0f;
    }

    std::vector<std::int32_t> GetState(std::monostate)
    {
        std::scoped_lock guard(g_rigLock);
        std::vector<std::int32_t> out;
        out.push_back(kStateVersion);
        out.push_back(g_rig.worn ? 1 : 0);
        out.push_back(static_cast<std::int32_t>(g_rig.mannequin));
        out.push_back(g_rig.rigOn ? 1 : 0);
        out.push_back(static_cast<std::int32_t>(g_rig.pouches.size()));
        for (const auto& p : g_rig.pouches) {
            out.push_back(p.kind);
            out.push_back(p.area);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    out.push_back(bits(p.pose.rotate.entry[i][j]));
            out.push_back(bits(p.pose.translate.x));
            out.push_back(bits(p.pose.translate.y));
            out.push_back(bits(p.pose.translate.z));
            for (const auto& s : p.items) {
                out.push_back(static_cast<std::int32_t>(s.form));
                out.push_back(s.count);
            }
        }
        return out;
    }

    // The layout a new game (or a game from before 0.4) starts with: the rig on the mannequin with every pouch where it
    // hangs in the Blender scene.
    void applyPreset() noexcept
    {
        g_rig.rigOn = true;
        g_rig.worn = false;
        g_rig.pouches.clear();
        for (const auto& p : g_def.presets) {
            Pouch pouch;
            pouch.kind = p.kind;
            pouch.area = p.area;
            pouch.pose = p.pose;
            g_rig.pouches.push_back(pouch);
        }
        note("Starting layout: %zu pouches on the rig", g_rig.pouches.size());
    }

    void markDirty() noexcept { g_dirty = true; }

    void loadState(const std::vector<std::int32_t>& in) noexcept
    {
        std::scoped_lock guard(g_rigLock);
        g_rig = Rig{};
        constexpr std::size_t kPer = 2 + 12 + 2 * kItemsPerPouch;
        if (in.size() >= 5 && in[0] == kStateVersion) {
            g_rig.worn = in[1] != 0;
            g_rig.mannequin = static_cast<std::uint32_t>(in[2]);
            g_rig.rigOn = in[3] != 0;
            const int count = std::clamp(in[4], 0, kMaxPouches);
            std::size_t at = 5;
            for (int n = 0; n < count && at + kPer <= in.size(); ++n) {
                Pouch p;
                p.kind = in[at++];
                p.area = in[at++];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        p.pose.rotate.entry[i][j] = unbits(in[at++]);
                p.pose.translate.x = unbits(in[at++]);
                p.pose.translate.y = unbits(in[at++]);
                p.pose.translate.z = unbits(in[at++]);
                p.pose.scale = 1.0f;
                for (auto& s : p.items) {
                    s.form = static_cast<std::uint32_t>(in[at++]);
                    s.count = std::max(0, in[at++]);
                    if (s.count == 0) s.form = 0;
                }
                if (p.kind >= 1 && p.kind <= kKinds && p.area >= 0 && p.area < static_cast<int>(g_def.areas.size())) {
                    g_rig.pouches.push_back(p);
                }
            }
            if (!g_rig.rigOn) {
                g_rig.worn = false;
            }
        } else {
            // nothing saved yet, or the layout of 0.1-0.3 (fixed places): start with the scene's layout, keep the mannequin
            if (in.size() >= 3 && in[0] == 1) {
                g_rig.mannequin = static_cast<std::uint32_t>(in[2]);
            }
            applyPreset();
            g_dirty = true;
        }
        g_rigLoaded = true;
        note("Rig loaded: %s, mannequin %08X, %zu pouches", !g_rig.rigOn ? "not hung" : g_rig.worn ? "worn" : "on the mannequin",
            g_rig.mannequin, g_rig.pouches.size());
    }

    bool addToPouch(Pouch& pouch, std::uint32_t form) noexcept
    {
        if (pouch.total() >= capacityOf(pouch.kind)) {
            return false;
        }
        for (auto& s : pouch.items) {
            if (s.form == form) {
                ++s.count;
                return true;
            }
        }
        for (auto& s : pouch.items) {
            if (!s.form) {
                s.form = form;
                s.count = 1;
                return true;
            }
        }
        return false;
    }

    // The last kind put in that is still in the inventory; its count goes down by one.
    std::uint32_t takeFromPouch(Pouch& pouch, RE::PlayerCharacter* player) noexcept
    {
        for (int i = kItemsPerPouch - 1; i >= 0; --i) {
            auto& s = pouch.items[i];
            if (!s.form || s.count <= 0) continue;
            const auto have = static_cast<std::int32_t>(inventoryCount(player, s.form));
            if (have <= 0) {
                s = Stack{};
                continue;
            }
            s.count = std::min(s.count, have) - 1;
            const auto form = s.form;
            if (s.count <= 0) s = Stack{};
            return form;
        }
        return 0;
    }

    // ------------------------------------------------------------ where the rig is: the mannequin or your body

    // A node by name under `root` (searched by hand: GetObjectByName sits elsewhere in the VR vtable).
    RE::NiNode* findNode(RE::NiAVObject* root, const std::string& name) noexcept
    {
        static std::map<std::pair<RE::NiAVObject*, std::string>, RE::NiNode*> cache;
        const auto key = std::make_pair(root, name);
        if (auto it = cache.find(key); it != cache.end() && looksLikeObject(it->second) && it->second->name.c_str() &&
                                         _stricmp(it->second->name.c_str(), name.c_str()) == 0) {
            return it->second;
        }
        RE::NiNode* found = nullptr;
        forEachNode(root, [&](RE::NiAVObject* object, int) {
            if (found) return false;
            const char* n = object->name.c_str();
            if (n && _stricmp(n, name.c_str()) == 0) {
                found = object->IsNode();
                return false;
            }
            return true;
        });
        if (found) cache[key] = found;
        return found;
    }

    // A place to hang models: on the mannequin everything goes under its root (its space is the body space); on the
    // body each model goes under the bone it follows.
    struct Holder
    {
        bool valid{ false };
        bool body{ false };
        RE::NiAVObject* skeleton{ nullptr };   // your body's 3D (the one with the bones FRIK moves)
        RE::NiNode* anchor{ nullptr };         // on the mannequin: models hang under your skeleton's root, placed at the mannequin
        RE::NiTransform manWorld{};            // the mannequin in the world (body space of the mannequin)

        RE::NiNode* parentFor(const std::string& bone) const noexcept
        {
            return body ? findNode(skeleton, bone) : anchor;
        }

        // on you the rig sits this much further out than on the mannequin (over armour)
        RE::NiTransform inflate(const RE::NiTransform& pose) const noexcept
        {
            if (!body) {
                return pose;
            }
            const float k = g_settings.bodyScale;
            const RE::NiPoint3 c{ 0.0f, 0.0f, g_settings.bodyScaleZ };
            RE::NiTransform t = pose;
            t.translate = c + (pose.translate - c) * k;
            t.scale = pose.scale * k;
            return t;
        }

        // local transform under parentFor(bone) for a pose in body space
        RE::NiTransform localFor(const std::string& bone, const RE::NiTransform& bodyPose) const noexcept
        {
            if (!body) {
                return looksLikeObject(anchor) ? relative(anchor->world, compose(manWorld, bodyPose)) : bodyPose;
            }
            const auto it = g_def.binds.find(bone);
            return it == g_def.binds.end() ? bodyPose : relative(it->second, inflate(bodyPose));
        }

        // body space in the world now (without the inflation)
        bool space(RE::NiTransform& out) const noexcept
        {
            if (!body) {
                out = manWorld;
                return true;
            }
            auto* pelvis = parentFor("Pelvis");
            const auto it = g_def.binds.find("Pelvis");
            if (!pelvis || it == g_def.binds.end()) {
                return false;
            }
            out = compose(pelvis->world, relative(it->second, identity()));
            return true;
        }
    };

    // the root of your body's 3D that holds the bones (null if none does)
    RE::NiAVObject* findSkeleton(RE::PlayerCharacter* player) noexcept
    {
        RE::NiAVObject* roots[3]{ player->Get3D(false), player->Get3D(true), player->Get3D() };
        for (auto* root : roots) {
            if (looksLikeObject(root) && findNode(root, "Pelvis")) {
                return root;
            }
        }
        static bool logged = false;
        if (!logged) {
            logged = true;
            for (int i = 0; i < 3; ++i) {
                note("Body root %d: %s, Pelvis not found under it", i, looksLikeObject(roots[i]) ? roots[i]->name.c_str() : "none");
            }
        }
        return nullptr;
    }

    // On the mannequin the models hang under your skeleton's root (a static object's own node is not drawn again when
    // children are added to it), placed every frame where the mannequin stands.
    Holder mannequinHolder(RE::PlayerCharacter* player, RE::TESObjectREFR* mannequin) noexcept
    {
        Holder h;
        auto* root = mannequin ? mannequin->Get3D() : nullptr;
        auto* skeleton = findSkeleton(player);
        if (!looksLikeObject(root) || !skeleton) {
            return h;
        }
        h.manWorld = root->world;
        h.manWorld.scale = 1.0f;
        h.anchor = skeleton->IsNode();
        h.valid = h.anchor != nullptr;
        return h;
    }

    Holder bodyHolder(RE::PlayerCharacter* player) noexcept
    {
        Holder h;
        h.body = true;
        h.skeleton = findSkeleton(player);
        h.valid = h.skeleton != nullptr;
        return h;
    }

    const std::string& boneOf(const Pouch& p) noexcept
    {
        static const std::string pelvis = "Pelvis";
        return p.area >= 0 && p.area < static_cast<int>(g_def.areas.size()) ? g_def.areas[p.area].bone : pelvis;
    }

    // the middle of a pouch in body space
    RE::NiPoint3 pouchMiddle(const Pouch& p) noexcept
    {
        return p.pose.translate + row(p.pose.rotate, 1) * depthOf(p.kind);
    }

    void drawRig(const Holder& h) noexcept
    {
        for (const auto& [path, bone] : g_def.models) {
            auto* parent = h.parentFor(bone);
            if (!parent) continue;
            show(acquire(path), parent, h.localFor(bone, identity()));
        }
    }

    // where a gun hangs in a holster (muzzle down, its right side out from the rig, its top towards the holster's -X)
    // or in a carbine holder when it was not hung there by hand
    RE::NiTransform fixedGunPose(int kind) noexcept
    {
        RE::NiTransform gun = identity();
        setRow(gun.rotate, 0, { 0.0f, 1.0f, 0.0f });
        setRow(gun.rotate, 1, { 0.0f, 0.0f, -1.0f });
        setRow(gun.rotate, 2, { -1.0f, 0.0f, 0.0f });
        const auto& o = kind == kHolster ? g_settings.holsterGun : g_settings.slingGun;
        gun.translate = { o[0], o[1], o[2] };
        return gun;
    }

    void drawPouch(const Holder& h, const Pouch& p) noexcept
    {
        const auto& bone = boneOf(p);
        auto* parent = h.parentFor(bone);
        if (!parent) return;
        show(acquire(pouchModel(p.kind)), parent, h.localFor(bone, p.pose));
        const auto index = static_cast<std::size_t>(&p - g_rig.pouches.data());
        if (index < g_rig.pouches.size() && index < g_gunVisuals.size() && p.items[0].form) {
            auto& v = g_gunVisuals[index];
            if (v.inst && v.form == p.items[0].form && looksLikeObject(v.inst->node.get())) {
                v.inst->used = true;
                show(v.inst, parent, compose(h.localFor(bone, p.pose), v.inPouch));
                return;
            }
        }
        // a gun put away before this game was started: its plain model (modular guns have none to show)
        if ((p.kind == kHolster || p.kind == kSling) && p.items[0].form) {
            auto* form = lookupForm(p.items[0].form);
            if (form && form->GetFormType() == RE::ENUM_FORM_ID::kWEAP) {
                auto* weapon = static_cast<RE::TESObjectWEAP*>(form);
                const char* path = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*weapon)).GetModel();
                if (path && *path && !std::strstr(path, "Dummy")) {
                    show(acquire(path), parent, h.localFor(bone, compose(p.pose, fixedGunPose(p.kind))));
                }
            }
        }
    }

    // ------------------------------------------------------------ pouch items in the hands

    // A pouch item (or the rig item) a gripping hand holds: the nearest one to the palm.
    struct Held
    {
        std::uint32_t ref{ 0 };
        int kind{ kEmpty };
        RE::NiTransform world{};
    };

    int kindOfItem(RE::TESForm* base) noexcept
    {
        if (!base) return kEmpty;
        const auto id = base->GetFormID();
        for (int k = 1; k <= kRigItem; ++k) {
            if (g_shared.forms[k] && g_shared.forms[k] == id) return k;
        }
        return kEmpty;
    }

    Held heldBy(RE::PlayerCharacter* player, const RE::NiPoint3& palm) noexcept
    {
        Held best;
        auto* cell = player->GetParentCell();
        if (!cell || !readable(cell, sizeof(RE::TESObjectCELL))) {
            return best;
        }
        float bestDistance = g_settings.holdRadius;
        const auto count = cell->references.size();
        if (count > 100000) {
            return best;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ref = cell->references[i].get();
            if (!ref || ref == player || !readable(ref, sizeof(RE::TESObjectREFR)) || ref->IsDisabled() || ref->IsDeleted()) continue;
            auto* base = ref->GetObjectReference();
            if (!base || !readable(base, sizeof(RE::TESForm))) continue;
            const int kind = kindOfItem(base);
            if (kind == kEmpty) continue;
            auto* root = ref->Get3D();
            if (!looksLikeObject(root)) continue;
            const float d = distance(root->world.translate, palm);
            if (d < bestDistance) {
                bestDistance = d;
                best.ref = ref->GetFormID();
                best.kind = kind;
                best.world = root->world;
            }
        }
        return best;
    }

    // A loose item near the palm (the one the hand just let go of) that the bag takes.
    RE::TESObjectREFR* looseItemNear(RE::PlayerCharacter* player, const RE::NiPoint3& palm) noexcept
    {
        auto* cell = player->GetParentCell();
        if (!cell || !readable(cell, sizeof(RE::TESObjectCELL))) {
            return nullptr;
        }
        RE::TESObjectREFR* best = nullptr;
        float bestDistance = g_settings.storeRadius;
        const auto count = cell->references.size();
        if (count > 100000) {
            return nullptr;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ref = cell->references[i].get();
            if (!ref || ref == player || !readable(ref, sizeof(RE::TESObjectREFR)) || ref->IsDisabled() || ref->IsDeleted()) continue;
            auto* base = ref->GetObjectReference();
            if (!base || !readable(base, sizeof(RE::TESForm)) || !bagAccepts(base) || kindOfItem(base) != kEmpty) continue;
            auto* root = ref->Get3D();
            if (!looksLikeObject(root)) continue;
            const float d = distance(root->world.translate, palm);   // a held item's own position lags behind its 3D
            if (d < bestDistance) {
                bestDistance = d;
                best = ref;
            }
        }
        return best;
    }

    // Where a pouch held at `item` (mannequin space) would hang: the nearest point of an area it may hang on, turned
    // with its back to the rig and its top up (or as the hand tilts it).
    struct Snap
    {
        bool ok{ false };
        int area{ -1 };
        float gap{ 1.0e9f };
        RE::NiTransform pose{};
    };

    Snap snapPouch(int kind, const RE::NiTransform& item) noexcept
    {
        Snap s;
        RE::NiPoint3 bestPoint{}, bestNormal{};
        const auto at = item.translate;
        for (std::size_t a = 0; a < g_def.areas.size(); ++a) {
            const auto& area = g_def.areas[a];
            if (!(area.kinds & (1u << kind))) continue;
            for (const auto& t : area.tris) {
                const auto q = closestOnTriangle(at, t[0], t[1], t[2]);
                const float d = distance(q, at);
                if (d < s.gap) {
                    s.gap = d;
                    s.area = static_cast<int>(a);
                    bestPoint = q;
                    bestNormal = normalized(cross(t[1] - t[0], t[2] - t[0]));
                }
            }
        }
        if (s.area < 0 || length(bestNormal) < 0.5f) {
            return s;
        }
        // out of the rig: away from the body's middle line; the area's faces around the point are averaged, so the pouch
        // is not turned by one small face
        const RE::NiPoint3 radial{ bestPoint.x, bestPoint.y, 0.0f };
        RE::NiPoint3 sum{};
        for (const auto& t : g_def.areas[s.area].tris) {
            const auto c = (t[0] + t[1] + t[2]) * (1.0f / 3.0f);
            const float d = distance(c, bestPoint);
            if (d > g_settings.areaSmooth) continue;
            auto n = cross(t[1] - t[0], t[2] - t[0]);   // its length weighs it by its size
            if (dot(n, radial) < 0.0f) n = n * -1.0f;
            sum += n * (1.0f - d / (g_settings.areaSmooth * 1.5f));
        }
        RE::NiPoint3 out = length(sum) > 1.0e-4f ? normalized(sum) : bestNormal;
        if (dot(out, radial) < 0.0f) {
            out = out * -1.0f;
        }
        const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
        auto upright = normalized(worldUp - out * dot(worldUp, out));
        auto tilted = row(item.rotate, 2);
        tilted = normalized(tilted - out * dot(tilted, out));
        RE::NiPoint3 up = upright;
        if (length(tilted) > 0.5f && length(upright) > 0.5f && dot(tilted, upright) < std::cos(g_settings.upSnapDegrees * kDegrees)) {
            up = tilted;
        } else if (length(upright) < 0.5f) {
            up = length(tilted) > 0.5f ? tilted : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
        }
        const auto right = normalized(cross(out, up));
        up = cross(right, out);
        s.pose = identity();
        setRow(s.pose.rotate, 0, right);
        setRow(s.pose.rotate, 1, out);
        setRow(s.pose.rotate, 2, up);
        s.pose.translate = bestPoint;
        s.ok = s.gap < g_settings.snapRadius;
        return s;
    }

    // ------------------------------------------------------------ state of the frame

    struct State
    {
        std::array<Held, 2> held{};          // the pouch each hand holds (while gripping)
        std::array<Clock::time_point, 2> gripAt{};
        Clock::time_point packStart{};
        int packHand{ -1 };
        Clock::time_point holsterAt{};
        RE::TESObjectWEAP* lastGun{ nullptr };
        RE::TESObjectWEAP* countedGun{ nullptr };
        std::uint32_t gunCount{ 0 };          // how many of the gun in the hand you had while holding it
        int bagPouch{ -1 };                   // an item let go in the bag that was not found at once is looked for a moment
        int bagHand{ 0 };
        Clock::time_point bagAt{};
        bool weaponLogged{ false };
        bool wearArmed{ true };
        bool bodyLogged{ false };
    } g_state;

    // Items dropped from a pouch into a hand: a chem / food among them is used at the mouth.
    struct InHand
    {
        std::uint32_t ref{ 0 };
        std::int32_t  token{ 0 };
        Clock::time_point at{};
    };
    std::vector<InHand> g_inHand;
    std::int32_t g_nextToken{ 1000 };

    void takeNewRefs() noexcept
    {
        std::vector<NewRef> refs;
        {
            std::scoped_lock guard(g_shared.lock);
            refs.swap(g_shared.newRefs);
        }
        for (const auto& r : refs) {
            for (auto& h : g_inHand) {
                if (h.token == r.token) h.ref = r.ref;
            }
        }
    }

    void dropToHand(std::uint32_t form, const RE::NiPoint3& palm, Clock::time_point now) noexcept
    {
        PendingCommand c;
        c.type = kDropToHand;
        c.form = form;
        c.pose = { palm.x, palm.y, palm.z, 0.0f, 0.0f, 0.0f };
        c.token = g_nextToken++;
        push(c);
        g_inHand.push_back(InHand{ 0, c.token, now });
    }

    // the pouch whose middle is nearest the palm (palm in body space), within its reach
    int nearestPouch(const RE::NiPoint3& palmBody, float scale = 1.0f) noexcept
    {
        int best = -1;
        float bestRatio = 1.0f;
        for (std::size_t i = 0; i < g_rig.pouches.size(); ++i) {
            const auto& p = g_rig.pouches[i];
            const float ratio = distance(pouchMiddle(p), palmBody) / (reachOf(p.kind) * scale);
            if (ratio < bestRatio) {
                bestRatio = ratio;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    // the palm in body space for a holder (mannequin root, or the body through the pelvis bone)
    bool palmInBody(const Holder& h, const RE::NiPoint3& palm, RE::NiPoint3& out) noexcept
    {
        RE::NiTransform space;
        if (!h.space(space)) {
            return false;
        }
        out = toLocal(space, palm);
        if (h.body) {
            // undo the inflation, so pouches are found where they are drawn
            const RE::NiPoint3 c{ 0.0f, 0.0f, g_settings.bodyScaleZ };
            out = c + (out - c) * (1.0f / std::max(0.5f, g_settings.bodyScale));
        }
        return true;
    }

    // ------------------------------------------------------------ at the mannequin

    void atMannequin(RE::PlayerCharacter* player, const Holder& h, Clock::time_point now, bool interact) noexcept
    {
        auto& st = g_state;
        const bool rigHere = g_rig.rigOn && !g_rig.worn;
        if (rigHere) {
            drawRig(h);
            for (const auto& p : g_rig.pouches) drawPouch(h, p);
        }
        if (!interact) {
            return;
        }
        const auto& manWorld = h.manWorld;
        bool areasShown = false;
        // both palms on the mannequin's shoulders: that is putting the rig on / off, not taking a pouch
        const RE::NiPoint3 right{ g_settings.shoulderX, g_settings.shoulderY, g_settings.shoulderZ };
        const RE::NiPoint3 left{ -g_settings.shoulderX, g_settings.shoulderY, g_settings.shoulderZ };
        RE::NiPoint3 p0, p1;
        const bool palms = g_hands[0].valid && g_hands[1].valid && palmInBody(h, g_hands[0].palm, p0) && palmInBody(h, g_hands[1].palm, p1);
        const float r = g_settings.wearRadius;
        const bool atShoulders = palms && ((distance(p0, right) < r && distance(p1, left) < r) || (distance(p0, left) < r && distance(p1, right) < r));

        for (int hi = 0; hi < 2; ++hi) {
            const auto& hand = g_hands[hi];
            if (!hand.valid) continue;
            RE::NiPoint3 palm;
            palmInBody(h, hand.palm, palm);

            // a pouch (or the rig) in this hand
            if (hand.grip || hand.released()) {
                if (hand.grip) {
                    st.held[hi] = heldBy(player, hand.palm);
                }
                const auto& held = st.held[hi];
                if (held.ref && held.kind >= 1 && held.kind <= kKinds && rigHere) {
                    const auto itemLocal = relative(manWorld, held.world);
                    const auto snap = snapPouch(held.kind, itemLocal);
                    const bool nearRig = distance(itemLocal.translate, RE::NiPoint3{ 0.0f, 0.0f, 85.0f }) < g_settings.showAreas;
                    const bool full = static_cast<int>(g_rig.pouches.size()) >= kMaxPouches;
                    if (nearRig && !areasShown && !full) {
                        areasShown = true;
                        for (const auto& area : g_def.areas) {
                            if ((area.kinds & (1u << held.kind)) && !area.highlight.empty()) {
                                show(acquire(area.highlight), h.anchor, h.localFor("", identity()));
                            }
                        }
                    }
                    if (snap.ok && !full) {
                        Pouch preview;
                        preview.kind = held.kind;
                        preview.area = snap.area;
                        preview.pose = snap.pose;
                        drawPouch(h, preview);
                        if (hand.released()) {
                            g_rig.pouches.push_back(preview);
                            pushSimple(kDelete, held.ref);
                            sound(0);
                            buzz(hi, 2500);
                            note("%s hung on %s at %.1f %.1f %.1f", kKindNames[held.kind], g_def.areas[snap.area].name.c_str(),
                                snap.pose.translate.x, snap.pose.translate.y, snap.pose.translate.z);
                            markDirty();
                            st.held[hi] = Held{};
                        }
                    } else if (hand.released() && full && nearRig) {
                        hud("На разгрузке больше нет места");
                    }
                } else if (held.ref && held.kind == kRigItem && !g_rig.rigOn && hand.released()) {
                    // the rig item let go at the mannequin's body: it hangs on it
                    const auto itemLocal = relative(manWorld, held.world);
                    if (distance(itemLocal.translate, RE::NiPoint3{ 0.0f, 0.0f, 88.0f }) < 30.0f) {
                        g_rig.rigOn = true;
                        g_rig.worn = false;
                        pushSimple(kDelete, held.ref);
                        sound(0);
                        buzz(hi, 2500);
                        hud("Разгрузка на манекене");
                        note("Rig hung on the mannequin");
                        markDirty();
                    }
                }
                if (hand.released()) {
                    st.held[hi] = Held{};
                }
                if (!hand.pressed()) {
                    continue;
                }
            }

            if (!hand.pressed() || st.held[hi].ref || atShoulders) {
                continue;
            }
            // an empty hand grips a pouch on the mannequin: it comes off into the hand
            if (rigHere) {
                const int index = nearestPouch(palm);
                if (index >= 0) {
                    const auto pouch = g_rig.pouches[index];
                    // what it held stays in the inventory; a holstered gun stays there too
                    g_rig.pouches.erase(g_rig.pouches.begin() + index);
                    if (const auto form = g_shared.forms[pouch.kind]) {
                        pushSimple(kGiveItem, 0, form);
                        dropToHand(form, hand.palm, now);
                    }
                    sound(1);
                    buzz(hi, 1500);
                    hud(kindTitle(pouch.kind));
                    note("%s taken off the rig", kKindNames[pouch.kind]);
                    markDirty();
                    continue;
                }
            }
            // fold the mannequin up: hold the grip on its head
            const RE::NiPoint3 head{ 0.0f, g_settings.shoulderY, g_settings.headZ };
            if (distance(head, palm) < 10.0f) {
                st.packStart = now;
                st.packHand = hi;
                hud("Держи грип, чтобы сложить манекен");
                continue;
            }
            // an empty hand grips the bare rig (no pouches on it): the rig comes off into the hand
            if (rigHere && palm.z > 64.0f && palm.z < 84.0f && std::sqrt(palm.x * palm.x + palm.y * palm.y) < 22.0f) {
                if (!g_rig.pouches.empty()) {
                    // only say so once the hand is really on the rig, not reaching past it
                    continue;
                }
                if (const auto form = g_shared.forms[kRigItem]) {
                    g_rig.rigOn = false;
                    pushSimple(kGiveItem, 0, form);
                    dropToHand(form, hand.palm, now);
                    sound(1);
                    buzz(hi, 1500);
                    hud("Разгрузка снята с манекена");
                    note("Rig taken off the mannequin");
                    markDirty();
                }
            }
        }

        // folding
        if (st.packHand >= 0) {
            const auto& hand = g_hands[st.packHand];
            if (!hand.grip) {
                st.packHand = -1;
            } else if (std::chrono::duration<float>(now - st.packStart).count() > g_settings.packSeconds) {
                st.packHand = -1;
                note("Mannequin %08X folded", g_rig.mannequin);
                pushSimple(kPack, g_rig.mannequin);
                {
                    std::scoped_lock guard(g_rigLock);
                    g_rig.mannequin = 0;
                }
                sound(1);
                hud("Манекен сложен в инвентарь");
                markDirty();
                return;
            }
        }

        // put the rig on / hang it back: both hands take the mannequin by the shoulders, both grips at once
        if (!g_rig.rigOn) {
            return;
        }
        const bool both = g_hands[0].grip && g_hands[1].grip;
        if (!both) {
            st.wearArmed = true;
            return;
        }
        const auto apart = st.gripAt[0] > st.gripAt[1] ? st.gripAt[0] - st.gripAt[1] : st.gripAt[1] - st.gripAt[0];
        if (st.wearArmed && atShoulders && !st.held[0].ref && !st.held[1].ref &&
            std::chrono::duration<float>(apart).count() < g_settings.wearWindow) {
            st.wearArmed = false;
            g_rig.worn = !g_rig.worn;
            sound(0);
            buzz(0, 3000);
            buzz(1, 3000);
            hud(g_rig.worn ? "Разгрузка надета" : "Разгрузка снята и висит на манекене");
            note(g_rig.worn ? "Rig put on" : "Rig hung back on the mannequin");
            markDirty();
        }
    }

    // ------------------------------------------------------------ on the body

    // The Weapon node of your body (the gun in the hand hangs under it).
    RE::NiNode* weaponNode(RE::PlayerCharacter* player) noexcept
    {
        RE::NiAVObject* roots[3]{ player->Get3D(true), player->Get3D(false), player->Get3D() };
        for (auto* root : roots) {
            if (!looksLikeObject(root)) continue;
            auto* node = findNode(root, "Weapon");
            if (node && node->GetRuntimeData().children.size() > 0) {
                return node;
            }
        }
        return nullptr;
    }

    // Keeps a copy of the gun in the hand (made again when the gun or its model changes) and where it is.
    void watchGun(RE::PlayerCharacter* player, RE::TESObjectWEAP* gun) noexcept
    {
        auto& g = g_gunSnapshot;
        g.seen = false;
        if (!gun) {
            return;
        }
        auto* node = weaponNode(player);
        if (!node) {
            if (!g_state.weaponLogged) {
                g_state.weaponLogged = true;
                note("Weapon node not found: a gun put away is shown by its plain model");
            }
            return;
        }
        auto& children = node->GetRuntimeData().children;
        RE::NiAVObject* model = children.size() > 0 ? children[0].get() : nullptr;
        if (!looksLikeObject(model)) {
            return;
        }
        if (g.form != gun->GetFormID() || g.source != model || !g.copy) {
            auto* copy = cloneNode(node);
            g.copy.reset(copy);
            g.form = gun->GetFormID();
            g.source = model;
            note("Gun %08X copied for the holster (model %s, %u under Weapon): %s", g.form, model->name.c_str() ? model->name.c_str() : "-",
                static_cast<unsigned>(children.size()), copy ? "ok" : "FAILED");
        }
        g.world = node->world;
        g.middle = model->worldBound.fRadius > 0.0f ? model->worldBound.center : node->world.translate;
        g.seen = true;
    }

    // Shows the gun just put into pouch `index`: the copy of it, hung as it was let go (carbine holder) or straight.
    void keepGunVisual(const Holder& h, int index, std::uint32_t form) noexcept
    {
        if (index < 0 || index >= static_cast<int>(g_gunVisuals.size())) {
            return;
        }
        auto& v = g_gunVisuals[index];
        auto& g = g_gunSnapshot;
        if (!g.copy || g.form != form) {
            v.form = 0;
            return;
        }
        if (!v.inst) {
            auto inst = std::make_unique<Instance>();
            inst->base = "gun in pouch " + std::to_string(index);
            v.inst = inst.get();
            g_instances.push_back(std::move(inst));
        }
        if (auto* old = v.inst->node.get(); looksLikeObject(old) && old->parent && looksLikeObject(old->parent)) {
            old->parent->DetachChild(old);
        }
        v.inst->node = g.copy;
        v.form = form;
        g.copy.reset();      // the next gun in the hand gets a copy of its own
        g.source = nullptr;
        const auto& p = g_rig.pouches[index];
        v.inPouch = fixedGunPose(p.kind);
        if (p.kind == kSling) {
            const auto& bone = boneOf(p);
            auto* parent = h.parentFor(bone);
            if (parent) {
                const auto pouchWorld = compose(parent->world, h.localFor(bone, p.pose));
                v.inPouch = relative(pouchWorld, g.world);
            }
        }
    }

    // the holster / carbine holder the gun in the hand is put into: the palm or the middle of the gun at it
    int gunPlaceNear(const Holder& h, const RE::NiPoint3& palmBody, bool withMiddle) noexcept
    {
        RE::NiPoint3 middle{};
        const bool hasMiddle = withMiddle && palmInBody(h, g_gunSnapshot.middle, middle);
        int best = -1;
        float bestRatio = 1.0f;
        for (std::size_t i = 0; i < g_rig.pouches.size(); ++i) {
            const auto& p = g_rig.pouches[i];
            if (p.kind != kHolster && p.kind != kSling) continue;
            float d = distance(pouchMiddle(p), palmBody);
            if (hasMiddle && p.kind == kSling) d = std::min(d, distance(pouchMiddle(p), middle));
            const float ratio = d / reachOf(p.kind);
            if (ratio < bestRatio) {
                bestRatio = ratio;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    void storeInBag(RE::PlayerCharacter* player, Pouch& pouch, RE::TESObjectREFR* ref, int hi) noexcept
    {
        auto* base = ref->GetObjectReference();
        if (addToPouch(pouch, base->GetFormID())) {
            pushSimple(kStoreRef, ref->GetFormID());
            char text[200]{};
            const auto name = RE::TESFullName::GetFullName(*base);
            std::snprintf(text, sizeof(text), "Сумка: %.*s (%d/%d)", static_cast<int>(name.size()), name.data(), pouch.total(),
                capacityOf(pouch.kind));
            hud(text);
            note("Bag: %08X stored", base->GetFormID());
            sound(3);
            buzz(hi, 1500);
            markDirty();
        } else {
            hud("Сумка полна");
        }
        (void)player;
    }

    void onBody(RE::PlayerCharacter* player, const Holder& h, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        drawRig(h);
        for (const auto& p : g_rig.pouches) drawPouch(h, p);
        if (!st.bodyLogged) {
            st.bodyLogged = true;
            auto* pelvis = h.parentFor("Pelvis");
            auto* spine = h.parentFor("SPINE2");
            note("Body: skeleton %s, Pelvis %s, SPINE2 %s", h.skeleton ? h.skeleton->name.c_str() : "-", pelvis ? "found" : "MISSING",
                spine ? "found" : "MISSING");
        }

        auto* gunNow = equippedGun(player);
        auto* gun = gunNow ? gunNow : st.lastGun;   // let go this frame: ROCK may already have dropped it
        st.lastGun = gunNow;
        watchGun(player, gunNow);
        if (gunNow) {
            const auto count = inventoryCount(player, gunNow->GetFormID());
            if (gunNow != st.countedGun) {
                st.countedGun = gunNow;
                st.gunCount = count;
            } else {
                st.gunCount = std::max(st.gunCount, count);
            }
        }
        const bool cooling = std::chrono::duration<float>(now - st.holsterAt).count() < g_settings.holsterCooldown;

        // an item let go in the bag a moment ago that was not found then
        if (st.bagPouch >= 0) {
            if (now - st.bagAt > std::chrono::milliseconds(400) || st.bagPouch >= static_cast<int>(g_rig.pouches.size())) {
                st.bagPouch = -1;
            } else {
                auto& pouch = g_rig.pouches[st.bagPouch];
                RE::NiTransform space;
                if (h.space(space)) {
                    const auto middle = toWorld(space, h.inflate(pouch.pose).translate + row(pouch.pose.rotate, 1) * depthOf(pouch.kind));
                    if (auto* ref = looseItemNear(player, middle)) {
                        storeInBag(player, pouch, ref, st.bagHand);
                        st.bagPouch = -1;
                    }
                }
            }
        }

        for (int hi = 0; hi < 2; ++hi) {
            const auto& hand = g_hands[hi];
            if (!hand.valid) continue;
            RE::NiPoint3 palm;
            if (!palmInBody(h, hand.palm, palm)) continue;
            const bool gunHand = hi == g_settings.gunHand;
            int index = gunHand ? gunPlaceNear(h, palm, gun != nullptr) : -1;
            if (index < 0) index = nearestPouch(palm);
            if (index < 0) continue;
            auto& pouch = g_rig.pouches[index];
            const bool gunPlace = pouch.kind == kHolster || pouch.kind == kSling;

            if (gunPlace && gunHand) {
                // holster: let go of (or press) the grip with a gun at an empty holster to put it away
                if (gun && pouch.total() == 0 && (hand.released() || (gunNow && hand.pressed())) && !cooling) {
                    pouch.items[0] = Stack{ gun->GetFormID(), 1 };
                    keepGunVisual(h, index, gun->GetFormID());
                    const auto want = st.countedGun == gun ? st.gunCount : inventoryCount(player, gun->GetFormID());
                    pushSimple(kUnequip, 0, gun->GetFormID(), static_cast<std::int32_t>(want));
                    note("Gun %08X put away in %s %d (you have %u)", gun->GetFormID(), pouch.kind == kSling ? "carbine holder" : "holster",
                        index, want);
                    st.holsterAt = now;
                    st.lastGun = nullptr;
                    st.countedGun = nullptr;
                    hud(pouch.kind == kSling ? "Оружие в держателе" : "Оружие в кобуре");
                    sound(0);
                    buzz(hi, 2500);
                    markDirty();
                    continue;
                }
                // ..and grip at a full holster with an empty hand to draw it
                if (!gunNow && pouch.total() > 0 && hand.pressed() && !cooling) {
                    const auto form = pouch.items[0].form;
                    pouch.items = {};
                    const auto have = inventoryCount(player, form);
                    note("Gun %08X taken from pouch %d (you have %u)", form, index, have);
                    if (have > 0) {
                        pushSimple(kEquip, 0, form);
                        sound(1);
                        buzz(hi, 2000);
                    } else {
                        hud("Этого оружия у тебя больше нет");
                    }
                    st.holsterAt = now;
                    markDirty();
                    continue;
                }
                if (hand.pressed() && pouch.total() > 0 && gunNow) {
                    note("Pouch %d holds a gun but the hand holds one too: put that one away first", index);
                }
                continue;
            }
            if (pouch.kind != kDump) {
                // magazine pouches hand out magazines with ManualReloadF (next stage)
                if (hand.pressed()) buzz(hi, 400);
                continue;
            }
            // the bag: let go of an item in it and it goes in, grip in it with an empty hand to take the last one out
            if (hand.released()) {
                if (auto* ref = looseItemNear(player, hand.palm)) {
                    storeInBag(player, pouch, ref, hi);
                } else {
                    st.bagPouch = index;   // the hand may still hold it this frame: look again for a moment
                    st.bagHand = hi;
                    st.bagAt = now;
                    detail("Bag: nothing let go near the palm yet");
                }
                continue;
            }
            if (!hand.pressed()) continue;
            const auto form = takeFromPouch(pouch, player);
            if (!form) {
                buzz(hi, 400);
                hud("Пусто");
                markDirty();
                continue;
            }
            if (auto* item = lookupForm(form)) {
                char text[200]{};
                const auto name = RE::TESFullName::GetFullName(*item);
                std::snprintf(text, sizeof(text), "%.*s (осталось %d)", static_cast<int>(name.size()), name.data(), pouch.total());
                hud(text);
            }
            dropToHand(form, hand.palm, now);
            sound(1);
            buzz(hi, 1500);
            markDirty();
        }
    }

    // A chem or food from a pouch brought to the mouth is used.
    void checkMouth(RE::PlayerCharacter* player, Clock::time_point now) noexcept
    {
        if (g_inHand.empty()) {
            return;
        }
        RE::NiPoint3 head;
        if (!bone("Head", head)) {
            const auto feet = player->GetPosition();
            head = { feet.x, feet.y, feet.z + 115.0f };
        }
        const float yaw = player->data.angle.z;
        const RE::NiPoint3 mouth = head + RE::NiPoint3{ std::sin(yaw), std::cos(yaw), 0.0f } * 4.0f - RE::NiPoint3{ 0.0f, 0.0f, 5.0f };
        for (auto it = g_inHand.begin(); it != g_inHand.end();) {
            if (now - it->at > std::chrono::seconds(60)) {
                it = g_inHand.erase(it);
                continue;
            }
            auto* ref = it->ref ? lookupRef(it->ref) : nullptr;
            if (it->ref && (!ref || ref->IsDeleted() || ref->IsDisabled())) {
                it = g_inHand.erase(it);
                continue;
            }
            if (ref) {
                auto* base = ref->GetObjectReference();
                if (base && base->GetFormType() == RE::ENUM_FORM_ID::kALCH && distance(ref->GetPosition(), mouth) < g_settings.mouthRadius) {
                    pushSimple(kConsume, it->ref);
                    buzz(0, 1000);
                    buzz(1, 1000);
                    it = g_inHand.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // ------------------------------------------------------------ the frame

    void update() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D()) {
            return;
        }
        const auto now = Clock::now();
        {
            std::vector<std::int32_t> incoming;
            bool arrived = false;
            std::uint32_t mannequin = 0;
            bool mannequinChanged = false;
            std::string hudText;
            {
                std::scoped_lock guard(g_shared.lock);
                if (g_shared.stateArrived) {
                    incoming.swap(g_shared.incoming);
                    g_shared.stateArrived = false;
                    arrived = true;
                }
                if (!g_shared.hudText.empty()) {
                    hudText.swap(g_shared.hudText);
                }
                if (g_shared.mannequinChanged) {
                    mannequin = g_shared.mannequinArrived;
                    g_shared.mannequinChanged = false;
                    mannequinChanged = true;
                }
            }
            if (!hudText.empty()) {
                hud(hudText.c_str());
            }
            if (arrived) {
                loadState(incoming);
                g_state = State{};
                g_inHand.clear();
            }
            if (mannequinChanged) {
                {
                    std::scoped_lock guard(g_rigLock);
                    if (g_rig.mannequin && g_rig.mannequin != mannequin) {
                        pushSimple(kPack, g_rig.mannequin);   // only one mannequin: the old one is folded
                    }
                    g_rig.mannequin = mannequin;
                }
                note("Mannequin set up: %08X", mannequin);
                hud(g_rig.rigOn && !g_rig.worn ? "Манекен поставлен, разгрузка на нём" : "Манекен поставлен");
                markDirty();
            }
        }
        if (!g_rigLoaded || !g_def.loaded) {
            return;
        }
        takeNewRefs();
        readHand(0);
        readHand(1);
        for (int h = 0; h < 2; ++h) {
            if (g_hands[h].pressed()) {
                g_state.gripAt[h] = now;
            }
        }

        beginFrameModels();
        const bool menu = menuOpen();

        auto* mannequin = lookupRef(g_rig.mannequin);
        if (mannequin && (mannequin->IsDeleted() || mannequin->IsDisabled())) {
            mannequin = nullptr;
        }
        if (mannequin) {
            const auto holder = mannequinHolder(player, mannequin);
            if (holder.valid) {
                const bool nearby = distance(mannequin->GetPosition(), player->GetPosition()) < g_settings.mannequinReach;
                atMannequin(player, holder, now, nearby && !menu);
            }
        }
        if (g_rig.rigOn && g_rig.worn) {
            const auto holder = bodyHolder(player);
            if (holder.valid) {
                if (menu) {
                    drawRig(holder);
                    for (const auto& p : g_rig.pouches) drawPouch(holder, p);
                } else {
                    onBody(player, holder, now);
                }
            }
        }
        endFrameModels();
        checkMouth(player, now);

        if (g_dirty && now - g_lastSave > std::chrono::milliseconds(500)) {
            g_dirty = false;
            g_lastSave = now;
            pushSimple(kSave);
        }
    }

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        __try {
            update();
        } __except (1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logWarning("A fault inside the frame update was caught; RigVR keeps running");
            }
        }
    }

    constexpr const char* kFrikTag = "RigVR";

    void gameReady() noexcept
    {
        static bool done = false;
        if (!done) {
            done = true;
            const int error = FRIKApiV2::initialize(3);
            if (error != 0) {
                note("FRIK API v2.3+ unavailable (error %d); RigVR cannot run", error);
            } else {
                const bool hooked = FRIKApiV2::inst->registerFrameCallback(kFrikTag,
                    static_cast<std::uint32_t>(FRIKApiV2::FramePhase::AfterWorldFinal), onFrikFrame, nullptr, 0);
                note("FRIK %s, API v2.%u: frame callback %s", FRIKApiV2::inst->getModVersion(), FRIKApiV2::inst->getVersion(),
                    hooked ? "registered" : "FAILED");
            }
        }
        if (!g_vr) {
            using GetInterface_t = void* (*)(const char*, vr::EVRInitError*);
            const auto module = REX::W32::GetModuleHandleW(L"openvr_api.dll");
            const auto get = module ? reinterpret_cast<GetInterface_t>(REX::W32::GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
            vr::EVRInitError error = vr::VRInitError_None;
            g_vr = get ? static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error)) : nullptr;
            logNote(g_vr ? "OpenVR ready" : "OpenVR not found: buttons cannot be read");
        }
        // a loaded save: the nodes the models hung on are gone; the script sends the rig again
        detachAllModels();
        g_state = State{};
        g_inHand.clear();
        logNote("RigVR ready (build " RIGVR_BUILD ")");
    }
}

namespace rigvr::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "RigVR",
            .pluginVersion = 1,
            .onGameReady = &gameReady,
        };
        return value;
    }

    void onLoad() noexcept
    {
        loadSettings();
        loadRigFile();
        prepareInstances();
        const auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(registerNatives)) {
            logWarning("Could not register Papyrus functions; RigVR cannot work");
        }
    }
}
