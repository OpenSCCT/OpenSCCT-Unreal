"""Build detachable game props from the shipped OpenSCCT rifle's separate optic and suppressor geometry."""
import json
from pathlib import Path
import struct
import sys

import unreal as ue
from build_merc_optic_cap import build, save_mesh
from scct_attachment_finishes import finish


def build_attachments(pack_root, pistol_only=False, suppressor_only=False):
    path = pack_root / "shared/chars/OpenScctRifles/m4a1s.glb"
    data = path.read_bytes()
    json_length = struct.unpack_from("<I", data, 12)[0]
    model = json.loads(data[20:20+json_length])
    binary = memoryview(data)[28+json_length:]
    materials = model["materials"]

    def values(index):
        accessor = model["accessors"][index]
        view = model["bufferViews"][accessor["bufferView"]]
        element = struct.Struct("<" + {5121: "B", 5123: "H", 5125: "I", 5126: "f"}[accessor["componentType"]]
                                * {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[accessor["type"]])
        offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", element.size)
        return [element.unpack_from(binary, offset + i*stride) for i in range(accessor["count"])]

    def accessory(name, parts, origin, compact_dot=False):
        mesh = ue.DynamicMesh()
        slots, finishes = {}, []
        triangles = 0
        for part in parts:
            material = materials[part["material"]]["name"].replace(".", "_")
            compact_mark = compact_dot and material.endswith("_OpticReticle")
            if material not in slots:
                surface = ue.load_asset("/Game/OpenSCCT/Weapons/Materials/M_ScctAcogChevron") if compact_mark else finish(material.removeprefix("OpenScctRifles_"))
                if not surface:
                    raise RuntimeError("Missing attachment finish " + material)
                slots[material] = len(finishes)
                finishes.append(surface)
            attr = part["attributes"]
            points = [ue.Vector((x-origin[0])*100, (z-origin[2])*100, (y-origin[1])*100)
                      for x, y, z in values(attr["POSITION"])]
            if compact_mark:
                # The pistol mount scales the whole sight to 55%. Widen the physical
                # dot around the optical axis to keep a distinct center at 720p.
                for point in points:
                    point.y *= 2.3
                    point.z *= 2.3
            normals = [ue.Vector(x, z, y) for x, y, z in values(attr["NORMAL"])]
            uvs = ([ue.Vector2D(u, v) for u, v in values(attr["TEXCOORD_0"])]
                   if "TEXCOORD_0" in attr else [ue.Vector2D(0, 0)] * len(points))
            indices = [v[0] for v in values(part["indices"])]
            faces = [ue.IntVector(a, c, b) for a,b,c in zip(indices[::3], indices[1::3], indices[2::3])]
            triangles += len(faces)
            mesh.append_buffers_to_mesh(ue.GeometryScriptSimpleMeshBuffers(
                vertices=points, normals=normals, uv0=uvs, triangles=faces), material_id=slots[material])
        if not triangles:
            raise ValueError("No geometry in " + name)
        save_mesh(mesh, name, triangles, finishes)

    meshes = {m["name"]: m["primitives"] for m in model["meshes"]}
    suppressor = [p for p in meshes["m4a1s"] if materials[p["material"]]["name"].endswith(".Suppressor")]
    if suppressor_only:
        accessory("SM_MercSuppressor", suppressor, (.55, .096, 0))
        return
    # Centres are in the game model's bind pose. Socket X follows the barrel.
    accessory("SM_MercPistolRedDot", meshes["MicroRedDot"], (.118, .1763, 0), compact_dot=True)
    if pistol_only:
        return
    accessory("SM_MercRedDot", meshes["MicroRedDot"], (.118, .1763, 0))
    accessory("SM_MercSuppressor", suppressor, (.55, .096, 0))
    build(torch_only=True)


if __name__ == "__main__":
    build_attachments(Path(sys.argv[1]), "--pistol-dot-only" in sys.argv[2:], "--suppressor-only" in sys.argv[2:])
