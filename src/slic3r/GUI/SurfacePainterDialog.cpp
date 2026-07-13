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
    const Model& model,
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
    m_target_choice->SetSelection(0);
    m_target_choice->Bind(wxEVT_CHOICE, &SurfacePainterDialog::on_controls_changed, this);
    settings->Add(m_target_choice, 0, wxRIGHT, 14);

    settings->Add(new wxStaticText(this, wxID_ANY, _L("Colors")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_palette_size = new wxSpinCtrl(this, wxID_ANY);
    m_palette_size->SetRange(2, 16);
    m_palette_size->SetValue(6);
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
    auto add_transform_spin = [this, transform](const wxString& label, double value, double min_value, double max_value) {
        transform->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        auto* spin = new wxSpinCtrlDouble(this, wxID_ANY);
        spin->SetRange(min_value, max_value);
        spin->SetDigits(2);
        spin->SetIncrement(0.05);
        spin->SetValue(value);
        spin->Bind(wxEVT_SPINCTRLDOUBLE, &SurfacePainterDialog::on_controls_changed, this);
        spin->Bind(wxEVT_TEXT, &SurfacePainterDialog::on_controls_changed, this);
        transform->Add(spin, 0, wxRIGHT, 14);
        return spin;
    };
    m_scale_u = add_transform_spin(_L("Scale U"), 1.0, 0.05, 10.0);
    m_scale_v = add_transform_spin(_L("Scale V"), 1.0, 0.05, 10.0);
    m_offset_u = add_transform_spin(_L("Position U"), 0.0, -10.0, 10.0);
    m_offset_v = add_transform_spin(_L("Position V"), 0.0, -10.0, 10.0);
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
    m_mouse_placement->Enable(m_target_volume != nullptr && m_canvas != nullptr);
    preview_row->Add(m_mouse_placement, 0, wxALIGN_CENTER_VERTICAL);
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

    const size_t painted = apply_to_volume(*m_target_volume, m_original_mesh ? &*m_original_mesh : nullptr);
    if (painted > 0) {
        m_preview_was_applied = true;
        if (m_preview_callback)
            m_preview_callback(painted);
    }
}

void SurfacePainterDialog::on_apply(wxCommandEvent&)
{
    m_create_virtual_extruders = target_kind() == SurfacePainter::AssignmentKind::VirtualExtruder
        && !m_generated_virtual_extruders.empty();
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

void SurfacePainterDialog::on_canvas_mouse(wxMouseEvent& event)
{
    if (m_mouse_placement == nullptr || !m_mouse_placement->GetValue() || !IsShown()) {
        event.Skip();
        return;
    }

    wxWindow* event_window = dynamic_cast<wxWindow*>(event.GetEventObject());
    if (event.LeftDown()) {
        m_dragging_placement = true;
        m_last_mouse_position = event.GetPosition();
        if (event_window != nullptr && !event_window->HasCapture())
            event_window->CaptureMouse();
        return;
    }

    if (event.LeftUp()) {
        m_dragging_placement = false;
        if (event_window != nullptr && event_window->HasCapture())
            event_window->ReleaseMouse();
        return;
    }

    if (event.Dragging() && event.LeftIsDown() && m_dragging_placement) {
        const wxPoint position = event.GetPosition();
        const int dx = position.x - m_last_mouse_position.x;
        const int dy = position.y - m_last_mouse_position.y;
        m_last_mouse_position = position;

        constexpr double sensitivity = 0.003;
        set_spin_value_clamped(m_offset_u, m_offset_u->GetValue() - double(dx) * sensitivity);
        set_spin_value_clamped(m_offset_v, m_offset_v->GetValue() + double(dy) * sensitivity);
        refresh_after_transform_change();
        return;
    }

    if (event.GetWheelRotation() != 0) {
        const int delta = event.GetWheelDelta() == 0 ? 120 : event.GetWheelDelta();
        const double steps = double(event.GetWheelRotation()) / double(delta);
        const double factor = std::pow(1.08, steps);
        set_spin_value_clamped(m_scale_u, m_scale_u->GetValue() * factor);
        set_spin_value_clamped(m_scale_v, m_scale_v->GetValue() * factor);
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
            "%s  ->  %s    image %zu    probe %zu",
            color_to_label(entry.color),
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
        _L("Surface Painter core sampled %zu surface points. Projection: %s. Target mode: %s. Detail: %d."),
        m_samples.size(),
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
    if (target_kind() != SurfacePainter::AssignmentKind::VirtualExtruder || m_num_physical < 2)
        return;

    for (const PaletteEntry& entry : m_palette) {
        FullSpectrum::VirtualExtruder ve;
        ve.id = entry.target.id;
        ve.color = color_to_hex(entry.color);
        ve.components = {
            FullSpectrum::VirtualExtruderComponent{1, 0.5},
            FullSpectrum::VirtualExtruderComponent{2, 0.5},
        };
        m_generated_virtual_extruders.push_back(std::move(ve));
    }
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
    projection.clamp = true;

    switch (m_projection_choice ? m_projection_choice->GetSelection() : 0) {
    case 1: projection.mode = SurfacePainter::ProjectionMode::PlanarXZ; break;
    case 2: projection.mode = SurfacePainter::ProjectionMode::PlanarYZ; break;
    case 3:
        projection.mode = SurfacePainter::ProjectionMode::CylindricalZ;
        projection.clamp = false;
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
    if (!m_bitmap.valid() || m_palette.empty() || volume.mesh().empty())
        return 0;

    const indexed_triangle_set& base_mesh = source_mesh != nullptr ? *source_mesh : volume.mesh().its;
    RefinedMesh refined = refine_mesh_for_surface_painting(base_mesh, m_bitmap, detail_level());
    volume.set_mesh(std::move(refined.mesh));
    volume.calculate_convex_hull();
    volume.set_new_unique_id();

    std::vector<SurfacePainter::PaintedSample> painted = SurfacePainter::paint_surface_points(
        make_volume_points(volume.mesh().its),
        m_bitmap,
        projection_settings_for_volume(volume),
        color_targets(),
        16
    );

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
}

int SurfacePainterDialog::detail_level() const
{
    return m_detail_level != nullptr ? std::clamp(m_detail_level->GetValue(), 1, 16) : 6;
}

void SurfacePainterDialog::apply_preview_if_enabled()
{
    const bool live_preview = m_live_preview != nullptr && m_live_preview->GetValue();
    const bool mouse_preview = m_mouse_placement != nullptr && m_mouse_placement->GetValue();
    if (!live_preview && !mouse_preview)
        return;

    wxCommandEvent event;
    on_preview(event);
}

void SurfacePainterDialog::refresh_after_transform_change()
{
    if (!m_bitmap.valid())
        return;

    run_projection_probe();
    refresh_result_ui();
    apply_preview_if_enabled();
}

void SurfacePainterDialog::bind_canvas_events()
{
    if (m_canvas_events_bound || m_canvas == nullptr || m_canvas->get_wxglcanvas() == nullptr)
        return;

    wxWindow* canvas_window = m_canvas->get_wxglcanvas();
    canvas_window->Bind(wxEVT_LEFT_DOWN, &SurfacePainterDialog::on_canvas_mouse, this);
    canvas_window->Bind(wxEVT_LEFT_UP, &SurfacePainterDialog::on_canvas_mouse, this);
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

unsigned int SurfacePainterDialog::next_virtual_id() const
{
    unsigned int next_id = std::max(100u, m_num_physical + 1);
    for (const FullSpectrum::VirtualExtruder& ve : m_model.virtual_extruders)
        next_id = std::max(next_id, ve.id + 1);
    return next_id;
}

} // namespace Slic3r::GUI
