#ifndef slic3r_SurfacePainterDialog_hpp_
#define slic3r_SurfacePainterDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"
#include "libslic3r/Feature/SurfacePainter/SurfacePainter.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <wx/bitmap.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include <functional>
#include <optional>
#include <chrono>
#include <vector>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxListBox;
class wxPanel;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;
class wxMouseEvent;

namespace Slic3r {
class DynamicPrintConfig;
class Model;
class ModelVolume;
}

namespace Slic3r::GUI {

class GLCanvas3D;

class SurfacePainterDialog : public DPIDialog
{
public:
    using PreviewCallback = std::function<void(size_t)>;
    using ApplyCallback = std::function<bool(SurfacePainterDialog&)>;

    SurfacePainterDialog(
        wxWindow* parent,
        Model& model,
        const DynamicPrintConfig& full_config,
        ModelVolume* target_volume = nullptr,
        PreviewCallback preview_callback = {},
        GLCanvas3D* canvas = nullptr,
        ApplyCallback apply_callback = {}
    );
    ~SurfacePainterDialog() override;

    const std::vector<FullSpectrum::VirtualExtruder>& result_virtual_extruders() const
    {
        return m_generated_virtual_extruders;
    }

    bool create_virtual_extruders() const
    {
        return m_create_virtual_extruders;
    }

    size_t apply_to_volume(ModelVolume& volume) const;
    void restore_target_volume();
    bool preview_was_applied() const { return m_preview_was_applied; }
    bool committed() const { return m_committed; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override;

private:
    struct PaletteEntry
    {
        SurfacePainter::ColorRGBA color;
        size_t count = 0;
        SurfacePainter::ColorTarget target;
    };

    Model& m_model;
    ModelVolume* m_target_volume{nullptr};
    PreviewCallback m_preview_callback;
    ApplyCallback m_apply_callback;
    GLCanvas3D* m_canvas{nullptr};
    std::optional<indexed_triangle_set> m_original_mesh;
    TriangleSelector::TriangleSplittingData m_original_mm_segmentation;
    FullSpectrum::VirtualExtruders m_original_virtual_extruders;
    bool m_original_had_mm_segmentation = false;
    bool m_preview_virtual_extruders_installed = false;
    bool m_preview_was_applied = false;
    bool m_committed = false;
    bool m_canvas_events_bound = false;
    bool m_dragging_placement = false;
    bool m_rotating_placement = false;
    bool m_fast_preview = false;
    size_t m_last_painted_facets = 0;
    wxPoint m_last_mouse_position;
    std::chrono::steady_clock::time_point m_last_drag_preview_time;
    unsigned int m_num_physical = 0;
    std::vector<std::string> m_physical_colors;

    SurfacePainter::Bitmap m_bitmap;
    wxString m_image_path;
    wxBitmap m_preview_bitmap;
    std::vector<PaletteEntry> m_palette;
    std::vector<SurfacePainter::PaintedSample> m_samples;
    std::vector<FullSpectrum::VirtualExtruder> m_generated_virtual_extruders;
    bool m_create_virtual_extruders = false;

    wxStaticText* m_file_label{nullptr};
    wxStaticBitmap* m_preview{nullptr};
    wxChoice* m_projection_choice{nullptr};
    wxChoice* m_target_choice{nullptr};
    wxSpinCtrl* m_palette_size{nullptr};
    wxSpinCtrl* m_detail_level{nullptr};
    wxCheckBox* m_live_preview{nullptr};
    wxButton* m_preview_button{nullptr};
    wxSpinCtrlDouble* m_scale_u{nullptr};
    wxSpinCtrlDouble* m_scale_v{nullptr};
    wxSpinCtrlDouble* m_offset_u{nullptr};
    wxSpinCtrlDouble* m_offset_v{nullptr};
    wxSpinCtrlDouble* m_rotation_degrees{nullptr};
    wxCheckBox* m_repeat_image{nullptr};
    wxCheckBox* m_mouse_placement{nullptr};
    wxCheckBox* m_preview_while_placing{nullptr};
    wxListBox* m_palette_list{nullptr};
    wxStaticText* m_result_label{nullptr};
    wxButton* m_apply_button{nullptr};

    void build_layout();
    void apply_colors();
    void on_load_image(wxCommandEvent& event);
    void on_controls_changed(wxCommandEvent& event);
    void on_preview(wxCommandEvent& event);
    void on_apply(wxCommandEvent& event);
    void on_fit_image(wxCommandEvent& event);
    void on_center_image(wxCommandEvent& event);
    void on_reset_transform(wxCommandEvent& event);
    void on_canvas_mouse(wxMouseEvent& event);

    bool load_image(const wxString& path);
    void rebuild_preview();
    void rebuild_palette();
    void run_projection_probe();
    void refresh_result_ui();
    void rebuild_generated_virtual_extruders();
    void ensure_preview_virtual_extruders();
    void restore_preview_virtual_extruders();

    SurfacePainter::ProjectionSettings projection_settings() const;
    SurfacePainter::ProjectionSettings projection_settings_for_volume(const ModelVolume& volume) const;
    SurfacePainter::AssignmentKind target_kind() const;
    std::vector<SurfacePainter::ColorTarget> color_targets() const;
    std::vector<SurfacePainter::SurfacePoint> make_probe_points() const;
    std::vector<SurfacePainter::SurfacePoint> make_volume_points(const indexed_triangle_set& mesh) const;
    int detail_level() const;
    void apply_preview_if_enabled(bool force = false);
    void refresh_after_transform_change(bool force = false);
    void bind_canvas_events();
    void unbind_canvas_events();
    void set_spin_value_clamped(wxSpinCtrlDouble* spin, double value);
    void set_transform_values(double scale_u, double scale_v, double offset_u, double offset_v, double rotation_degrees);
    std::optional<std::pair<int, int>> target_volume_indices() const;
    std::optional<SurfacePainter::UV> surface_uv_at_mouse(const wxPoint& position) const;
    bool center_decal_at_mouse(const wxPoint& position, bool auto_projection);
    void center_decal_at_uv(const SurfacePainter::UV& uv);
    void set_projection_from_normal(const Vec3d& normal);
    void fit_image_to_decal();
    void center_image_in_decal();
    int detected_image_color_count() const;
    size_t apply_to_volume(ModelVolume& volume, const indexed_triangle_set* source_mesh) const;
    size_t apply_to_volume(ModelVolume& volume, const indexed_triangle_set* source_mesh, bool keep_existing_on_empty) const;
    unsigned int next_virtual_id() const;
};

} // namespace Slic3r::GUI

#endif // slic3r_SurfacePainterDialog_hpp_
