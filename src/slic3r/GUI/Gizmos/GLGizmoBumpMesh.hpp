#ifndef slic3r_GLGizmoBumpMesh_hpp_
#define slic3r_GLGizmoBumpMesh_hpp_

#include "GLGizmoPainterBase.hpp"
#include "libslic3r/BumpMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <string>
#include <unordered_map>
#include <vector>

class wxString;

namespace Slic3r {
class ModelVolume;
}

namespace Slic3r::GUI {

class GLGizmoBumpMesh : public GLGizmoPainterBase
{
public:
    explicit GLGizmoBumpMesh(GLCanvas3D &parent);
    void render_painter_gizmo() override;

protected:
    bool on_init() override { return true; }
    std::string on_get_name() const override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override;
    void data_changed(bool is_serializing) override;

private:
    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;
    TriangleStateType get_left_button_state_type() const override;
    TriangleStateType get_right_button_state_type() const override;
    ColorRGBA get_cursor_sphere_left_button_color() const override;
    ColorRGBA get_cursor_sphere_right_button_color() const override;
    void on_painting_changed(bool temporary_preview) override;

    bool can_apply() const;
    bool load_texture(const wxString &path);
    bool load_texture_from_file();
    void apply_to_selection();
    void remove_from_selection();
    void assign_auto_color_to_extruders(ModelVolume &volume,
                                        const BumpMesh::Settings *settings_override = nullptr,
                                        const std::vector<uint32_t> *source_face_ids = nullptr) const;
    void apply_live_auto_color_to_selection();
    void apply_preset(int preset_idx);
    void apply_print_safe_settings();
    int estimate_triangle_count() const;
    BumpMesh::Settings build_settings(const ModelVolume *volume = nullptr, const indexed_triangle_set *source = nullptr) const;
    void update_model_object() const override;
    void update_from_model_object() override;
    int selector_index_for_volume(const ModelVolume &volume) const;
    void build_bump_face_masks(const ModelVolume &volume, const indexed_triangle_set *source, size_t face_count,
                               std::vector<uint8_t> &include_mask,
                               std::vector<uint8_t> &exclude_mask) const;
    bool has_bump_mask(const ModelVolume *volume = nullptr) const;
    bool has_current_painter_mask(const ModelVolume &volume) const;
    bool has_stored_source_mask(const ModelVolume &volume) const;
    void clear_bump_mask();
    void invalidate_preview();
    void prune_original_meshes();
    void render_preview_models();
    void update_geometry_preview();
    void update_auto_color_preview();
    ColorRGBA color_for_bucket(int bucket, int steps) const;
    int extruder_index_for_bucket(int bucket, int steps) const;

    BumpMesh::Texture m_texture;
    std::string m_texture_path;
    int m_builtin_texture_idx = -1;

    // Projection + main controls
    int   m_mapping_mode = 0;   // index into the mode list (see .cpp)
    float m_amplitude    = 0.4f;
    float m_scale        = 1.f;
    float m_detail       = 150.f;
    int   m_preset_idx   = 0;
    bool  m_apply_to_copy = false;
    bool  m_live_geometry_preview = true;
    mutable bool  m_geometry_preview_dirty = true;
    GLModel m_geometry_preview_model;

    // Auto color preview
    bool  m_auto_color_preview = false;
    bool  m_auto_color_to_extruders = true;
    int   m_auto_color_mode    = 0; // 0 = depth, 1 = texture
    int   m_auto_color_steps   = 4;
    float m_texture_dark_cutoff = 0.22f;
    int   m_dark_extruder_idx  = 0;
    int   m_light_extruder_idx = 1;
    mutable bool  m_preview_dirty      = true;
    std::vector<GLModel> m_preview_models;

    // Direct Bump Mesh surface mask.
    int   m_surface_paint_mode = 0; // 0 = include, 1 = exclude

    // Side selection (px, nx, py, ny, pz, nz)
    bool  m_apply_dir[BumpMesh::DIR_COUNT] = {true, true, true, true, true, true};

    // Advanced
    bool  m_symmetric    = true;
    bool  m_invert       = false;
    bool  m_overhang_safe = false;
    float m_blend        = 0.f;
    float m_offset_u     = 0.f;
    float m_offset_v     = 0.f;
    float m_rotation     = 0.f;
    float m_top_angle    = 0.f;
    float m_bottom_angle = 0.f;
    float m_falloff      = 0.f;
    int   m_blend_smooth = 0;
    int   m_target_triangles = 0;

    // Original meshes captured before the first Bump Mesh apply in this session.
    std::unordered_map<ModelVolume*, indexed_triangle_set> m_original_meshes;
    struct BumpMaskData {
        TriangleSelector::TriangleSplittingData selector_data;
        size_t source_face_count = 0;
    };
    mutable std::unordered_map<ModelVolume*, BumpMaskData> m_bump_mask_facets;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoBumpMesh_hpp_
