"""Build the two optical viewmodels. The mesh origin is the centre of the rear pane."""
import math

import unreal as ue

from build_merc_optic_cap import save_mesh
from scct_materials import FLOAT1, FLOAT2, FLOAT3, Graph
from scct_rifle_cosmetics import solid_finish


class ScopeMesh:
    def __init__(self):
        self.mesh = ue.DynamicMesh()
        self.triangles = 0
        self.clear()

    def clear(self):
        self.points, self.normals, self.uvs, self.faces = [], [], [], []

    def vertex(self, p, n, uv):
        self.points.append(ue.Vector(*p))
        self.normals.append(ue.Vector(*n))
        self.uvs.append(ue.Vector2D(*uv))

    def triangle(self, a, b, c):
        self.faces.append(ue.IntVector(a, b, c))

    def finish_section(self, material):
        buffers = ue.GeometryScriptSimpleMeshBuffers(
            vertices=self.points, normals=self.normals, uv0=self.uvs, triangles=self.faces)
        self.mesh.append_buffers_to_mesh(buffers, material_id=material)
        self.triangles += len(self.faces)
        self.clear()

    def revolve(self, profile, center=(0, 0, 0), axis="x", slices=64):
        def orient(x, y, z):
            if axis == "y":
                return z, x, y
            if axis == "z":
                return y, z, x
            return x, y, z

        start = len(self.points)
        for j, (x, radius) in enumerate(profile):
            xa, ra = profile[max(j - 1, 0)]
            xb, rb = profile[min(j + 1, len(profile) - 1)]
            length = math.hypot(rb - ra, xb - xa)
            for i in range(slices):
                angle = i * math.tau / slices
                y, z = math.cos(angle), math.sin(angle)
                delta = orient(x, radius * y, radius * z)
                self.vertex(tuple(a + b for a, b in zip(center, delta)),
                            orient((ra-rb)/length, (xb-xa)*y/length, (xb-xa)*z/length),
                            (i/slices, j/(len(profile)-1)))
                if j:
                    a = start + (j-1)*slices + i
                    b = start + (j-1)*slices + (i+1) % slices
                    self.triangle(a, b, a+slices)
                    self.triangle(b, b+slices, a+slices)

    def quad(self, points, normal):
        a = len(self.points)
        for p, uv in zip(points, ((0,0), (1,0), (1,1), (0,1))):
            self.vertex(p, normal, uv)
        self.triangle(a, a+1, a+2)
        self.triangle(a, a+2, a+3)

    def box(self, x0, x1, y0, y1, z0, z1):
        a,b,c,d = (x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0)
        e,f,g,h = (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)
        for points, normal in (((a,d,c,b),(0,0,-1)), ((e,f,g,h),(0,0,1)),
                               ((a,b,f,e),(0,-1,0)), ((b,c,g,f),(1,0,0)),
                               ((c,d,h,g),(0,1,0)), ((d,a,e,h),(-1,0,0))):
            self.quad(points, normal)

    def pane(self, x, r, objective=False):
        start = len(self.points)
        face = 1 if objective else -1
        sag = .045 * r
        def surface_triangle(a, b, c):
            if objective:
                self.triangle(a, c, b)
            else:
                self.triangle(a, b, c)
        self.vertex((x+face*sag, 0, 0), (face,0,0), (.5,.5))
        for ring in range(1, 5):
            t = ring / 4
            for i in range(64):
                a = math.tau*i/64
                y, z = t*math.cos(a), t*math.sin(a)
                ny, nz = 2*sag*y/r, 2*sag*z/r
                size = math.sqrt(1 + ny*ny + nz*nz)
                self.vertex((x+face*sag*(1-t*t),r*y,r*z), (face/size,ny/size,nz/size),
                            (.5+.5*y,.5+.5*z))
                here = start+1+(ring-1)*64+i
                next_around = start+1+(ring-1)*64+(i+1)%64
                if ring == 1:
                    surface_triangle(start, next_around, here)
                else:
                    surface_triangle(here-64, next_around, here)
                    surface_triangle(here-64, next_around-64, next_around)


