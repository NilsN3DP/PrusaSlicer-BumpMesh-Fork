# Bump Mesh Fork Changes

This document lists the user-visible changes in this PrusaSlicer fork.

## Bump Mesh Gizmo

Adds a native **Bump Mesh** gizmo to PrusaSlicer.

Implemented areas:

* Native C++ displacement core in `src/libslic3r/BumpMesh.*`.
* Adaptive subdivision and displacement helpers.
* Bump Mesh GUI gizmo in `src/slic3r/GUI/Gizmos/GLGizmoBumpMesh.*`.
* Built-in texture assets in `resources/textures/bump_mesh/`.
* Toolbar icon in `resources/icons/bump_mesh.svg`.
* Unit tests in `tests/libslic3r/test_bump_mesh.cpp`.

## Bump Mesh Features

* Built-in displacement textures.
* Custom texture loading.
* Triplanar, Cubic, Cylindrical, Spherical, and Planar projections.
* Live geometry preview.
* Apply to selected volume or duplicate copy.
* Remove/restores the original mesh for the current session.
* Side selection for +X, -X, +Y, -Y, +Z, -Z.
* Print-safe preset.
* Advanced controls for height inversion, overhang protection, seams, offsets, rotation, angle limits, falloff, normal smoothing, and triangle cap.
* Auto-color preview.
* Auto-assignment to physical or virtual extruders.

## Validation

Local validation used while preparing this branch:

```powershell
cmake --build build-app --config RelWithDebInfo --target PrusaSlicer_app_gui --parallel 1
build-app\tests\libslic3r\RelWithDebInfo\libslic3r_tests.exe "[BumpMesh]"
build-app\src\RelWithDebInfo\prusa-slicer.exe --help
```

## Upstream Projects

This fork builds on:

* [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer), licensed under GNU AGPL v3.0.
* [BumpMesh by CNC Kitchen](https://bumpmesh.com/) / [CNCKitchen/stlTexturizer](https://github.com/CNCKitchen/stlTexturizer), also open source.

