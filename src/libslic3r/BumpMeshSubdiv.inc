// Adaptive subdivision — port of js/subdivision.js (accurate/export path).
// Included inside namespace Slic3r::BumpMesh { namespace { ... } }; relies on
// V3, Soup, PointMap, QUANTISE defined in BumpMesh.cpp.

namespace subdiv_detail {

constexpr size_t SAFETY_CAP = 16'000'000;
const double SHARP_COS = std::cos(30.0 * M_PI / 180.0);

struct VertStore {
    std::vector<double> pos, nrm;
    std::vector<int32_t> canon;
    size_t count = 0;
    size_t addVertex(double x, double y, double z, double nx, double ny, double nz, int32_t c) {
        size_t idx = count;
        pos.push_back(x); pos.push_back(y); pos.push_back(z);
        nrm.push_back(nx); nrm.push_back(ny); nrm.push_back(nz);
        canon.push_back(c); count++;
        return idx;
    }
};

inline double edgeLenSq(const double *p, uint32_t a, uint32_t b) {
    double dx = p[a * 3] - p[b * 3], dy = p[a * 3 + 1] - p[b * 3 + 1], dz = p[a * 3 + 2] - p[b * 3 + 2];
    return dx * dx + dy * dy + dz * dz;
}

void normalizeStoreNormals(VertStore &v) {
    for (size_t i = 0; i < v.count; i++) {
        double nx = v.nrm[i * 3], ny = v.nrm[i * 3 + 1], nz = v.nrm[i * 3 + 2];
        double len = std::sqrt(nx * nx + ny * ny + nz * nz); if (len == 0) len = 1;
        v.nrm[i * 3] = nx / len; v.nrm[i * 3 + 1] = ny / len; v.nrm[i * 3 + 2] = nz / len;
    }
}

struct Cluster { int32_t idx; double fnU[3]; };

PointMap toIndexed(const Soup &geo, VertStore &verts, std::vector<uint32_t> &indices) {
    size_t n = geo.count();
    indices.resize(n);
    std::vector<float> fnU(n * 3), fnR(n * 3);
    for (size_t t = 0; t < n; t += 3) {
        double ax = geo.pos[t * 3], ay = geo.pos[t * 3 + 1], az = geo.pos[t * 3 + 2];
        double bx = geo.pos[(t + 1) * 3], by = geo.pos[(t + 1) * 3 + 1], bz = geo.pos[(t + 1) * 3 + 2];
        double cx = geo.pos[(t + 2) * 3], cy = geo.pos[(t + 2) * 3 + 1], cz = geo.pos[(t + 2) * 3 + 2];
        double e1x = bx - ax, e1y = by - ay, e1z = bz - az, e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        double rx = e1y * e2z - e1z * e2y, ry = e1z * e2x - e1x * e2z, rz = e1x * e2y - e1y * e2x;
        double len = std::sqrt(rx * rx + ry * ry + rz * rz); if (len == 0) len = 1;
        double ux = rx / len, uy = ry / len, uz = rz / len;
        for (int v = 0; v < 3; v++) {
            fnU[(t + v) * 3] = (float)ux; fnU[(t + v) * 3 + 1] = (float)uy; fnU[(t + v) * 3 + 2] = (float)uz;
            fnR[(t + v) * 3] = (float)rx; fnR[(t + v) * 3 + 1] = (float)ry; fnR[(t + v) * 3 + 2] = (float)rz;
        }
    }
    PointMap posCanonMap(QUANTISE, std::min<size_t>(n, 1u << 22));
    std::unordered_map<int32_t, std::vector<Cluster>> clustersByCanon;
    for (size_t i = 0; i < n; i++) {
        double px = geo.pos[i * 3], py = geo.pos[i * 3 + 1], pz = geo.pos[i * 3 + 2];
        double fnUx = fnU[i * 3], fnUy = fnU[i * 3 + 1], fnUz = fnU[i * 3 + 2];
        double fnRx = fnR[i * 3], fnRy = fnR[i * 3 + 1], fnRz = fnR[i * 3 + 2];
        int32_t canonId = posCanonMap.getOrSet(px, py, pz, (int32_t)verts.count);
        if (!posCanonMap.inserted) {
            auto &clusters = clustersByCanon[canonId];
            bool matched = false;
            for (auto &cl : clusters) {
                double dot = cl.fnU[0] * fnUx + cl.fnU[1] * fnUy + cl.fnU[2] * fnUz;
                if (dot >= SHARP_COS) {
                    int32_t idx = cl.idx;
                    verts.nrm[idx * 3] += fnRx; verts.nrm[idx * 3 + 1] += fnRy; verts.nrm[idx * 3 + 2] += fnRz;
                    cl.fnU[0] += fnUx; cl.fnU[1] += fnUy; cl.fnU[2] += fnUz;
                    double rl = std::sqrt(cl.fnU[0] * cl.fnU[0] + cl.fnU[1] * cl.fnU[1] + cl.fnU[2] * cl.fnU[2]); if (rl == 0) rl = 1;
                    cl.fnU[0] /= rl; cl.fnU[1] /= rl; cl.fnU[2] /= rl;
                    indices[i] = (uint32_t)idx; matched = true; break;
                }
            }
            if (!matched) {
                int32_t idx = (int32_t)verts.addVertex(px, py, pz, fnRx, fnRy, fnRz, canonId);
                clusters.push_back({idx, {fnUx, fnUy, fnUz}});
                indices[i] = (uint32_t)idx;
            }
        } else {
            int32_t idx = (int32_t)verts.addVertex(px, py, pz, fnRx, fnRy, fnRz, canonId);
            clustersByCanon[canonId] = {{idx, {fnUx, fnUy, fnUz}}};
            indices[i] = (uint32_t)idx;
        }
    }
    normalizeStoreNormals(verts);
    return posCanonMap;
}

uint32_t getMidpoint(VertStore &verts, PointMap &cache, PointMap &posCanonMap, uint32_t a, uint32_t b) {
    uint32_t lo = a < b ? a : b, hi = a < b ? b : a;
    int32_t cached = cache.get(lo, hi, 0);
    if (cached != -1) return (uint32_t)cached;
    double mx = (verts.pos[a * 3] + verts.pos[b * 3]) / 2;
    double my = (verts.pos[a * 3 + 1] + verts.pos[b * 3 + 1]) / 2;
    double mz = (verts.pos[a * 3 + 2] + verts.pos[b * 3 + 2]) / 2;
    double nx = verts.nrm[a * 3] + verts.nrm[b * 3], ny = verts.nrm[a * 3 + 1] + verts.nrm[b * 3 + 1], nz = verts.nrm[a * 3 + 2] + verts.nrm[b * 3 + 2];
    double nl = std::sqrt(nx * nx + ny * ny + nz * nz); if (nl == 0) nl = 1;
    int32_t idx = (int32_t)verts.addVertex(mx, my, mz, nx / nl, ny / nl, nz / nl, 0);
    verts.canon[idx] = posCanonMap.getOrSet(mx, my, mz, idx);
    cache.getOrSet(lo, hi, 0, idx);
    return (uint32_t)idx;
}

struct PassResult {
    std::vector<uint32_t> newIndices;
    std::vector<uint32_t> newFaceIds;
    bool changed = false;
    bool capped = false;
};

PassResult subdividePass(VertStore &verts, const std::vector<uint32_t> &indices,
                         const std::vector<uint32_t> &faceIds, double maxEdge,
                         PointMap &posCanonMap) {
    double maxSq = maxEdge * maxEdge;
    PointMap midCache(1, 1u << 16), splitEdges(1, 1u << 16);
    auto markEdge = [&](uint32_t a, uint32_t b) { int32_t u = verts.canon[a], v = verts.canon[b]; if (u < v) splitEdges.getOrSet(u, v, 0, 1); else splitEdges.getOrSet(v, u, 0, 1); };
    auto isMarked = [&](uint32_t a, uint32_t b) -> bool { int32_t u = verts.canon[a], v = verts.canon[b]; return (u < v ? splitEdges.get(u, v, 0) : splitEdges.get(v, u, 0)) != -1; };

    {
        const double *P = verts.pos.data();
        for (size_t t = 0; t < indices.size(); t += 3) {
            uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
            if (edgeLenSq(P, a, b) > maxSq) markEdge(a, b);
            if (edgeLenSq(P, b, c) > maxSq) markEdge(b, c);
            if (edgeLenSq(P, c, a) > maxSq) markEdge(c, a);
        }
    }
    PassResult out;
    if (splitEdges.size() == 0) { out.newIndices = indices; out.newFaceIds = faceIds; return out; }

    size_t predicted = 0;
    for (size_t t = 0; t < indices.size(); t += 3) {
        uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        int n = (isMarked(a, b) ? 1 : 0) + (isMarked(b, c) ? 1 : 0) + (isMarked(c, a) ? 1 : 0);
        predicted += n == 0 ? 1 : n + 1;
    }
    if (predicted > SAFETY_CAP) { out.newIndices = indices; out.newFaceIds = faceIds; out.capped = true; return out; }

    std::vector<uint32_t> &ni = out.newIndices;
    std::vector<uint32_t> &nf = out.newFaceIds;
    ni.reserve(predicted * 3);
    nf.reserve(predicted);
    auto p3 = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t faceId) {
        ni.push_back(x); ni.push_back(y); ni.push_back(z);
        nf.push_back(faceId);
    };
    for (size_t t = 0; t < indices.size(); t += 3) {
        const uint32_t faceId = (t / 3) < faceIds.size() ? faceIds[t / 3] : uint32_t(t / 3);
        uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        bool sAB = isMarked(a, b), sBC = isMarked(b, c), sCA = isMarked(c, a);
        int n = (sAB ? 1 : 0) + (sBC ? 1 : 0) + (sCA ? 1 : 0);
        if (n == 0) { p3(a, b, c, faceId); }
        else if (n == 3) {
            uint32_t mAB = getMidpoint(verts, midCache, posCanonMap, a, b), mBC = getMidpoint(verts, midCache, posCanonMap, b, c), mCA = getMidpoint(verts, midCache, posCanonMap, c, a);
            p3(a, mAB, mCA, faceId); p3(mAB, b, mBC, faceId); p3(mCA, mBC, c, faceId); p3(mAB, mBC, mCA, faceId);
        } else if (n == 1) {
            if (sAB) { uint32_t m = getMidpoint(verts, midCache, posCanonMap, a, b); p3(a, m, c, faceId); p3(m, b, c, faceId); }
            else if (sBC) { uint32_t m = getMidpoint(verts, midCache, posCanonMap, b, c); p3(a, b, m, faceId); p3(a, m, c, faceId); }
            else { uint32_t m = getMidpoint(verts, midCache, posCanonMap, c, a); p3(a, b, m, faceId); p3(m, b, c, faceId); }
        } else {
            if (!sAB) {
                uint32_t mBC = getMidpoint(verts, midCache, posCanonMap, b, c), mCA = getMidpoint(verts, midCache, posCanonMap, c, a);
                p3(a, b, mBC, faceId); p3(a, mBC, mCA, faceId); p3(c, mCA, mBC, faceId);
            } else if (!sBC) {
                uint32_t mAB = getMidpoint(verts, midCache, posCanonMap, a, b), mCA = getMidpoint(verts, midCache, posCanonMap, c, a);
                p3(a, mAB, mCA, faceId); p3(mAB, b, c, faceId); p3(mAB, c, mCA, faceId);
            } else {
                uint32_t mAB = getMidpoint(verts, midCache, posCanonMap, a, b), mBC = getMidpoint(verts, midCache, posCanonMap, b, c);
                p3(b, mBC, mAB, faceId); p3(a, mAB, mBC, faceId); p3(a, mBC, c, faceId);
            }
        }
    }
    out.changed = true;
    return out;
}

