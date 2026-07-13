#ifndef slic3r_SurfacePainterDialog_hpp_
#define slic3r_SurfacePainterDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"
#include "libslic3r/Feature/SurfacePainter/SurfacePainter.hpp"

#include <wx/bitmap.h>
#include <wx/string.h>

#include <vector>

class wxButton;
class wxChoice;
class wxListBox;
class wxPanel;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r {
class DynamicPrintConfig;
class Model;
class ModelVolume;
}

namespace Slic3r::GUI {

class SurfacePainterDialog : public DPIDialog
{
public:
    SurfacePainterDialog(wxWindow* parent, const Model& model, const DynamicPrintConfig& full_config);
    ~SurfacePainterDialog() override = default;

    const std::vector<FullSpectrum::VirtualExtruder>& result_virtual_extruders() const
    {
        return m_generated_virtual_extruders;
    }

    bool create_virtual_extruders() const
    {
        return m_create_virtual_extruders;
    }

    size_t apply_to_volume(ModelVolume& volume) const;

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

    const Model& m_model;
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
    wxListBox* m_palette_list{nullptr};
    wxStaticText* m_result_label{nullptr};
    wxButton* m_apply_button{nullptr};

    void build_layout();
    void apply_colors();
    void on_load_image(wxCommandEvent& event);
    void on_controls_changed(wxCommandEvent& event);
    void on_apply(wxCommandEvent& event);

    bool load_image(const wxString& path);
    void rebuild_preview();
    void rebuild_palette();
    void run_projection_probe();
    void refresh_result_ui();
    void rebuild_generated_virtual_extruders();

    SurfacePainter::ProjectionSettings projection_settings() const;
    SurfacePainter::ProjectionSettings projection_settings_for_volume(const ModelVolume& volume) const;
    SurfacePainter::AssignmentKind target_kind() const;
    std::vector<SurfacePainter::ColorTarget> color_targets() const;
    std::vector<SurfacePainter::SurfacePoint> make_probe_points() const;
    std::vector<SurfacePainter::SurfacePoint> make_volume_points(const ModelVolume& volume) const;
    unsigned int next_virtual_id() const;
};

} // namespace Slic3r::GUI

#endif // slic3r_SurfacePainterDialog_hpp_
