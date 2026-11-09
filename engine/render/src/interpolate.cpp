#include "dream/render/interpolate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace dream::render {

namespace {

// Same signature and the same texture coordinates: games that cull triangles on the CPU drop
// strips every frame, and the next strip of the same signature (a neighbouring piece of the same
// mesh) would otherwise pair with it and tear. A strip keeps its UVs from frame to frame.
bool same(const Frame& fa, const Polygon& a, const Frame& fb, const Polygon& b) {
    if (!(a.count == b.count && a.pcw == b.pcw && a.isp == b.isp && a.tsp == b.tsp &&
          a.tcw == b.tcw && a.tsp1 == b.tsp1 && a.tcw1 == b.tcw1))
        return false;
    for (std::uint32_t k = 0; k < a.count; ++k) {
        const Vertex& va = fa.vertices[a.first + k];
        const Vertex& vb = fb.vertices[b.first + k];
        if (va.u != vb.u || va.v != vb.v)
            return false;
    }
    return true;
}

// Hash of everything `same` compares.
std::uint64_t strip_key(const Frame& f, const Polygon& p) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint32_t w) { h = (h ^ w) * 1099511628211ull; };
    mix(p.count); mix(p.pcw); mix(p.isp); mix(p.tsp); mix(p.tcw); mix(p.tsp1); mix(p.tcw1);
    for (std::uint32_t k = 0; k < p.count; ++k) {
        std::uint32_t u, v;
        std::memcpy(&u, &f.vertices[p.first + k].u, 4);
        std::memcpy(&v, &f.vertices[p.first + k].v, 4);
        mix(u);
        mix(v);
    }
    return h;
}

// Mean on-screen distance between two strips' vertices (same count).
float distance(const Frame& fa, const Polygon& a, const Frame& fb, const Polygon& b) {
    float d = 0.0f;
    for (std::uint32_t k = 0; k < a.count; ++k) {
        const Vertex& va = fa.vertices[a.first + k];
        const Vertex& vb = fb.vertices[b.first + k];
        d += std::abs(vb.x - va.x) + std::abs(vb.y - va.y);
    }
    return d / static_cast<float>(a.count);
}

}  // namespace

MatchStats match_frames(const Frame& older, const Frame& newer, unsigned window,
                        std::vector<StripPair>& out) {
    // Pairing by key, not by submission order: games re-sort translucent strips by depth every
    // frame (Rayman 2: the VMU menu's text and icons swap places while the camera moves), which an
    // in-order walk with a lookahead of `window` strips cannot follow. A strip pairs with an
    // unused strip of the older frame with the same signature and texture coordinates, the one
    // nearest on screen (identical copies: both hands, repeated letters).
    (void)window;
    out.clear();
    MatchStats st;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key, b_key;
    std::vector<char> used_a, used_b;
    struct Cand {
        float d;
        std::uint32_t i, j;
    };
    std::vector<Cand> cand;
    for (std::uint32_t l = 0; l < newer.lists.size(); ++l) {
        const auto& A = older.lists[l];
        const auto& B = newer.lists[l];
        st.strips_a += static_cast<std::uint32_t>(A.size());
        st.strips_b += static_cast<std::uint32_t>(B.size());
        for (const auto& p : B) st.vertices_b += p.count;
        by_key.clear();
        for (std::uint32_t i = 0; i < A.size(); ++i)
            by_key[strip_key(older, A[i])].push_back(i);
        b_key.clear();
        for (std::uint32_t j = 0; j < B.size(); ++j)
            b_key[strip_key(newer, B[j])].push_back(j);
        used_a.assign(A.size(), 0);
        used_b.assign(B.size(), 0);
        for (const auto& [key, js] : b_key) {
            const auto it = by_key.find(key);
            if (it == by_key.end())
                continue;
            const auto& is = it->second;
            // Nearest pairs first, over the whole group of copies: walking the newer copies in
            // order lets a new copy take an old one's partner (Rayman 2 name entry: a letter turned
            // into the third "A" stole the next A's partner, which stole the last one's).
            cand.clear();
            if (is.size() * js.size() <= 65536) {
                for (const std::uint32_t j : js)
                    for (const std::uint32_t i : is)
                        if (same(older, A[i], newer, B[j]))
                            cand.push_back({distance(older, A[i], newer, B[j]), i, j});
                std::sort(cand.begin(), cand.end(),
                          [](const Cand& x, const Cand& y) { return x.d < y.d; });
            } else {
                // shortcut: huge groups of identical strips keep the cheaper per-strip nearest pick.
                for (const std::uint32_t j : js) {
                    float best = 0.0f;
                    std::uint32_t best_i = 0;
                    bool found = false;
                    for (const std::uint32_t i : is) {
                        if (used_a[i] || !same(older, A[i], newer, B[j]))
                            continue;
                        const float e = distance(older, A[i], newer, B[j]);
                        if (!found || e < best) best = e, best_i = i, found = true;
                    }
                    if (found) {
                        used_a[best_i] = 1;
                        cand.push_back({best, best_i, j});
                    }
                }
                for (auto& c : cand) used_a[c.i] = 0;
            }
            for (const Cand& c : cand) {
                if (used_a[c.i] || used_b[c.j])
                    continue;
                used_a[c.i] = used_b[c.j] = 1;
                out.push_back({l, c.i, c.j});
                ++st.matched;
                st.matched_vertices += B[c.j].count;
            }
        }
    }
    return st;
}

