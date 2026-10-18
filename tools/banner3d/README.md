# Banner 3D del menú HOME

Banner "extendido" (modelo CGFX animado) siguiendo el método probado en consola real de la
guía de GBAtemp "How to Make a 3D Banner Like an Official Nintendo 3DS Game" y los scripts
de [ClouDS-Music-FA](https://github.com/Epic0522/ClouDS-Music-FA) (`tools/banner`).

1. Escena (Blender, sin ventana): `blender -b --factory-startup --python build_scene.py -- logo.png banner-scene.gltf [preview.png]`
   Jerarquía `COMMON -> world (mallas rígidas animadas) + name (logo)`; solo animación TRS
   de mallas rígidas, sin esqueletos; una malla por material.
2. CGFX con [pycgfx](https://github.com/skyfloogle/pycgfx) (commit 1f78850, `pip install gltflib pillow`):
   `python3 convert_cgfx.py banner-scene.gltf ../../cia/banner.cgfx --pycgfx-root ruta/a/pycgfx`
   Rellena `mesh_node_name` (pycgfx lo deja vacío y el menú HOME se cae al leerlo), logo en
   billboard Y (5), materiales mate, y valida que no haya piel ni animaciones raras.
3. Jingle: `make_jingle.py voz.wav salida.wav` (FluidR3_GM + voz Piper) y después
   `sox salida.wav -r 44100 -c 2 -b 16 ../../cia/banner.wav`.

`scripts/build_cia.sh` usa `cia/banner.cgfx` si existe y entonces agrega la bandera
`extendedbanner` al SMDH (obligatoria para banners 3D).

`banner_experimental.cgfx` es el primer intento, que colgaba el menú HOME: no usarlo.
