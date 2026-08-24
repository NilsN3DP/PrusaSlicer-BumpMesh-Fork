#include "BumpMesh.hpp"

#include "libslic3r/QuadricEdgeCollapse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Full port of the stlTexturizer / BumpMesh mesh-baking core. Validated to be
// bit-identical to the original JavaScript for subdivision + displacement (see
// the standalone texturizer-core harness). The pipeline runs on a non-indexed
// "triangle soup" of double-precision vertices and re-welds at the end.

namespace Slic3r::BumpMesh {
namespace {

#ifndef M_PI
constexpr double M_PI = 3.14159265358979323846;
#endif

// ── Minimal double vector ───────────────────────────────────────────────────
struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    V3 operator+(const V3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
    V3 cross(const V3 &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
};

struct Soup {
    std::vector<float> pos;  // count*3, non-indexed
    std::vector<float> nrm;  // count*3
    std::vector<uint32_t> face_id; // optional, one original face id per triangle
    size_t count() const { return pos.size() / 3; }
};

struct Bounds {
    V3 min, max, center, size;
};

// ── QuantizedPointMap (port of js/meshIndex.js) ─────────────────────────────
class PointMap {
public:
    bool inserted = false;
    explicit PointMap(double quant, size_t expected = 256) : quant_(quant) {
        size_t cap = 16, target = std::max<size_t>(16, (size_t)std::ceil(expected / 0.6));
        while (cap < target) cap *= 2;
        alloc(cap);
    }
    size_t size() const { return size_; }
    int32_t get(double x, double y, double z) const {
        return val_[slot(std::round(x * quant_), std::round(y * quant_), std::round(z * quant_))];
    }
    int32_t getOrSet(double x, double y, double z, int32_t value) {
        double qx = std::round(x * quant_), qy = std::round(y * quant_), qz = std::round(z * quant_);
        size_t i = slot(qx, qy, qz);
        if (val_[i] != -1) { inserted = false; return val_[i]; }
        qx_[i] = qx; qy_[i] = qy; qz_[i] = qz; val_[i] = value;
        inserted = true;
        if (++size_ > val_.size() * 0.7) grow();
        return value;
    }
private:
    double quant_;
    size_t size_ = 0, mask_ = 0;
    std::vector<double> qx_, qy_, qz_;
    std::vector<int32_t> val_;
    void alloc(size_t cap) { mask_ = cap - 1; qx_.assign(cap, 0); qy_.assign(cap, 0); qz_.assign(cap, 0); val_.assign(cap, -1); }
    static int32_t toInt32(double d) { double m = std::fmod(d, 4294967296.0); if (m < 0) m += 4294967296.0; return (int32_t)(uint32_t)m; }
    static int32_t imul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
    size_t slot(double qx, double qy, double qz) const {
        uint32_t h = (uint32_t)(imul(toInt32(qx), (int32_t)0x9E3779B1) ^ imul(toInt32(qy), (int32_t)0x85EBCA77) ^ imul(toInt32(qz), (int32_t)0xC2B2AE3D));
        h ^= h >> 15;
        size_t i = h & mask_;
        while (val_[i] != -1) { if (qx_[i] == qx && qy_[i] == qy && qz_[i] == qz) return i; i = (i + 1) & mask_; }
        return i;
    }
    void grow() {
        auto oqx = std::move(qx_); auto oqy = std::move(qy_); auto oqz = std::move(qz_); auto oval = std::move(val_);
        size_t ocap = oval.size(); alloc(ocap * 2);
        for (size_t i = 0; i < ocap; i++) { if (oval[i] == -1) continue; size_t s = slot(oqx[i], oqy[i], oqz[i]); qx_[s] = oqx[i]; qy_[s] = oqy[i]; qz_[s] = oqz[i]; val_[s] = oval[i]; }
    }
};

constexpr double QUANTISE = 1e5;  // 10 um grid (subdivision/displacement weld)

// ── Mapping (port of js/mapping.js) ─────────────────────────────────────────
constexpr double CUBIC_AXIS_EPSILON = 1e-4;
inline double fract(double x) { return x - std::floor(x); }
inline double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

int dominantCubicAxis(const V3 &n) {
    double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay - CUBIC_AXIS_EPSILON && ax >= az - CUBIC_AXIS_EPSILON) return 0;
    if (ay >= az - CUBIC_AXIS_EPSILON) return 1;
    return 2;
}

struct CW { double x, y, z; };
CW cubicBlendWeights(const V3 &normal, double blend, double band) {
    int axis = dominantCubicAxis(normal);
    double ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
    double primary = axis == 0 ? ax : axis == 1 ? ay : az;
    double secondary = axis == 0 ? std::max(ay, az) : axis == 1 ? std::max(ax, az) : std::max(ax, ay);
    CW oneHot{axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0};
    if (blend <= 0.001) return oneHot;
    double seamWidth = std::max(band, CUBIC_AXIS_EPSILON * 2);
    double seamMixRaw = 1.0 - clamp01((primary - secondary) / seamWidth);
    double seamMix = blend * seamMixRaw * seamMixRaw * (3.0 - 2.0 * seamMixRaw);
    if (seamMix <= 0.001) return oneHot;
    double power = 1.0 + (1.0 - seamMix) * 11.0;
    double sx = std::pow(ax, power), sy = std::pow(ay, power), sz = std::pow(az, power);
    double sm = sx + sy + sz + 1e-6;
    CW smooth{sx / sm, sy / sm, sz / sm};
    double mx = oneHot.x * (1 - seamMix) + smooth.x * seamMix;
    double my = oneHot.y * (1 - seamMix) + smooth.y * seamMix;
    double mz = oneHot.z * (1 - seamMix) + smooth.z * seamMix;
    double s = mx + my + mz;
    return {mx / s, my / s, mz / s};
}

struct UVSample { double u, v, w; };
struct UVResult { bool triplanar = false; double u = 0, v = 0; std::vector<UVSample> samples; };

UVResult applyTransform(double u, double v, double sU, double sV, double oU, double oV, double cR, double sR) {
    double uu = u / sU + oU, vv = v / sV + oV;
    if (cR != 1 || sR != 0) { uu -= 0.5; vv -= 0.5; double ru = cR * uu - sR * vv, rv = sR * uu + cR * vv; uu = ru + 0.5; vv = rv + 0.5; }
    UVResult r; r.triplanar = false; r.u = fract(uu); r.v = fract(vv); return r;
}

UVResult computeUV(const V3 &pos, const V3 &normal, MappingMode mode, const Settings &s,
                   const Bounds &b, double aspectU, double aspectV) {
    const V3 &mn = b.min; const V3 &size = b.size; const V3 &center = b.center;
    double scaleU = s.scale_u / aspectU, scaleV = s.scale_v / aspectV;
    double oU = s.offset_u, oV = s.offset_v;
    double rot = s.rotation * M_PI / 180.0, cR = std::cos(rot), sR = std::sin(rot);
    double md = std::max({size.x, size.y, size.z, 1e-6});
    const double TWO_PI = M_PI * 2.0;
    double u = 0, v = 0;

    switch (mode) {
    case MappingMode::PlanarXY: u = (pos.x - mn.x) / md; v = (pos.y - mn.y) / md; break;
    case MappingMode::PlanarXZ: u = (pos.x - mn.x) / md; v = (pos.z - mn.z) / md; break;
    case MappingMode::PlanarYZ: u = (pos.y - mn.y) / md; v = (pos.z - mn.z) / md; break;

    case MappingMode::Cylindrical: {
        double cx = std::isnan(s.cylinder_center_x) ? center.x : s.cylinder_center_x;
        double cy = std::isnan(s.cylinder_center_y) ? center.y : s.cylinder_center_y;
        double r = std::max(std::isnan(s.cylinder_radius) ? std::max(size.x, size.y) * 0.5 : (double)s.cylinder_radius, 1e-6);
        double C = TWO_PI * r, rx = pos.x - cx, ry = pos.y - cy, blend = s.mapping_blend;
        double theta = std::atan2(ry, rx), uRaw = theta / TWO_PI + 0.5, vSide = (pos.z - mn.z) / C;
        double seamBand = s.seam_band_width * 0.1, seamDist = std::min(uRaw, 1.0 - uRaw);
        bool inSeam = seamBand > 0.001 && seamDist < seamBand;
        std::vector<UVSample> side;
        if (inSeam) {
            double d = uRaw < 0.5 ? uRaw : uRaw - 1.0, tRaw = (d + seamBand) / (2.0 * seamBand), t = tRaw * tRaw * (3 - 2 * tRaw);
            UVResult tL = applyTransform(1.0 + d, vSide, scaleU, scaleV, oU, oV, cR, sR);
            UVResult tR = applyTransform(d, vSide, scaleU, scaleV, oU, oV, cR, sR);
            side.push_back({tR.u, tR.v, t}); side.push_back({tL.u, tL.v, 1 - t});
        } else { UVResult tS = applyTransform(uRaw, vSide, scaleU, scaleV, oU, oV, cR, sR); side.push_back({tS.u, tS.v, 1}); }
        auto sideRes = [&]() { UVResult r; if (side.size() == 1 && side[0].w == 1) { r.u = side[0].u; r.v = side[0].v; } else { r.triplanar = true; r.samples = side; } return r; };
        if (blend <= 0.001) return sideRes();
        double capThresh = std::cos(s.cap_angle * M_PI / 180.0), blendHalf = s.seam_band_width * 0.5, absnz = std::fabs(normal.z);
        double capW = clamp01((absnz - (capThresh - blendHalf)) / (2 * blendHalf + 1e-6));
        if (capW <= 0) return sideRes();
        UVResult tCap = applyTransform(rx / C + 0.5, ry / C + 0.5, scaleU, scaleV, oU, oV, cR, sR);
        if (capW >= 1) return tCap;
        UVResult res; res.triplanar = true;
        for (auto &sp : side) res.samples.push_back({sp.u, sp.v, sp.w * (1 - capW)});
        res.samples.push_back({tCap.u, tCap.v, capW});
        return res;
    }

    case MappingMode::Spherical: {
        double rx = pos.x - center.x, ry = pos.y - center.y, rz = pos.z - center.z;
        double r = std::sqrt(rx * rx + ry * ry + rz * rz);
        double phi = std::acos(std::max(-1.0, std::min(1.0, rz / std::max(r, 1e-6))));
        double theta = std::atan2(ry, rx), uRaw = theta / TWO_PI + 0.5, vRaw = phi / M_PI;
        double seamBand = s.seam_band_width * 0.1, seamDist = std::min(uRaw, 1.0 - uRaw);
        if (seamBand > 0.001 && seamDist < seamBand) {
            double d = uRaw < 0.5 ? uRaw : uRaw - 1.0, tRaw = (d + seamBand) / (2.0 * seamBand), t = tRaw * tRaw * (3 - 2 * tRaw);
            UVResult tL = applyTransform(1.0 + d, vRaw, scaleU, scaleV, oU, oV, cR, sR);
            UVResult tR = applyTransform(d, vRaw, scaleU, scaleV, oU, oV, cR, sR);
            UVResult r; r.triplanar = true; r.samples.push_back({tR.u, tR.v, t}); r.samples.push_back({tL.u, tL.v, 1 - t}); return r;
        }
        u = uRaw; v = vRaw; break;
    }

    case MappingMode::Cubic: {
        CW w = cubicBlendWeights(normal, s.mapping_blend, s.seam_band_width);
        double yzU = (pos.y - mn.y) / md; if (normal.x < 0) yzU = -yzU;
        double xzU = (pos.x - mn.x) / md; if (normal.y > 0) xzU = -xzU;
        double xyU = (pos.x - mn.x) / md; if (normal.z < 0) xyU = -xyU;
        UVResult tYZ = applyTransform(yzU, (pos.z - mn.z) / md, scaleU, scaleV, oU, oV, cR, sR);
        UVResult tXZ = applyTransform(xzU, (pos.z - mn.z) / md, scaleU, scaleV, oU, oV, cR, sR);
        UVResult tXY = applyTransform(xyU, (pos.y - mn.y) / md, scaleU, scaleV, oU, oV, cR, sR);
        if (w.x > 0.999) return tYZ;
        if (w.y > 0.999) return tXZ;
        if (w.z > 0.999) return tXY;
        UVResult r; r.triplanar = true;
        r.samples.push_back({tXY.u, tXY.v, w.z});
        r.samples.push_back({tXZ.u, tXZ.v, w.y});
        r.samples.push_back({tYZ.u, tYZ.v, w.x});
        return r;
    }

    case MappingMode::Triplanar:
    default: {
        double ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
        double bx = ax * ax * ax * ax, by = ay * ay * ay * ay, bz = az * az * az * az, sum = bx + by + bz + 1e-6;
        double wx = bx / sum, wy = by / sum, wz = bz / sum;
        double yzU = (pos.y - mn.y) / md; if (normal.x < 0) yzU = -yzU;
        double xzU = (pos.x - mn.x) / md; if (normal.y > 0) xzU = -xzU;
        double xyU = (pos.x - mn.x) / md; if (normal.z < 0) xyU = -xyU;
        auto T = [&](double uu, double vv, double w) { UVResult t = applyTransform(uu, vv, scaleU, scaleV, oU, oV, cR, sR); return UVSample{t.u, t.v, w}; };
        UVResult r; r.triplanar = true;
        r.samples.push_back(T(xyU, (pos.y - mn.y) / md, wz));
        r.samples.push_back(T(xzU, (pos.z - mn.z) / md, wy));
        r.samples.push_back(T(yzU, (pos.z - mn.z) / md, wx));
        return r;
    }
    }
    return applyTransform(u, v, scaleU, scaleV, oU, oV, cR, sR);
}

// ── Texture sampling ────────────────────────────────────────────────────────
double sampleBilinear(const Texture &t, double u, double v) {
    int w = t.width, h = t.height;
    u = std::fmod(u, 1.0); if (u < 0) u += 1.0;
    v = std::fmod(v, 1.0); if (v < 0) v += 1.0;
    v = 1.0 - v;
    double fx = u * w - 0.5, fy = v * h - 0.5;
    long x0 = (long)std::floor(fx), y0 = (long)std::floor(fy);
    double tx = fx - x0, ty = fy - y0;
    long x1 = ((x0 + 1) % w + w) % w, y1 = ((y0 + 1) % h + h) % h;
    x0 = ((x0 % w) + w) % w; y0 = ((y0 % h) + h) % h;
    const auto &g = t.gray;
    double v00 = g[y0 * w + x0], v10 = g[y0 * w + x1], v01 = g[y1 * w + x0], v11 = g[y1 * w + x1];
    return v00 * (1 - tx) * (1 - ty) + v10 * tx * (1 - ty) + v01 * (1 - tx) * ty + v11 * tx * ty;
}

void cubicUV(double rawU, double rawV, const Settings &s, double rot, double aU, double aV, double &outU, double &outV) {
    double u = rawU * aU / s.scale_u + s.offset_u, v = rawV * aV / s.scale_v + s.offset_v;
    if (rot != 0) { double c = std::cos(rot), si = std::sin(rot); u -= 0.5; v -= 0.5; double ru = c * u - si * v, rv = si * u + c * v; u = ru + 0.5; v = rv + 0.5; }
    outU = u - std::floor(u); outV = v - std::floor(v);
}

#include "BumpMeshSubdiv.inc"
#include "BumpMeshDisplace.inc"

// ── indexed_triangle_set <-> Soup adapters ──────────────────────────────────
Soup soupFromITS(const indexed_triangle_set &its) {
    Soup s;
    size_t nf = its.indices.size();
    s.pos.resize(nf * 9);
    s.nrm.resize(nf * 9);
    s.face_id.resize(nf);
    for (size_t f = 0; f < nf; f++) {
        s.face_id[f] = uint32_t(f);
        const auto &face = its.indices[f];
        V3 p[3];
        for (int k = 0; k < 3; k++) {
            const Vec3f &vtx = its.vertices[face[k]];
            s.pos[f * 9 + k * 3] = vtx.x(); s.pos[f * 9 + k * 3 + 1] = vtx.y(); s.pos[f * 9 + k * 3 + 2] = vtx.z();
            p[k] = {vtx.x(), vtx.y(), vtx.z()};
        }
        V3 n = (p[1] - p[0]).cross(p[2] - p[0]);
        double len = n.length(); if (len > 0) n = n * (1.0 / len); else n = {0, 0, 1};
        for (int k = 0; k < 3; k++) { s.nrm[f * 9 + k * 3] = (float)n.x; s.nrm[f * 9 + k * 3 + 1] = (float)n.y; s.nrm[f * 9 + k * 3 + 2] = (float)n.z; }
    }
    return s;
}

indexed_triangle_set itsFromSoup(const Soup &s, std::vector<uint32_t> *source_face_ids = nullptr) {
    indexed_triangle_set out;
    size_t n = s.count();
    PointMap map(QUANTISE, std::min<size_t>(n, 1u << 22));
    int next = 0;
    std::vector<int> id(n);
    for (size_t i = 0; i < n; i++) {
        double x = s.pos[i * 3], y = s.pos[i * 3 + 1], z = s.pos[i * 3 + 2];
        int vid = map.getOrSet(x, y, z, next);
        if (map.inserted) { out.vertices.emplace_back(Vec3f((float)x, (float)y, (float)z)); next++; }
        id[i] = vid;
    }
    out.indices.reserve(n / 3);
    for (size_t f = 0; f < n / 3; f++) {
        int a = id[f * 3], b = id[f * 3 + 1], c = id[f * 3 + 2];
        if (a == b || b == c || a == c) continue;  // drop degenerate
        out.indices.emplace_back(stl_triangle_vertex_indices{a, b, c});
        if (source_face_ids != nullptr)
            source_face_ids->push_back(f < s.face_id.size() ? s.face_id[f] : uint32_t(f));
    }
    return out;
}

Bounds boundsOf(const indexed_triangle_set &its) {
    BoundingBoxf3 bb = bounding_box(its);
    Bounds b;
    b.min = {bb.min.x(), bb.min.y(), bb.min.z()};
    b.max = {bb.max.x(), bb.max.y(), bb.max.z()};
    b.size = b.max - b.min;
    b.center = b.min + b.size * 0.5;
    return b;
}

int faceDir(const V3 &n) {
    double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az) return n.x >= 0 ? DIR_PX : DIR_NX;
    if (ay >= az) return n.y >= 0 ? DIR_PY : DIR_NY;
    return n.z >= 0 ? DIR_PZ : DIR_NZ;
}

V3 faceNormal(const indexed_triangle_set &its, size_t face_idx)
{
    const stl_triangle_vertex_indices &face = its.indices[face_idx];
    const Vec3f &a = its.vertices[face[0]];
    const Vec3f &b = its.vertices[face[1]];
    const Vec3f &c = its.vertices[face[2]];
    V3 n{double((b - a).cross(c - a).x()), double((b - a).cross(c - a).y()), double((b - a).cross(c - a).z())};
    const double len = n.length();
    return len > 1e-12 ? n * (1.0 / len) : V3{0, 0, 1};
}

} // namespace

