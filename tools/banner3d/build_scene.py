"""Escena del banner 3D con la estructura que acepta el menú HOME real.

    blender -b --factory-startup --python build_scene.py -- logo.png banner-scene.gltf [preview.png]

Sigue el método probado en consola (guía de GBAtemp "How to Make a 3D Banner Like an
Official Nintendo 3DS Game" y tools/banner de ClouDS-Music-FA):

    COMMON            raíz vacía
    ├── world         raíz del modelo (la rota el menú HOME), sin animación
    │   ├── Cube      malla rígida animada (TRS): cuerpo, ojos, rubor y boca
    │   └── Notes     malla rígida animada (TRS): las tres notas
    └── name          logo plano; convert_cgfx.py le pone billboard eje Y (5)

Solo se animan mallas rígidas (nada de esqueletos ni nodos vacíos animados).
Escala: cámara del menú en (0, 1, 44.8) mirando -Z, 30° vertical (~40 x 24 unidades).
"""
import bpy, math, sys, json, os
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
LOGO, OUT = os.path.abspath(args[0]), os.path.abspath(args[1])
PREVIEW = os.path.abspath(args[2]) if len(args) > 2 else None

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
scene = bpy.context.scene
scene.render.fps, scene.render.fps_base = 30, 1.0
scene.frame_start, scene.frame_end = 1, 60   # 2 s exactos, sin cola vacía


def srgb(r, g, b):
    f = lambda c: (c / 255) / 12.92 if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4
    return (f(r), f(g), f(b), 1.0)


def mat(name, color):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    b.inputs["Base Color"].default_value = color
    b.inputs["Roughness"].default_value = 0.9
    b.inputs["Metallic"].default_value = 0.0
    return m


M_BODY, M_INK = mat("Body", srgb(168, 85, 247)), mat("Ink", srgb(24, 22, 34))
M_BLUSH = mat("Blush", srgb(255, 140, 190))
M_CYAN, M_LILAC = mat("NoteCyan", srgb(34, 211, 238)), mat("NoteLilac", srgb(221, 190, 252))


def part(prim, loc, scale, rot, material, **kw):
    getattr(bpy.ops.mesh, prim)(location=loc, rotation=rot, **kw)
    o = bpy.context.object
    o.scale = scale
    o.data.materials.append(material)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return o


