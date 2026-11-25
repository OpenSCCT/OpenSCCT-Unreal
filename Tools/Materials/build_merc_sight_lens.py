"""Build the single refractive coating on the merc's first-person M4 micro red dot."""
from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Import"))
from scct_materials import Graph, FLOAT3
from scct_sight_glass import coat_sight_glass


def build():
    g = Graph("/Game/OpenSCCT/Materials/M_ScctMercSightLens", rebuild_in_place=True)
    uv = g.node(unreal.MaterialExpressionTextureCoordinate)
    coords = g.custom("return float3(UV * 2 - 1, 1);", FLOAT3, UV=uv)
    # After depth of field, as the ACOG glass and the scope picture: the maps' glass and water are drawn in that pass, and
    # a lens drawn before it had them laid over it untinted and unbent.
    coat_sight_glass(g, coords, translucency_pass=unreal.MaterialTranslucencyPass.MTP_AFTER_DOF,
                     magnification=1.07)
    unreal.log_warning("SCCT-IMPORT M4 forward optical coating saved")


build()
