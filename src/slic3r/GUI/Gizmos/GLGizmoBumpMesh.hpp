#ifndef slic3r_GLGizmoBumpMesh_hpp_
#define slic3r_GLGizmoBumpMesh_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/BumpMesh.hpp"

#include <string>
#include <unordered_map>
#include <vector>

class wxString;

namespace Slic3r {
class ModelVolume;
}

namespace Slic3r::GUI {

class GLGizmoBumpMesh : public GLGizmoBase
{
public:
    explicit GLGizmoBumpMesh(GLCanvas3D &parent);

protected:
    bool on_init() override { return true; }
    std::string on_get_name() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override { return true; }
    void data_changed(bool is_serializing) override;

private:
    bool can_apply() const;
    bool load_texture(const wxString &path);
    bool load_texture_from_file();
    void apply_to_selection();
    void remove_from_selection();
    void assign_auto_color_to_extruders(ModelVolume &volume) const;
    void apply_live_auto_color_to_selection();
    void apply_preset(int preset_idx);
    void apply_print_safe_settings();
    int estimate_triangle_count() const;
    BumpMesh::Settings build_settings() const;
    void invalidate_preview();
    void prune_original_meshes();
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
    bool  m_geometry_preview_dirty = true;
    GLModel m_geometry_preview_model;

    // Auto color preview
    bool  m_auto_color_preview = false;
    bool  m_auto_color_to_extruders = true;
    int   m_auto_color_mode    = 0; // 0 = depth, 1 = texture
    int   m_auto_color_steps   = 4;
    float m_texture_dark_cutoff = 0.22f;
    int   m_dark_extruder_idx  = 0;
    int   m_light_extruder_idx = 1;
    bool  m_preview_dirty      = true;
    std::vector<GLModel> m_preview_models;

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
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoBumpMesh_hpp_
