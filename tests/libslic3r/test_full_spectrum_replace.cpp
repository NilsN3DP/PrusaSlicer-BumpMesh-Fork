#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

TEST_CASE("Full Spectrum replaces existing extruder assignments with a virtual extruder", "[FullSpectrum]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh{its_make_cube(10.0, 10.0, 10.0)});

    object->config.set("extruder", 2);
    volume->config.set("extruder", 2);

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, TriangleStateType::Extruder2);
    selector.set_facet(1, TriangleStateType::Extruder1);
    volume->mm_segmentation_facets.set(selector);

    REQUIRE(FullSpectrum::replace_model_extruder(model, 2, 5) == 3);

    CHECK(object->config.extruder() == 5);
    CHECK(volume->config.extruder() == 5);
    CHECK(volume->mm_segmentation_facets.has_facets(*volume, TriangleStateType::Extruder5));
    CHECK_FALSE(volume->mm_segmentation_facets.has_facets(*volume, TriangleStateType::Extruder2));
    CHECK(volume->mm_segmentation_facets.has_facets(*volume, TriangleStateType::Extruder1));
}
