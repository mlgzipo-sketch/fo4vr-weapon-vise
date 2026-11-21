#include "Zones.h"

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
#include "Log.h"
#include "NiMath.h"
#include "Settings.h"
#include "SafeMemory.h"
#include "Angles.h"
#include "Gun.h"
#include "Nodes.h"
#include "Hands.h"
#include "Paths.h"
#include "ScriptBridge.h"
#include "State.h"
#include "Rail.h"
#include "Palm.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ core2: the zones, editable (Blender) per gun
    // Data/F4SE/Plugins/ManualReloadF_zones/<weapon>.txt, in the gun root's frame (game units):
    //   magazine lo x y z hi x y z     the box the palm touches to take / push the magazine (seated)
    //   slide lo x y z hi x y z        ..the slide / bolt (at rest)
    //   well side S ahead A            the well catches a magazine this close to its line, from A below the mouth
    //   well mouth x y z axis x y z    (written for reference: where the magazine's top leaves the well, the way out)
    //   slide route x y z x y z ...   the slide / charging handle's way (first point at rest), instead of the recorded one
    // Written from what ROCK measured when there is no file; read instead of it when there is.
    // At equip: the opening part's turn drawn in Blender (route cylinder / barrels / breech), so a gun opens by
    // hand even with no recording of the game's reload.
    // a turn that is a turn: three unit axes at right angles, right-handed (a broken hold line is not used)
    bool squareRotation(const RE::NiTransform& t) noexcept
    {
        const auto x = axisOf(t, 0), y = axisOf(t, 1), z = axisOf(t, 2);
        return std::fabs(dot(x, x) - 1.0f) < 0.05f && std::fabs(dot(y, y) - 1.0f) < 0.05f && std::fabs(dot(x, y)) < 0.05f &&
               dot(cross(x, y), z) > 0.9f;
    }

    void loadZoneRoutes(std::uint32_t weapon) noexcept
    {
        auto& st = g_state;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_zones/%08X.txt", weapon);
        std::ifstream in(path);
        st.hasZonesFile = static_cast<bool>(in);
        std::vector<std::string> lines;
        std::unordered_map<std::string, RE::NiPoint3> boxMiddle;
        bool hingeKnown = false;
        RE::NiPoint3 hinge{};
        for (std::string line; in && std::getline(in, line);) {
            std::istringstream words(line);
            std::string head, name, word;
            words >> head >> name;
            RE::NiPoint3 lo{}, hi{};
            if (head == "roundhold") {
                std::istringstream all(line);
                std::string skip;
                all >> skip;
                float v[12]{};
                int n = 0;
                while (n < 12 && all >> v[n]) {
                    ++n;
                }
                if (n == 12) {
                    auto& h = st.roundHold;
                    h = identityTransform();
                    h.translate = RE::NiPoint3{ v[0], v[1], v[2] };
                    for (int r = 0; r < 3; ++r) {
                        for (int c = 0; c < 3; ++c) {
                            h.rotate.entry[r][c] = v[3 + r * 3 + c];
                        }
                    }
                    st.roundHoldKnown = squareRotation(h);
                    note(st.roundHoldKnown ? "ZONES: a round from the belt is held as it was shown" : "ZONES: the roundhold line is broken: not used");
                }
            } else if (head == "point" && name == "hinge" && words >> hinge.x >> hinge.y >> hinge.z) {
                hingeKnown = true;
            } else if (head == "box" && words >> word >> lo.x >> lo.y >> lo.z >> word >> hi.x >> hi.y >> hi.z) {
                boxMiddle[name] = (lo + hi) * 0.5f;
            } else if (head == "socket" && (name == "nuke" || name == "missile" || name == "cell") && words >> word >> lo.x >> lo.y >> lo.z) {
                st.singleShot = name != "cell";
                st.breechMouth = lo;
                float side = 0.0f, ahead = 0.0f;
                RE::NiPoint3 axis{};
                bool axisRead = false;
                while (words >> word) {
                    if (word == "side" && words >> side) {
                        st.breechReach = std::max(side, 0.5f);
                    } else if (word == "ahead") {
                        words >> ahead;
                    } else if (word == "axis" && words >> axis.x >> axis.y >> axis.z) {
                        axisRead = true;
                    }
                }
                // the cylinder exactly as drawn (Blender or the game's editor): no reach of its own
                st.breechZone = axisRead && dot(axis, axis) > 0.5f && side > 0.0f && ahead > 0.0f;
                if (st.breechZone) {
                    st.breechAxis = normalized(axis);
                    st.breechSide = side;
                    st.breechAhead = ahead;
                    st.breechReach = side;
                }
                note("ZONES: one %s goes in at (%.1f %.1f %.1f), reach %.1f", name.c_str(), lo.x, lo.y, lo.z, st.breechReach);
            }
            lines.push_back(line);
        }
        for (const auto& line : lines) {
            std::istringstream words(line);
            std::string head, name;
            words >> head >> name;
            if (head != "route" || (name != "cylinder" && name != "barrels" && name != "breech" && name != "crank" && name != "lever")) {
                continue;
            }
            std::vector<RE::NiPoint3> pts;
            float x, y, z;
            while (words >> x >> y >> z) {
                pts.push_back(RE::NiPoint3{ x, y, z });
            }
            if (name == "crank") {
                if (pts.size() >= 5) {
                    const std::size_t n = distance(pts.front(), pts.back()) < 0.5f ? pts.size() - 1 : pts.size();
                    RE::NiPoint3 c{};
                    for (std::size_t i = 0; i < n; ++i) c = c + pts[i];
                    c = c * (1.0f / static_cast<float>(n));
                    const auto axis = normalized(cross(pts[0] - c, pts[n / 4] - c));
                    if (dot(axis, axis) > 0.5f) {
                        st.crankKnown = true;
                        st.crankCenter = c;
                        st.crankAxis = axis;
                        st.crankStart = pts[0];
                        note("ZONES: crank turns about (%.2f %.2f %.2f) at (%.1f %.1f %.1f), handle radius %.1f", axis.x, axis.y, axis.z, c.x, c.y, c.z,
                            distance(pts[0], c));
                    }
                }
                continue;
            }
            // the way starts where the part stands closed: a route drawn from the open end is turned round
            if (const auto box = boxMiddle.find(name); box != boxMiddle.end() && pts.size() >= 2 &&
                                                       distance(pts.back(), box->second) < distance(pts.front(), box->second)) {
                std::reverse(pts.begin(), pts.end());
                note("ZONES: route %s was drawn from its open end: turned round", name.c_str());
            }
            std::vector<float> at(pts.size(), 0.0f);
            float total = 0.0f;
            for (std::size_t i = 1; i < pts.size(); ++i) {
                total += distance(pts[i], pts[i - 1]);
                at[i] = total;
            }
            for (auto& a : at) {
                a = total > 0.0f ? a / total : 0.0f;
            }
            auto fit = fitArc(pts, at);
            if (hingeKnown && (name == "barrels" || name == "breech") && pts.size() >= 2) {
                // the hinge is where it was put (zones file "point hinge"): the part turns about it from the way's first
                // point to its last, whatever circle the drawn points make
                auto u = pts.front() - hinge, v = pts.back() - hinge;
                const auto n = cross(u, v);
                if (dot(n, n) > 1e-6f) {
                    fit = ArcFit{};
                    fit.axis = normalized(n);
                    u = u - fit.axis * dot(u, fit.axis);
                    v = v - fit.axis * dot(v, fit.axis);
                    fit.angle = std::atan2(std::sqrt(dot(cross(u, v), cross(u, v))), dot(u, v));
                    fit.pivot = hinge;
                    fit.arc = fit.angle > 0.05f;
                    fit.arcAt = 1.0f;
                    fit.k = pts.size() - 1;
                    note("ZONES: %s turns about its hinge point (%.1f %.1f %.1f)", name.c_str(), hinge.x, hinge.y, hinge.z);
                }
            }
            if (name == "lever") {
                if (pts.size() >= 2 && total > 0.5f) {
                    st.leverKnown = true;
                    st.leverPts = pts;
                    st.leverAt = at;
                    st.leverFit = fit;
                    note("ZONES: the handle turns %.0f deg (first %.0f%% of its way), then goes straight; way %.1f long", fit.arc ? fit.angle * 57.29578f : 0.0f,
                        fit.arc ? fit.arcAt * 100.0f : 0.0f, total);
                }
                continue;
            }
            if (fit.arc && fit.angle > 0.2f) {
                st.zoneHinge = fit;
                note("ZONES: %s opens %.0f deg about (%.2f %.2f %.2f) at (%.1f %.1f %.1f) (route from Blender)", name.c_str(), fit.angle * 57.29578f, fit.axis.x,
                    fit.axis.y, fit.axis.z, fit.pivot.x, fit.pivot.y, fit.pivot.z);
            } else {
                note("ZONES: route %s is not a turn (%zu points): ignored", name.c_str(), pts.size());
            }
        }
    }

    void zonesFor(std::uint32_t weapon, bool mayWrite) noexcept
    {
        auto& st = g_state;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_zones/%08X.txt", weapon);
        std::ifstream in(path);
        if (!in && !mayWrite) {
            note("ZONES: no %s (the gun's own distances are used)", path);
            return;
        }
        if (!in) {
            std::error_code ignored;
            std::filesystem::create_directories("Data/F4SE/Plugins/ManualReloadF_zones", ignored);
            std::ofstream out(path);
            if (!out) {
                return;
            }
            const float scale = st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f;
            const auto mouth = st.magRest.translate + st.axis * (st.length - st.topAt * scale);   // the magazine's top as it leaves the well
            char text[256];
            out << "; ManualReloadF zones of weapon " << std::hex << weapon << std::dec << ", gun root frame (game units). Edit, or move the cubes in Blender.\n";
            std::snprintf(text, sizeof(text), "magazine lo %.3f %.3f %.3f hi %.3f %.3f %.3f\n", st.magLo.x, st.magLo.y, st.magLo.z, st.magHi.x, st.magHi.y, st.magHi.z);
            out << text;
            if (st.slideBoxKnown) {
                std::snprintf(text, sizeof(text), "slide lo %.3f %.3f %.3f hi %.3f %.3f %.3f\n", st.slideLo.x, st.slideLo.y, st.slideLo.z, st.slideHi.x, st.slideHi.y,
                    st.slideHi.z);
                out << text;
            }
            std::snprintf(text, sizeof(text), "well side %.3f ahead %.3f\n", g_settings.catchSide, g_settings.catchAhead);
            out << text;
            std::snprintf(text, sizeof(text), "well mouth %.3f %.3f %.3f axis %.4f %.4f %.4f\n", mouth.x, mouth.y, mouth.z, st.axis.x, st.axis.y, st.axis.z);
            out << text;
            note("ZONES: %s written from ROCK's boxes (edit it, or in Blender)", path);
            return;
        }
        std::string line;
        int used = 0;
        st.chambers.clear();
        while (std::getline(in, line)) {
            std::vector<std::string> words;
            std::string word;
            for (const char c : line) {
                if (c == ' ' || c == '\t' || c == '\r') {
                    if (!word.empty()) words.push_back(word), word.clear();
                } else {
                    word += c;
                }
            }
            if (!word.empty()) words.push_back(word);
            if (words.empty() || words[0][0] == ';') {
                continue;
            }
            auto num = [&](std::size_t i) { return i < words.size() ? std::strtof(words[i].c_str(), nullptr) : 0.0f; };
            if ((words[0] == "magazine" || words[0] == "slide") && words.size() >= 9 && words[1] == "lo" && words[5] == "hi") {
                const RE::NiPoint3 lo{ std::min(num(2), num(6)), std::min(num(3), num(7)), std::min(num(4), num(8)) };
                const RE::NiPoint3 hi{ std::max(num(2), num(6)), std::max(num(3), num(7)), std::max(num(4), num(8)) };
                if (words[0] == "magazine") {
                    st.magLo = lo, st.magHi = hi, st.magBoxKnown = true;
                } else {
                    st.slideLo = lo, st.slideHi = hi, st.slideBoxKnown = true, st.zoneSlide = true;
                    if (st.slideCenterKnown) {
                        st.slideCenter = (lo + hi) * 0.5f;
                    }
                }
                ++used;
            } else if (words[0] == "well" && words.size() >= 9 && words[1] == "mouth" && words[5] == "axis") {
                const RE::NiPoint3 mouth{ num(2), num(3), num(4) };
                const RE::NiPoint3 axis{ num(6), num(7), num(8) };
                if (dot(axis, axis) > 0.25f) {
                    st.axis = normalized(axis);
                    st.zoneMouth = mouth;
                    st.zoneMouthKnown = true;
                    const float scale = st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f;
                    st.length = std::max(2.0f, dot(mouth - st.magRest.translate, st.axis) + st.topAt * scale);
                    st.zoneWell = true;
                    // a cell, a core or a drum sits whole in its box: it is out once it has moved its own size along the way
                    // (also when the mouth was put in the middle of the magazine, which leaves almost no way out)
                    const float straight = std::max({ std::fabs(st.axis.x), std::fabs(st.axis.y), std::fabs(st.axis.z) });
                    const float raw = dot(mouth - st.magRest.translate, st.axis) + st.topAt * scale;
                    if (st.magBoxKnown && ((st.cellFed && straight > 0.95f) || raw < 3.0f)) {
                        const auto size = st.magHi - st.magLo;
                        st.length = std::fabs(size.x * st.axis.x) + std::fabs(size.y * st.axis.y) + std::fabs(size.z * st.axis.z) + 0.5f;
                        note("ZONES: the magazine's own size along the way out: it leaves after %.1f", st.length);
                    }
                    note("ZONES: the well's mouth at (%.1f %.1f %.1f), way out (%.2f %.2f %.2f): the magazine leaves it after %.1f", mouth.x, mouth.y, mouth.z,
                        st.axis.x, st.axis.y, st.axis.z, st.length);
                    ++used;
                }
            } else if (words[0] == "box" && words.size() >= 10 && words[2] == "lo" && words[6] == "hi") {
                const RE::NiPoint3 lo{ std::min(num(3), num(7)), std::min(num(4), num(8)), std::min(num(5), num(9)) };
                const RE::NiPoint3 hi{ std::max(num(3), num(7)), std::max(num(4), num(8)), std::max(num(5), num(9)) };
                st.zoneBoxes[words[1]] = { lo, hi };
                note("ZONES: box %s (%.1f %.1f %.1f)..(%.1f %.1f %.1f)", words[1].c_str(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                ++used;
            } else if (words[0] == "route" && words.size() >= 8) {
                auto& pts = st.zoneRoutes[words[1]];
                pts.clear();
                for (std::size_t i = 2; i + 2 < words.size(); i += 3) {
                    pts.push_back(RE::NiPoint3{ num(i), num(i + 1), num(i + 2) });
                }
                ++used;
            } else if (words[0] == "point" && words.size() >= 5 && words[1] == "magtop") {
                const RE::NiPoint3 at{ num(2), num(3), num(4) };
                st.catchPointInNode = relative(st.magRest, pointTransform(at)).translate;
                st.catchPointKnown = true;
                note("ZONES: the magazine is caught by its point (%.1f %.1f %.1f) in the gun, (%.2f %.2f %.2f) on the magazine", at.x, at.y, at.z,
                    st.catchPointInNode.x, st.catchPointInNode.y, st.catchPointInNode.z);
            } else if (words[0] == "hold" && words.size() >= 13) {
                auto& h = st.holdInWand;
                h = identityTransform();
                h.translate = RE::NiPoint3{ num(1), num(2), num(3) };
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        h.rotate.entry[r][c] = num(4 + r * 3 + c);
                    }
                }
                st.holdInWandKnown = squareRotation(h);
                if (!st.holdInWandKnown) {
                    note("ZONES: the hold line is broken (its turn is flat): not used, save it again");
                } else
                note("ZONES: a magazine from the belt comes as it was shown in the hand (at %.1f %.1f %.1f of the controller)", h.translate.x, h.translate.y, h.translate.z);
            } else if (words[0] == "point" && words.size() >= 5 && words[1] == "maghold") {
                st.holdPointInGun = RE::NiPoint3{ num(2), num(3), num(4) };
                st.holdPointKnown = true;
                note("ZONES: a magazine from the belt is held at (%.1f %.1f %.1f) of it (seated, in the gun)", num(2), num(3), num(4));
            } else if (words[0] == "point" && words.size() >= 5 && words[1].rfind("chamber", 0) == 0) {
                st.chambers.push_back(RE::NiPoint3{ num(2), num(3), num(4) });
                ++used;
            } else if (words[0] == "slide" && words.size() >= 8 && words[1] == "route") {
                st.route.clear();
                for (std::size_t i = 2; i + 2 < words.size(); i += 3) {
                    st.route.push_back(RE::NiPoint3{ num(i), num(i + 1), num(i + 2) });
                }
                st.routeAt.assign(st.route.size(), 0.0f);
                float total = 0.0f;
                for (std::size_t i = 1; i < st.route.size(); ++i) {
                    total += distance(st.route[i], st.route[i - 1]);
                    st.routeAt[i] = total;
                }
                if (st.route.size() >= 2 && total > 0.2f) {
                    for (auto& at : st.routeAt) {
                        at /= total;
                    }
                    st.slideFit = fitArc(st.route, st.routeAt);
                    if (st.slideFit.arc && st.slideFit.angle > 0.35f) {
                        st.boltAction = true;
                    }
                    note("ZONES: the slide's way from the file: %zu points, %.1f long; turns %.0f deg about (%.2f %.2f %.2f) at (%.1f %.1f %.1f) over the first %.0f%%",
                        st.route.size(), total, st.slideFit.angle * 57.29578f, st.slideFit.axis.x, st.slideFit.axis.y, st.slideFit.axis.z, st.slideFit.pivot.x,
                        st.slideFit.pivot.y, st.slideFit.pivot.z, st.slideFit.arcAt * 100.0f);
                    ++used;
                } else {
                    st.route.clear();
                    st.routeAt.clear();
                }
            } else if (words[0] == "well" && words.size() >= 5 && words[1] == "side" && words[3] == "ahead") {
                st.zoneCatchSide = num(2);
                st.zoneCatchAhead = num(4);
                ++used;
            }
        }
        st.zonesFromFile = used > 0;
        note("ZONES: %s read, %d zones (magazine (%.1f %.1f %.1f)..(%.1f %.1f %.1f), slide (%.1f %.1f %.1f)..(%.1f %.1f %.1f), well side %.1f ahead %.1f)", path, used,
            st.magLo.x, st.magLo.y, st.magLo.z, st.magHi.x, st.magHi.y, st.magHi.z, st.slideLo.x, st.slideLo.y, st.slideLo.z, st.slideHi.x, st.slideHi.y,
            st.slideHi.z, st.zoneCatchSide, st.zoneCatchAhead);
    }
}
