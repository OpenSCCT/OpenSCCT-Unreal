"""Build the merc's sight and rail accessory meshes in Unreal centimetres."""
import math
import unreal as ue
from scct_attachment_finishes import finish


def save_mesh(mesh, name, triangle_count, materials, sockets=()):
    path = "/Game/OpenSCCT/Weapons/" + name
    asset = ue.load_asset(path) if ue.EditorAssetLibrary.does_asset_exist(path) else None
    if asset:
        lod = ue.GeometryScriptMeshWriteLOD()
        options = ue.GeometryScriptCopyMeshToAssetOptions()
        _, outcome = ue.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(mesh, asset, options, lod)
    else:
        options = ue.GeometryScriptCreateNewStaticMeshAssetOptions(enable_recompute_tangents=True, enable_collision=False)
        asset, outcome = ue.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(mesh, path, options)
    if asset is None or outcome != ue.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError("Could not build " + path + ": " + str(outcome))
    slots = [ue.StaticMaterial(material_interface=m, material_slot_name=m.get_name()) for m in materials]
    asset.set_editor_property("static_materials", slots)
    for label, location in sockets:
        socket = asset.find_socket(label)
        if not socket:
            socket = ue.StaticMeshSocket(outer=asset)
            socket.set_editor_property("socket_name", label)
            asset.add_socket(socket)
        socket.set_editor_property("relative_location", ue.Vector(*location))
        socket.set_editor_property("relative_rotation", ue.Rotator())
    if not ue.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError("Could not save " + path)
    for label, location in sockets:
        socket = asset.find_socket(label)
        if not socket or not socket.relative_location.equals(ue.Vector(*location)):
            raise RuntimeError("Missing or misplaced " + label + " on " + path)
    ue.log("SCCT-CAP %s: %s triangles, %s sections, materials %s, bounds %s" %
           (name, triangle_count, asset.get_num_sections(0), [m.get_name() for m in materials], asset.get_bounding_box()))