std::vector<std::vector<size_t>> build_face_adjacency(const indexed_triangle_set &input)
{
    std::vector<std::vector<size_t>> adjacency(input.indices.size());
    if (input.indices.empty())
        return adjacency;

    struct EdgeRef { size_t face_idx; int a; int b; };
    struct EdgeKey {
        int a;
        int b;
        bool operator==(const EdgeKey &rhs) const { return a == rhs.a && b == rhs.b; }
    };
    struct EdgeKeyHash {
        size_t operator()(const EdgeKey &key) const {
            return (size_t(uint32_t(key.a)) * 73856093u) ^ (size_t(uint32_t(key.b)) * 19349663u);
        }
    };

    std::unordered_map<EdgeKey, EdgeRef, EdgeKeyHash> edge_to_face;
    auto add_edge = [&](size_t face_idx, int va, int vb) {
        EdgeKey key{std::min(va, vb), std::max(va, vb)};
        auto it = edge_to_face.find(key);
        if (it == edge_to_face.end()) {
            edge_to_face.emplace(key, EdgeRef{face_idx, va, vb});
            return;
        }

        const size_t other = it->second.face_idx;
        if (other != face_idx) {
            adjacency[face_idx].push_back(other);
            adjacency[other].push_back(face_idx);
        }
    };

    for (size_t face_idx = 0; face_idx < input.indices.size(); ++face_idx) {
        const stl_triangle_vertex_indices &face = input.indices[face_idx];
        add_edge(face_idx, face[0], face[1]);
        add_edge(face_idx, face[1], face[2]);
        add_edge(face_idx, face[2], face[0]);
    }

    for (std::vector<size_t> &neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    return adjacency;
}

std::vector<uint8_t> bucket_fill_faces(const indexed_triangle_set &input,
                                       const std::vector<std::vector<size_t>> &adjacency,
                                       size_t start_face,
                                       float max_dihedral_angle_deg)
{
    std::vector<uint8_t> selected(input.indices.size(), 0);
    if (start_face >= input.indices.size() || adjacency.size() != input.indices.size())
        return selected;

    const double min_dot = std::cos(std::max(0.f, max_dihedral_angle_deg) * M_PI / 180.0);
    std::vector<V3> normals(input.indices.size());
    for (size_t face_idx = 0; face_idx < input.indices.size(); ++face_idx)
        normals[face_idx] = faceNormal(input, face_idx);

    std::queue<size_t> queue;
    selected[start_face] = 1;
    queue.push(start_face);

    while (!queue.empty()) {
        const size_t face_idx = queue.front();
        queue.pop();

        for (size_t next : adjacency[face_idx]) {
            if (next >= input.indices.size() || selected[next])
                continue;
            if (normals[face_idx].dot(normals[next]) < min_dot)
                continue;
            selected[next] = 1;
            queue.push(next);
        }
    }

    return selected;
}

BakeResult bake_displacement_with_face_ids(const indexed_triangle_set &input, const Texture &texture, const Settings &settings) {
    if (!texture.valid())
        throw std::invalid_argument("BumpMesh texture dimensions do not match grayscale data");
    if (input.indices.empty() || input.vertices.empty())
        return {input, {}};

    Bounds bounds = boundsOf(input);
    double maxDim = std::max({bounds.size.x, bounds.size.y, bounds.size.z});
    double maxEdge = settings.max_edge_length > 0.f ? settings.max_edge_length
                                                    : maxDim / std::max(1.f, settings.detail);

    Soup soup = soupFromITS(input);
    Soup sub = subdivide(soup, maxEdge);

    // Per-(subdivided)-face side and painted-face mask from the geometric face
    // direction plus the propagated original face id.
    std::vector<uint8_t> faceExcluded;
    if (!settings.all_dirs_enabled() || settings.face_mask_mode != FaceMaskMode::None ||
        !settings.include_face_mask.empty() || !settings.exclude_face_mask.empty()) {
        size_t tri = sub.count() / 3;
        faceExcluded.assign(tri, 0);
        for (size_t t = 0; t < tri; t++) {
            V3 a{sub.pos[t * 9], sub.pos[t * 9 + 1], sub.pos[t * 9 + 2]};
            V3 b{sub.pos[t * 9 + 3], sub.pos[t * 9 + 4], sub.pos[t * 9 + 5]};
            V3 c{sub.pos[t * 9 + 6], sub.pos[t * 9 + 7], sub.pos[t * 9 + 8]};
            V3 n = (b - a).cross(c - a);
            bool excluded = !settings.apply_dir[faceDir(n)];
            if (settings.face_mask_mode != FaceMaskMode::None) {
                const uint32_t original_face = t < sub.face_id.size() ? sub.face_id[t] : uint32_t(t);
                const bool marked = original_face < settings.face_mask.size() && settings.face_mask[original_face] != 0;
                if (settings.face_mask_mode == FaceMaskMode::Exclude)
                    excluded = excluded || marked;
                else if (settings.face_mask_mode == FaceMaskMode::IncludeOnly)
                    excluded = excluded || !marked;
            }
            const uint32_t original_face = t < sub.face_id.size() ? sub.face_id[t] : uint32_t(t);
            if (!settings.include_face_mask.empty()) {
                const bool included = original_face < settings.include_face_mask.size() && settings.include_face_mask[original_face] != 0;
                excluded = excluded || !included;
            }
            if (!settings.exclude_face_mask.empty()) {
                const bool explicitly_excluded = original_face < settings.exclude_face_mask.size() && settings.exclude_face_mask[original_face] != 0;
                excluded = excluded || explicitly_excluded;
            }
            if (excluded)
                faceExcluded[t] = 1;
        }
    }

    Soup displaced = applyDisplacement(sub, texture, settings, bounds,
                                       faceExcluded.empty() ? nullptr : &faceExcluded);

    BakeResult result;
    result.mesh = itsFromSoup(displaced, &result.source_face_ids);

    if (settings.target_triangles > 0 && (int)result.mesh.indices.size() > settings.target_triangles) {
        its_quadric_edge_collapse(result.mesh, (uint32_t)settings.target_triangles);
        result.source_face_ids.clear();
    }

    return result;
}

indexed_triangle_set bake_displacement(const indexed_triangle_set &input, const Texture &texture, const Settings &settings) {
    return bake_displacement_with_face_ids(input, texture, settings).mesh;
}

float sample_gray(const Vec3f &posf, const Vec3f &nf, const Settings &s,
                  const BoundingBoxf3 &bb, const Texture &tex) {
    if (!tex.valid())
        return 0.5f;
    V3 pos{posf.x(), posf.y(), posf.z()};
    V3 n{nf.x(), nf.y(), nf.z()};
    Bounds b;
    b.min = {bb.min.x(), bb.min.y(), bb.min.z()};
    b.max = {bb.max.x(), bb.max.y(), bb.max.z()};
    b.size = b.max - b.min;
    b.center = b.min + b.size * 0.5;

    double tmax = std::max({(double)tex.width, (double)tex.height, 1.0});
    double aU = tmax / std::max(tex.width, 1);
    double aV = tmax / std::max(tex.height, 1);
    double rot = s.rotation * M_PI / 180.0;
    double md = std::max({b.size.x, b.size.y, b.size.z, 1e-6});

    // Cubic samples each contributing axis plane exactly like displacement Pass 2,
    // using the supplied (face) normal both for the blend weights and the U-flip.
    if (s.mapping_mode == MappingMode::Cubic) {
        CW w = cubicBlendWeights(n, s.mapping_blend, s.seam_band_width);
        if (w.x + w.y + w.z > 0) {
            double grey = 0, uu, vv;
            if (w.x > 0) { double rawU = (pos.y - b.min.y) / md; if (n.x < 0) rawU = -rawU; cubicUV(rawU, (pos.z - b.min.z) / md, s, rot, aU, aV, uu, vv); grey += sampleBilinear(tex, uu, vv) * w.x; }
            if (w.y > 0) { double rawU = (pos.x - b.min.x) / md; if (n.y > 0) rawU = -rawU; cubicUV(rawU, (pos.z - b.min.z) / md, s, rot, aU, aV, uu, vv); grey += sampleBilinear(tex, uu, vv) * w.y; }
            if (w.z > 0) { double rawU = (pos.x - b.min.x) / md; if (n.z < 0) rawU = -rawU; cubicUV(rawU, (pos.y - b.min.y) / md, s, rot, aU, aV, uu, vv); grey += sampleBilinear(tex, uu, vv) * w.z; }
            return (float)grey;
        }
    }

    UVResult uv = computeUV(pos, n, s.mapping_mode, s, b, aU, aV);
    if (uv.triplanar) {
        double grey = 0;
        for (auto &sp : uv.samples) grey += sampleBilinear(tex, sp.u, sp.v) * sp.w;
        return (float)grey;
    }
    return (float)sampleBilinear(tex, uv.u, uv.v);
}

} // namespace Slic3r::BumpMesh
