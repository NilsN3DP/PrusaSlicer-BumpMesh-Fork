#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "libslic3r/BumpMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

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

static indexed_triangle_set make_bent_two_quad_strip()
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f{-1.f, -1.f, 0.f},
        Vec3f{ 1.f, -1.f, 0.f},
        Vec3f{ 1.f,  1.f, 0.f},
        Vec3f{-1.f,  1.f, 0.f},
        Vec3f{ 1.f, -1.f, 2.f},
        Vec3f{ 1.f,  1.f, 2.f},
    };
    its.indices = {
        stl_triangle_vertex_indices{0, 1, 2},
        stl_triangle_vertex_indices{0, 2, 3},
        stl_triangle_vertex_indices{1, 4, 5},
        stl_triangle_vertex_indices{1, 5, 2},
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

TEST_CASE("Bump mesh bucket fill follows connected faces until the dihedral angle limit", "[BumpMesh]")
{
    indexed_triangle_set input = make_bent_two_quad_strip();
    const std::vector<std::vector<size_t>> adjacency = BumpMesh::build_face_adjacency(input);

    std::vector<uint8_t> shallow = BumpMesh::bucket_fill_faces(input, adjacency, 0, 30.f);
    REQUIRE(shallow.size() == input.indices.size());
    REQUIRE(shallow[0] == 1);
    REQUIRE(shallow[1] == 1);
    REQUIRE(shallow[2] == 0);
    REQUIRE(shallow[3] == 0);

    std::vector<uint8_t> wide = BumpMesh::bucket_fill_faces(input, adjacency, 0, 100.f);
    REQUIRE(wide.size() == input.indices.size());
    for (uint8_t selected : wide)
        REQUIRE(selected == 1);
}

TEST_CASE("Bump mesh include-only face mask displaces only selected faces", "[BumpMesh]")
{
    indexed_triangle_set input = make_bent_two_quad_strip();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 1.f;
    settings.symmetric = false;
    settings.max_edge_length = 10.f;
    settings.face_mask_mode = BumpMesh::FaceMaskMode::IncludeOnly;
    settings.face_mask = {1, 1, 0, 0};

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    bool saw_displaced_top = false;
    bool saw_untouched_wall = false;
    for (const Vec3f &v : output.vertices) {
        if (v.z() > 0.9f && v.x() < 1.001f)
            saw_displaced_top = true;
        if (v.x() == Catch::Approx(1.f).margin(1e-5f) && v.z() > 0.5f)
            saw_untouched_wall = true;
    }
    REQUIRE(saw_displaced_top);
    REQUIRE(saw_untouched_wall);
}

TEST_CASE("Triangle selector can export a source face mask", "[BumpMesh]")
{
    TriangleMesh mesh(make_xy_quad());
    TriangleSelector selector(mesh);

    selector.set_facet(0, TriangleStateType::ENFORCER);
    selector.set_facet(1, TriangleStateType::BLOCKER);

    const std::vector<uint8_t> include = selector.source_triangle_mask(TriangleStateType::ENFORCER, 2);
    const std::vector<uint8_t> exclude = selector.source_triangle_mask(TriangleStateType::BLOCKER, 2);

    REQUIRE(include.size() == 2);
    REQUIRE(include[0] == 1);
    REQUIRE(include[1] == 0);
    REQUIRE(exclude.size() == 2);
    REQUIRE(exclude[0] == 0);
    REQUIRE(exclude[1] == 1);
}

TEST_CASE("Bump mesh direct include and exclude masks can be combined", "[BumpMesh]")
{
    indexed_triangle_set input = make_bent_two_quad_strip();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 1.f;
    settings.symmetric = false;
    settings.max_edge_length = 10.f;
    settings.include_face_mask = {1, 1, 1, 1};
    settings.exclude_face_mask = {0, 0, 1, 1};

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    bool saw_displaced_top = false;
    bool saw_untouched_wall = false;
    for (const Vec3f &v : output.vertices) {
        if (v.z() > 0.9f && v.x() < 1.001f)
            saw_displaced_top = true;
        if (v.x() == Catch::Approx(1.f).margin(1e-5f) && v.z() > 0.5f)
            saw_untouched_wall = true;
    }
    REQUIRE(saw_displaced_top);
    REQUIRE(saw_untouched_wall);
}

TEST_CASE("Bump mesh direct include mask overrides side checkboxes", "[BumpMesh]")
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
    settings.include_face_mask = {1, 1};

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    REQUIRE(output.vertices.size() == input.vertices.size());
    for (const Vec3f &v : output.vertices)
        REQUIRE(v.x() == Catch::Approx(0.8f).margin(1e-5f));
}

