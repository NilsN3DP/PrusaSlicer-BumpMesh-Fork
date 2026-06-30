#include "GLGizmoBumpMesh.hpp"

#include "libslic3r/BumpMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiPureWrap.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <wx/filedlg.h>
#include <wx/image.h>

#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>

namespace Slic3r::GUI {
namespace {

struct BuiltinTexture
{
    const char *label;
    const char *resource;
};

static constexpr BuiltinTexture BUILTIN_TEXTURES[] = {
    {"Basket", "textures/bump_mesh/basket.png"},
    {"Brick", "textures/bump_mesh/brick.png"},
    {"Bubble", "textures/bump_mesh/bubble.png"},
    {"Carbon Fiber", "textures/bump_mesh/carbonFiber.jpg"},
    {"Crystal", "textures/bump_mesh/crystal.png"},
    {"Dots", "textures/bump_mesh/dots.png"},
    {"Grid", "textures/bump_mesh/grid.png"},
    {"Grip Surface", "textures/bump_mesh/gripSurface.jpg"},
    {"Hexagon", "textures/bump_mesh/hexagon.jpg"},
    {"Hexagons", "textures/bump_mesh/hexagons.jpg"},
    {"Isogrid", "textures/bump_mesh/isogrid.png"},
    {"Knitting", "textures/bump_mesh/knitting.png"},
    {"Knurling", "textures/bump_mesh/knurling.jpg"},
    {"Leather", "textures/bump_mesh/leather2.png"},
    {"Noise", "textures/bump_mesh/noise.jpg"},
    {"Stripes", "textures/bump_mesh/stripes.png"},
    {"Stripes 02", "textures/bump_mesh/stripes_02.png"},
    {"Voronoi", "textures/bump_mesh/voronoi.jpg"},
    {"Weave", "textures/bump_mesh/weave.png"},
    {"Weave 02", "textures/bump_mesh/weave_02.jpg"},
    {"Weave 03", "textures/bump_mesh/weave_03.jpg"},
    {"Wood", "textures/bump_mesh/wood.jpg"},
    {"Woodgrain 02", "textures/bump_mesh/woodgrain_02.jpg"},
    {"Woodgrain 03", "textures/bump_mesh/woodgrain_03.jpg"},
};

BumpMesh::Texture make_preview_texture()
{
    constexpr int size = 64;
    constexpr float pi = 3.14159265358979323846f;
    BumpMesh::Texture texture;
    texture.width = size;
    texture.height = size;
    texture.gray.resize(size * size);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = float(x) / float(size);
            const float v = float(y) / float(size);
            texture.gray[y * size + x] = 0.5f + 0.25f * std::sin(u * 8.f * pi) +
                                         0.25f * std::sin(v * 8.f * pi);
        }
    }

    return texture;
}

struct MappingModeEntry { const char *label; BumpMesh::MappingMode mode; };
static constexpr MappingModeEntry MAPPING_MODES[] = {
    {"Triplanar", BumpMesh::MappingMode::Triplanar},
    {"Cubic (box)", BumpMesh::MappingMode::Cubic},
    {"Cylindrical", BumpMesh::MappingMode::Cylindrical},
    {"Spherical", BumpMesh::MappingMode::Spherical},
    {"Planar XY", BumpMesh::MappingMode::PlanarXY},
    {"Planar XZ", BumpMesh::MappingMode::PlanarXZ},
    {"Planar YZ", BumpMesh::MappingMode::PlanarYZ},
};

struct BumpPreset
{
    const char *label;
    float amplitude;
    float scale;
    float detail;
    float falloff;
    float seam_blend;
    bool overhang_safe;
    int target_triangles;
};

static constexpr BumpPreset BUMP_PRESETS[] = {
    {"Custom", 0.4f, 1.0f, 150.f, 0.0f, 0.0f, false, 0},
    {"Subtle", 0.25f, 1.4f, 110.f, 0.5f, 0.2f, true, 350000},
    {"Brick", 0.75f, 1.0f, 180.f, 0.6f, 0.15f, true, 650000},
    {"Grip", 0.55f, 0.8f, 220.f, 0.3f, 0.1f, true, 800000},
    {"Fabric", 0.35f, 1.8f, 170.f, 0.4f, 0.35f, true, 500000},
    {"Deep Relief", 1.35f, 1.0f, 260.f, 0.8f, 0.2f, true, 1200000},
};

void advanced_tooltip(const std::string &text)
{
    if (ImGui::IsItemHovered())
        ImGuiPureWrap::tooltip(text, ImGui::GetFontSize() * 24.0f);
}

float otsu_threshold(const BumpMesh::Texture &texture)
{
    if (!texture.valid())
        return 0.5f;

    std::array<int, 256> hist{};
    for (float v : texture.gray) {
        const int bucket = std::clamp(int(std::round(std::clamp(v, 0.f, 1.f) * 255.f)), 0, 255);
        ++hist[size_t(bucket)];
    }

    const int total = texture.width * texture.height;
    double sum = 0.0;
    for (int i = 0; i < 256; ++i)
        sum += double(i) * double(hist[size_t(i)]);

    double sum_b = 0.0;
    int weight_b = 0;
    double best_variance = -1.0;
    int threshold = 127;
    for (int i = 0; i < 256; ++i) {
        weight_b += hist[size_t(i)];
        if (weight_b == 0)
            continue;
        const int weight_f = total - weight_b;
        if (weight_f == 0)
            break;

        sum_b += double(i) * double(hist[size_t(i)]);
        const double mean_b = sum_b / double(weight_b);
        const double mean_f = (sum - sum_b) / double(weight_f);
        const double variance = double(weight_b) * double(weight_f) * (mean_b - mean_f) * (mean_b - mean_f);
        if (variance > best_variance) {
            best_variance = variance;
            threshold = i;
        }
    }
    return float(threshold) / 255.f;
}

