#include "SurfacePainterDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

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

} // namespace

SurfacePainterDialog::SurfacePainterDialog(
    wxWindow* parent,
    const Model& model,
    const DynamicPrintConfig& full_config
) :
    DPIDialog(
        parent,
        wxID_ANY,
        _L("Surface Painter Lab"),
        wxDefaultPosition,
        wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER
    ),
    m_model(model)
{
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
    settings->Add(m_palette_size, 0);
    root->Add(settings, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

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
}

void SurfacePainterDialog::on_controls_changed(wxCommandEvent&)
{
    if (!m_bitmap.valid())
        return;
    rebuild_palette();
    run_projection_probe();
    refresh_result_ui();
}

void SurfacePainterDialog::on_apply(wxCommandEvent&)
{
    m_create_virtual_extruders = target_kind() == SurfacePainter::AssignmentKind::VirtualExtruder
        && !m_generated_virtual_extruders.empty();
    EndModal(wxID_OK);
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
    if (!virtual_mode)
        m_generated_virtual_extruders.clear();

    m_result_label->SetLabel(wxString::Format(
        _L("Surface Painter core sampled %zu surface points. Projection: %s. Target mode: %s."),
        m_samples.size(),
        m_projection_choice->GetStringSelection(),
        m_target_choice->GetStringSelection()
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

std::vector<SurfacePainter::SurfacePoint> SurfacePainterDialog::make_volume_points(const ModelVolume& volume) const
{
    const indexed_triangle_set& its = volume.mesh().its;
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
    if (!m_bitmap.valid() || m_palette.empty() || volume.mesh().empty())
        return 0;

    std::vector<SurfacePainter::PaintedSample> painted = SurfacePainter::paint_surface_points(
        make_volume_points(volume),
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

unsigned int SurfacePainterDialog::next_virtual_id() const
{
    unsigned int next_id = std::max(100u, m_num_physical + 1);
    for (const FullSpectrum::VirtualExtruder& ve : m_model.virtual_extruders)
        next_id = std::max(next_id, ve.id + 1);
    return next_id;
}

} // namespace Slic3r::GUI