def build(torch_only=False, *, include_sight=True, rifle_dot_only=False):
    materials = [finish(suffix) for suffix in ("OpticRubber", "OpticAnodized")]
    mesh = ue.DynamicMesh()
    vertices, normals, triangles, uvs = [], [], [], []
    triangle_count = 0

    def flush(material_id):
        nonlocal triangle_count
        triangle_count += len(triangles)
        buffers = ue.GeometryScriptSimpleMeshBuffers(vertices=vertices, normals=normals, uv0=uvs, triangles=triangles)
        mesh.append_buffers_to_mesh(buffers, material_id=material_id)
        vertices.clear()
        normals.clear()
        triangles.clear()
        uvs.clear()

    def lathe(profile, sections, transform, strips=None):
        # Keep the profile normal at a material boundary so the metal and rubber meet smoothly.
        offsets = {}
        def ring(j):
            if j in offsets:
                return offsets[j]
            offsets[j] = len(vertices)
            along, radius = profile[j]
            a0, r0 = profile[j - 1]
            a1, r1 = profile[(j + 1) % len(profile)]
            na, nr = r0 - r1, a1 - a0
            length = math.hypot(na, nr)
            for i in range(sections):
                c, s = math.cos(math.tau * i / sections), math.sin(math.tau * i / sections)
                vertices.append(ue.Vector(*transform(along, c * radius, s * radius, False)))
                normals.append(ue.Vector(*transform(na / length, c * nr / length, s * nr / length, True)))
                uvs.append(ue.Vector2D(i / sections, j / len(profile)))
            return offsets[j]
        if strips is None:
            strips = range(len(profile))
        for j in strips:
            start, end = ring(j), ring((j + 1) % len(profile))
            for i in range(sections):
                following = (i + 1) % sections
                triangles.extend((ue.IntVector(start+i, start+following, end+i),
                                  ue.IntVector(start+following, end+following, end+i)))

    # The cover is retained by the front housing. Its slight forward tilt exposes the inner rubber lip.
    tilt = math.radians(30)
    c, s = math.cos(tilt), math.sin(tilt)
    def lid(x, y, z, direction):
        if not direction:
            z += 1.53
        return (c*x + s*z + (0 if direction else 3.05), y,
                -s*x + c*z + (0 if direction else 1.80))

    lathe([(-.24, 1.43), (-.18, 1.51), (-.09, 1.54), (.12, 1.54), (.20, 1.46),
           (.20, 1.24), (.13, 1.16), (.04, 1.15), (-.16, 1.17), (-.24, 1.25)], 64, lid)

    def hinge(x, y, z, direction):
        return (z + (0 if direction else 3.05), x, y + (0 if direction else 1.80))

    # The interleaved knuckles share a single recessed pin. Their bevels read at full sight magnification.
    for lo, hi in [(-.77, -.38), (-.35, .35), (.38, .77)]:
        lathe([(lo, .185), (lo+.025, .215), (hi-.025, .215), (hi, .185), (hi, .105), (lo, .105)], 24, hinge)
    lathe([(-.78, .055), (-.77, .111), (.77, .111), (.78, .055), (.78, .025), (-.78, .025)], 16, hinge)
    flush(0)
    if not torch_only and include_sight and not rifle_dot_only:
        save_mesh(mesh, "SM_MercOpticCap", triangle_count, materials[:1])

    # The sight looks through the two existing animated panes at X -3.0 and +2.7. Tapering
    # the ocular end preserves the rear pane while keeping the rim narrow in first person.
    def bore(x, y, z, direction):
        return x, y, z

    lathe([(-3.46, 1.305), (-3.42, 1.335), (-3.34, 1.355), (-3.16, 1.348), (-3.11, 1.325),
           (-2.91, 1.325), (-2.84, 1.355), (-2.56, 1.373), (1.89, 1.545), (2.12, 1.565),
           (2.88, 1.59), (3.15, 1.59), (3.25, 1.565), (3.25, 1.49), (3.17, 1.46),
           (2.67, 1.45), (-2.98, 1.175), (-3.41, 1.17), (-3.46, 1.20)], 72, bore)

    def cap_z(x, y, z, direction):
        # Axis is Z; right and forward form the circular cross section.
        return (y + (0 if direction else -.08), z, x + (0 if direction else -.09))

    # Short domed elevation control, below the cover window.
    lathe([(1.48, .40), (1.53, .60), (1.68, .64), (1.75, .59), (1.79, .61),
           (1.88, .61), (2.02, .54), (2.13, .38), (2.19, .20), (2.20, .012), (1.48, .012)], 36, cap_z)

    def cap_y(x, y, z, direction):
        return (z + (0 if direction else .92), x + (0 if direction else -.12), y)

    # A compact round battery control on the far side of the housing.
    lathe([(1.43, .51), (1.49, .59), (1.62, .60), (1.69, .58), (1.76, .57),
           (1.80, .47), (1.82, .16), (1.82, .012), (1.43, .012)], 40, cap_y)

    def mount_tier(length, width, bottom, top, bevel, center_x=0., center_y=0.):
        x, y = length * .5, width * .5
        corners = [(-x+bevel,-y), (x-bevel,-y), (x,-y+bevel), (x,y-bevel), (x-bevel,y),
                   (-x+bevel,y), (-x,y-bevel), (-x,-y+bevel)]
        def quad_face(points, normal):
            base = len(vertices)
            for px, py, pz in points:
                vertices.append(ue.Vector(px + center_x, py + center_y, pz))
                normals.append(ue.Vector(*normal))
                if normal[2] != 0:
                    u, v = px, py
                else:
                    u, v = px * -normal[1] + py * normal[0], pz
                uvs.append(ue.Vector2D(u/6+.5, v/6+.5))
            for k in range(1, len(points) - 1):
                triangles.append(ue.IntVector(base, base+k, base+k+1))
        for i, (u,v) in enumerate(corners):
            p,q = corners[(i+1)%len(corners)]
            side = math.hypot(p-u,q-v)
            quad_face([(u,v,bottom),(p,q,bottom),(p,q,top),(u,v,top)], ((q-v)/side,(u-p)/side,0))
        quad_face([(u,v,top) for u,v in corners], (0,0,1))
        quad_face([(u,v,bottom) for u,v in reversed(corners)], (0,0,-1))

    mount_tier(4.6, 2.22, -2.35, -1.30, .27)
    mount_tier(4.3, 1.95, -4.24, -2.19, .25)
    mount_tier(5.2, 2.97, -4.91, -4.13, .29)
    mount_tier(2.0, 1.90, -5.27, -4.85, .12)
    flush(1)

    if not torch_only and include_sight and not rifle_dot_only:
        save_mesh(mesh, "SM_MercOpticHousing", triangle_count, materials)

    if rifle_dot_only:
        def disc(x, radius, slot):
            vertices.append(ue.Vector(x, 0, 0))
            normals.append(ue.Vector(-1, 0, 0))
            uvs.append(ue.Vector2D(.5, .5))
            for i in range(72):
                angle = math.tau * i / 72
                y, z = math.cos(angle), math.sin(angle)
                vertices.append(ue.Vector(x, radius*y, radius*z))
                normals.append(ue.Vector(-1, 0, 0))
                uvs.append(ue.Vector2D(.5 + .5*y, .5 + .5*z))
                triangles.append(ue.IntVector(0, 1+(i+1)%72, 1+i))
            flush(slot)

        surfaces = [ue.load_asset("/Game/OpenSCCT/Materials/M_ScctRifleDotLens"),
                    ue.load_asset("/Game/OpenSCCT/Weapons/Materials/M_ScctAcogChevron")]
        if not all(surfaces):
            raise RuntimeError("Missing coated glass or reticle material")
        disc(2.67, 1.45, 2)
        disc(2.65, .04, 3)
        save_mesh(mesh, "SM_MercRedDot", triangle_count, materials + surfaces)
        return

    # The accessory's local origin sits on the outer surface of the virtual rifle. Both game
    # views use the same model-space frame. Its two emitters follow the recessed faces below.
    mesh = ue.DynamicMesh()
    triangle_count = 0
    def axial(y, z):
        def place(x, v, w, direction):
            return (x, v + (0 if direction else y), w + (0 if direction else z))
        return place

    torch = axial(2.24, .54)
    laser = axial(1.02, 1.82)
    # The four raised grip bands are moulded rubber inserts on an anodized metal barrel.
    profile = [(-5.24, .77), (-5.08, 1.03), (-4.78, 1.20), (-4.37, 1.24)]
    grip_strips = []
    for start in (-4.20, -2.82, -1.44, -.06):
        grip_strips.extend(range(len(profile), len(profile) + 3))
        profile.extend([(start, 1.26), (start+.13, 1.34), (start+.42, 1.34), (start+.58, 1.24)])
    profile.extend([(2.14, 1.22), (3.83, 1.26), (4.31, 1.21), (4.35, .94), (4.05, .90),
                    (-4.79, .78), (-5.24, .70)])

    def inset_face(center, face_x, radius):
        lathe([(face_x-.04, radius*.95), (face_x-.02, radius), (face_x, radius*.96),
               (face_x+.025, radius*.47), (face_x+.029, .002), (face_x-.04, .002)], 24, center)

    lathe(profile, 32, torch, grip_strips)
    inset_face(torch, -5.18, .67)
    flush(0)
    lathe(profile, 32, torch, [j for j in range(len(profile)) if j not in grip_strips])
    if not torch_only:
        lathe([(-4.04, .32), (-3.88, .54), (-3.54, .58), (3.74, .57), (4.15, .52),
           (4.17, .33), (3.93, .31), (-3.83, .31), (-4.04, .28)], 24, laser)
    mount_tier(8.52, 2.16, -1.35, .18, .36, -.20, 1.66)
    mount_tier(6.16, 1.96, -.12, 1.46, .31, -.48, .94)

    # A short shoe visibly joins the handguard; the front bezels wrap two distinct recesses.
    mount_tier(6.45, .75, -.88, .84, .15, -.25, .13)
    lathe([(4.02, 1.16), (4.19, 1.27), (4.72, 1.25), (4.88, 1.15), (4.88, .90),
           (4.75, .83), (4.43, .79), (4.02, .81)], 36, torch)
    if not torch_only:
        lathe([(3.91, .54), (4.09, .61), (4.42, .58), (4.55, .51), (4.55, .32),
           (4.44, .27), (4.22, .25), (3.91, .27)], 28, laser)
    lathe([(-5.30, .61), (-5.28, .81), (-5.14, .92), (-4.96, .90), (-4.96, .57),
           (-5.17, .55)], 32, torch)
    flush(1)

    # The recessed push switch shares the grip material. The forward emitter faces retain
    # their original hardware finish. The same three sections serve both player views.
    finishes = "/Game/OpenSCCT/Weapons/Materials/MI_ScctScope"
    grip, body = (ue.load_asset(finishes + suffix) for suffix in ("Rubber", "Anodized"))
    hardware = finish("OpticFasteners")
    if not all((grip, body, hardware)):
        raise RuntimeError("Missing rail module finish")
    inset_face(torch, 4.54, .82)
    if not torch_only:
        inset_face(laser, 4.34, .29)
    flush(2)
    sockets = [("TorchEmitter", (4.569, 2.24, .54))]
    if not torch_only:
        sockets.append(("LaserEmitter", (4.369, 1.02, 1.82)))
    save_mesh(mesh, "SM_MercTorch" if torch_only else "SM_MercRailModule", triangle_count, [grip, body, hardware], sockets)


if __name__ == "__main__":
    import sys
    build(rifle_dot_only="--rifle-dot" in sys.argv)
