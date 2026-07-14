#include "SurfacePainterDialog.hpp"

#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/window.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>

namespace Slic3r::GUI {
namespace {

wxColour window_bg_color()
{
    return wxGetApp().get_window_default_clr();
}

wxString color_to_label(const SurfacePainter::ColorRGBA& color)
{
    return wxString::Format("#%02X%02X%02X", color.r, color.g, color.b);
}

struct NamedColor
{
    const char* name;
    SurfacePainter::ColorRGBA color;
};

int color_distance_sq(const SurfacePainter::ColorRGBA& a, const SurfacePainter::ColorRGBA& b)
{
    const int dr = int(a.r) - int(b.r);
    const int dg = int(a.g) - int(b.g);
    const int db = int(a.b) - int(b.b);
    return dr * dr + dg * dg + db * db;
}

const char* color_name(const SurfacePainter::ColorRGBA& color)
{
    static constexpr std::array<NamedColor, 18> named_colors{{
        {"black", {0, 0, 0, 255}},
        {"white", {255, 255, 255, 255}},
        {"gray", {128, 128, 128, 255}},
        {"red", {255, 0, 0, 255}},
        {"orange", {255, 128, 0, 255}},
        {"yellow", {255, 255, 0, 255}},
        {"green", {0, 180, 0, 255}},
        {"lime", {128, 255, 0, 255}},
        {"cyan", {0, 255, 255, 255}},
        {"blue", {0, 0, 255, 255}},
        {"navy", {0, 0, 128, 255}},
        {"purple", {128, 0, 255, 255}},
        {"magenta", {255, 0, 255, 255}},
        {"pink", {255, 128, 192, 255}},
        {"brown", {128, 64, 0, 255}},
        {"beige", {210, 190, 150, 255}},
        {"silver", {192, 192, 192, 255}},
        {"gold", {255, 190, 0, 255}},
    }};

    int best_distance = std::numeric_limits<int>::max();
    const char* best_name = "unknown";
    for (const NamedColor& named_color : named_colors) {
        const int distance = color_distance_sq(color, named_color.color);
        if (distance < best_distance) {
            best_distance = distance;
            best_name = named_color.name;
        }
    }
    return best_name;
}

std::string color_to_hex(const SurfacePainter::ColorRGBA& color)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return buffer;
}

int bucket_key(const SurfacePainter::ColorRGBA& color)
{
    const int r = int(color.r) / 32;
    const int g = int(color.g) / 32;
    const int b = int(color.b) / 32;
    return (r << 8) | (g << 4) | b;
}

int exact_color_key(const SurfacePainter::ColorRGBA& color)
{
    return (int(color.r) << 16) | (int(color.g) << 8) | int(color.b);
}

struct Bucket
{
    size_t count = 0;
    size_t r = 0;
    size_t g = 0;
    size_t b = 0;
};

struct RefinedMesh
{
    indexed_triangle_set mesh;
    size_t subdivisions = 1;
};

size_t subdivision_factor_for(const indexed_triangle_set& mesh, const SurfacePainter::Bitmap& bitmap, int detail_level)
{
    if (mesh.indices.empty())
        return 1;

    const size_t detail = size_t(std::clamp(detail_level, 1, 16));
    const size_t max_facets = 40000 + detail * detail * 25000;
    const size_t image_divisor = std::max<size_t>(4, 112 - detail * 6);
    const size_t image_hint = std::max<size_t>(1, std::min<size_t>(128, std::max(bitmap.width, bitmap.height) / image_divisor));
    const size_t budget_hint = std::max<size_t>(1, static_cast<size_t>(std::sqrt(double(max_facets) / double(mesh.indices.size()))));
    return std::clamp(std::min(image_hint, budget_hint), size_t(1), size_t(128));
}

int add_vertex(indexed_triangle_set& out, const Vec3f& vertex)
{
    out.vertices.push_back(vertex);
    return int(out.vertices.size() - 1);
}

RefinedMesh refine_mesh_for_surface_painting(const indexed_triangle_set& input, const SurfacePainter::Bitmap& bitmap, int detail_level)
{
    RefinedMesh refined;
    refined.subdivisions = subdivision_factor_for(input, bitmap, detail_level);
    if (refined.subdivisions <= 1) {
        refined.mesh = input;
        return refined;
    }

    const int n = int(refined.subdivisions);
    const size_t verts_per_triangle = size_t((n + 1) * (n + 2) / 2);
    refined.mesh.vertices.reserve(input.indices.size() * verts_per_triangle);
    refined.mesh.indices.reserve(input.indices.size() * size_t(n) * size_t(n));

    for (const stl_triangle_vertex_indices& face : input.indices) {
        const Vec3f a = input.vertices[face[0]];
        const Vec3f b = input.vertices[face[1]];
        const Vec3f c = input.vertices[face[2]];

        std::vector<int> grid(verts_per_triangle, -1);
        auto grid_offset = [n](int i, int j) {
            return i * (n + 1) - (i * (i - 1)) / 2 + j;
        };

        for (int i = 0; i <= n; ++i) {
            for (int j = 0; j <= n - i; ++j) {
                const float wa = float(n - i - j) / float(n);
                const float wb = float(i) / float(n);
                const float wc = float(j) / float(n);
                grid[grid_offset(i, j)] = add_vertex(refined.mesh, wa * a + wb * b + wc * c);
            }
        }

        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n - i; ++j) {
                const int v00 = grid[grid_offset(i, j)];
                const int v10 = grid[grid_offset(i + 1, j)];
                const int v01 = grid[grid_offset(i, j + 1)];
                refined.mesh.indices.emplace_back(v00, v10, v01);
                if (j < n - i - 1) {
                    const int v11 = grid[grid_offset(i + 1, j + 1)];
                    refined.mesh.indices.emplace_back(v10, v11, v01);
                }
            }
        }
    }

    its_merge_vertices(refined.mesh);
    its_compactify_vertices(refined.mesh);
    return refined;
}

} // namespace

