"""Modelo 3D del banner (cubo mascota + notas + título) exportado a glTF para pycgfx.

    blender -b --factory-startup --python build_model.py -- salida.glb [preview.png]

Escala: la cámara del menú HOME está en (0, 1, 44.8) mirando a -Z con 30° de campo
vertical, así que a la altura del modelo se ven ~40 x 24 unidades. En Blender (Z
arriba) eso es: el frente mira a -Y y la cámara está en y = -44.8, z = 1.
"""
import bpy, math, sys, os
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = os.path.abspath(argv[0] if argv else "banner.glb")
PREVIEW = os.path.abspath(argv[1]) if len(argv) > 1 else None
FONT = "/usr/share/fonts/rsms-inter-fonts/InterDisplay-ExtraBold.ttf"

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
scene = bpy.context.scene


def srgb(r, g, b, a=1.0):
    f = lambda c: (c / 255) / 12.92 if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4
    return (f(r), f(g), f(b), a)


def mat(name, color, rough=0.6, blend=False):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = 0.0
    if blend:
        bsdf.inputs["Alpha"].default_value = color[3]
        m.surface_render_method = "BLENDED"
    return m


M_BODY = mat("Body", srgb(168, 85, 247), 0.45)
M_INK = mat("Ink", srgb(24, 22, 34), 0.8)
M_BLUSH = mat("Blush", srgb(255, 140, 190), 0.8)
M_NOTE_A = mat("NoteCyan", srgb(34, 211, 238), 0.5)
M_NOTE_B = mat("NoteLilac", srgb(221, 190, 252), 0.5)
M_TITLE = mat("Title", srgb(255, 255, 255), 0.5)
M_TITLE_BACK = mat("TitleBack", srgb(91, 33, 182), 0.6)
M_SHADOW = mat("Shadow", srgb(20, 10, 40, 0.35), 1.0, blend=True)