def build_scope(bolt, materials):
    m = ScopeMesh()
    front = 23.7 if bolt else 16.6
    bell = 3.30 if bolt else 2.59
    rear = 2.18 if bolt else 1.90
    tube = 2.42 if bolt else 2.14
    # A rolled ocular lip surrounds the wide rear lens. The return profile closes the bore
    # so that an oblique view shows the shaded inner wall all the way to the front glass.
    aperture = rear * .93
    rim = rear * 1.069
    m.revolve([(-.02,aperture),(-.11,aperture+.01),(-.18,aperture+.05),
               (-.22,rim-.11),(-.20,rim-.045),(-.14,rim-.012),(-.05,rim),
               (.24,rim),(.36,rim-.035),(.49,rim-.055),(1.85,rim-.055),
               (2.18,rear+.02),(3.4,tube),(front*.66,tube),(front-3,bell-.11),
               (front-.32,bell+.04),(front+.23,bell),(front+.26,bell-.14),(front-.12,bell-.19),
               (front-3,bell-.36),(front*.66,tube-.25),(3.4,tube-.25),
               (2.2,aperture),(.32,aperture),(-.02,aperture)])
    seats = (6.5,13) if bolt else (4.5,10)
    for x in seats:
        m.revolve([(x-.76,tube-.01),(x-.72,tube+.19),(x-.60,tube+.28),
                   (x+.60,tube+.28),(x+.72,tube+.19),(x+.76,tube-.01)])
        m.box(x-.65,x+.65,-.85,.85,-3.75,-tube+.04)
        m.box(x-.93,x+.93,-1.27,1.27,-4.48,-3.65)
    cap_x = sum(seats)*.5
    cap = [(0,.67),(.18,.85),(.28,.90),(.83,.90),(.95,.76),(.95,.025)]
    m.revolve(cap, (cap_x,0,tube-.04), "z", 32)
    m.revolve(cap, (cap_x,tube-.04,0), "y", 32)
    m.finish_section(0)

    # Recessed grip ridges sit behind the smooth lip and stay within its outline from the eye.
    for x in (.56,.88,1.20,1.52):
        m.revolve([(x,rim-.07),(x+.035,rim-.027),(x+.13,rim-.027),(x+.18,rim-.07)])
    m.finish_section(1)

    for x in seats:
        m.revolve([(0,.32),(.13,.37),(.31,.34),(.35,.23),(.35,.01)],
                  (x,1.25,-4.02), "y", 12)
    m.finish_section(2)
    # The objective faces out. The magnified scene is already transmitted through it;
    # seeing its back as a second coated disc would add a hard circle inside the ocular.
    m.pane(front, bell-.17, objective=True)
    # The eye-side coating reflects the room even when the magnified image fills the bore.
    m.pane(-.045, aperture + .015)
    m.finish_section(3)
    m.pane(0, aperture + .015)
    m.finish_section(4)
    name = "SM_MercBoltScope" if bolt else "SM_MercDmrScope"
    save_mesh(m.mesh, name, m.triangles, materials,
              [("Ocular",(0,0,0)), ("ApertureEdge",(0,aperture,0)), ("Objective",(front,0,0))])


