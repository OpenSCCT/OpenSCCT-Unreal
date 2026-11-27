"""Build the refractive coating of the shared rifle red dot (SM_MercRedDot), the M4's optic on every other rifle."""
from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Import"))
from scct_materials import Graph, FLOAT3
from scct_sight_glass import coat_sight_glass


def build():
    g = Graph("/Game/OpenSCCT/Materials/M_ScctRifleDotLens", rebuild_in_place=True)
    # The static disc maps its aperture to UV 0..1 (build_merc_optic_cap.build(rifle_dot_only=True)). Centre it as the
    # M4's skinned coating does from its bind pose, so both rifles' glass tints and bends alike.
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    coords = g.custom("return float3(UV * 2 - 1, 1);", FLOAT3, UV=uv)
    # After depth of field, as the M4's coating: the maps' glass and water are drawn in that pass.
    material = coat_sight_glass(g, coords, translucency_pass=unreal.MaterialTranslucencyPass.MTP_AFTER_DOF)
    # The flat disc's winding faces the muzzle; seen from the eye it would be culled and the glass never drawn.
    material.set_editor_property("two_sided", True)
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError("\n".join(errors))
    if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
        raise RuntimeError("Could not save " + material.get_path_name())
    unreal.log_warning("SCCT-IMPORT rifle red dot coating saved")


build()
