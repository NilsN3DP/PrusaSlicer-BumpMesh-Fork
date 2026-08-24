// Displacement baking — port of js/displacement.js, including angle masking,
// per-face exclusion (side selection) with watertight boundary sealing,
// boundary falloff, blend-normal smoothing, symmetric/invert and overhang
// protection. Included inside namespace Slic3r::BumpMesh { namespace { ... } }.

Soup applyDisplacement(const Soup &geo, const Texture &img, const Settings &settings,
                       const Bounds &bounds, const std::vector<uint8_t> *faceExcludedIn) {
    size_t count = geo.count();
    double tmax = std::max({(double)img.width, (double)img.height, 1.0});
    double aspectU = tmax / std::max(img.width, 1);
    double aspectV = tmax / std::max(img.height, 1);

    bool needIdPositions = settings.boundary_falloff > 0.f;

    PointMap dedup(QUANTISE, std::min<size_t>(count, 1u << 22));
    std::vector<uint32_t> vertexId(count);
    int32_t nextId = 0;
    std::vector<double> idPosX, idPosY, idPosZ;
    if (needIdPositions) { idPosX.assign(count, 0); idPosY.assign(count, 0); idPosZ.assign(count, 0); }
    for (size_t i = 0; i < count; i++) {
        double x = geo.pos[i * 3], y = geo.pos[i * 3 + 1], z = geo.pos[i * 3 + 2];
        int32_t id = dedup.getOrSet(x, y, z, nextId);
        if (dedup.inserted) { if (needIdPositions) { idPosX[id] = x; idPosY[id] = y; idPosZ[id] = z; } nextId++; }
        vertexId[i] = (uint32_t)id;
    }
    size_t uniqueCount = (size_t)nextId;

    std::vector<double> snX(uniqueCount, 0), snY(uniqueCount, 0), snZ(uniqueCount, 0);
    std::vector<double> zaX(uniqueCount, 0), zaY(uniqueCount, 0), zaZ(uniqueCount, 0);
    std::vector<double> mfMasked(uniqueCount, 0), mfTotal(uniqueCount, 0);
    std::vector<double> dispVal(uniqueCount, 0);
    std::vector<uint8_t> dispSet(uniqueCount, 0);

    const std::vector<uint8_t> *fe = faceExcludedIn;
    std::vector<uint8_t> userExcludedFaces;  // per-triangle copy (kept for indexing)
    std::vector<uint8_t> excludedPos;
    if (fe) { userExcludedFaces = *fe; excludedPos.assign(uniqueCount, 0); }

    bool isCubic = settings.mapping_mode == MappingMode::Cubic;

    // ── Pass 1: smooth normals, cubic zones, angle masking, exclusion ──
    for (size_t t = 0; t < count; t += 3) {
        V3 vA{geo.pos[t * 3], geo.pos[t * 3 + 1], geo.pos[t * 3 + 2]};
        V3 vB{geo.pos[(t + 1) * 3], geo.pos[(t + 1) * 3 + 1], geo.pos[(t + 1) * 3 + 2]};
        V3 vC{geo.pos[(t + 2) * 3], geo.pos[(t + 2) * 3 + 1], geo.pos[(t + 2) * 3 + 2]};
        V3 faceNrm = (vB - vA).cross(vC - vA);
        double faceArea = faceNrm.length();
        double faceNz = faceArea > 1e-12 ? faceNrm.z / faceArea : 0;
        double faceAngle = std::acos(std::min(1.0, std::fabs(faceNz))) * (180.0 / M_PI);
        bool angleMasked = faceNz < 0
            ? (settings.bottom_angle_limit > 0 && faceAngle <= settings.bottom_angle_limit)
            : (settings.top_angle_limit > 0 && faceAngle <= settings.top_angle_limit);
        bool faceMasked = angleMasked;
        bool faceExcl = fe ? (userExcludedFaces[t / 3] != 0) : false;

        double czX = 0, czY = 0, czZ = 0;
        if (isCubic && faceArea > 1e-12) {
            V3 unit{faceNrm.x / faceArea, faceNrm.y / faceArea, faceNrm.z / faceArea};
            CW w = cubicBlendWeights(unit, settings.mapping_blend, settings.seam_band_width);
            czX = w.x * faceArea; czY = w.y * faceArea; czZ = w.z * faceArea;
        }
        for (int v = 0; v < 3; v++) {
            uint32_t vid = vertexId[t + v];
            if (faceExcl && !excludedPos.empty()) excludedPos[vid] = 1;
            V3 nrm{geo.nrm[(t + v) * 3], geo.nrm[(t + v) * 3 + 1], geo.nrm[(t + v) * 3 + 2]};
            snX[vid] += nrm.x * faceArea; snY[vid] += nrm.y * faceArea; snZ[vid] += nrm.z * faceArea;
            if (czX > 1e-12 || czY > 1e-12 || czZ > 1e-12) { zaX[vid] += czX; zaY[vid] += czY; zaZ[vid] += czZ; }
            if (faceMasked) mfMasked[vid] += faceArea;
            mfTotal[vid] += faceArea;
        }
    }

    std::vector<double> reliability(uniqueCount, 0);
    for (size_t id = 0; id < uniqueCount; id++) {
        double len = std::sqrt(snX[id] * snX[id] + snY[id] * snY[id] + snZ[id] * snZ[id]);
        double tA = mfTotal[id];
        reliability[id] = (len > 0 && tA > 0) ? len / tA : 0;
        double inv = len > 0 ? 1 / len : 1;
        snX[id] *= inv; snY[id] *= inv; snZ[id] *= inv;
    }

    // ── Pass 1.5: Laplacian-smoothed blend normal ──
    std::vector<double> bnX = snX, bnY = snY, bnZ = snZ;
    int iters = std::max(0, settings.blend_normal_smoothing);
    if (iters > 0) {
        std::vector<uint32_t> degree(uniqueCount, 0);
        auto bump = [&](uint32_t a, uint32_t b) { if (a != b) { degree[a]++; degree[b]++; } };
        for (size_t t = 0; t < count; t += 3) { uint32_t a = vertexId[t], b = vertexId[t + 1], c = vertexId[t + 2]; bump(a, b); bump(b, c); bump(c, a); }
        std::vector<uint32_t> csr(uniqueCount + 1, 0);
        for (size_t id = 0; id < uniqueCount; id++) csr[id + 1] = csr[id] + degree[id];
        std::vector<uint32_t> nbr(csr[uniqueCount]), cur(uniqueCount, 0);
        auto link = [&](uint32_t a, uint32_t b) { if (a != b) { nbr[csr[a] + cur[a]++] = b; nbr[csr[b] + cur[b]++] = a; } };
        for (size_t t = 0; t < count; t += 3) { uint32_t a = vertexId[t], b = vertexId[t + 1], c = vertexId[t + 2]; link(a, b); link(b, c); link(c, a); }
        std::vector<double> cX = snX, cY = snY, cZ = snZ, nX(uniqueCount), nY(uniqueCount), nZ(uniqueCount);
        for (int it = 0; it < iters; it++) {
            for (size_t id = 0; id < uniqueCount; id++) {
                uint32_t s = csr[id], e = csr[id + 1];
                if (e == s) { nX[id] = cX[id]; nY[id] = cY[id]; nZ[id] = cZ[id]; continue; }
                double sx = 0, sy = 0, sz = 0;
                for (uint32_t k = s; k < e; k++) { uint32_t nb = nbr[k]; sx += cX[nb]; sy += cY[nb]; sz += cZ[nb]; }
                double inv = 1.0 / (e - s); sx *= inv; sy *= inv; sz *= inv;
                double len = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (len > 1e-12) { double r = 1 / len; nX[id] = sx * r; nY[id] = sy * r; nZ[id] = sz * r; }
                else { nX[id] = cX[id]; nY[id] = cY[id]; nZ[id] = cZ[id]; }
            }
            std::swap(cX, nX); std::swap(cY, nY); std::swap(cZ, nZ);
        }
        bnX = cX; bnY = cY; bnZ = cZ;
    }

    auto apply_falloff_curve = [](double t, FalloffCurve curve) {
        t = std::clamp(t, 0.0, 1.0);
        switch (curve) {
        case FalloffCurve::SCurve:
            return t * t * (3.0 - 2.0 * t);
        case FalloffCurve::EaseIn:
            return t * t;
        case FalloffCurve::Linear:
        default:
            return t;
        }
    };

    // ── Boundary falloff distance field ──
    double boundaryFalloff = settings.boundary_falloff;
    std::vector<double> falloffArr;
    if (boundaryFalloff > 0 && needIdPositions) {
        std::vector<double> bpX, bpY, bpZ;
        double gMinX = 1e300, gMinY = 1e300, gMinZ = 1e300, gMaxX = -1e300, gMaxY = -1e300, gMaxZ = -1e300;
        for (size_t id = 0; id < uniqueCount; id++) {
            double mft = mfTotal[id]; double mf = mft > 0 ? mfMasked[id] / mft : 0;
            bool onExcl = !excludedPos.empty() && excludedPos[id] == 1;
            if (onExcl || (mf > 0 && mf < 1)) {
                double x = idPosX[id], y = idPosY[id], z = idPosZ[id];
                bpX.push_back(x); bpY.push_back(y); bpZ.push_back(z);
                gMinX = std::min(gMinX, x); gMaxX = std::max(gMaxX, x);
                gMinY = std::min(gMinY, y); gMaxY = std::max(gMaxY, y);
                gMinZ = std::min(gMinZ, z); gMaxZ = std::max(gMaxZ, z);
            }
        }
        size_t bpCount = bpX.size();
        if (bpCount > 0) {
            double gPad = boundaryFalloff + 1e-3;
            gMinX -= gPad; gMinY -= gPad; gMinZ -= gPad; gMaxX += gPad; gMaxY += gPad; gMaxZ += gPad;
            int gRes = (int)std::max(4.0, std::min(128.0, std::ceil(std::cbrt((double)bpCount) * 2)));
            double gDx = (gMaxX - gMinX) / gRes; if (gDx == 0) gDx = 1;
            double gDy = (gMaxY - gMinY) / gRes; if (gDy == 0) gDy = 1;
            double gDz = (gMaxZ - gMinZ) / gRes; if (gDz == 0) gDz = 1;
            double invDx = 1 / gDx, invDy = 1 / gDy, invDz = 1 / gDz;
            int gResMax = gRes - 1;
            size_t gridSize = (size_t)gRes * gRes * gRes;
            std::vector<uint32_t> cellCount(gridSize, 0), bpCell(bpCount);
            auto clampi = [&](int v) { return v < 0 ? 0 : (v > gResMax ? gResMax : v); };
            for (size_t i = 0; i < bpCount; i++) {
                int ix = clampi((int)((bpX[i] - gMinX) * invDx)), iy = clampi((int)((bpY[i] - gMinY) * invDy)), iz = clampi((int)((bpZ[i] - gMinZ) * invDz));
                uint32_t ck = ((uint32_t)ix * gRes + iy) * gRes + iz; bpCell[i] = ck; cellCount[ck]++;
            }
            std::vector<uint32_t> cellStart(gridSize + 1, 0);
            for (size_t c = 0; c < gridSize; c++) cellStart[c + 1] = cellStart[c] + cellCount[c];
            std::vector<uint32_t> cellCur(gridSize, 0), cellIdx(bpCount);
            for (size_t i = 0; i < bpCount; i++) { uint32_t ck = bpCell[i]; cellIdx[cellStart[ck] + cellCur[ck]++] = (uint32_t)i; }
            int searchX = (int)std::ceil(boundaryFalloff * invDx), searchY = (int)std::ceil(boundaryFalloff * invDy), searchZ = (int)std::ceil(boundaryFalloff * invDz);
            double maxDist2 = boundaryFalloff * boundaryFalloff, invFall = 1 / boundaryFalloff;
            falloffArr.assign(uniqueCount, 1.0);
            for (size_t id = 0; id < uniqueCount; id++) {
                double mft = mfTotal[id]; double mf = mft > 0 ? mfMasked[id] / mft : 0;
                bool onExcl = !excludedPos.empty() && excludedPos[id] == 1;
                if (mf > 0 || onExcl) continue;
                double px = idPosX[id], py = idPosY[id], pz = idPosZ[id];
                int cix = clampi((int)((px - gMinX) * invDx)), ciy = clampi((int)((py - gMinY) * invDy)), ciz = clampi((int)((pz - gMinZ) * invDz));
                int nixLo = std::max(0, cix - searchX), nixHi = std::min(gResMax, cix + searchX);
                int niyLo = std::max(0, ciy - searchY), niyHi = std::min(gResMax, ciy + searchY);
                int nizLo = std::max(0, ciz - searchZ), nizHi = std::min(gResMax, ciz + searchZ);
                double minD2 = maxDist2;
                for (int nix = nixLo; nix <= nixHi; nix++)
                    for (int niy = niyLo; niy <= niyHi; niy++)
                        for (int niz = nizLo; niz <= nizHi; niz++) {
                            uint32_t ck = ((uint32_t)nix * gRes + niy) * gRes + niz;
                            for (uint32_t k = cellStart[ck]; k < cellStart[ck + 1]; k++) {
                                uint32_t idx = cellIdx[k];
                                double dx = px - bpX[idx], dy = py - bpY[idx], dz = pz - bpZ[idx], d2 = dx * dx + dy * dy + dz * dz;
                                if (d2 < minD2) minD2 = d2;
                            }
                        }
                if (minD2 < maxDist2)
                    falloffArr[id] = apply_falloff_curve(std::sqrt(minD2) * invFall, settings.falloff_curve);
            }
        }
    }

    double rot = settings.rotation * M_PI / 180.0;
    double md = std::max({bounds.size.x, bounds.size.y, bounds.size.z, 1e-6});

    // ── Pass 2: sample once per unique position ──
    for (size_t i = 0; i < count; i++) {
        uint32_t vid = vertexId[i];
        if (dispSet[vid]) continue;
        dispSet[vid] = 1;
        V3 pos{geo.pos[i * 3], geo.pos[i * 3 + 1], geo.pos[i * 3 + 2]};
        if (isCubic) {
            double wX = 0, wY = 0, wZ = 0;
            if (reliability[vid] > 0.5) {
                V3 sn{bnX[vid], bnY[vid], bnZ[vid]};
                CW w = cubicBlendWeights(sn, settings.mapping_blend, settings.seam_band_width);
                wX = w.x; wY = w.y; wZ = w.z;
            } else {
                double t = zaX[vid] + zaY[vid] + zaZ[vid];
                if (t > 0) { wX = zaX[vid] / t; wY = zaY[vid] / t; wZ = zaZ[vid] / t; }
            }
            if (wX + wY + wZ > 0) {
                double grey = 0, uu, vv;
                if (wX > 0) { double rawU = (pos.y - bounds.min.y) / md; if (snX[vid] < 0) rawU = -rawU; cubicUV(rawU, (pos.z - bounds.min.z) / md, settings, rot, aspectU, aspectV, uu, vv); grey += sampleBilinear(img, uu, vv) * wX; }
                if (wY > 0) { double rawU = (pos.x - bounds.min.x) / md; if (snY[vid] > 0) rawU = -rawU; cubicUV(rawU, (pos.z - bounds.min.z) / md, settings, rot, aspectU, aspectV, uu, vv); grey += sampleBilinear(img, uu, vv) * wY; }
                if (wZ > 0) { double rawU = (pos.x - bounds.min.x) / md; if (snZ[vid] < 0) rawU = -rawU; cubicUV(rawU, (pos.y - bounds.min.y) / md, settings, rot, aspectU, aspectV, uu, vv); grey += sampleBilinear(img, uu, vv) * wZ; }
                dispVal[vid] = grey;
                continue;
            }
        }
        V3 bn{bnX[vid], bnY[vid], bnZ[vid]};
        UVResult uv = computeUV(pos, bn, settings.mapping_mode, settings, bounds, aspectU, aspectV);
        double grey;
        if (uv.triplanar) { grey = 0; for (auto &sp : uv.samples) grey += sampleBilinear(img, sp.u, sp.v) * sp.w; }
        else grey = sampleBilinear(img, uv.u, uv.v);
        dispVal[vid] = grey;
    }

    // ── Pass 3: displace ──
    Soup out;
    out.pos.resize(count * 3);
    out.nrm.resize(count * 3);
    out.face_id = geo.face_id;
    for (size_t i = 0; i < count; i++) {
        V3 pos{geo.pos[i * 3], geo.pos[i * 3 + 1], geo.pos[i * 3 + 2]};
        uint32_t vid = vertexId[i];
        double grey = dispVal[vid];
        if (settings.invert) grey = 1.0 - grey;

        bool isFaceExcl = fe ? (userExcludedFaces[i / 3] != 0) : false;
        bool isSealed = !isFaceExcl && !excludedPos.empty() && excludedPos[vid] == 1;
        double mft = mfTotal[vid]; double mf = mft > 0 ? mfMasked[vid] / mft : 0;
        double centered = settings.symmetric ? (grey - 0.5) : grey;
        double fall = falloffArr.empty() ? 1.0 : falloffArr[vid];
        double disp = (isFaceExcl || isSealed) ? 0.0 : fall * (1 - mf) * centered * settings.amplitude;

        double nx = pos.x + snX[vid] * disp, ny = pos.y + snY[vid] * disp, nz = pos.z + snZ[vid] * disp;
        if (mf > 0) {
            if (settings.bottom_angle_limit > 0 && nz < pos.z) nz = pos.z;
            if (settings.top_angle_limit > 0 && nz > pos.z) nz = pos.z;
        }
        if (settings.no_downward_z && nz < pos.z) nz = pos.z;
        if (settings.no_downward_z && pos.z <= bounds.min.z + 1e-5) nz = pos.z;
        out.pos[i * 3] = (float)nx; out.pos[i * 3 + 1] = (float)ny; out.pos[i * 3 + 2] = (float)nz;
    }

    for (size_t t = 0; t < count; t += 3) {
        V3 a{out.pos[t * 3], out.pos[t * 3 + 1], out.pos[t * 3 + 2]};
        V3 b{out.pos[(t + 1) * 3], out.pos[(t + 1) * 3 + 1], out.pos[(t + 1) * 3 + 2]};
        V3 c{out.pos[(t + 2) * 3], out.pos[(t + 2) * 3 + 1], out.pos[(t + 2) * 3 + 2]};
        V3 fn = (b - a).cross(c - a); double len = fn.length(); if (len > 0) fn = fn * (1.0 / len); else fn = {0, 0, 1};
        for (int v = 0; v < 3; v++) { out.nrm[(t + v) * 3] = (float)fn.x; out.nrm[(t + v) * 3 + 1] = (float)fn.y; out.nrm[(t + v) * 3 + 2] = (float)fn.z; }
    }
    return out;
}