TEST_CASE("Bump mesh direct include mask is the exclusive affected surface", "[BumpMesh]")
{
    indexed_triangle_set input = make_bent_two_quad_strip();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 1.f;
    settings.symmetric = false;
    settings.max_edge_length = 10.f;
    settings.apply_dir = {false, false, false, false, false, false};
    settings.include_face_mask = {1, 1, 0, 0};

    indexed_triangle_set output = BumpMesh::bake_displacement(input, texture, settings);

    bool saw_displaced_selected_surface = false;
    bool saw_untouched_unselected_surface = false;
    for (const Vec3f &v : output.vertices) {
        if (v.z() > 0.9f && v.x() < 1.001f)
            saw_displaced_selected_surface = true;
        if (v.x() == Catch::Approx(1.f).margin(1e-5f) && v.z() > 0.5f)
            saw_untouched_unselected_surface = true;
    }
    REQUIRE(saw_displaced_selected_surface);
    REQUIRE(saw_untouched_unselected_surface);
}

TEST_CASE("Bump mesh bake reports source faces for subdivided output", "[BumpMesh]")
{
    indexed_triangle_set input = make_xy_quad();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 0.2f;
    settings.symmetric = false;
    settings.max_edge_length = 0.75f;

    BumpMesh::BakeResult result = BumpMesh::bake_displacement_with_face_ids(input, texture, settings);

    REQUIRE(result.mesh.indices.size() > input.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.indices.size());
    for (uint32_t source_face : result.source_face_ids)
        REQUIRE(source_face < input.indices.size());
}

TEST_CASE("Bump mesh masks subdivided faces by their original source face", "[BumpMesh]")
{
    indexed_triangle_set input = make_bent_two_quad_strip();
    BumpMesh::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.gray = {1.f};

    BumpMesh::Settings settings;
    settings.amplitude = 1.f;
    settings.symmetric = false;
    settings.max_edge_length = 0.5f;
    settings.include_face_mask = {1, 1, 1, 1};
    settings.exclude_face_mask = {0, 0, 1, 1};

    BumpMesh::BakeResult result = BumpMesh::bake_displacement_with_face_ids(input, texture, settings);

    REQUIRE(result.mesh.indices.size() > input.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.indices.size());

    bool saw_included_subface = false;
    bool saw_excluded_subface = false;
    for (size_t facet_idx = 0; facet_idx < result.mesh.indices.size(); ++facet_idx) {
        const uint32_t source_face = result.source_face_ids[facet_idx];
        REQUIRE(source_face < input.indices.size());

        const stl_triangle_vertex_indices &face = result.mesh.indices[facet_idx];
        const Vec3f center = (result.mesh.vertices[face[0]] +
                              result.mesh.vertices[face[1]] +
                              result.mesh.vertices[face[2]]) / 3.f;

        if (source_face < 2) {
            saw_included_subface = true;
            REQUIRE(center.z() > 0.05f);
        } else {
            saw_excluded_subface = true;
            REQUIRE(center.x() == Catch::Approx(1.f).margin(1e-5f));
        }
    }
    REQUIRE(saw_included_subface);
    REQUIRE(saw_excluded_subface);
}
