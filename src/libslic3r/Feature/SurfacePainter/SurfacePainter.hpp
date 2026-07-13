#pragma once

#include "libslic3r/Point.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Slic3r::SurfacePainter {

struct ColorRGBA
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    bool operator==(const ColorRGBA& other) const
    {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

struct Bitmap
{
    size_t width = 0;
    size_t height = 0;
    std::vector<ColorRGBA> pixels;

    bool valid() const;
    ColorRGBA sample_nearest(double u, double v) const;
};

enum class ProjectionMode
{
    PlanarXY,
    PlanarXZ,
    PlanarYZ,
    CylindricalZ,
};

struct ProjectionSettings
{
    ProjectionMode mode = ProjectionMode::PlanarXY;
    Vec3d origin = Vec3d::Zero();
    Vec3d size = Vec3d::Ones();
    Vec3d cylinder_center = Vec3d::Zero();
    double cylinder_radius = 1.0;
    double scale_u = 1.0;
    double scale_v = 1.0;
    double offset_u = 0.0;
    double offset_v = 0.0;
    bool clamp = true;
};

struct UV
{
    double u = 0.0;
    double v = 0.0;
};

struct SurfacePoint
{
    Vec3d position = Vec3d::Zero();
    Vec3d normal = Vec3d::UnitZ();
    size_t face_id = 0;
};

enum class AssignmentKind
{
    FixedExtruder,
    VirtualExtruder,
};

struct ColorTarget
{
    ColorRGBA color;
    AssignmentKind kind = AssignmentKind::FixedExtruder;
    unsigned int id = 0;
};

struct PaintedSample
{
    size_t face_id = 0;
    UV uv;
    ColorRGBA source_color;
    size_t palette_index = 0;
    ColorTarget target;
};

UV project_point(const SurfacePoint& point, const ProjectionSettings& settings);

std::optional<size_t> nearest_palette_index(
    const ColorRGBA& color,
    const std::vector<ColorTarget>& palette,
    uint8_t alpha_threshold = 1
);

std::vector<PaintedSample> paint_surface_points(
    const std::vector<SurfacePoint>& points,
    const Bitmap& bitmap,
    const ProjectionSettings& projection,
    const std::vector<ColorTarget>& palette,
    uint8_t alpha_threshold = 1
);

} // namespace Slic3r::SurfacePainter