int auto_color_bucket(float grey, int steps, int mode, float texture_threshold, bool invert_depth)
{
    grey = std::clamp(grey, 0.f, 1.f);
    if (mode == 1) {
        if (steps <= 2)
            return grey < texture_threshold ? 0 : 1;

        if (grey < texture_threshold)
            return 0;

        const float light_range = std::max(1.f - texture_threshold, 1e-6f);
        const float t = std::clamp((grey - texture_threshold) / light_range, 0.f, 0.9999f);
        return 1 + std::clamp(int(std::floor(t * float(steps - 1))), 0, steps - 2);
    }

    if (invert_depth)
        grey = 1.f - grey;
    return std::clamp(int(std::floor(std::clamp(grey, 0.f, 0.9999f) * float(steps))), 0, steps - 1);
}

int face_dir(const Vec3f &n)
{
    const float ax = std::abs(n.x());
    const float ay = std::abs(n.y());
    const float az = std::abs(n.z());
    if (ax >= ay && ax >= az) return n.x() >= 0.f ? BumpMesh::DIR_PX : BumpMesh::DIR_NX;
    if (ay >= az) return n.y() >= 0.f ? BumpMesh::DIR_PY : BumpMesh::DIR_NY;
    return n.z() >= 0.f ? BumpMesh::DIR_PZ : BumpMesh::DIR_NZ;
}

ColorRGBA preview_color(int bucket, int steps)
{
    static const std::array<ColorRGBA, 8> palette = {{
        {0.05f, 0.14f, 0.36f, 0.58f},
        {0.00f, 0.38f, 0.55f, 0.58f},
        {0.00f, 0.56f, 0.38f, 0.58f},
        {0.53f, 0.68f, 0.18f, 0.58f},
        {0.95f, 0.67f, 0.20f, 0.58f},
        {0.90f, 0.34f, 0.20f, 0.58f},
        {0.70f, 0.16f, 0.36f, 0.58f},
        {0.94f, 0.86f, 0.62f, 0.58f},
    }};
    if (steps <= 1)
        return palette.front();
    const int idx = std::clamp(int(std::round(float(bucket) * 7.f / float(steps - 1))), 0, 7);
    return palette[idx];
}

TriangleStateType extruder_state_for_index(int extruder_idx)
{
    const int state = int(TriangleStateType::Extruder1) + extruder_idx;
    return TriangleStateType(std::clamp(state, int(TriangleStateType::Extruder1), int(TriangleStateType::Extruder16)));
}

std::vector<std::string> extruder_labels(int count)
{
    std::vector<std::string> labels;
    labels.reserve(size_t(std::max(0, count)));

    const int physical_count = wxGetApp().extruders_edited_cnt();
    const FullSpectrum::VirtualExtruders &virtual_extruders = wxGetApp().plater()->model().virtual_extruders;
    for (int i = 0; i < count; ++i) {
        if (i < physical_count) {
            labels.emplace_back(_u8L("Extruder") + " " + std::to_string(i + 1));
            continue;
        }

        const size_t virtual_idx = size_t(i - physical_count);
        if (virtual_idx < virtual_extruders.size()) {
            const FullSpectrum::VirtualExtruder &ve = virtual_extruders[virtual_idx];
            const char *kind = ve.type() == FullSpectrum::VirtualExtruder::Type::Gradient ? "Color gradient" : "Color mix";
            labels.emplace_back(_u8L(kind) + " " + std::to_string(ve.id));
        } else {
            labels.emplace_back(_u8L("Extruder") + " " + std::to_string(i + 1));
        }
    }
    return labels;
}

