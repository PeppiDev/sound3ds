import bpy, math
from mathutils import Vector

scene = bpy.context.scene
for o in list(bpy.data.objects): bpy.data.objects.remove(o, do_unlink=True)

def srgb(r, g, b):
    f = lambda c: (c / 255) / 12.92 if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4
    return (f(r), f(g), f(b), 1)

def mat(name, color, rough=0.45, emit=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = color
    bsdf.inputs["Roughness"].default_value = rough
    if emit > 0:
        bsdf.inputs["Emission Color"].default_value = color
        bsdf.inputs["Emission Strength"].default_value = emit
    return m

purple = mat("body", srgb(168, 85, 247), 0.38)
ink = mat("ink", srgb(24, 22, 34), 0.5)
blush = mat("blush", srgb(255, 140, 190), 0.6)
note_a = mat("noteA", srgb(221, 190, 252), 0.4, 0.6)
note_b = mat("noteB", srgb(34, 211, 238), 0.4, 0.8)

# Cubo con aristas redondeadas, en pleno salto e inclinado como en el baile
bpy.ops.mesh.primitive_cube_add(size=2, location=(0, 0, 1.55))
cube = bpy.context.object
bev = cube.modifiers.new("bevel", "BEVEL"); bev.width = 0.22; bev.segments = 6
cube.data.materials.append(purple)
cube.scale = (1.04, 1.0, 0.96)
cube.rotation_euler = (math.radians(0), math.radians(-9), math.radians(-28))
bpy.ops.object.shade_smooth()

def part(prim, loc, scale, rot, material, **kw):
    getattr(bpy.ops.mesh, prim)(location=(0, 0, 0), **kw)
    o = bpy.context.object
    o.scale = scale
    o.rotation_euler = rot
    o.location = loc
    o.data.materials.append(material)
    o.parent = cube
    return o

# Ojos ^ ^ (dos barritas que se juntan arriba) sobre la cara frontal (-Y)
for sx in (-0.42, 0.42):
    for side in (-1, 1):
        part("primitive_cube_add", (sx + side * 0.11, -1.0, 0.22), (0.15, 0.04, 0.045),
             (0, math.radians(side * 42), 0), ink)
# Rubor
for sx in (-0.7, 0.7):
    part("primitive_cylinder_add", (sx, -1.0, -0.12), (0.17, 0.11, 0.02),
         (math.radians(90), 0, 0), blush, vertices=32)
# Boca abierta y feliz
part("primitive_uv_sphere_add", (0, -0.99, -0.25), (0.16, 0.05, 0.12), (0, 0, 0), ink)

# Notas musicales flotando
def note(loc, s, material, tilt):
    bpy.ops.mesh.primitive_uv_sphere_add(location=(0, 0, 0))
    head = bpy.context.object; head.scale = (0.26 * s, 0.2 * s, 0.19 * s)
    head.data.materials.append(material)
    bpy.ops.mesh.primitive_cylinder_add(vertices=16, location=(0.22 * s, 0, 0.5 * s))
    stem = bpy.context.object; stem.scale = (0.045 * s, 0.045 * s, 0.5 * s)
    stem.data.materials.append(material)
    bpy.ops.mesh.primitive_cube_add(location=(0.36 * s, 0, 0.88 * s))
    flag = bpy.context.object; flag.scale = (0.16 * s, 0.04 * s, 0.07 * s); flag.rotation_euler = (0, math.radians(-30), 0)
    flag.data.materials.append(material)
    bpy.ops.object.empty_add(location=loc)
    root = bpy.context.object
    for o in (head, stem, flag): o.parent = root
    root.rotation_euler = (0, math.radians(tilt), math.radians(-28))
import os
NO_NOTES = os.environ.get("NO_NOTES") == "1"   # variante sin notas (banner de Sound3DS QR)
if not NO_NOTES:
    note((-2.6, 0.2, 2.35), 0.95, note_b, 14)
    note((2.35, 0.4, 2.55), 0.8, note_a, -12)
    note((3.05, -0.2, 1.05), 0.6, note_b, 8)

# Piso que solo recibe la sombra (el cubo está en el aire)
bpy.ops.mesh.primitive_plane_add(size=20, location=(0, 0, 0))
floor = bpy.context.object
floor.is_shadow_catcher = True

# Luces y cámara
bpy.ops.object.light_add(type="AREA", location=(-3, -4, 6)); key = bpy.context.object
key.data.energy = 900; key.data.size = 4
key.rotation_euler = (math.radians(40), 0, math.radians(-35))
bpy.ops.object.light_add(type="AREA", location=(4, 3, 3)); rim = bpy.context.object
rim.data.energy = 500; rim.data.size = 3; rim.data.color = (0.55, 0.85, 1.0)
rim.rotation_euler = (math.radians(-60), 0, math.radians(130))
bpy.ops.object.light_add(type="POINT", location=(0, -4, 1)); fill = bpy.context.object
fill.data.energy = 120

bpy.ops.object.camera_add(location=(0, -12.6, 3.3))
cam = bpy.context.object
direction = Vector((0, 0, 1.7)) - cam.location
cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
cam.data.lens = 50
scene.camera = cam

world = bpy.data.worlds.new("w"); scene.world = world
world.use_nodes = True
bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
bg.inputs["Color"].default_value = (0.02, 0.02, 0.04, 1); bg.inputs["Strength"].default_value = 0.6

scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 96
scene.render.film_transparent = True
scene.render.resolution_x = 768
scene.render.resolution_y = 288
scene.render.filepath = __import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "cube_nonotes.png" if NO_NOTES else "cube.png")
scene.view_settings.view_transform = "Standard"
bpy.ops.render.render(write_still=True)
