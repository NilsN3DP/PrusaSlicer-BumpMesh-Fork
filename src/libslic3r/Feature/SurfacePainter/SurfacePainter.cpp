#include "SurfacePainter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::SurfacePainter {
namespace {

double normalize_component(double value, double origin, double size)
{
    if (std::abs(size) < EPSILON)
        return 0.0;
    return (value - origin) / size;
}

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double wrap01(double value)
{
    value = std::fmod(value, 1.0);
    return value < 0.0 ? value + 1.0 : value;
}

double resolve_uv_component(double value, bool clamp)
{
    return clamp ? clamp01(value) : wrap01(value);
}

double apply_uv_transform(double value, double scale, double offset)
{
    const double safe_scale = std::abs(scale) < EPSILON ? 1.0 : scale;
    return (value - offset) / safe_scale;
}

int color_distance_sq(const ColorRGBA& a, const ColorRGBA& b)
{
    const int dr = int(a.r) - int(b.r);
    const int dg = int(a.g) - int(b.g);
    const int db = int(a.b) - int(b.b);
    return dr * dr + dg * dg + db * db;
}

} // namespace

bool Bitmap::valid() const
{
    return width > 0 && height > 0 && pixels.size() == width * height;
}

ColorRGBA Bitmap::sample_nearest(double u, double v) const
{
    if (!valid())
        return {};

    u = clamp01(u);
    v = clamp01(v);

    const size_t x = std::min<size_t>(size_t(std::round(u * double(width - 1))), width - 1);
    const size_t y = std::min<size_t>(size_t(std::round(v * double(height - 1))), height - 1);
    return pixels[y * width + x];
}

UV project_point(const SurfacePoint& point, const ProjectionSettings& settings)
{
    UV uv;

    switch (settings.mode) {
    case ProjectionMode::PlanarXY:
        uv.u = normalize_component(point.position.x(), settings.origin.x(), settings.size.x());
        uv.v = normalize_component(point.position.y(), settings.origin.y(), settings.size.y());
        break;
    case ProjectionMode::PlanarXZ:
        uv.u = normalize_component(point.position.x(), settings.origin.x(), settings.size.x());
        uv.v = normalize_component(point.position.z(), settings.origin.z(), settings.size.z());
        break;
    case ProjectionMode::PlanarYZ:
        uv.u = normalize_component(point.position.y(), settings.origin.y(), settings.size.y());
        uv.v = normalize_component(point.position.z(), settings.origin.z(), settings.size.z());
        break;
    case ProjectionMode::CylindricalZ: {
        const double angle = std::atan2(
            point.position.y() - settings.cylinder_center.y(),
            point.position.x() - settings.cylinder_center.x()
        );
        uv.u = (angle + PI) / (2.0 * PI);
        uv.v = normalize_component(point.position.z(), settings.origin.z(), settings.size.z());
        break;
    }
    }

    uv.u = resolve_uv_component(apply_uv_transform(uv.u, settings.scale_u, settings.offset_u), settings.clamp);
    uv.v = resolve_uv_component(apply_uv_transform(uv.v, settings.scale_v, settings.offset_v), settings.clamp);
    return uv;
}

std::optional<size_t> nearest_palette_index(
    const ColorRGBA& color,
    const std::vector<ColorTarget>& palette,
    uint8_t alpha_threshold
)
{
    if (palette.empty() || color.a < alpha_threshold)
        return std::nullopt;

    int best_distance = std::numeric_limits<int>::max();
    size_t best_index = 0;

    for (size_t i = 0; i < palette.size(); ++i) {
        const int distance = color_distance_sq(color, palette[i].color);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }

    return best_index;
}

std::vector<PaintedSample> paint_surface_points(
    const std::vector<SurfacePoint>& points,
    const Bitmap& bitmap,
    const ProjectionSettings& projection,
    const std::vector<ColorTarget>& palette,
    uint8_t alpha_threshold
)
{
    std::vector<PaintedSample> painted;
    if (!bitmap.valid() || palette.empty())
        return painted;

    painted.reserve(points.size());

    for (const SurfacePoint& point : points) {
        const UV uv = project_point(point, projection);
        const ColorRGBA source_color = bitmap.sample_nearest(uv.u, uv.v);
        const std::optional<size_t> palette_index = nearest_palette_index(source_color, palette, alpha_threshold);
        if (!palette_index)
            continue;

        painted.push_back(PaintedSample{
            point.face_id,
            uv,
            source_color,
            *palette_index,
            palette[*palette_index],
        });
    }

    return painted;
}

} // namespace Slic3r::SurfacePainter