bool extruder_combo(const char *label, int &selected_idx, int count)
{
    if (count <= 0)
        return false;

    selected_idx = std::clamp(selected_idx, 0, count - 1);
    const std::vector<std::string> labels = extruder_labels(count);
    bool changed = false;
    if (ImGui::BeginCombo(label, labels[size_t(selected_idx)].c_str())) {
        for (int i = 0; i < count; ++i) {
            const bool selected = i == selected_idx;
            if (ImGui::Selectable(labels[size_t(i)].c_str(), selected)) {
                selected_idx = i;
                changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

GLGizmoBumpMesh::GLGizmoBumpMesh(GLCanvas3D &parent)
    : GLGizmoBase(parent, "bump_mesh.svg", 15)
{
}

std::string GLGizmoBumpMesh::on_get_name() const
{
    return _u8L("Bump Mesh");
}

bool GLGizmoBumpMesh::on_is_activable() const
{
    return can_apply();
}

bool GLGizmoBumpMesh::can_apply() const
{
    const Selection &selection = m_parent.get_selection();
    if (selection.volumes_count() != 1 || selection.get_model() == nullptr)
        return false;

    const GLVolume *gl_volume = selection.get_first_volume();
    if (gl_volume == nullptr)
        return false;

    const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    return volume != nullptr && volume->is_model_part() && !volume->mesh().its.indices.empty();
}

void GLGizmoBumpMesh::data_changed(bool)
{
    prune_original_meshes();
    invalidate_preview();
}

void GLGizmoBumpMesh::prune_original_meshes()
{
    // Drop captured originals whose ModelVolume no longer exists, so a deleted
    // volume can't leave a stale pointer (a new volume could reuse the address)
    // and the per-session cache stays bounded.
    const Model *model = m_parent.get_selection().get_model();
    if (model == nullptr) {
        m_original_meshes.clear();
        return;
    }
    std::unordered_set<const ModelVolume *> alive;
    for (const ModelObject *object : model->objects)
        for (const ModelVolume *volume : object->volumes)
            alive.insert(volume);
    for (auto it = m_original_meshes.begin(); it != m_original_meshes.end();)
        it = (alive.count(it->first) != 0) ? std::next(it) : m_original_meshes.erase(it);
}

void GLGizmoBumpMesh::invalidate_preview()
{
    // Only mark the overlay for rebuild — keep the existing models on screen
    // until the rebuild replaces them, so it never flickers to an empty frame.
    m_preview_dirty = true;
    m_geometry_preview_dirty = true;
    set_dirty();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

bool GLGizmoBumpMesh::load_texture(const wxString &path)
{
    wxImage image;
    if (!image.LoadFile(path))
        return false;

    BumpMesh::Texture texture;
    texture.width = image.GetWidth();
    texture.height = image.GetHeight();
    texture.gray.resize(size_t(texture.width * texture.height));

    const unsigned char *rgb = image.GetData();
    const unsigned char *alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    for (int y = 0; y < texture.height; ++y) {
        for (int x = 0; x < texture.width; ++x) {
            const int pixel = y * texture.width + x;
            const int rgb_idx = pixel * 3;
            float value = (0.2126f * float(rgb[rgb_idx]) +
                           0.7152f * float(rgb[rgb_idx + 1]) +
                           0.0722f * float(rgb[rgb_idx + 2])) / 255.f;
            if (alpha != nullptr)
                value *= float(alpha[pixel]) / 255.f;
            texture.gray[pixel] = value;
        }
    }

    m_texture = std::move(texture);
    m_texture_path = into_u8(path);
    invalidate_preview();
    return true;
}

bool GLGizmoBumpMesh::load_texture_from_file()
{
    wxFileDialog dialog(wxGetApp().plater(), _L("Choose a bump texture"), wxEmptyString, wxEmptyString,
                        _L("Images (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff)|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"),
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    if (!load_texture(dialog.GetPath()))
        return false;

    m_builtin_texture_idx = -1;
    return true;
}

BumpMesh::Settings GLGizmoBumpMesh::build_settings() const
{
    BumpMesh::Settings s;
    const int mode_idx = std::clamp(m_mapping_mode, 0, int(std::size(MAPPING_MODES)) - 1);
    s.mapping_mode = MAPPING_MODES[mode_idx].mode;
    s.amplitude = m_amplitude;
    s.scale_u = m_scale;
    s.scale_v = m_scale;
    s.offset_u = m_offset_u;
    s.offset_v = m_offset_v;
    s.rotation = m_rotation;
    s.mapping_blend = m_blend;
    s.symmetric = m_symmetric;
    s.invert = m_invert;
    s.no_downward_z = m_overhang_safe;
    s.top_angle_limit = m_top_angle;
    s.bottom_angle_limit = m_bottom_angle;
    s.boundary_falloff = m_falloff;
    s.blend_normal_smoothing = m_blend_smooth;
    s.detail = m_detail;
    s.max_edge_length = -1.f;
    s.target_triangles = m_target_triangles;
    for (int i = 0; i < BumpMesh::DIR_COUNT; ++i)
        s.apply_dir[i] = m_apply_dir[i];
    return s;
}

void GLGizmoBumpMesh::apply_preset(int preset_idx)
{
    if (preset_idx <= 0 || preset_idx >= int(std::size(BUMP_PRESETS)))
        return;

    const BumpPreset &preset = BUMP_PRESETS[preset_idx];
    m_preset_idx = preset_idx;
    m_amplitude = preset.amplitude;
    m_scale = preset.scale;
    m_detail = preset.detail;
    m_falloff = preset.falloff;
    m_blend = preset.seam_blend;
    m_overhang_safe = preset.overhang_safe;
    m_target_triangles = preset.target_triangles;
}

void GLGizmoBumpMesh::apply_print_safe_settings()
{
    m_overhang_safe = true;
    m_falloff = std::max(m_falloff, 0.6f);
    m_blend = std::max(m_blend, 0.15f);
    m_target_triangles = m_target_triangles <= 0 ? 650000 : std::min(m_target_triangles, 650000);
    m_detail = std::min(m_detail, 180.f);
    m_amplitude = std::min(m_amplitude, 1.0f);
    m_preset_idx = 0;
}

int GLGizmoBumpMesh::estimate_triangle_count() const
{
    const Selection &selection = m_parent.get_selection();
    if (!can_apply())
        return 0;

    const GLVolume *gl_volume = selection.get_first_volume();
    if (gl_volume == nullptr)
        return 0;

    const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return 0;

    const indexed_triangle_set &its = volume->mesh().its;
    if (its.indices.empty())
        return 0;

    const BoundingBoxf3 bb = bounding_box(its);
    const float max_dim = std::max({bb.size().x(), bb.size().y(), bb.size().z()});
    const float max_edge = max_dim / std::max(1.f, m_detail);
    if (max_edge <= 1e-6f)
        return int(its.indices.size());

    double area = 0.0;
    for (const stl_triangle_vertex_indices &face : its.indices) {
        const Vec3f v0 = its.vertices[face[0]];
        const Vec3f v1 = its.vertices[face[1]];
        const Vec3f v2 = its.vertices[face[2]];
        area += 0.5 * double((v1 - v0).cross(v2 - v0).norm());
    }

    const double ideal_tri_area = 0.4330127018922193 * double(max_edge) * double(max_edge);
    const int estimated = int(std::ceil(area / std::max(ideal_tri_area, 1e-9)));
    return std::max<int>(int(its.indices.size()), estimated);
}

void GLGizmoBumpMesh::update_geometry_preview()
{
    if (!m_live_geometry_preview || !can_apply())
        return;

    const Selection &selection = m_parent.get_selection();
    const GLVolume *gl_volume = selection.get_first_volume();
    if (gl_volume == nullptr)
        return;

    ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return;

    const auto original_it = m_original_meshes.find(volume);
    const indexed_triangle_set &source = original_it != m_original_meshes.end() ? original_it->second : volume->mesh().its;
    if (source.indices.empty())
        return;

    m_geometry_preview_model.reset();

    BumpMesh::Settings settings = build_settings();
    constexpr int preview_triangle_cap = 350000;
    if (settings.target_triangles <= 0 || settings.target_triangles > preview_triangle_cap)
        settings.target_triangles = preview_triangle_cap;

    const BumpMesh::Texture &texture = m_texture.valid() ? m_texture : make_preview_texture();
    indexed_triangle_set preview = BumpMesh::bake_displacement(source, texture, settings);
    if (preview.indices.empty())
        return;

    m_geometry_preview_model.init_from(preview);
    m_geometry_preview_model.set_color({0.12f, 0.55f, 0.95f, 0.48f});
    m_geometry_preview_dirty = false;
}

void GLGizmoBumpMesh::update_auto_color_preview()
{
    // Leave m_preview_dirty set and keep any existing models if we can't rebuild
    // right now, so the overlay survives a transient unselectable state instead
    // of clearing itself permanently.
    if (!m_auto_color_preview || !can_apply())
        return;

    const Selection &selection = m_parent.get_selection();
    const GLVolume *gl_volume = selection.get_first_volume();
    ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return;

    for (GLModel &model : m_preview_models)
        model.reset();

    const indexed_triangle_set &its = volume->mesh().its;
    const int steps = std::clamp(m_auto_color_steps, 2, 8);
    std::vector<GLModel::Geometry> buckets{size_t(steps)};
    for (GLModel::Geometry &data : buckets)
        data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};

    const BumpMesh::Texture &texture = m_texture.valid() ? m_texture : make_preview_texture();
    const BumpMesh::Settings settings = build_settings();
    const BoundingBoxf3 bb = bounding_box(its);
    const float texture_threshold = std::clamp(m_texture_dark_cutoff, 0.01f, 0.99f);
    std::vector<int> bucket_vertex_count(size_t(steps), 0);

    for (const stl_triangle_vertex_indices &face : its.indices) {
        const Vec3f v0 = its.vertices[face[0]];
        const Vec3f v1 = its.vertices[face[1]];
        const Vec3f v2 = its.vertices[face[2]];
        Vec3f n = (v1 - v0).cross(v2 - v0);
        const float n_len = n.norm();
        if (n_len <= 1e-12f)
            continue;
        n /= n_len;
        if (!settings.apply_dir[face_dir(n)])
            continue;

        const Vec3f center = (v0 + v1 + v2) / 3.f;
        float grey = BumpMesh::sample_gray(center, n, settings, bb, texture);
        const int bucket = auto_color_bucket(grey, steps, m_auto_color_mode, texture_threshold, settings.invert);
        GLModel::Geometry &data = buckets[size_t(bucket)];
        const Vec3f offset = n * 0.025f;
        const unsigned int base = (unsigned int)bucket_vertex_count[size_t(bucket)];
        data.add_vertex(v0 + offset, n);
        data.add_vertex(v1 + offset, n);
        data.add_vertex(v2 + offset, n);
        data.add_triangle(base, base + 1, base + 2);
        bucket_vertex_count[size_t(bucket)] += 3;
    }

    m_preview_models.clear();
    m_preview_models.resize(size_t(steps));
    for (int i = 0; i < steps; ++i) {
        if (buckets[size_t(i)].is_empty())
            continue;
        m_preview_models[size_t(i)].init_from(std::move(buckets[size_t(i)]));
        m_preview_models[size_t(i)].set_color(color_for_bucket(i, steps));
    }
    m_preview_dirty = false;
}

ColorRGBA GLGizmoBumpMesh::color_for_bucket(int bucket, int steps) const
{
    if (m_auto_color_mode == 1) {
        const std::vector<ColorRGBA> colors = wxGetApp().plater()->get_extruder_colors_from_plater_config();
        if (!colors.empty()) {
            const int fallback = std::clamp(bucket, 0, int(colors.size()) - 1);
            const int idx = bucket == 0 ? std::clamp(m_dark_extruder_idx, 0, int(colors.size()) - 1) :
                                          std::clamp(m_light_extruder_idx, 0, int(colors.size()) - 1);
            return colors[size_t(idx >= 0 ? idx : fallback)];
        }
    }
    return preview_color(bucket, steps);
}

int GLGizmoBumpMesh::extruder_index_for_bucket(int bucket, int steps) const
{
    if (m_auto_color_mode == 1) {
        const int total_extruders = std::min<int>(16, wxGetApp().extruders_edited_cnt() + wxGetApp().virtual_extruders_cnt());
        const int dark = std::clamp(m_dark_extruder_idx, 0, std::max(0, total_extruders - 1));
        const int light = std::clamp(m_light_extruder_idx, 0, std::max(0, total_extruders - 1));
        return bucket == 0 ? dark : light;
    }
    return std::clamp(bucket, 0, steps - 1);
}

void GLGizmoBumpMesh::on_render()
{
    if ((!m_auto_color_preview && !m_live_geometry_preview) || !can_apply())
        return;

    if (m_live_geometry_preview && m_geometry_preview_dirty)
        update_geometry_preview();
    if (m_auto_color_preview && m_preview_dirty)
        update_auto_color_preview();
    if ((!m_live_geometry_preview || m_geometry_preview_model.is_empty()) &&
        (!m_auto_color_preview || m_preview_models.empty()))
        return;

    const Selection &selection = m_parent.get_selection();
    const GLVolume *volume = selection.get_first_volume();
    if (volume == nullptr)
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d model_matrix = volume->world_matrix();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glDepthMask(GL_FALSE));
    if (m_live_geometry_preview && !m_geometry_preview_model.is_empty())
        m_geometry_preview_model.render();
    if (m_auto_color_preview) {
        for (GLModel &model : m_preview_models) {
            if (!model.is_empty())
                model.render();
        }
    }
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

void GLGizmoBumpMesh::assign_auto_color_to_extruders(ModelVolume &volume) const
{
    if (!m_auto_color_preview || !m_auto_color_to_extruders)
        return;

    const int total_extruders = std::min<int>(16, wxGetApp().extruders_edited_cnt() + wxGetApp().virtual_extruders_cnt());
    if (total_extruders <= 1)
        return;

    const indexed_triangle_set &its = volume.mesh().its;
    if (its.indices.empty())
        return;

    const int steps = std::clamp(m_auto_color_steps, 2, std::min(8, total_extruders));
    const BumpMesh::Texture &texture = m_texture.valid() ? m_texture : make_preview_texture();
    const BumpMesh::Settings settings = build_settings();
    const BoundingBoxf3 bb = bounding_box(its);
    const float texture_threshold = std::clamp(m_texture_dark_cutoff, 0.01f, 0.99f);

    TriangleSelector selector(volume.mesh());
    for (size_t facet_idx = 0; facet_idx < its.indices.size(); ++facet_idx) {
        const stl_triangle_vertex_indices &face = its.indices[facet_idx];
        const Vec3f v0 = its.vertices[face[0]];
        const Vec3f v1 = its.vertices[face[1]];
        const Vec3f v2 = its.vertices[face[2]];
        Vec3f n = (v1 - v0).cross(v2 - v0);
        const float n_len = n.norm();
        if (n_len <= 1e-12f)
            continue;
        n /= n_len;
        if (!settings.apply_dir[face_dir(n)])
            continue;

        const Vec3f center = (v0 + v1 + v2) / 3.f;
        float grey = BumpMesh::sample_gray(center, n, settings, bb, texture);
        const int bucket = auto_color_bucket(grey, steps, m_auto_color_mode, texture_threshold, settings.invert);
        selector.set_facet(int(facet_idx), extruder_state_for_index(extruder_index_for_bucket(bucket, steps)));
    }

    volume.mm_segmentation_facets.set(selector);
}

void GLGizmoBumpMesh::apply_live_auto_color_to_selection()
{
    if (!m_auto_color_preview || !m_auto_color_to_extruders || !can_apply())
        return;

    const Selection &selection = m_parent.get_selection();
    const GLVolume *gl_volume = selection.get_first_volume();
    if (gl_volume == nullptr)
        return;

    ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return;

    assign_auto_color_to_extruders(*volume);
    if (selection.get_object_idx() >= 0)
        wxGetApp().obj_list()->update_info_items(selection.get_object_idx());

    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoBumpMesh::apply_to_selection()
{
    const Selection &selection = m_parent.get_selection();
    if (!can_apply())
        return;

    const GLVolume *gl_volume = selection.get_first_volume();
    ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return;

    ModelObject *object = volume->get_object();
    if (object == nullptr)
        return;

    Plater *plater = wxGetApp().plater();
    plater->take_snapshot(m_apply_to_copy ? _u8L("Bump Mesh Copy") : _u8L("Bump Mesh"));

    if (m_apply_to_copy) {
        ModelVolume *copy = object->add_volume(*volume, ModelVolumeType::MODEL_PART);
        copy->name = volume->name.empty() ? "Bump Mesh" : volume->name + " Bump Mesh";
        volume = copy;
    } else {
        plater->clear_before_change_mesh(selection.get_object_idx(), _u8L("Custom supports, seams and multimaterial painting were removed after displacing the mesh."));
    }

    auto original_it = m_original_meshes.find(volume);
    if (original_it == m_original_meshes.end())
        original_it = m_original_meshes.emplace(volume, volume->mesh().its).first;

    const BumpMesh::Settings settings = build_settings();
    const BumpMesh::Texture &texture = m_texture.valid() ? m_texture : make_preview_texture();
    indexed_triangle_set displaced = BumpMesh::bake_displacement(original_it->second, texture, settings);

    volume->set_mesh(std::move(displaced));
    assign_auto_color_to_extruders(*volume);
    volume->calculate_convex_hull();
    volume->set_new_unique_id();
    object->invalidate_bounding_box();
    object->ensure_on_bed(true);

    plater->changed_object(*object);
    wxGetApp().obj_list()->update_info_items(selection.get_object_idx());
    invalidate_preview();
    m_parent.reload_scene(true, true);
}

void GLGizmoBumpMesh::remove_from_selection()
{
    const Selection &selection = m_parent.get_selection();
    if (!can_apply())
        return;

    const GLVolume *gl_volume = selection.get_first_volume();
    ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
    if (volume == nullptr)
        return;

    auto original_it = m_original_meshes.find(volume);
    if (original_it == m_original_meshes.end())
        return;

    ModelObject *object = volume->get_object();
    if (object == nullptr)
        return;

    Plater *plater = wxGetApp().plater();
    plater->take_snapshot(_u8L("Remove Bump Mesh"));
    plater->clear_before_change_mesh(selection.get_object_idx(), _u8L("Custom supports, seams and multimaterial painting were removed after restoring the original mesh."));
    wxGetApp().obj_list()->update_info_items(selection.get_object_idx());

    volume->set_mesh(indexed_triangle_set(original_it->second));
    volume->calculate_convex_hull();
    volume->set_new_unique_id();
    object->invalidate_bounding_box();
    object->ensure_on_bed(true);

    plater->changed_object(*object);
    invalidate_preview();
    m_parent.reload_scene(true, true);
}

void GLGizmoBumpMesh::on_render_input_window(float, float, float)
{
    bool preview_changed = false;
    ImGui::TextUnformatted(_u8L("Bump Mesh").c_str());
    ImGui::Separator();

    const bool enabled = can_apply();
    bool has_original = false;
    if (enabled) {
        const Selection &selection = m_parent.get_selection();
        if (const GLVolume *gl_volume = selection.get_first_volume()) {
            ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
            has_original = volume != nullptr && m_original_meshes.find(volume) != m_original_meshes.end();
        }
    }

    if (!enabled) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    if (ImGui::Button(_u8L("Apply").c_str()) && enabled) {
        apply_to_selection();
        if (!enabled) {
            ImGui::PopItemFlag();
            ImGui::PopStyleVar();
        }
        return;
    }
    if (!enabled) {
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
    }

    ImGui::SameLine();
    if (!enabled || !has_original) {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    }
    if (ImGui::Button(_u8L("Remove").c_str()) && enabled && has_original) {
        remove_from_selection();
        if (!enabled || !has_original) {
            ImGui::PopItemFlag();
            ImGui::PopStyleVar();
        }
        return;
    }
    if (!enabled || !has_original) {
        ImGui::PopItemFlag();
        ImGui::PopStyleVar();
    }

    ImGui::SameLine();
    ImGui::Checkbox(_u8L("Apply to copy").c_str(), &m_apply_to_copy);
    advanced_tooltip(_u8L("Creates a duplicated model volume and applies Bump Mesh to the copy, leaving the selected source volume unchanged."));

    const char *preset_label = BUMP_PRESETS[std::clamp(m_preset_idx, 0, int(std::size(BUMP_PRESETS)) - 1)].label;
    if (ImGui::BeginCombo(_u8L("Preset").c_str(), preset_label)) {
        for (int i = 0; i < int(std::size(BUMP_PRESETS)); ++i) {
            const bool selected = i == m_preset_idx;
            if (ImGui::Selectable(BUMP_PRESETS[i].label, selected)) {
                if (i == 0)
                    m_preset_idx = 0;
                else
                    apply_preset(i);
                preview_changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    advanced_tooltip(_u8L("Quick starting points for common printable texture styles."));

    if (ImGui::Button(_u8L("Print-safe").c_str())) {
        apply_print_safe_settings();
        preview_changed = true;
    }
    advanced_tooltip(_u8L("Uses conservative values: overhang protection, boundary falloff, capped detail and triangle decimation."));

    preview_changed |= ImGui::Checkbox(_u8L("Live geometry preview").c_str(), &m_live_geometry_preview);
    advanced_tooltip(_u8L("Shows an approximate displaced mesh before Apply. The preview is capped to keep slider changes responsive; Apply still uses the full selected settings."));

    const char *selected_texture = m_builtin_texture_idx >= 0 ? BUILTIN_TEXTURES[m_builtin_texture_idx].label : "Custom";
    if (ImGui::BeginCombo(_u8L("Texture").c_str(), selected_texture)) {
        for (int i = 0; i < int(std::size(BUILTIN_TEXTURES)); ++i) {
            const bool selected = i == m_builtin_texture_idx;
            if (ImGui::Selectable(BUILTIN_TEXTURES[i].label, selected)) {
                if (load_texture(from_u8(Slic3r::resources_dir() + "/" + BUILTIN_TEXTURES[i].resource))) {
                    m_builtin_texture_idx = i;
                    preview_changed = true;
                }
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button(_u8L("Load texture").c_str()))
        preview_changed |= load_texture_from_file();
    if (m_texture.valid())
        ImGui::Text("%dx%d texture", m_texture.width, m_texture.height);
    else
        ImGui::TextUnformatted(_u8L("Using generated preview texture").c_str());
    const char *mode_label = MAPPING_MODES[std::clamp(m_mapping_mode, 0, int(std::size(MAPPING_MODES)) - 1)].label;
    if (ImGui::BeginCombo(_u8L("Projection").c_str(), mode_label)) {
        for (int i = 0; i < int(std::size(MAPPING_MODES)); ++i) {
            const bool selected = i == m_mapping_mode;
            if (ImGui::Selectable(MAPPING_MODES[i].label, selected)) {
                m_mapping_mode = i;
                preview_changed = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::SliderFloat(_u8L("Amplitude").c_str(), &m_amplitude, 0.0f, 5.0f, "%.2f mm")) {
        preview_changed = true;
        m_preset_idx = 0;
    }
    if (ImGui::SliderFloat(_u8L("Scale").c_str(), &m_scale, 0.1f, 10.0f, "%.2f")) {
        preview_changed = true;
        m_preset_idx = 0;
    }
    if (ImGui::SliderFloat(_u8L("Detail").c_str(), &m_detail, 20.0f, 600.0f, "%.0f")) {
        preview_changed = true;
        m_preset_idx = 0;
    }

    const int estimated_triangles = estimate_triangle_count();
    if (estimated_triangles > 0) {
        const int final_estimate = m_target_triangles > 0 ? std::min(estimated_triangles, m_target_triangles) : estimated_triangles;
        if (estimated_triangles > 1000000 && m_target_triangles <= 0)
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.22f, 1.0f), "%s %.1fM", _u8L("Estimated triangles:").c_str(), double(estimated_triangles) / 1000000.0);
        else
            ImGui::Text("%s %.1fM", _u8L("Estimated triangles:").c_str(), double(final_estimate) / 1000000.0);
    }

    ImGui::Separator();
    preview_changed |= ImGui::Checkbox(_u8L("Auto color preview").c_str(), &m_auto_color_preview);
    advanced_tooltip(_u8L("Shows a transparent color overlay on the selected model. This is a preview of how automatic coloring would be split into color steps."));
    if (m_auto_color_preview) {
        static constexpr const char *AUTO_COLOR_MODES[] = {"Depth", "Texture dark/light"};
        const int mode_idx = std::clamp(m_auto_color_mode, 0, int(std::size(AUTO_COLOR_MODES)) - 1);
        if (ImGui::BeginCombo(_u8L("Color source").c_str(), AUTO_COLOR_MODES[mode_idx])) {
            for (int i = 0; i < int(std::size(AUTO_COLOR_MODES)); ++i) {
                const bool selected = i == m_auto_color_mode;
                if (ImGui::Selectable(AUTO_COLOR_MODES[i], selected)) {
                    m_auto_color_mode = i;
                    if (m_auto_color_mode == 1)
                        m_auto_color_steps = 2;
                    preview_changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        advanced_tooltip(_u8L("Depth splits by displacement height. Texture dark/light automatically separates dark texture areas, such as mortar lines, from brighter areas."));
        if (m_auto_color_mode == 1) {
            preview_changed |= ImGui::SliderFloat(_u8L("Dark cutoff").c_str(), &m_texture_dark_cutoff, 0.02f, 0.60f, "%.2f");
            advanced_tooltip(_u8L("Texture pixels darker than this value become the dark/mortar extruder. Lower values remove noisy dark speckles inside bright areas."));
            const int available_extruders = wxGetApp().extruders_edited_cnt() + wxGetApp().virtual_extruders_cnt();
            if (available_extruders > 1) {
                preview_changed |= extruder_combo(_u8L("Dark areas").c_str(), m_dark_extruder_idx, available_extruders);
                advanced_tooltip(_u8L("Extruder used for dark texture areas, for example brick mortar lines."));
                preview_changed |= extruder_combo(_u8L("Light areas").c_str(), m_light_extruder_idx, available_extruders);
                advanced_tooltip(_u8L("Extruder used for bright texture areas, for example brick faces."));
            }
        }
        preview_changed |= ImGui::SliderInt(_u8L("Color steps").c_str(), &m_auto_color_steps, 2, 8);
        advanced_tooltip(_u8L("Number of color zones shown on the model. More steps preserve more texture variation; fewer steps are easier to print with few colors."));
        preview_changed |= ImGui::Checkbox(_u8L("Live assign to extruders").c_str(), &m_auto_color_to_extruders);
        advanced_tooltip(_u8L("Writes the preview color zones as real multimaterial painting while you adjust the controls, and writes them again after Apply."));
        const int available_extruders = wxGetApp().extruders_edited_cnt() + wxGetApp().virtual_extruders_cnt();
        if (available_extruders <= 1)
            ImGui::TextUnformatted(_u8L("Requires a printer profile with at least 2 extruders.").c_str());
        else if (m_auto_color_steps > available_extruders)
            ImGui::Text("%s %d", _u8L("Limited by available extruders:").c_str(), available_extruders);
    }

    ImGui::Separator();
    ImGui::TextUnformatted(_u8L("Sides").c_str());
    preview_changed |= ImGui::Checkbox(_u8L("Top (+Z)").c_str(), &m_apply_dir[BumpMesh::DIR_PZ]);
    ImGui::SameLine();
    preview_changed |= ImGui::Checkbox(_u8L("Bottom (-Z)").c_str(), &m_apply_dir[BumpMesh::DIR_NZ]);
    ImGui::SameLine();
    preview_changed |= ImGui::Checkbox(_u8L("Right (+X)").c_str(), &m_apply_dir[BumpMesh::DIR_PX]);
    preview_changed |= ImGui::Checkbox(_u8L("Left (-X)").c_str(), &m_apply_dir[BumpMesh::DIR_NX]);
    ImGui::SameLine();
    preview_changed |= ImGui::Checkbox(_u8L("Front (+Y)").c_str(), &m_apply_dir[BumpMesh::DIR_PY]);
    ImGui::SameLine();
    preview_changed |= ImGui::Checkbox(_u8L("Back (-Y)").c_str(), &m_apply_dir[BumpMesh::DIR_NY]);

    if (ImGui::CollapsingHeader(_u8L("Advanced").c_str())) {
        preview_changed |= ImGui::Checkbox(_u8L("Symmetric (grey 0.5 = neutral)").c_str(), &m_symmetric);
        advanced_tooltip(_u8L("Treats middle grey as neutral. Darker pixels move inward, brighter pixels move outward."));
        preview_changed |= ImGui::Checkbox(_u8L("Invert height").c_str(), &m_invert);
        advanced_tooltip(_u8L("Flips the height map so bright areas become low and dark areas become high."));
        preview_changed |= ImGui::Checkbox(_u8L("Overhang safe (no downward Z)").c_str(), &m_overhang_safe);
        advanced_tooltip(_u8L("Prevents the displacement from moving vertices downward in Z. Useful when the texture would create unsupported overhangs."));
        preview_changed |= ImGui::SliderFloat(_u8L("Seam blend").c_str(), &m_blend, 0.0f, 1.0f, "%.2f");
        advanced_tooltip(_u8L("Softens projection seams, especially with Cubic or cylindrical mapping. Higher values reduce hard transitions but can blur the pattern near edges."));
        preview_changed |= ImGui::SliderFloat(_u8L("Offset U").c_str(), &m_offset_u, -1.0f, 1.0f, "%.2f");
        advanced_tooltip(_u8L("Moves the texture horizontally in UV space without changing the mesh position."));
        preview_changed |= ImGui::SliderFloat(_u8L("Offset V").c_str(), &m_offset_v, -1.0f, 1.0f, "%.2f");
        advanced_tooltip(_u8L("Moves the texture vertically in UV space without changing the mesh position."));
        preview_changed |= ImGui::SliderFloat(_u8L("Rotation").c_str(), &m_rotation, 0.0f, 360.0f, "%.0f deg");
        advanced_tooltip(_u8L("Rotates the texture projection before it is baked into the mesh."));
        preview_changed |= ImGui::SliderFloat(_u8L("Top angle limit").c_str(), &m_top_angle, 0.0f, 90.0f, "%.0f deg");
        advanced_tooltip(_u8L("Limits displacement on upward-facing surfaces near the top direction. Use it to keep shallow top areas flatter."));
        preview_changed |= ImGui::SliderFloat(_u8L("Bottom angle limit").c_str(), &m_bottom_angle, 0.0f, 90.0f, "%.0f deg");
        advanced_tooltip(_u8L("Limits displacement on downward-facing surfaces near the bottom direction. Useful for keeping underside geometry printable."));
        preview_changed |= ImGui::SliderFloat(_u8L("Boundary falloff").c_str(), &m_falloff, 0.0f, 10.0f, "%.2f mm");
        advanced_tooltip(_u8L("Fades displacement near masked or disabled areas over this distance, reducing sharp steps at the boundary."));
        preview_changed |= ImGui::SliderInt(_u8L("Blend normal smoothing").c_str(), &m_blend_smooth, 0, 64);
        advanced_tooltip(_u8L("Smooths the normals used for blending projections. Higher values can make transitions cleaner but may soften crisp details."));
        preview_changed |= ImGui::SliderInt(_u8L("Max triangles (0 = off)").c_str(), &m_target_triangles, 0, 2000000);
        advanced_tooltip(_u8L("Simplifies the result after displacement to stay below this triangle count. 0 keeps the full generated detail."));
    }

    if (preview_changed) {
        invalidate_preview();
        apply_live_auto_color_to_selection();
    }
}

} // namespace Slic3r::GUI
