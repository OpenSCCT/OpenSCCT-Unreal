"""Build a clear, radially refractive coating from centered XY coordinates and a Z pane mask."""
import unreal

from scct_materials import FLOAT1, FLOAT2


def coat_sight_glass(g, coords, *, translucency_pass, scope_handoff=False):
    material = g.material
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    material.set_editor_property("translucency_pass", translucency_pass)
    material.set_editor_property("refraction_method", unreal.RefractionMode.RM_2D_OFFSET)
    # The optic covers the entire bore, while its thin coating only absorbs a fraction of the light.
    material.set_editor_property("refraction_coverage_mode", unreal.RefractionCoverageMode.RCM_COVERAGE_IGNORED)
    material.set_editor_property("two_sided", False)

    rim = g.custom("return smoothstep(0.40, 1.0, length(C.xy));", FLOAT1, C=coords)
    weight_input = g.scalar("ScopeGlassWeight", 1.0) if scope_handoff else g.node(unreal.MaterialExpressionConstant, r=1.0)
    pane = g.custom("return C.z * Weight;", FLOAT1, C=coords, Weight=weight_input)
    view = g.node(unreal.MaterialExpressionCameraVectorWS)
    normal = g.node(unreal.MaterialExpressionPixelNormalWS)
    grazing = g.custom("float c = 1.0 - saturate(abs(dot(V,N))); return c*c*c;", FLOAT1, V=view, N=normal)
    # Coverage weights the coated layer over the visible scene. It stays optically clear at
    # the aim point, even when forward translucency has no reflection probe in a dark room.
    coverage = g.custom("return Pane * saturate(.022 + .34*Rim + .14*Grazing);",
                        FLOAT1, Pane=pane, Rim=rim, Grazing=grazing)
    slab = g.node(unreal.MaterialExpressionSubstrateSlabBSDF)
    pigment = g.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(.016,.046,.062,1))
    g.link(pigment, slab, "Diffuse Albedo")
    g.link(g.node(unreal.MaterialExpressionConstant, r=.04), slab, "F0")
    g.link(g.node(unreal.MaterialExpressionConstant, r=.135), slab, "Roughness")
    weight = g.node(unreal.MaterialExpressionSubstrateWeight)
    g.link(slab, weight, "A")
    g.link(coverage, weight, "Weight")
    g.output(weight, unreal.MaterialProperty.MP_FRONT_MATERIAL)
    # Remap the outer annulus inside the modeled aperture. Straight lines bow and then compress
    # at the rim. Sampling inward keeps the nearer housing outside the optical path.
    bend = g.custom("""
        float radius = length(C.xy);
        // Keep 72% of the radius straight. This profile peaks near 91% of the radius
        // and joins the clear centre and fixed housing smoothly without reversing the image.
        float edge = saturate((radius - .72) / .28);
        float edge2 = edge * edge;
        float lip = 1.0 - edge;
        float remapDistance = 3.0 * edge2 * edge2 * lip * lip;
        float2 lensOffset = -C.xy * (Pane * remapDistance / max(radius, 1e-5));

        // Invert the projected aperture's Jacobian, including rotation and foreshortening at the hip.
        float2 xAxis = ddx(C.xy), yAxis = ddy(C.xy);
        float determinant = xAxis.x*yAxis.y - xAxis.y*yAxis.x;
        if (abs(determinant) < 1e-10) return float2(0,0);
        float2 pixels = float2(lensOffset.x*yAxis.y - lensOffset.y*yAxis.x,
                               xAxis.x*lensOffset.y - xAxis.y*lensOffset.x) / determinant;

        // UE DistortionCommon.ush applies the unscaled output width and projection scale to
        // RM_2D_OFFSET on both axes. Cancel that conversion to preserve lens-relative displacement
        // at different screen percentages, output resolutions, aspect ratios and camera FOVs.
        float outputWidth = View.ViewSizeAndInvSize.x / max(View.ViewResolutionFraction, 1e-4);
        float distortionScale = max(.00023 * outputWidth * abs(View.ViewToClip[0][0]), 1e-4);
        return pixels * float2(1,-1) * View.ViewSizeAndInvSize.zw / distortionScale;
    """, FLOAT2, C=coords, Pane=pane)
    g.output(bend, unreal.MaterialProperty.MP_REFRACTION)
    # Explicit Substrate replaces the editor's legacy conversion node and its internal connections.
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError("\n".join(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError("Could not save " + material.get_path_name())
    return material
