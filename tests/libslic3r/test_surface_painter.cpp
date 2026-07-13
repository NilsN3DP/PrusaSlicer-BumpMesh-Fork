#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Feature/SurfacePainter/SurfacePainter.hpp"

using namespace Slic3r;
using namespace Slic3r::SurfacePainter;

TEST_CASE("SurfacePainter maps planar bitmap samples to fixed extruders", "[SurfacePainter]")
{
    const Bitmap bitmap{
        2,
        1,
        {
            ColorRGBA{255, 0, 0, 255},
            ColorRGBA{0, 0, 255, 255},
        },
    };

    const std::vector<ColorTarget> palette{
        ColorTarget{ColorRGBA{255, 0, 0, 255}, AssignmentKind::FixedExtruder, 1},
        ColorTarget{ColorRGBA{0, 0, 255, 255}, AssignmentKind::FixedExtruder, 2},
    };

    ProjectionSettings projection;
    projection.mode = ProjectionMode::PlanarXY;
    projection.origin = Vec3d(0.0, 0.0, 0.0);
    projection.size = Vec3d(10.0, 10.0, 1.0);

    const std::vector<SurfacePoint> points{
        SurfacePoint{Vec3d(0.0, 5.0, 0.0), Vec3d::UnitZ(), 7},
        SurfacePoint{Vec3d(10.0, 5.0, 0.0), Vec3d::UnitZ(), 8},
    };

    const std::vector<PaintedSample> painted = paint_surface_points(points, bitmap, projection, palette);

    REQUIRE(painted.size() == 2);
    CHECK(painted[0].face_id == 7);
    CHECK(painted[0].target.kind == AssignmentKind::FixedExtruder);
    CHECK(painted[0].target.id == 1);
    CHECK(painted[1].face_id == 8);
    CHECK(painted[1].target.id == 2);
}

TEST_CASE("SurfacePainter cylindrical projection wraps around Z", "[SurfacePainter]")
{
    ProjectionSettings projection;
    projection.mode = ProjectionMode::CylindricalZ;
    projection.origin = Vec3d(0.0, 0.0, 0.0);
    projection.size = Vec3d(1.0, 1.0, 20.0);
    projection.cylinder_center = Vec3d(0.0, 0.0, 0.0);

    const UV left = project_point(SurfacePoint{Vec3d(-1.0, 0.0, 10.0), Vec3d::UnitX(), 1}, projection);
    const UV right = project_point(SurfacePoint{Vec3d(1.0, 0.0, 20.0), Vec3d::UnitX(), 2}, projection);

    CHECK(left.u == 1.0);
    CHECK(left.v == 0.5);
    CHECK(right.u == 0.5);
    CHECK(right.v == 1.0);
}

TEST_CASE("SurfacePainter applies image scale and offset in UV space", "[SurfacePainter]")
{
    ProjectionSettings projection;
    projection.mode = ProjectionMode::PlanarXY;
    projection.origin = Vec3d(0.0, 0.0, 0.0);
    projection.size = Vec3d(10.0, 10.0, 1.0);
    projection.scale_u = 0.5;
    projection.scale_v = 2.0;
    projection.offset_u = 0.25;
    projection.offset_v = -0.5;

    const UV uv = project_point(SurfacePoint{Vec3d(5.0, 5.0, 0.0), Vec3d::UnitZ(), 1}, projection);

    CHECK(uv.u == 0.5);
    CHECK(uv.v == 0.5);
}

TEST_CASE("SurfacePainter can target virtual extruders for Color Mix integration", "[SurfacePainter]")
{
    const ColorRGBA orange{250, 120, 20, 255};
    const std::vector<ColorTarget> palette{
        ColorTarget{ColorRGBA{255, 0, 0, 255}, AssignmentKind::FixedExtruder, 1},
        ColorTarget{ColorRGBA{255, 128, 0, 255}, AssignmentKind::VirtualExtruder, 101},
    };

    const std::optional<size_t> match = nearest_palette_index(orange, palette);

    REQUIRE(match.has_value());
    CHECK(*match == 1);
    CHECK(palette[*match].kind == AssignmentKind::VirtualExtruder);
    CHECK(palette[*match].id == 101);
}