SurfacePainterDialog::SurfacePainterDialog(
    wxWindow* parent,
    Model& model,
    const DynamicPrintConfig& full_config,
    ModelVolume* target_volume,
    PreviewCallback preview_callback,
    GLCanvas3D* canvas,
    ApplyCallback apply_callback
) :
    DPIDialog(
        parent,
        wxID_ANY,
        _L("Surface Painter Lab"),
        wxDefaultPosition,
        wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
    ),
    m_model(model),
    m_target_volume(target_volume),
    m_preview_callback(std::move(preview_callback)),
    m_apply_callback(std::move(apply_callback)),
    m_canvas(canvas)
{
    m_original_virtual_extruders = m_model.virtual_extruders;

    if (m_target_volume != nullptr) {
        m_original_mesh = m_target_volume->mesh().its;
        m_original_mm_segmentation = m_target_volume->mm_segmentation_facets.get_data();
        m_original_had_mm_segmentation = !m_target_volume->mm_segmentation_facets.empty();
    }

    if (const ConfigOptionFloats* nozzle_diameter_opt =
            full_config.option<ConfigOptionFloats>("nozzle_diameter"))
        m_num_physical = static_cast<unsigned int>(nozzle_diameter_opt->values.size());

    if (const ConfigOptionStrings* extruder_colour_opt =
            full_config.option<ConfigOptionStrings>("extruder_colour"))
        m_physical_colors = extruder_colour_opt->values;
    if (const ConfigOptionStrings* filament_colour_opt =
            full_config.option<ConfigOptionStrings>("filament_colour")) {
        if (m_physical_colors.size() < filament_colour_opt->values.size())
            m_physical_colors.resize(filament_colour_opt->values.size());
        for (size_t i = 0; i < filament_colour_opt->values.size(); ++i)
            if (m_physical_colors[i].empty())
                m_physical_colors[i] = filament_colour_opt->values[i];
    }

    if (m_num_physical == 0)
        m_num_physical = std::max<size_t>(1, m_physical_colors.size());
    if (m_physical_colors.size() < m_num_physical)
        m_physical_colors.resize(m_num_physical, "#808080");

    build_layout();
    apply_colors();

    const int em = wxGetApp().em_unit();
    this->GetSizer()->SetSizeHints(this);
    this->SetMinSize(wxSize(92 * em, 54 * em));
    this->SetSize(wxSize(104 * em, 62 * em));
    this->CentreOnParent();
    bind_canvas_events();
}

SurfacePainterDialog::~SurfacePainterDialog()
{
    unbind_canvas_events();
}

void SurfacePainterDialog::on_dpi_changed(const wxRect&)
{
    this->Layout();
}

void SurfacePainterDialog::on_sys_color_changed()
{
    apply_colors();
}