def apply_all(o):
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    for m in list(o.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    o.select_set(False)


def add(prim, name, loc, scale, rot, material, parent=None, **kw):
    getattr(bpy.ops.mesh, prim)(location=(0, 0, 0), **kw)
    o = bpy.context.object
    o.name = name
    o.scale = scale
    o.data.materials.append(material)
    apply_all(o)
    o.rotation_euler = rot
    o.location = loc
    if parent:
        o.parent = parent
    return o


# --- Raíz animada del cubo (todo lo del cubo cuelga de aquí) ---
bpy.ops.object.empty_add(location=(0, 0, 3.2))
rig = bpy.context.object
rig.name = "CubeRig"

S = 4.2   # medio lado del cubo
cube = add("primitive_cube_add", "Cube", (0, 0, 0), (S * 1.03, S, S * 0.96), (0, 0, 0), M_BODY, rig, size=2)
bev = cube.modifiers.new("bevel", "BEVEL"); bev.width = 0.85; bev.segments = 3
apply_all(cube)
for p in cube.data.polygons:
    p.use_smooth = True

front = -S - 0.02
# Ojos ^ ^
for sx in (-1.75, 1.75):
    for side in (-1, 1):
        add("primitive_cube_add", "Eye", (sx + side * 0.42, front, 0.95), (1.0, 0.18, 0.3),
            (0, math.radians(side * 42), 0), M_INK, cube, size=1.0)
# Rubor
for sx in (-2.9, 2.9):
    add("primitive_cylinder_add", "Blush", (sx, front + 0.03, -0.5), (0.7, 0.45, 0.06),
        (math.radians(90), 0, 0), M_BLUSH, cube, vertices=16)
# Boca
add("primitive_uv_sphere_add", "Mouth", (0, front + 0.05, -1.0), (0.62, 0.15, 0.48), (0, 0, 0), M_INK, cube,
    segments=12, ring_count=8)

# Sombra en el piso (no se mueve con el cubo)
add("primitive_cylinder_add", "Shadow", (0.4, 0.6, -2.35), (4.6, 2.2, 0.02), (0, 0, 0), M_SHADOW, None, vertices=24)


# --- Notas musicales ---
def note(name, loc, s, material, tilt):
    bpy.ops.object.empty_add(location=loc)
    root = bpy.context.object
    root.name = name
    add("primitive_uv_sphere_add", name + "Head", (0, 0, 0), (1.05 * s, 0.8 * s, 0.75 * s), (0, 0, 0), material, root,
        segments=12, ring_count=8)
    add("primitive_cylinder_add", name + "Stem", (0.88 * s, 0, 2.0 * s), (0.18 * s, 0.18 * s, 2.0 * s), (0, 0, 0),
        material, root, vertices=8)
    add("primitive_cube_add", name + "Flag", (1.45 * s, 0, 3.55 * s), (1.3 * s, 0.32 * s, 0.56 * s),
        (0, math.radians(-30), 0), material, root, size=1.0)
    root.rotation_euler = (0, math.radians(tilt), 0)
    return root


notes = [note("NoteL", (-11.5, 0, 6.5), 1.1, M_NOTE_A, 14),
         note("NoteR", (10.5, 0, 6.6), 0.95, M_NOTE_B, -12),
         note("NoteR2", (13.0, 0, 1.5), 0.7, M_NOTE_A, 8)]

# --- Título en 3D ---
bpy.ops.object.text_add(location=(0, 0, 0))
txt = bpy.context.object
txt.data.body = "Sound3DS"
if os.path.exists(FONT):
    txt.data.font = bpy.data.fonts.load(FONT)
txt.data.align_x = "CENTER"
txt.data.align_y = "CENTER"
txt.data.size = 4.6
txt.data.extrude = 0.35
txt.data.resolution_u = 4
txt.data.bevel_depth = 0.0
txt.rotation_euler = (math.radians(90), 0, 0)
txt.location = (0, -0.6, -6.0)
txt.data.materials.append(M_TITLE)
bpy.context.view_layer.objects.active = txt
txt.select_set(True)
bpy.ops.object.convert(target="MESH")
txt = bpy.context.object
txt.name = "Title"
txt.select_set(False)
# Placa morada detrás del título
add("primitive_cube_add", "TitleBack", (0, 0.2, -6.0), (17.5, 0.5, 3.6), (0, 0, 0), M_TITLE_BACK, None, size=1.0)
plate = bpy.data.objects["TitleBack"]
pb = plate.modifiers.new("bevel", "BEVEL"); pb.width = 1.2; pb.segments = 3
apply_all(plate)

# --- Animación en bucle (2 s a 30 fps): balanceo, flotación y notas que suben y bajan ---
scene.render.fps = 30
scene.frame_start, scene.frame_end = 0, 60
rig.rotation_mode = "XYZ"
for f, ry, rz, z in ((0, -4, -26, 3.2), (15, 4, -14, 4.0), (30, 4, 22, 3.2), (45, -4, 14, 4.0), (60, -4, -26, 3.2)):
    rig.rotation_euler = (0, math.radians(ry), math.radians(rz))
    rig.location = (0, 0, z)
    rig.keyframe_insert("rotation_euler", frame=f)
    rig.keyframe_insert("location", frame=f)
for i, n in enumerate(notes):
    base = n.location.copy()
    for f in (0, 30, 60):
        up = 0.9 if (f // 30 + i) % 2 else 0.0
        n.location = (base.x, base.y, base.z + up)
        n.keyframe_insert("location", frame=f)

bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", export_animations=True, export_apply=True,
                          export_yup=True, export_cameras=False, export_lights=False)
print("glb:", OUT)

if PREVIEW:
    # Vista previa con la cámara del menú HOME
    scene.frame_set(8)
    bpy.ops.object.camera_add(location=(0, -44.786, 1))
    cam = bpy.context.object
    cam.rotation_euler = (math.radians(90), 0, 0)
    cam.data.sensor_fit = "VERTICAL"
    cam.data.angle = 0.523599
    cam.data.clip_start = 26.5
    scene.camera = cam
    bpy.ops.object.light_add(type="SUN", location=(0, -10, 20))
    sun = bpy.context.object
    sun.rotation_euler = (math.radians(40), 0, math.radians(-25))
    sun.data.energy = 3.5
    world = bpy.data.worlds.new("w"); scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.85, 0.86, 0.9, 1)
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 32
    scene.render.resolution_x, scene.render.resolution_y = 400, 240
    scene.render.filepath = PREVIEW
    bpy.ops.render.render(write_still=True)
