#!/usr/bin/env python3
"""glTF -> CGFX para el banner del menú HOME, con los ajustes que pycgfx no hace.

    python3 convert_cgfx.py banner-scene.gltf banner.cgfx --pycgfx-root /ruta/a/pycgfx

Basado en tools/banner/convert_banner_cgfx.py de ClouDS-Music-FA (probado en consola):
  - rellena mesh_node_name de cada malla (pycgfx lo deja vacío y el menú HOME lo lee:
    vacío -> lectura de puntero nulo, el cuelgue que tuvimos)
  - logo "name" en billboard eje Y (5)
  - materiales mate (pycgfx agrega un especular blanco muy fuerte)
  - rechaza piel/esqueletos y animaciones que no sean Transform (5) de mallas rígidas
"""
import argparse, sys
from pathlib import Path

RIGID_ANIMATED_MESHES = {"CubeBody", "CubeInk", "CubeBlush", "NotesCyan", "NotesLilac"}

ap = argparse.ArgumentParser()
ap.add_argument("input_gltf", type=Path)
ap.add_argument("output_cgfx", type=Path)
ap.add_argument("--pycgfx-root", type=Path, required=True)
args = ap.parse_args()

sys.path.insert(0, str(args.pycgfx_root.resolve()))
import gltflib
import main as pycgfx
from cgfx.mtob import ColorFloat, FragmentLightingFlags
from cgfx.canm import CANMBoneTransform, PrimitiveType
from cgfx.primitives import VertexAttributeUsage
from cgfx.sobj import BillboardMode

gltf = gltflib.GLTF.load(str(args.input_gltf.resolve()), load_file_resources=True)
# nombre de malla glTF -> nombre del nodo que la lleva
mesh_to_node = {gltf.model.meshes[n.mesh].name: n.name for n in gltf.model.nodes if n.mesh is not None}
cgfx = pycgfx.convert_gltf(gltf)

model = cgfx.data.models["COMMON"]
if model is None:
    raise SystemExit("el CGFX no tiene el modelo COMMON")
logo_bone = model.skeleton.bones["name"]
if logo_bone is None:
    raise SystemExit("falta el nodo 'name' del logo")
logo_bone.billboard_mode = BillboardMode.YAxial

bound = []
for mesh in model.meshes.data.contents:
    owner = max((m for m in mesh_to_node if mesh.name == m or mesh.name.startswith(m + "_")), key=len, default=None)
    if owner is None:
        raise SystemExit(f"no se encontró el nodo de la malla {mesh.name}")
    mesh.mesh_node_name = mesh_to_node[owner]
    bound.append(f"{mesh.name}->{mesh.mesh_node_name}")

for shape in model.shapes.data.contents:
    for ps in shape.primitive_sets.data.contents:
        if ps.skinning_mode != 0:
            raise SystemExit(f"{shape.name} usa skinning {ps.skinning_mode}")
    for attribute in shape.vertex_attributes.data.contents:
        for usage in (attribute.vertex_streams.data.contents if hasattr(attribute, "vertex_streams") else (attribute,)):
            if usage.usage in (VertexAttributeUsage.BoneIndex, VertexAttributeUsage.BoneWeight):
                raise SystemExit(f"{shape.name} tiene atributos de piel")

for name in model.materials:
    m = model.materials[name]
    m.material_color.constant[0] = ColorFloat(0, 0, 0, 1)
    spec = m.fragment_shader.texture_combiners[2]
    spec.src_rgb = 0xFFF
    spec.combine_rgb = 0
    m.fragment_shader.fragment_lighting.flags = FragmentLightingFlags(0)
    m.fragment_shader.fragment_lighting_table.distribution_0_sampler = None

anim = cgfx.data.skeletal_animations["COMMON"]
if anim is None:
    raise SystemExit("falta la animación COMMON")
members = {k: anim.member_animations_data[k] for k in anim.member_animations_data}
if set(members) != RIGID_ANIMATED_MESHES:
    raise SystemExit("animación inesperada en: " + ", ".join(sorted(members)))
for k, mem in members.items():
    if not isinstance(mem, CANMBoneTransform) or mem.primitive_type != PrimitiveType.Transform:
        raise SystemExit(f"{k} no es Transform (5)")

data = pycgfx.write(cgfx)
args.output_cgfx.write_bytes(data)
print(f"{args.output_cgfx}: {len(data)} bytes ({len(data) / 1024:.0f} KiB)")
print("mallas:", ", ".join(bound))
print("animados:", ", ".join(sorted(members)), "| logo billboard Y")
if len(data) > 512 * 1024:
    raise SystemExit("el CGFX pasa de 512 KiB")
