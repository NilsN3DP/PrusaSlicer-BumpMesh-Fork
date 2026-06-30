#ifndef slic3r_BumpMesh_hpp_
#define slic3r_BumpMesh_hpp_

#include <array>
#include <limits>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::BumpMesh {

// Projection modes — mirror the original stlTexturizer / BumpMesh tool.
enum class MappingMode {
    Triplanar,    // blend three planar projections by surface normal (default)
    Cubic,        // project each face from its dominant axis (crisp box faces)
    Cylindrical,  // wrap around the Z axis
    Spherical,    // wrap around a sphere
    PlanarXY,
    PlanarXZ,
    PlanarYZ,
};

// Face/side selection. A face receives displacement only if the entry for its
// dominant outward direction is enabled; vertices shared with a disabled face
// are pinned so the mesh stays watertight at the boundary.
enum FaceDir { DIR_PX = 0, DIR_NX, DIR_PY, DIR_NY, DIR_PZ, DIR_NZ, DIR_COUNT };

struct Settings {
    MappingMode mapping_mode = MappingMode::Triplanar;

    float amplitude = 1.f;       // displacement height (mm)
    float scale_u   = 1.f;       // texture scale
    float scale_v   = 1.f;
    float offset_u  = 0.f;       // texture offset (UV)
    float offset_v  = 0.f;
    float rotation  = 0.f;       // texture rotation (deg)

    float mapping_blend   = 0.f;     // cubic/cylindrical seam blend [0..1]
    float seam_band_width = 0.35f;
    float cap_angle       = 20.f;    // cylindrical cap threshold (deg)

    bool  symmetric   = true;        // grey 0.5 = neutral; else 0..1 outward
    bool  invert      = false;       // invert the height map
    bool  no_downward_z = false;     // overhang protection (never lower Z)

    float top_angle_limit    = 0.f;  // angle-based masking near +Z (0 = off)
    float bottom_angle_limit = 0.f;  // angle-based masking near -Z (0 = off)
    float boundary_falloff   = 0.f;  // fade displacement near mask boundaries (mm)
    int   blend_normal_smoothing = 0;// Laplacian smoothing of the blend normal

    // Subdivision: refine until every edge <= max_edge_length. If <= 0, it is
    // derived as (largest model dimension / detail).
    float max_edge_length = -1.f;
    float detail          = 200.f;

    // Decimation after displacement (built-in QEM). <= 0 disables.
    int target_triangles = 0;

    // Which face directions receive displacement (default: all).
    std::array<bool, DIR_COUNT> apply_dir = {true, true, true, true, true, true};

    // Cylinder overrides (NaN => derive from the bounding box).
    float cylinder_center_x = std::numeric_limits<float>::quiet_NaN();
    float cylinder_center_y = std::numeric_limits<float>::quiet_NaN();
    float cylinder_radius   = std::numeric_limits<float>::quiet_NaN();

    bool all_dirs_enabled() const {
        for (bool b : apply_dir) if (!b) return false;
        return true;
    }
};

struct Texture {
    int width = 0;
    int height = 0;
    std::vector<float> gray;  // row-major, top-left origin, 0..1

    bool valid() const { return width > 0 && height > 0 && gray.size() == size_t(width * height); }
};

indexed_triangle_set bake_displacement(const indexed_triangle_set &input, const Texture &texture, const Settings &settings);

// Sample the displacement grey value (0..1) at a single surface point with the
// given outward unit normal, using the exact projection math of
// bake_displacement. Lets the GUI preview / auto-color match the baked result.
float sample_gray(const Vec3f &pos, const Vec3f &normal, const Settings &settings,
                  const BoundingBoxf3 &bounds, const Texture &texture);

} // namespace Slic3r::BumpMesh

#endif // slic3r_BumpMesh_hpp_
