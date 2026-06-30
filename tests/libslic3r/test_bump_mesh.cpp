#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/BumpMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

static indexed_triangle_set make_xy_quad()
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f{-1.f, -1.f, 0.f},
        Vec3f{ 1.f, -1.f, 0.f},
        Vec3f{ 1.f,  1.f, 0.f},
        Vec3f{-1.f,  1.f, 0.f},
    };
    its.indices = {
        stl_triangle_vertex_indices{0, 1, 2},
        stl_triangle_vertex_indices{0, 2, 3},
    };
    return its;
}

static indexed_triangle_set make_yz_quad_px()
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f{0.f, -1.f, -1.f},
        Vec3f{0.f,  1.f, -1.f},
        Vec3f{0.f,  1.f,  1.f},
        Vec3f{0.f, -1.f,  1.f},
    };
    its.indices = {
        stl_triangle_vertex_indices{0, 1, 2},
        stl_triangle_vertex_indices{0, 2, 3},
    };
    return its;
}

TEST_CASE("Bump mesh keeps neutral displacement unchanged", "[BumpMesh]")
{
    indexed_triangle_set input = make_xy_quad();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {0.5f};

    BumpMesh::Settings settings;
    settings.amplitude = 2.f;
    settings.max_edge_length = 10.f;

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    REQUIRE(output.indices.size() == input.indices.size());
    for (size_t i = 0; i < input.vertices.size(); ++i) {
        REQUIRE(output.vertices[i].x() == input.vertices[i].x());
        REQUIRE(output.vertices[i].y() == input.vertices[i].y());
        REQUIRE(output.vertices[i].z() == Catch::Approx(input.vertices[i].z()).margin(1e-5f));
    }
}

TEST_CASE("Bump mesh displaces an XY plane along smooth normals", "[BumpMesh]")
{
    indexed_triangle_set input = make_xy_quad();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 0.8f;
    settings.max_edge_length = 10.f;

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    for (const Vec3f &v : output.vertices) {
        REQUIRE(v.x() >= -1.001f);
        REQUIRE(v.x() <=  1.001f);
        REQUIRE(v.y() >= -1.001f);
        REQUIRE(v.y() <=  1.001f);
        REQUIRE(v.z() == Catch::Approx(0.4f).margin(1e-5f));
    }
}

TEST_CASE("Bump mesh can subdivide before displacement", "[BumpMesh]")
{
    indexed_triangle_set input = make_xy_quad();
    BumpMesh::Texture texture;
    texture.width = 2;
    texture.height = 2;
    texture.gray = {0.f, 1.f, 1.f, 0.f};

    BumpMesh::Settings settings;
    settings.amplitude = 0.2f;
    settings.max_edge_length = 1.f;

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() > input.vertices.size());
    REQUIRE(output.indices.size() > input.indices.size());
}

TEST_CASE("Bump mesh side mask can disable a top face", "[BumpMesh]")
{
    indexed_triangle_set input = make_xy_quad();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 0.8f;
    settings.symmetric = false;
    settings.max_edge_length = 10.f;
    settings.apply_dir[BumpMesh::DIR_PZ] = false;

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    for (const Vec3f &v : output.vertices)
        REQUIRE(v.z() == 0.f);
}

TEST_CASE("Bump mesh side mask can target a positive X face", "[BumpMesh]")
{
    indexed_triangle_set input = make_yz_quad_px();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 0.8f;
    settings.symmetric = false;
    settings.max_edge_length = 10.f;
    settings.apply_dir = {false, false, false, false, false, false};
    settings.apply_dir[BumpMesh::DIR_PX] = true;

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    for (const Vec3f &v : output.vertices)
        REQUIRE(v.x() == Catch::Approx(0.8f).margin(1e-5f));
}