def join(objs, name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    o = bpy.context.object
    o.name = name
    for other in bpy.data.meshes:   # liberar el nombre si quedó una malla huérfana con él
        if other.name == name and other is not o.data:
            other.name = name + "_old"
    o.data.name = name           # el nombre de la malla glTF = nombre del nodo
    return o


# --- Cubo (todo en una malla rígida con su origen en el centro) ---
S = 4.2
body = part("primitive_cube_add", (0, 0, 0), (S * 1.03, S, S * 0.96), (0, 0, 0), M_BODY, size=2)
bev = body.modifiers.new("bevel", "BEVEL"); bev.width = 0.85; bev.segments = 3
bpy.context.view_layer.objects.active = body
bpy.ops.object.modifier_apply(modifier="bevel")
for p in body.data.polygons:
    p.use_smooth = True
front = -S - 0.02
ink, blush = [], []
for sx in (-1.75, 1.75):
    for side in (-1, 1):
        ink.append(part("primitive_cube_add", (sx + side * 0.42, front, 0.95), (1.0, 0.18, 0.3),
                           (0, math.radians(side * 42), 0), M_INK, size=1.0))
for sx in (-2.9, 2.9):
    blush.append(part("primitive_cylinder_add", (sx, front + 0.03, -0.5), (0.7, 0.45, 0.06),
                       (math.radians(90), 0, 0), M_BLUSH, vertices=16))
ink.append(part("primitive_uv_sphere_add", (0, front + 0.05, -1.0), (0.62, 0.15, 0.48), (0, 0, 0), M_INK,
                segments=12, ring_count=8))
# Un objeto por material: pycgfx nombra mal las mallas con varios materiales
cube_parts = [join([body], "CubeBody"), join(ink, "CubeInk"), join(blush, "CubeBlush")]


# --- Notas (una malla rígida, origen en el centro del grupo) ---
def note_parts(base, s, material, tilt):
    out = []
    rot = (0, math.radians(tilt), 0)
    def at(dx, dz):
        c, si = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
        return (base[0] + dx * c + dz * si, base[1], base[2] - dx * si + dz * c)
    out.append(part("primitive_uv_sphere_add", at(0, 0), (1.05 * s, 0.8 * s, 0.75 * s), rot, material,
                    segments=12, ring_count=8))
    out.append(part("primitive_cylinder_add", at(0.88 * s, 2.0 * s), (0.18 * s, 0.18 * s, 2.0 * s), rot, material,
                    vertices=8))
    out.append(part("primitive_cube_add", at(1.45 * s, 3.55 * s), (1.3 * s, 0.32 * s, 0.56 * s),
                    (0, math.radians(tilt - 30), 0), material, size=1.0))
    return out


note_parts_ = [join(note_parts((-11.5, 0, 6.5), 1.1, M_CYAN, 14) + note_parts((13.0, 0, 1.5), 0.7, M_CYAN, 8),
                    "NotesCyan"),
               join(note_parts((10.5, 0, 6.6), 0.95, M_LILAC, -12), "NotesLilac")]

# --- Jerarquía COMMON / world / name ---
common = bpy.data.objects.new("COMMON", None); scene.collection.objects.link(common)
world = bpy.data.objects.new("world", None); scene.collection.objects.link(world)
world.parent = common
for o in cube_parts + note_parts_:
    o.parent = world

# Logo: plano con el PNG, transformaciones horneadas en los vértices (nodo identidad)
bpy.ops.mesh.primitive_plane_add(size=2.0, location=(0, -6.5, -6.2), rotation=(math.radians(90), 0, 0))
logo = bpy.context.object
logo.name = "name"
logo.data.name = "name"
logo.scale = (8.6, 4.3, 1.0)
lm = bpy.data.materials.new("LogoMask")
lm.use_nodes = True
lm.use_backface_culling = False
nodes, links = lm.node_tree.nodes, lm.node_tree.links
pr = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
tex = nodes.new("ShaderNodeTexImage")
tex.image = bpy.data.images.load(LOGO, check_existing=True)
links.new(tex.outputs["Color"], pr.inputs["Base Color"])
links.new(tex.outputs["Alpha"], pr.inputs["Alpha"])
pr.inputs["Metallic"].default_value = 0.0
pr.inputs["Roughness"].default_value = 0.9
logo.data.materials.append(lm)
bpy.context.view_layer.objects.active = logo
logo.select_set(True)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
logo.select_set(False)
logo.parent = common

# --- Animación rígida (solo TRS sobre Cube y Notes), bucle limpio de 60 cuadros ---
from mathutils import Euler
for o in cube_parts + note_parts_:
    o.rotation_mode = "QUATERNION"
for f in range(1, 61):
    ph = 2 * math.pi * (f - 1) / 60
    q = Euler((0, math.radians(4 * math.sin(ph)), math.radians(-6 + 22 * math.sin(ph))), "XYZ").to_quaternion()
    for o in cube_parts:      # todas las piezas del cubo comparten el centro (0, 0, 0) y el movimiento
        o.location = (0, 0, 3.2 + 0.8 * abs(math.sin(ph)))
        o.rotation_quaternion = q
    for o in note_parts_:
        o.location = (0, 0, 0.6 * math.sin(ph))
        o.rotation_quaternion = (1, 0, 0, 0)
    for o in cube_parts + note_parts_:
        o.scale = (1, 1, 1)
        o.keyframe_insert("location", frame=f)
        o.keyframe_insert("rotation_quaternion", frame=f)
        o.keyframe_insert("scale", frame=f)

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLTF_SEPARATE", export_animations=True,
                          export_frame_range=True, export_force_sampling=True, export_skins=False,
                          export_cameras=False, export_lights=False, export_yup=True)
doc = json.load(open(OUT))
for m in doc.get("materials", []):
    if m.get("name") == "LogoMask":
        m["alphaMode"], m["alphaCutoff"], m["doubleSided"] = "MASK", 0.5, True
json.dump(doc, open(OUT, "w"), indent=1)
print("gltf:", OUT, [n.get("name") for n in doc["nodes"]])

if PREVIEW:
    scene.frame_set(10)
    bpy.ops.object.camera_add(location=(0, -44.786, 1), rotation=(math.radians(90), 0, 0))
    cam = bpy.context.object
    cam.data.sensor_fit, cam.data.angle, cam.data.clip_start = "VERTICAL", 0.523599, 26.5
    scene.camera = cam
    bpy.ops.object.light_add(type="SUN", rotation=(math.radians(40), 0, math.radians(-25)))
    bpy.context.object.data.energy = 3.5
    w = bpy.data.worlds.new("w"); scene.world = w; w.use_nodes = True
    next(n for n in w.node_tree.nodes if n.type == "BACKGROUND").inputs["Color"].default_value = (0.85, 0.86, 0.9, 1)
    scene.render.engine = "CYCLES"; scene.cycles.samples = 32
    scene.render.resolution_x, scene.render.resolution_y = 400, 240
    scene.render.filepath = PREVIEW
    bpy.ops.render.render(write_still=True)
