"""Build the single refractive coating on the merc's first-person M4 micro red dot."""
from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Import"))
from scct_materials import Graph, FLOAT3
from scct_sight_glass import coat_sight_glass


def build():
    g = Graph("/Game/OpenSCCT/Materials/M_ScctMercSightLens", rebuild_in_place=True)
    g.material.set_editor_property("used_with_skeletal_mesh", True)
    # Imported UE reference-pose positions in cm. Both panes share one slot. Use the forward pane:
    # the nearer one sits 5.7 cm behind the visible aperture and approaches the first-person near plane in ADS.
    # The separate reticle at X=31.17 is drawn by the rifle's authored material.
    bind_pos = g.node(unreal.MaterialExpressionPreSkinnedPosition)
    coating = g.custom("return float3((P.yz - float2(13.79,-5.30)) / 1.43, step(28.35,P.x));",
                       FLOAT3, P=bind_pos)
    coords = g.node(unreal.MaterialExpressionVertexInterpolator)
    g.link(coating, coords, "Input")
    # After depth of field, as the ACOG glass and the scope picture: the maps' glass and water are drawn in that pass, and
    # a lens drawn before it had them laid over it untinted and unbent.
    coat_sight_glass(g, coords, translucency_pass=unreal.MaterialTranslucencyPass.MTP_AFTER_DOF)
    unreal.log_warning("SCCT-IMPORT M4 forward optical coating saved")


build()