void SurfacePainterDialog::build_layout()
{
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(
        this,
        wxID_ANY,
        _L("Load a bitmap, reduce it to printable target colors, and run the Surface Painter projection core against a planar or cylindrical probe surface.")
    );
    intro->Wrap(760);
    root->Add(intro, 0, wxEXPAND | wxALL, 12);

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    auto* load_btn = new wxButton(this, wxID_ANY, _L("Load image") + dots);
    load_btn->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_load_image, this);
    top->Add(load_btn, 0, wxRIGHT, 8);

    m_file_label = new wxStaticText(this, wxID_ANY, _L("No image loaded"));
    top->Add(m_file_label, 1, wxALIGN_CENTER_VERTICAL);
    root->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* settings = new wxBoxSizer(wxHORIZONTAL);
    settings->Add(new wxStaticText(this, wxID_ANY, _L("Projection")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_projection_choice = new wxChoice(this, wxID_ANY);
    m_projection_choice->Append(_L("Planar XY"));
    m_projection_choice->Append(_L("Planar XZ"));
    m_projection_choice->Append(_L("Planar YZ"));
    m_projection_choice->Append(_L("Cylindrical Z"));
    m_projection_choice->SetSelection(0);
    m_projection_choice->Bind(wxEVT_CHOICE, &SurfacePainterDialog::on_controls_changed, this);
    settings->Add(m_projection_choice, 0, wxRIGHT, 14);

    settings->Add(new wxStaticText(this, wxID_ANY, _L("Targets")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_target_choice = new wxChoice(this, wxID_ANY);
    m_target_choice->Append(_L("Fixed extruders"));
    m_target_choice->Append(_L("Color Mix virtual extruders"));
    m_target_choice->SetSelection(1);
    m_target_choice->Bind(wxEVT_CHOICE, &SurfacePainterDialog::on_controls_changed, this);
    settings->Add(m_target_choice, 0, wxRIGHT, 14);

    settings->Add(new wxStaticText(this, wxID_ANY, _L("Colors")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_palette_size = new wxSpinCtrl(this, wxID_ANY);
    m_palette_size->SetRange(2, 16);
    m_palette_size->SetValue(2);
    m_palette_size->Bind(wxEVT_SPINCTRL, &SurfacePainterDialog::on_controls_changed, this);
    settings->Add(m_palette_size, 0, wxRIGHT, 14);

    settings->Add(new wxStaticText(this, wxID_ANY, _L("Detail")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_detail_level = new wxSpinCtrl(this, wxID_ANY);
    m_detail_level->SetRange(1, 16);
    m_detail_level->SetValue(6);
    m_detail_level->Bind(wxEVT_SPINCTRL, &SurfacePainterDialog::on_controls_changed, this);
    settings->Add(m_detail_level, 0);
    root->Add(settings, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* transform = new wxBoxSizer(wxHORIZONTAL);
    auto add_transform_spin = [this, transform](const wxString& label, double value, double min_value, double max_value, double increment = 0.01, int digits = 3) {
        transform->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* spin = new wxSpinCtrlDouble(this, wxID_ANY);
        spin->SetRange(min_value, max_value);
        spin->SetDigits(digits);
        spin->SetIncrement(increment);
        spin->SetValue(value);
        spin->Bind(wxEVT_SPINCTRLDOUBLE, &SurfacePainterDialog::on_controls_changed, this);
        spin->Bind(wxEVT_TEXT, &SurfacePainterDialog::on_controls_changed, this);
        transform->Add(spin, 0, wxRIGHT, 14);
        return spin;
    };
    m_scale_u = add_transform_spin(_L("Scale U"), 0.45, 0.02, 10.0);
    m_scale_v = add_transform_spin(_L("Scale V"), 0.45, 0.02, 10.0);
    m_offset_u = add_transform_spin(_L("Position U"), 0.275, -10.0, 10.0);
    m_offset_v = add_transform_spin(_L("Position V"), 0.275, -10.0, 10.0);
    m_rotation_degrees = add_transform_spin(_L("Rotation"), 0.0, -180.0, 180.0, 1.0, 1);
    m_repeat_image = new wxCheckBox(this, wxID_ANY, _L("Repeat image"));
    m_repeat_image->Bind(wxEVT_CHECKBOX, &SurfacePainterDialog::on_controls_changed, this);
    transform->Add(m_repeat_image, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);
    auto* fit_btn = new wxButton(this, wxID_ANY, _L("Fit"));
    fit_btn->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_fit_image, this);
    transform->Add(fit_btn, 0, wxRIGHT, 6);
    auto* center_btn = new wxButton(this, wxID_ANY, _L("Center"));
    center_btn->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_center_image, this);
    transform->Add(center_btn, 0, wxRIGHT, 6);
    auto* reset_btn = new wxButton(this, wxID_ANY, _L("Reset"));
    reset_btn->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_reset_transform, this);
    transform->Add(reset_btn, 0);
    root->Add(transform, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* preview_row = new wxBoxSizer(wxHORIZONTAL);
    m_live_preview = new wxCheckBox(this, wxID_ANY, _L("Live preview on selected volume"));
    m_live_preview->Bind(wxEVT_CHECKBOX, &SurfacePainterDialog::on_controls_changed, this);
    m_live_preview->Enable(m_target_volume != nullptr);
    preview_row->Add(m_live_preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    m_preview_button = new wxButton(this, wxID_ANY, _L("Preview on model"));
    m_preview_button->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_preview, this);
    m_preview_button->Enable(false);
    preview_row->Add(m_preview_button, 0, wxRIGHT, 12);
    m_mouse_placement = new wxCheckBox(this, wxID_ANY, _L("Mouse placement"));
    m_mouse_placement->Bind(wxEVT_CHECKBOX, &SurfacePainterDialog::on_controls_changed, this);
    m_mouse_placement->Enable(m_target_volume != nullptr && m_canvas != nullptr);
    m_mouse_placement->SetValue(m_target_volume != nullptr && m_canvas != nullptr);
    preview_row->Add(m_mouse_placement, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    m_preview_while_placing = new wxCheckBox(this, wxID_ANY, _L("Preview while placing"));
    m_preview_while_placing->Bind(wxEVT_CHECKBOX, &SurfacePainterDialog::on_controls_changed, this);
    m_preview_while_placing->Enable(m_target_volume != nullptr && m_canvas != nullptr);
    m_preview_while_placing->SetValue(false);
    preview_row->Add(m_preview_while_placing, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(preview_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* body = new wxBoxSizer(wxHORIZONTAL);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxBitmap(300, 220));
    body->Add(m_preview, 0, wxRIGHT | wxBOTTOM, 12);

    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(new wxStaticText(this, wxID_ANY, _L("Palette and projection result")), 0, wxBOTTOM, 6);
    m_palette_list = new wxListBox(this, wxID_ANY);
    right->Add(m_palette_list, 1, wxEXPAND | wxBOTTOM, 8);
    m_result_label = new wxStaticText(this, wxID_ANY, _L("Load an image to calculate a projection preview."));
    m_result_label->Wrap(440);
    right->Add(m_result_label, 0, wxEXPAND);
    body->Add(right, 1, wxEXPAND);
    root->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* close_btn = new wxButton(this, wxID_CANCEL, _L("Close"));
    buttons->Add(close_btn, 0, wxRIGHT, 8);
    m_apply_button = new wxButton(this, wxID_OK, _L("Apply to selected volume"));
    m_apply_button->Bind(wxEVT_BUTTON, &SurfacePainterDialog::on_apply, this);
    m_apply_button->Enable(false);
    buttons->Add(m_apply_button, 0);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    this->SetSizer(root);
}

void SurfacePainterDialog::apply_colors()
{
#ifdef _WIN32
    wxGetApp().UpdateDlgDarkUI(this);
#endif
    this->SetBackgroundColour(window_bg_color());
}

void SurfacePainterDialog::on_load_image(wxCommandEvent&)
{
    wxFileDialog dialog(
        this,
        _L("Choose image for Surface Painter"),
        wxEmptyString,
        wxEmptyString,
        _L("Image files (*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff)|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|All files (*.*)|*.*"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (dialog.ShowModal() != wxID_OK)
        return;

    if (!load_image(dialog.GetPath())) {
        ErrorDialog(this, _L("The selected image could not be loaded."), false).ShowModal();
        return;
    }

    if (m_palette_size != nullptr) {
        const int detected_colors = detected_image_color_count();
        m_palette_size->SetValue(detected_colors);
        if (m_target_choice != nullptr && detected_colors > int(m_num_physical))
            m_target_choice->SetSelection(1);
    }

    fit_image_to_decal();
    rebuild_preview();
    rebuild_palette();
    run_projection_probe();
    refresh_result_ui();
    apply_preview_if_enabled();
}

void SurfacePainterDialog::on_controls_changed(wxCommandEvent&)
{
    if (!m_bitmap.valid())
        return;
    rebuild_palette();
    run_projection_probe();
    refresh_result_ui();
    apply_preview_if_enabled();
}

void SurfacePainterDialog::on_preview(wxCommandEvent&)
{
    if (m_target_volume == nullptr || !m_bitmap.valid() || m_palette.empty())
        return;

    ensure_preview_virtual_extruders();
    const bool keep_existing_on_empty = m_fast_preview || m_dragging_placement || m_rotating_placement;
    const size_t painted = apply_to_volume(*m_target_volume, m_original_mesh ? &*m_original_mesh : nullptr, keep_existing_on_empty);
    const bool kept_previous_preview = keep_existing_on_empty && painted == 0 && m_last_painted_facets > 0;
    if (!kept_previous_preview)
        m_last_painted_facets = painted;
    m_preview_was_applied = true;
    if (m_preview_callback)
        m_preview_callback(kept_previous_preview ? m_last_painted_facets : painted);
}

void SurfacePainterDialog::on_apply(wxCommandEvent&)
{
    m_create_virtual_extruders = target_kind() == SurfacePainter::AssignmentKind::VirtualExtruder
        && !m_generated_virtual_extruders.empty();
    restore_preview_virtual_extruders();

    if (m_apply_callback) {
        if (m_apply_callback(*this)) {
            m_committed = true;
            Close();
        }
        return;
    }

    m_committed = true;
    if (IsModal())
        EndModal(wxID_OK);
    else
        Close();
}

void SurfacePainterDialog::on_fit_image(wxCommandEvent&)
{
    fit_image_to_decal();
    refresh_after_transform_change(true);
}

void SurfacePainterDialog::on_center_image(wxCommandEvent&)
{
    center_image_in_decal();
    refresh_after_transform_change(true);
}

void SurfacePainterDialog::on_reset_transform(wxCommandEvent&)
{
    if (m_repeat_image != nullptr)
        m_repeat_image->SetValue(false);
    fit_image_to_decal();
    refresh_after_transform_change(true);
}

void SurfacePainterDialog::on_canvas_mouse(wxMouseEvent& event)
{
    if (m_mouse_placement == nullptr || !m_mouse_placement->GetValue() || !IsShown() || !m_bitmap.valid()) {
        event.Skip();
        return;
    }

    wxWindow* event_window = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (event.LeftDown()) {
        m_dragging_placement = true;
        m_rotating_placement = false;
        m_last_mouse_position = event.GetPosition();
        m_last_drag_preview_time = std::chrono::steady_clock::time_point{};
        center_decal_at_mouse(m_last_mouse_position, true);
        refresh_after_transform_change();
        if (event_window != nullptr && !event_window->HasCapture())
            event_window->CaptureMouse();
        return;
    }

    if (event.RightDown()) {
        m_rotating_placement = true;
        m_dragging_placement = false;
        m_last_mouse_position = event.GetPosition();
        m_last_drag_preview_time = std::chrono::steady_clock::time_point{};
        if (event_window != nullptr && !event_window->HasCapture())
            event_window->CaptureMouse();
        return;
    }

    if (event.LeftUp()) {
        if (event_window != nullptr && event_window->HasCapture())
            event_window->ReleaseMouse();
        refresh_after_transform_change(true);
        m_dragging_placement = false;
        return;
    }

    if (event.RightUp()) {
        if (event_window != nullptr && event_window->HasCapture())
            event_window->ReleaseMouse();
        refresh_after_transform_change(true);
        m_rotating_placement = false;
        return;
    }

    if (event.Dragging() && event.LeftIsDown() && m_dragging_placement) {
        const wxPoint position = event.GetPosition();
        const int dx = position.x - m_last_mouse_position.x;
        const int dy = position.y - m_last_mouse_position.y;
        m_last_mouse_position = position;

        const wxSize size = event_window != nullptr ? event_window->GetClientSize() : wxSize(1000, 800);
        if (!center_decal_at_mouse(position, false)) {
            const double modifier = event.ControlDown() ? 4.0 : (event.ShiftDown() ? 0.2 : 1.0);
            const double u_step = 2.5 * std::max(0.05, m_scale_u->GetValue()) * modifier / double(std::max(1, size.GetWidth()));
            const double v_step = 2.5 * std::max(0.05, m_scale_v->GetValue()) * modifier / double(std::max(1, size.GetHeight()));
            set_spin_value_clamped(m_offset_u, m_offset_u->GetValue() - double(dx) * u_step);
            set_spin_value_clamped(m_offset_v, m_offset_v->GetValue() + double(dy) * v_step);
        }
        refresh_after_transform_change();
        return;
    }

    if (event.Dragging() && event.RightIsDown() && m_rotating_placement) {
        const wxPoint position = event.GetPosition();
        const int dx = position.x - m_last_mouse_position.x;
        const int dy = position.y - m_last_mouse_position.y;
        m_last_mouse_position = position;

        const double modifier = event.ControlDown() ? 3.0 : (event.ShiftDown() ? 0.2 : 1.0);
        set_spin_value_clamped(m_rotation_degrees, m_rotation_degrees->GetValue() + double(dx + dy) * 0.5 * modifier);
        refresh_after_transform_change();
        return;
    }

    if (event.GetWheelRotation() != 0) {
        const std::optional<SurfacePainter::UV> anchor = surface_uv_at_mouse(event.GetPosition());
        const int delta = event.GetWheelDelta() == 0 ? 120 : event.GetWheelDelta();
        const double steps = double(event.GetWheelRotation()) / double(delta);
        const double base = event.ControlDown() ? 1.35 : (event.ShiftDown() ? 1.03 : 1.15);
        const double factor = std::pow(base, steps);
        set_spin_value_clamped(m_scale_u, m_scale_u->GetValue() * factor);
        set_spin_value_clamped(m_scale_v, m_scale_v->GetValue() * factor);
        if (anchor)
            center_decal_at_uv(*anchor);
        refresh_after_transform_change();
        return;
    }

    event.Skip();
}

bool SurfacePainterDialog::load_image(const wxString& path)
{
    wxImage image;
    if (!image.LoadFile(path) || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return false;

    m_image_path = path;
    m_file_label->SetLabel(path);

    m_bitmap.width = static_cast<size_t>(image.GetWidth());
    m_bitmap.height = static_cast<size_t>(image.GetHeight());
    m_bitmap.pixels.clear();
    m_bitmap.pixels.reserve(m_bitmap.width * m_bitmap.height);

    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    for (size_t i = 0; i < m_bitmap.width * m_bitmap.height; ++i) {
        m_bitmap.pixels.push_back(SurfacePainter::ColorRGBA{
            rgb[i * 3 + 0],
            rgb[i * 3 + 1],
            rgb[i * 3 + 2],
            alpha ? alpha[i] : uint8_t(255),
        });
    }

    return true;
}

void SurfacePainterDialog::rebuild_preview()
{
    if (!m_bitmap.valid())
        return;

    wxImage image(static_cast<int>(m_bitmap.width), static_cast<int>(m_bitmap.height));
    unsigned char* rgb = image.GetData();
    image.InitAlpha();
    unsigned char* alpha = image.GetAlpha();

    for (size_t i = 0; i < m_bitmap.pixels.size(); ++i) {
        rgb[i * 3 + 0] = m_bitmap.pixels[i].r;
        rgb[i * 3 + 1] = m_bitmap.pixels[i].g;
        rgb[i * 3 + 2] = m_bitmap.pixels[i].b;
        alpha[i] = m_bitmap.pixels[i].a;
    }

    const int max_w = 300;
    const int max_h = 220;
    const double scale = std::min(double(max_w) / double(image.GetWidth()), double(max_h) / double(image.GetHeight()));
    if (scale < 1.0)
        image.Rescale(std::max(1, int(image.GetWidth() * scale)), std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_BILINEAR);

    m_preview_bitmap = wxBitmap(image);
    m_preview->SetBitmap(m_preview_bitmap);
}

void SurfacePainterDialog::rebuild_palette()
{
    m_palette.clear();
    if (!m_bitmap.valid())
        return;

    std::map<int, Bucket> buckets;
    const size_t stride = std::max<size_t>(1, m_bitmap.pixels.size() / 20000);
    for (size_t i = 0; i < m_bitmap.pixels.size(); i += stride) {
        const SurfacePainter::ColorRGBA& color = m_bitmap.pixels[i];
        if (color.a < 16)
            continue;
        Bucket& bucket = buckets[bucket_key(color)];
        ++bucket.count;
        bucket.r += color.r;
        bucket.g += color.g;
        bucket.b += color.b;
    }

    std::vector<Bucket> sorted;
    sorted.reserve(buckets.size());
    for (const auto& pair : buckets)
        sorted.push_back(pair.second);
    std::sort(sorted.begin(), sorted.end(), [](const Bucket& a, const Bucket& b) {
        return a.count > b.count;
    });

    const int wanted = m_palette_size ? m_palette_size->GetValue() : 6;
    const int count = std::min<int>(wanted, static_cast<int>(sorted.size()));
    const SurfacePainter::AssignmentKind kind = target_kind();
    const unsigned int base_virtual_id = next_virtual_id();

    for (int i = 0; i < count; ++i) {
        const Bucket& bucket = sorted[i];
        const SurfacePainter::ColorRGBA color{
            static_cast<uint8_t>(bucket.r / bucket.count),
            static_cast<uint8_t>(bucket.g / bucket.count),
            static_cast<uint8_t>(bucket.b / bucket.count),
            255,
        };
        const unsigned int target_id = kind == SurfacePainter::AssignmentKind::VirtualExtruder
            ? base_virtual_id + static_cast<unsigned int>(i)
            : static_cast<unsigned int>(i % std::max(1u, m_num_physical)) + 1;
        m_palette.push_back(PaletteEntry{
            color,
            bucket.count,
            SurfacePainter::ColorTarget{color, kind, target_id},
        });
    }

    rebuild_generated_virtual_extruders();
}

void SurfacePainterDialog::run_projection_probe()
{
    m_samples.clear();
    if (!m_bitmap.valid() || m_palette.empty())
        return;

    m_samples = SurfacePainter::paint_surface_points(
        make_probe_points(),
        m_bitmap,
        projection_settings(),
        color_targets(),
        16
    );
}

void SurfacePainterDialog::refresh_result_ui()
{
    m_palette_list->Clear();

    std::vector<size_t> sample_counts(m_palette.size(), 0);
    for (const SurfacePainter::PaintedSample& sample : m_samples)
        if (sample.palette_index < sample_counts.size())
            ++sample_counts[sample.palette_index];

    for (size_t i = 0; i < m_palette.size(); ++i) {
        const PaletteEntry& entry = m_palette[i];
        const wxString target = entry.target.kind == SurfacePainter::AssignmentKind::VirtualExtruder
            ? wxString::Format(_L("VE %u"), entry.target.id)
            : wxString::Format(_L("E%u"), entry.target.id);
        m_palette_list->Append(wxString::Format(
            "%s %s  ->  %s    image %zu    probe %zu",
            color_to_label(entry.color),
            wxString::FromUTF8(color_name(entry.color)),
            target,
            entry.count,
            sample_counts[i]
        ));
    }

    const bool virtual_mode = target_kind() == SurfacePainter::AssignmentKind::VirtualExtruder;
    m_apply_button->SetLabel(virtual_mode ? _L("Create Color Mix targets and apply") : _L("Apply to selected volume"));
    m_apply_button->Enable(!m_palette.empty());
    if (m_preview_button != nullptr)
        m_preview_button->Enable(m_target_volume != nullptr && !m_palette.empty());
    if (!virtual_mode)
        m_generated_virtual_extruders.clear();

    m_result_label->SetLabel(wxString::Format(
        _L("Surface Painter core sampled %zu surface points. Model preview: %zu facets. Projection: %s. Target mode: %s. Detail: %d."),
        m_samples.size(),
        m_last_painted_facets,
        m_projection_choice->GetStringSelection(),
        m_target_choice->GetStringSelection(),
        detail_level()
    ));
    m_result_label->Wrap(440);
    this->Layout();
}

void SurfacePainterDialog::rebuild_generated_virtual_extruders()
{
    m_generated_virtual_extruders.clear();
    if (target_kind() != SurfacePainter::AssignmentKind::VirtualExtruder || m_num_physical < 1) {
        restore_preview_virtual_extruders();
        return;
    }

    for (const PaletteEntry& entry : m_palette) {
        FullSpectrum::VirtualExtruder ve;
        ve.id = entry.target.id;
        ve.color = color_to_hex(entry.color);
        ve.components = m_num_physical >= 2
            ? FullSpectrum::VirtualExtruderComponents{
                FullSpectrum::VirtualExtruderComponent{1, 0.5},
                FullSpectrum::VirtualExtruderComponent{2, 0.5},
            }
            : FullSpectrum::VirtualExtruderComponents{
                FullSpectrum::VirtualExtruderComponent{1, 1.0},
            };
        m_generated_virtual_extruders.push_back(std::move(ve));
    }
}

void SurfacePainterDialog::ensure_preview_virtual_extruders()
{
    if (target_kind() != SurfacePainter::AssignmentKind::VirtualExtruder || m_generated_virtual_extruders.empty())
        return;

    std::vector<FullSpectrum::VirtualExtruder> merged = m_original_virtual_extruders;
    merged.insert(merged.end(), m_generated_virtual_extruders.begin(), m_generated_virtual_extruders.end());
    m_model.virtual_extruders = FullSpectrum::normalize_virtual_extruders(merged);
    m_preview_virtual_extruders_installed = true;
}

void SurfacePainterDialog::restore_preview_virtual_extruders()
{
    if (!m_preview_virtual_extruders_installed)
        return;

    m_model.virtual_extruders = m_original_virtual_extruders;
    m_preview_virtual_extruders_installed = false;
}

SurfacePainter::ProjectionSettings SurfacePainterDialog::projection_settings() const
{
    SurfacePainter::ProjectionSettings projection;
    projection.origin = Vec3d(0.0, 0.0, 0.0);
    projection.size = Vec3d(100.0, 100.0, 100.0);
    projection.cylinder_center = Vec3d(50.0, 50.0, 0.0);
    projection.cylinder_radius = 50.0;
    projection.scale_u = m_scale_u ? m_scale_u->GetValue() : 1.0;
    projection.scale_v = m_scale_v ? m_scale_v->GetValue() : 1.0;
    projection.offset_u = m_offset_u ? m_offset_u->GetValue() : 0.0;
    projection.offset_v = m_offset_v ? m_offset_v->GetValue() : 0.0;
    projection.rotation_radians = (m_rotation_degrees ? m_rotation_degrees->GetValue() : 0.0) * PI / 180.0;
    projection.repeat = m_repeat_image != nullptr && m_repeat_image->GetValue();
    projection.clamp = !projection.repeat;

    switch (m_projection_choice ? m_projection_choice->GetSelection() : 0) {
    case 1: projection.mode = SurfacePainter::ProjectionMode::PlanarXZ; break;
    case 2: projection.mode = SurfacePainter::ProjectionMode::PlanarYZ; break;
    case 3:
        projection.mode = SurfacePainter::ProjectionMode::CylindricalZ;
        break;
    default: projection.mode = SurfacePainter::ProjectionMode::PlanarXY; break;
    }
    return projection;
}

SurfacePainter::ProjectionSettings SurfacePainterDialog::projection_settings_for_volume(const ModelVolume& volume) const
{
    SurfacePainter::ProjectionSettings projection = projection_settings();
    const BoundingBoxf3 bbox = volume.mesh().bounding_box();
    Vec3d size = bbox.size();
    for (int axis = 0; axis < 3; ++axis)
        if (size(axis) <= 1e-6)
            size(axis) = 1.0;

    projection.origin = bbox.min;
    projection.size = size;
    projection.cylinder_center = Vec3d(bbox.center().x(), bbox.center().y(), bbox.min.z());
    projection.cylinder_radius = std::max(1e-6, 0.5 * std::max(size.x(), size.y()));
    return projection;
}

SurfacePainter::AssignmentKind SurfacePainterDialog::target_kind() const
{
    return m_target_choice && m_target_choice->GetSelection() == 1
        ? SurfacePainter::AssignmentKind::VirtualExtruder
        : SurfacePainter::AssignmentKind::FixedExtruder;
}

std::vector<SurfacePainter::ColorTarget> SurfacePainterDialog::color_targets() const
{
    std::vector<SurfacePainter::ColorTarget> targets;
    targets.reserve(m_palette.size());
    for (const PaletteEntry& entry : m_palette)
        targets.push_back(entry.target);
    return targets;
}

std::vector<SurfacePainter::SurfacePoint> SurfacePainterDialog::make_probe_points() const
{
    std::vector<SurfacePainter::SurfacePoint> points;
    points.reserve(32 * 32);

    if (projection_settings().mode == SurfacePainter::ProjectionMode::CylindricalZ) {
        size_t face_id = 0;
        for (int z = 0; z < 32; ++z) {
            for (int a = 0; a < 32; ++a) {
                const double angle = (2.0 * PI * double(a)) / 32.0;
                points.push_back(SurfacePainter::SurfacePoint{
                    Vec3d(50.0 + 50.0 * std::cos(angle), 50.0 + 50.0 * std::sin(angle), (100.0 * z) / 31.0),
                    Vec3d(std::cos(angle), std::sin(angle), 0.0),
                    face_id++,
                });
            }
        }
    } else {
        size_t face_id = 0;
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                points.push_back(SurfacePainter::SurfacePoint{
                    Vec3d((100.0 * x) / 31.0, (100.0 * y) / 31.0, 50.0),
                    Vec3d::UnitZ(),
                    face_id++,
                });
            }
        }
    }

    return points;
}

std::vector<SurfacePainter::SurfacePoint> SurfacePainterDialog::make_volume_points(const indexed_triangle_set& its) const
{
    std::vector<SurfacePainter::SurfacePoint> points;
    points.reserve(its.indices.size());

    for (size_t face_id = 0; face_id < its.indices.size(); ++face_id) {
        const stl_triangle_vertex_indices& face = its.indices[face_id];
        const Vec3d a = its.vertices[face[0]].cast<double>();
        const Vec3d b = its.vertices[face[1]].cast<double>();
        const Vec3d c = its.vertices[face[2]].cast<double>();
        Vec3d normal = (b - a).cross(c - a);
        if (normal.squaredNorm() > 1e-12)
            normal.normalize();
        else
            normal = Vec3d::UnitZ();

        points.push_back(SurfacePainter::SurfacePoint{
            (a + b + c) / 3.0,
            normal,
            face_id,
        });
    }

    return points;
}

size_t SurfacePainterDialog::apply_to_volume(ModelVolume& volume) const
{
    return apply_to_volume(volume, nullptr);
}

size_t SurfacePainterDialog::apply_to_volume(ModelVolume& volume, const indexed_triangle_set* source_mesh) const
{
    return apply_to_volume(volume, source_mesh, false);
}

size_t SurfacePainterDialog::apply_to_volume(ModelVolume& volume, const indexed_triangle_set* source_mesh, bool keep_existing_on_empty) const
{
    if (!m_bitmap.valid() || m_palette.empty() || volume.mesh().empty())
        return 0;

    const indexed_triangle_set& base_mesh = source_mesh != nullptr ? *source_mesh : volume.mesh().its;
    RefinedMesh refined = refine_mesh_for_surface_painting(base_mesh, m_bitmap, detail_level());
    const SurfacePainter::ProjectionSettings projection = projection_settings_for_volume(volume);

    std::vector<SurfacePainter::PaintedSample> painted = SurfacePainter::paint_surface_points(
        make_volume_points(refined.mesh),
        m_bitmap,
        projection,
        color_targets(),
        16
    );

    if (painted.empty() && keep_existing_on_empty)
        return 0;

    volume.set_mesh(std::move(refined.mesh));
    volume.calculate_convex_hull();
    volume.set_new_unique_id();

    TriangleSelector selector(volume.mesh());
    for (const SurfacePainter::PaintedSample& sample : painted)
        selector.set_facet(static_cast<int>(sample.face_id), static_cast<TriangleStateType>(sample.target.id));
    selector.garbage_collect();
    volume.mm_segmentation_facets.set(selector);

    return painted.size();
}

void SurfacePainterDialog::restore_target_volume()
{
    if (m_target_volume == nullptr || !m_original_mesh)
        return;

    m_target_volume->set_mesh(*m_original_mesh);
    m_target_volume->calculate_convex_hull();
    m_target_volume->set_new_unique_id();
    m_target_volume->mm_segmentation_facets.reset();

    if (m_original_had_mm_segmentation) {
        TriangleSelector selector(m_target_volume->mesh());
        selector.deserialize(m_original_mm_segmentation, false);
        m_target_volume->mm_segmentation_facets.set(selector);
    }

    if (m_preview_callback)
        m_preview_callback(0);
    restore_preview_virtual_extruders();
}

int SurfacePainterDialog::detail_level() const
{
    const int detail = m_detail_level != nullptr ? std::clamp(m_detail_level->GetValue(), 1, 16) : 6;
    return m_fast_preview ? std::min(detail, 2) : detail;
}

void SurfacePainterDialog::apply_preview_if_enabled(bool force)
{
    const bool live_preview = m_live_preview != nullptr && m_live_preview->GetValue();
    if (!force && !live_preview)
        return;

    wxCommandEvent event;
    on_preview(event);
}

void SurfacePainterDialog::refresh_after_transform_change(bool force)
{
    if (!m_bitmap.valid())
        return;

    const bool interactive_drag = m_dragging_placement || m_rotating_placement;
    if (interactive_drag && !force) {
        const bool preview_while_placing = m_preview_while_placing != nullptr && m_preview_while_placing->GetValue();
        if (!preview_while_placing) {
            run_projection_probe();
            refresh_result_ui();
            m_fast_preview = false;
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_last_drag_preview_time.time_since_epoch().count() != 0 &&
            now - m_last_drag_preview_time < std::chrono::milliseconds(80))
            return;

        m_last_drag_preview_time = now;
        m_fast_preview = true;
        apply_preview_if_enabled(true);
        m_fast_preview = false;
        return;
    }

    run_projection_probe();
    refresh_result_ui();
    m_fast_preview = false;
    apply_preview_if_enabled(force);
}

void SurfacePainterDialog::bind_canvas_events()
{
    if (m_canvas_events_bound || m_canvas == nullptr || m_canvas->get_wxglcanvas() == nullptr)
        return;

    wxWindow* canvas_window = m_canvas->get_wxglcanvas();
    canvas_window->Bind(wxEVT_LEFT_DOWN, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_LEFT_UP, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_RIGHT_DOWN, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_RIGHT_UP, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_MOTION, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_MOUSEWHEEL, &SurfacePainterDialog::on_canvas_mouse, this);
    m_canvas_events_bound = true;
}

void SurfacePainterDialog::unbind_canvas_events()
{
    if (!m_canvas_events_bound || m_canvas == nullptr || m_canvas->get_wxglcanvas() == nullptr)
        return;

    wxWindow* canvas_window = m_canvas->get_wxglcanvas();
    if (canvas_window->HasCapture())
        canvas_window->ReleaseMouse();
    canvas_window->Unbind(wxEVT_LEFT_DOWN, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Unbind(wxEVT_LEFT_UP, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Unbind(wxEVT_RIGHT_DOWN, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Unbind(wxEVT_RIGHT_UP, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Unbind(wxEVT_MOTION, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Unbind(wxEVT_MOUSEWHEEL, &SurfacePainterDialog::on_canvas_mouse, this);
    m_canvas_events_bound = false;
}

void SurfacePainterDialog::set_spin_value_clamped(wxSpinCtrlDouble* spin, double value)
{
    if (spin == nullptr)
        return;

    spin->SetValue(std::clamp(value, spin->GetMin(), spin->GetMax()));
}

void SurfacePainterDialog::set_transform_values(
    double scale_u,
    double scale_v,
    double offset_u,
    double offset_v,
    double rotation_degrees
)
{
    set_spin_value_clamped(m_scale_u, scale_u);
    set_spin_value_clamped(m_scale_v, scale_v);
    set_spin_value_clamped(m_offset_u, offset_u);
    set_spin_value_clamped(m_offset_v, offset_v);
    set_spin_value_clamped(m_rotation_degrees, rotation_degrees);
}

std::optional<std::pair<int, int>> SurfacePainterDialog::target_volume_indices() const
{
    if (m_target_volume == nullptr)
        return std::nullopt;

    for (size_t object_idx = 0; object_idx < m_model.objects.size(); ++object_idx) {
        const ModelObject* object = m_model.objects[object_idx];
        for (size_t volume_idx = 0; volume_idx < object->volumes.size(); ++volume_idx)
            if (object->volumes[volume_idx] == m_target_volume)
                return std::make_pair(static_cast<int>(object_idx), static_cast<int>(volume_idx));
    }

    return std::nullopt;
}

std::optional<SurfacePainter::UV> SurfacePainterDialog::surface_uv_at_mouse(const wxPoint& position) const
{
    if (m_canvas == nullptr || m_target_volume == nullptr)
        return std::nullopt;

    const std::optional<std::pair<int, int>> indices = target_volume_indices();
    if (!indices)
        return std::nullopt;

    const std::optional<GLCanvas3D::ModelVolumeHit> local_hit = m_canvas->mouse_hit_on_model_volume(
        Point(position.x, position.y),
        indices->first,
        indices->second
    );
    if (!local_hit)
        return std::nullopt;

    SurfacePainter::ProjectionSettings projection = projection_settings_for_volume(*m_target_volume);
    projection.scale_u = 1.0;
    projection.scale_v = 1.0;
    projection.offset_u = 0.0;
    projection.offset_v = 0.0;
    projection.rotation_radians = 0.0;
    projection.repeat = false;
    projection.clamp = true;

    return SurfacePainter::project_point(
        SurfacePainter::SurfacePoint{ local_hit->position, local_hit->normal, 0 },
        projection
    );
}

bool SurfacePainterDialog::center_decal_at_mouse(const wxPoint& position, bool auto_projection)
{
    if (auto_projection && m_canvas != nullptr) {
        const std::optional<std::pair<int, int>> indices = target_volume_indices();
        if (indices) {
            const std::optional<GLCanvas3D::ModelVolumeHit> local_hit = m_canvas->mouse_hit_on_model_volume(
                Point(position.x, position.y),
                indices->first,
                indices->second
            );
            if (local_hit)
                set_projection_from_hit(local_hit->position, local_hit->normal);
        }
    }

    const std::optional<SurfacePainter::UV> uv = surface_uv_at_mouse(position);
    if (!uv)
        return false;

    center_decal_at_uv(*uv);
    return true;
}

void SurfacePainterDialog::center_decal_at_uv(const SurfacePainter::UV& uv)
{
    const double scale_u = m_scale_u != nullptr ? m_scale_u->GetValue() : 0.45;
    const double scale_v = m_scale_v != nullptr ? m_scale_v->GetValue() : 0.45;
    set_spin_value_clamped(m_offset_u, uv.u - 0.5 * scale_u);
    set_spin_value_clamped(m_offset_v, uv.v - 0.5 * scale_v);
}

void SurfacePainterDialog::set_projection_from_hit(const Vec3d& position, const Vec3d& normal)
{
    if (m_projection_choice == nullptr)
        return;

    if (hit_looks_cylindrical(position, normal)) {
        if (m_projection_choice->GetSelection() != 3) {
            m_projection_choice->SetSelection(3);
            fit_image_to_decal();
        }
        return;
    }

    if (m_projection_choice->GetSelection() == 3)
        return;

    const Vec3d abs_normal(std::abs(normal.x()), std::abs(normal.y()), std::abs(normal.z()));
    int selection = 0;
    if (abs_normal.x() >= abs_normal.y() && abs_normal.x() >= abs_normal.z())
        selection = 2;
    else if (abs_normal.y() >= abs_normal.z())
        selection = 1;

    if (m_projection_choice->GetSelection() != selection)
        m_projection_choice->SetSelection(selection);
}

bool SurfacePainterDialog::hit_looks_cylindrical(const Vec3d& position, const Vec3d& normal) const
{
    if (m_target_volume == nullptr || m_target_volume->mesh().empty())
        return false;

    const BoundingBoxf3 bbox = m_target_volume->mesh().bounding_box();
    const Vec3d size = bbox.size();
    if (size.x() <= 1e-6 || size.y() <= 1e-6 || size.z() <= 1e-6)
        return false;

    const double xy_ratio = std::min(size.x(), size.y()) / std::max(size.x(), size.y());
    if (xy_ratio < 0.72)
        return false;

    const Vec2d radial(position.x() - bbox.center().x(), position.y() - bbox.center().y());
    if (radial.squaredNorm() <= 1e-8)
        return false;

    const Vec2d radial_normal = radial.normalized();
    const Vec2d surface_normal(normal.x(), normal.y());
    if (surface_normal.squaredNorm() <= 1e-8)
        return false;

    const double horizontal_normal = surface_normal.norm();
    const double radial_alignment = std::abs(surface_normal.normalized().dot(radial_normal));
    return horizontal_normal > 0.72 && radial_alignment > 0.82;
}

void SurfacePainterDialog::fit_image_to_decal()
{
    double scale_u = 0.45;
    double scale_v = 0.45;
    if (m_bitmap.valid() && m_bitmap.width > 0 && m_bitmap.height > 0) {
        const double aspect = double(m_bitmap.width) / double(m_bitmap.height);
        if (m_target_volume != nullptr && projection_settings().mode == SurfacePainter::ProjectionMode::CylindricalZ) {
            const SurfacePainter::ProjectionSettings projection = projection_settings_for_volume(*m_target_volume);
            const double circumference = 2.0 * PI * projection.cylinder_radius;
            const double height = std::max(1e-6, projection.size.z());
            double physical_width = std::min(0.35 * circumference, 0.70 * height * aspect);
            double physical_height = physical_width / aspect;
            if (physical_height > 0.75 * height) {
                physical_height = 0.75 * height;
                physical_width = physical_height * aspect;
            }

            scale_u = std::clamp(physical_width / std::max(1e-6, circumference), 0.02, 0.95);
            scale_v = std::clamp(physical_height / height, 0.02, 0.95);
        } else if (aspect >= 1.0)
            scale_v = scale_u / aspect;
        else
            scale_u = scale_v * aspect;
    }

    set_transform_values(
        scale_u,
        scale_v,
        (1.0 - scale_u) * 0.5,
        (1.0 - scale_v) * 0.5,
        0.0
    );
}

void SurfacePainterDialog::center_image_in_decal()
{
    const double scale_u = m_scale_u != nullptr ? m_scale_u->GetValue() : 0.45;
    const double scale_v = m_scale_v != nullptr ? m_scale_v->GetValue() : 0.45;
    set_transform_values(
        scale_u,
        scale_v,
        (1.0 - scale_u) * 0.5,
        (1.0 - scale_v) * 0.5,
        m_rotation_degrees != nullptr ? m_rotation_degrees->GetValue() : 0.0
    );
}

int SurfacePainterDialog::detected_image_color_count() const
{
    if (!m_bitmap.valid())
        return 2;

    std::map<int, size_t> exact_counts;
    std::map<int, size_t> bucket_counts;
    const size_t stride = std::max<size_t>(1, m_bitmap.pixels.size() / 250000);
    size_t opaque_samples = 0;

    for (size_t i = 0; i < m_bitmap.pixels.size(); i += stride) {
        const SurfacePainter::ColorRGBA& color = m_bitmap.pixels[i];
        if (color.a < 16)
            continue;
        ++opaque_samples;
        ++exact_counts[exact_color_key(color)];
        ++bucket_counts[bucket_key(color)];
    }

    if (opaque_samples == 0)
        return 2;

    if (exact_counts.size() >= 2 && exact_counts.size() <= 16)
        return static_cast<int>(exact_counts.size());

    std::vector<size_t> bucket_sizes;
    bucket_sizes.reserve(bucket_counts.size());
    for (const auto& pair : bucket_counts)
        bucket_sizes.push_back(pair.second);
    std::sort(bucket_sizes.begin(), bucket_sizes.end(), std::greater<size_t>());

    const size_t min_bucket_size = std::max<size_t>(1, opaque_samples / 200);
    int detected = 0;
    for (size_t count : bucket_sizes) {
        if (count < min_bucket_size)
            break;
        ++detected;
        if (detected >= 16)
            break;
    }

    return std::clamp(detected, 2, 16);
}

unsigned int SurfacePainterDialog::next_virtual_id() const
{
    unsigned int next_id = std::max<unsigned int>(
        1u,
        m_num_physical + static_cast<unsigned int>(m_original_virtual_extruders.size()) + 1
    );
    bool found_free_id = false;
    while (!found_free_id) {
        found_free_id = true;
        for (const FullSpectrum::VirtualExtruder& ve : m_original_virtual_extruders) {
            if (ve.id == next_id) {
                ++next_id;
                found_free_id = false;
                break;
            }
        }
    }
    return next_id;
}

} // namespace Slic3r::GUI
