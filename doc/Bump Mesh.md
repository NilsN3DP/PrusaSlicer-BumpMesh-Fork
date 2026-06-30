# Bump Mesh

Bump Mesh is a PrusaSlicer gizmo that bakes a grayscale displacement texture into the selected model volume. It is meant for printable surface detail such as brick, leather, carbon fiber, wood grain, grip patterns, knurling, and similar relief textures.

The implementation is based on the geometry ideas from [BumpMesh by CNC Kitchen](https://bumpmesh.com/) and the open-source [CNCKitchen/stlTexturizer](https://github.com/CNCKitchen/stlTexturizer) project, but it is integrated as native C++ code inside this PrusaSlicer fork.

## What It Does

* Loads built-in or custom grayscale textures.
* Projects the texture with Triplanar, Cubic, Cylindrical, Spherical, Planar XY, Planar XZ, or Planar YZ mapping.
* Adaptively subdivides the selected mesh before displacement.
* Displaces vertices along smoothed surface normals.
* Can limit the effect to selected model sides.
* Can preview displaced geometry before applying.
* Can split dark/light or depth regions into PrusaSlicer extruder painting.
* Can restore the original mesh during the same editing session.

## How To Use

1. Load a model and select one model volume.
2. Open the left toolbar gizmo named **Bump Mesh**.
3. Choose a built-in texture or load a custom image.
4. Pick a projection mode.
5. Adjust **Amplitude**, **Scale**, and **Detail**.
6. Enable or disable the target sides under **Sides**.
7. Use **Live geometry preview** to inspect the displaced shape.
8. Click **Apply** to bake the texture into the selected volume.

Use **Remove** to restore the original selected volume if Bump Mesh was applied during the current session.

## Main Controls

**Texture** selects a built-in image or a custom file. Dark and bright pixels become lower or higher parts of the final mesh depending on the displacement settings.

**Projection** controls how the 2D image is mapped onto the 3D object:

* **Triplanar** is the safest default for organic or mixed shapes.
* **Cubic (box)** is best for rectangular parts and crisp sides.
* **Cylindrical** wraps around the Z axis.
* **Spherical** wraps around the whole object.
* **Planar XY/XZ/YZ** project from a single axis plane.

**Amplitude** is the displacement height in millimeters.

**Scale** controls the size of the texture pattern on the model.

**Detail** controls subdivision density before displacement. Higher values preserve more texture detail but create more triangles.

**Apply to copy** duplicates the selected volume and applies Bump Mesh to the copy.

**Print safe** applies conservative settings for normal slicing: overhang protection, boundary falloff, capped detail, and triangle decimation.

## Side Selection

The **Sides** section lets you choose where displacement is allowed:

* Top (+Z)
* Bottom (-Z)
* Right (+X)
* Left (-X)
* Front (+Y)
* Back (-Y)

Disabled sides stay pinned so the mesh remains watertight at boundaries where possible. Use **Boundary falloff** in Advanced settings to soften transitions between enabled and disabled sides.

## Auto Color

Auto color is optional and is intended for multi-material or multi-color printing.

Enable **Auto color preview** to see the split on the model. Then choose:

* **Depth** to split colors by displacement height.
* **Texture dark/light** to split colors by the original texture brightness. This is useful for brick textures: dark mortar lines can use one extruder and bright brick faces another.

Enable **Live assign to extruders** to write the preview zones as real PrusaSlicer multi-material painting while you adjust the settings. The assignment is also written again after **Apply**.

If the project contains virtual extruders, Bump Mesh can use them in the same auto-color extruder fields as physical extruders.

## Advanced Settings

**Symmetric** treats 50 percent gray as neutral. Darker pixels move inward, brighter pixels move outward.

**Invert height** flips the texture so bright areas become low and dark areas become high.

**Overhang safe** prevents displacement from moving vertices downward in Z.

**Seam blend** softens projection seams, especially for Cubic and Cylindrical projection.

**Offset U / Offset V** move the texture in UV space.

**Rotation** rotates the texture projection.

**Top angle limit** and **Bottom angle limit** suppress displacement near upward or downward facing areas.

**Boundary falloff** fades displacement near masked or disabled areas.

**Blend normal smoothing** smooths normals used for projection blending.

**Max triangles** decimates the generated mesh after displacement. Set it to `0` to keep all generated detail.

## Texture Assets

Built-in textures live in:

```text
resources/textures/bump_mesh/
```

They are ordinary PNG/JPG assets and are shipped with the source tree so builds are reproducible.

## Limitations

* Bump Mesh changes the mesh geometry. It is not a shader and it is not the same as PrusaSlicer's fuzzy skin.
* Very high detail settings can create large meshes and slow slicing.
* Restore/remove is session-based: it can restore meshes that were modified since opening the current project/session.
* The feature is experimental and should be validated with real previews before long prints.

## Open Source And Attribution

This fork follows the upstream PrusaSlicer license, **GNU AGPL v3.0**. See [LICENSE](../LICENSE).

Attribution:

* PrusaSlicer: [prusa3d/PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)
* BumpMesh by CNC Kitchen: [bumpmesh.com](https://bumpmesh.com/)
* BumpMesh source project: [CNCKitchen/stlTexturizer](https://github.com/CNCKitchen/stlTexturizer)