bool blend_frames(const Frame& older, const Frame& newer, const BlendOptions& opt,
                  std::vector<StripPair>& pairs, Frame& out, BlendStats& stats) {
    out = newer;
    stats = BlendStats{};
    stats.match = match_frames(older, newer, opt.window, pairs);
    // Camera cut test: the median movement of paired, on-screen first vertices.
    std::vector<float> motion;
    motion.reserve(pairs.size());
    for (const auto& p : pairs) {
        const Vertex& va = older.vertices[older.lists[p.list][p.a].first];
        const Vertex& vb = newer.vertices[newer.lists[p.list][p.b].first];
        if (vb.x >= 0.0f && vb.x <= 640.0f && vb.y >= 0.0f && vb.y <= 480.0f)
            motion.push_back(std::hypot(vb.x - va.x, vb.y - va.y));
    }
    if (!motion.empty()) {
        std::nth_element(motion.begin(), motion.begin() + motion.size() / 2, motion.end());
        stats.median_motion = motion[motion.size() / 2];
    }
    if (stats.median_motion > opt.cut_median) {
        stats.cut = true;
        return false;
    }
    const float t = opt.t;
    std::vector<char> moved(out.vertices.size(), 0);
    for (const auto& p : pairs) {
        const Polygon& pa = older.lists[p.list][p.a];
        const Polygon& pb = out.lists[p.list][p.b];
        bool ok = true;
        for (std::uint32_t k = 0; k < pb.count && ok; ++k) {
            const Vertex& va = older.vertices[pa.first + k];
            const Vertex& vb = out.vertices[pb.first + k];
            ok = std::isfinite(va.x) && std::isfinite(va.y) && std::isfinite(va.z) &&
                 std::isfinite(vb.x) && std::isfinite(vb.y) && std::isfinite(vb.z) &&
                 std::abs(vb.x - va.x) <= opt.max_jump && std::abs(vb.y - va.y) <= opt.max_jump;
        }
        if (!ok) {
            ++stats.jumped_strips;
            continue;
        }
        for (std::uint32_t k = 0; k < pb.count; ++k) {
            const Vertex& va = older.vertices[pa.first + k];
            Vertex& vb = out.vertices[pb.first + k];
            vb.x = va.x + (vb.x - va.x) * t;
            vb.y = va.y + (vb.y - va.y) * t;
            vb.z = va.z + (vb.z - va.z) * t;
            moved[pb.first + k] = 1;
        }
        ++stats.blended_strips;
    }
    // Strips that were not blended (unpaired, UV-animated, rejected jumps) share edges with blended
    // ones of the same mesh: the shared vertices sit at the exact same position in the newer frame.
    // Give them the blended position too, or the seam opens and the background shows through
    // (Rayman 2: bright lines across the water and the cliffs).
    auto key = [&](const Vertex& v) {
        std::uint32_t b[3];
        std::memcpy(&b[0], &v.x, 4);
        std::memcpy(&b[1], &v.y, 4);
        std::memcpy(&b[2], &v.z, 4);
        return (static_cast<std::uint64_t>(b[0]) * 0x9E3779B97F4A7C15ull) ^
               (static_cast<std::uint64_t>(b[1]) * 0xC2B2AE3D27D4EB4Full) ^ b[2];
    };
    struct Seen {
        float nx, ny, nz;  // newer-frame position, to reject hash collisions
        float x, y, z;     // blended position
    };
    std::unordered_map<std::uint64_t, Seen> shared;
    shared.reserve(out.vertices.size());
    for (std::size_t i = 0; i < out.vertices.size(); ++i)
        if (moved[i]) {
            const Vertex& n = newer.vertices[i];
            const Vertex& o = out.vertices[i];
            shared.emplace(key(n), Seen{n.x, n.y, n.z, o.x, o.y, o.z});
        }
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
        if (moved[i]) continue;
        const Vertex& n = newer.vertices[i];
        const auto it = shared.find(key(n));
        if (it == shared.end() || it->second.nx != n.x || it->second.ny != n.y ||
            it->second.nz != n.z)
            continue;
        out.vertices[i].x = it->second.x;
        out.vertices[i].y = it->second.y;
        out.vertices[i].z = it->second.z;
        ++stats.welded_vertices;
    }
    // Blended depths lie between the two frames': keep the projection range covering both.
    out.min_z = std::min(older.min_z, newer.min_z);
    out.max_z = std::max(older.max_z, newer.max_z);
    return true;
}

}  // namespace dream::render