def precision_image(root):
    # The main view looks through the physical coating from the hip. When the eye is
    # aligned this curved surface supplies both the captured image and its reflection.
    g = Graph(root + "M_ScctPrecisionImage", rebuild_in_place=True)
    g.material.set_editor_property("blend_mode", ue.BlendMode.BLEND_TRANSLUCENT)
    g.material.set_editor_property("shading_model", ue.MaterialShadingModel.MSM_DEFAULT_LIT)
    g.material.set_editor_property("translucency_lighting_mode",
                                   ue.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    g.material.set_editor_property("two_sided", True)
    # The maps' glass, water and haze sheets are drawn after depth of field, over everything drawn before it and tested
    # only against opaque depth. Drawn before depth of field, the picture had the world's glass laid over it unmagnified
    # (the Sea Tunnel's panes over the scope's view of the tunnel, without the water). In the same pass the nearer lens
    # sorts in front of them.
    g.material.set_editor_property("translucency_pass", ue.MaterialTranslucencyPass.MTP_AFTER_DOF)
    uv = g.node(ue.MaterialExpressionTextureCoordinate)
    # Bend straight lines only near the rim. Sampling inward keeps the edge inside
    # the captured scene at every magnification and preserves the centre alignment.
    scene_uv = g.custom("""
        float2 radial = float2(UV.x, 1.0 - UV.y) * 2.0 - 1.0;
        float r = length(radial);
        // Match the M4 coating's outer annulus. The centre and the circumference
        // join without a slope change, so straight edges bend only in the glass.
        float e = saturate((r - .72) / .28);
        float e2 = e * e, lip = 1.0 - e;
        float bend = 3.0 * e2 * e2 * lip * lip;
        return .5 + .5 * radial * (1.0 - BendStrength * bend / max(r, 1e-5));
        """, FLOAT2, UV=uv, BendStrength=g.scalar("ScopeEdgeBend", 1.0))
    scene = g.texture("ScopeScene", "/Engine/EngineResources/SlatePost0_RT",
                      sampler=ue.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, uvs=scene_uv)
    blend = g.scalar("ScopeBlend", 0.0)
    eye_box = g.colour("ScopeEyeBox", (0, 0, 0, 0))
    pupil = g.custom("return 1.0 - smoothstep(.82, 1.04, length((UV * 2.0 - 1.0) - Eye.xy));",
                     FLOAT1, UV=uv, Eye=eye_box)
    radius = g.custom("return length(2.0 * UV - 1.0);", FLOAT1, UV=uv)
    # Derivatives limit subpixel flicker on small viewports. Both guns carry the same fine
    # red point as the reference. Generate it on the image so the two cannot sort apart.
    dot = g.custom("""
        float width = max(fwidth(Radius) * 0.5, 0.0005);
        return Enabled * (1.0 - smoothstep(max(0.0, 0.007 - width), 0.007 + width, Radius));
        """, FLOAT1, Radius=radius, Enabled=g.scalar("ScopeDot", 1.0))
    glow = g.in_the_picture(g.colour("ReticleTint", (2.0, .04, .018, 1)))
    image = g.custom("""
        float edge = smoothstep(.71, 1.0, Radius);
        float3 transmitted = Scene * lerp(float3(.989,.996,.998), float3(.76,.84,.88), edge);
        return lerp(transmitted, DotColour, Dot) * lerp(.10, 1.0, Pupil);
        """, FLOAT3, Radius=radius, Scene=(scene, "RGB"), Pupil=pupil, DotColour=glow, Dot=dot)
    # When the pupil moves out of the bore, leave the native tube and curved coating
    # visible. Fade in the crescent where the two apertures overlap as the eye aligns.
    opacity = g.custom("""
        float overlap = smoothstep(0.0, .55, 2.0 - length(Eye.xy));
        return Blend * lerp(Pupil, 1.0, overlap);
        """, FLOAT1, Eye=eye_box, Blend=blend, Pupil=pupil)
    # A second, fully lit translucent layer lifts blacks under nearby room lights.
    # The optical image is scene radiance. Give its antireflective surface no diffuse
    # response and only a weak single dielectric lobe along the outermost glass.
    surface = g.node(ue.MaterialExpressionSubstrateSlabBSDF)
    g.link(g.node(ue.MaterialExpressionConstant, r=0), surface, "Diffuse Albedo")
    g.link(g.custom("return .006 * smoothstep(.84, 1.0, R);", FLOAT1, R=radius), surface, "F0")
    g.link(g.node(ue.MaterialExpressionConstant, r=.16), surface, "Roughness")
    g.link(image, surface, "Emissive Color")
    weight = g.node(ue.MaterialExpressionSubstrateWeight)
    g.link(surface, weight, "A")
    g.link(opacity, weight, "Weight")
    g.output(weight, ue.MaterialProperty.MP_FRONT_MATERIAL)
    return g.finish()


def housing_materials():
    # Mounted optics share the pawn's shadowed room lights. Each finish also responds
    # to native fixtures through its own roughness and metal value.
    return [solid_finish("MI_ScctScopeAnodized", (.048,.052,.055), .42, .52),
            solid_finish("MI_ScctScopeRubber", (.020,.022,.023), .76, 0.0),
            solid_finish("MI_ScctScopeHardware", (.053,.056,.058), .33, .78)]


def build():
    materials = housing_materials()
    root = "/Game/OpenSCCT/Weapons/Materials/"
    glass = ue.load_asset(root+"M_ScctAcogGlass")
    if not glass:
        raise RuntimeError("Build the ACOG glass first.")
    materials.extend([glass, precision_image(root)])
    build_scope(True, materials)
    build_scope(False, materials)


if __name__ == "__main__":
    build()