Soup toNonIndexed(const VertStore &verts, const std::vector<uint32_t> &indices, const std::vector<uint32_t> &faceIds) {
    Soup m;
    size_t tri = indices.size() / 3;
    m.pos.resize(tri * 9); m.nrm.resize(tri * 9);
    m.face_id = faceIds;
    for (size_t t = 0; t < tri; t++)
        for (int v = 0; v < 3; v++) {
            uint32_t vid = indices[t * 3 + v];
            m.pos[t * 9 + v * 3] = (float)verts.pos[vid * 3]; m.pos[t * 9 + v * 3 + 1] = (float)verts.pos[vid * 3 + 1]; m.pos[t * 9 + v * 3 + 2] = (float)verts.pos[vid * 3 + 2];
            m.nrm[t * 9 + v * 3] = (float)verts.nrm[vid * 3]; m.nrm[t * 9 + v * 3 + 1] = (float)verts.nrm[vid * 3 + 1]; m.nrm[t * 9 + v * 3 + 2] = (float)verts.nrm[vid * 3 + 2];
        }
    return m;
}

} // namespace subdiv_detail

Soup subdivide(const Soup &in, double maxEdgeLength) {
    using namespace subdiv_detail;
    VertStore verts;
    std::vector<uint32_t> indices;
    PointMap posCanonMap = toIndexed(in, verts, indices);
    std::vector<uint32_t> cur = indices;
    std::vector<uint32_t> curFaceIds = in.face_id;
    if (curFaceIds.size() != cur.size() / 3) {
        curFaceIds.resize(cur.size() / 3);
        for (size_t i = 0; i < curFaceIds.size(); ++i)
            curFaceIds[i] = uint32_t(i);
    }
    for (int iter = 0; iter < 12; iter++) {
        if (cur.size() / 3 >= SAFETY_CAP) break;
        PassResult res = subdividePass(verts, cur, curFaceIds, maxEdgeLength, posCanonMap);
        bool stop = !res.changed || res.capped;
        cur = std::move(res.newIndices);
        curFaceIds = std::move(res.newFaceIds);
        if (stop) break;
    }
    return toNonIndexed(verts, cur, curFaceIds);
}
