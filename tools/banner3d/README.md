# Banner 3D del menú HOME

El banner de Sound3DS es un modelo 3D (CGFX) animado en bucle, con su jingle.

1. Modelo (Blender, sin ventana):
   `blender -b --factory-startup --python build_model.py -- banner.glb preview.png`
2. glTF -> CGFX con [pycgfx](https://github.com/skyfloogle/pycgfx) (`pip install gltflib pillow`):
   `python3 pycgfx/main.py banner.glb ../../cia/banner.cgfx` (debe pesar menos de 512 KB)
3. Jingle: voz con [Piper](https://github.com/rhasspy/piper) (voz `en_US-amy-medium`) y
   música con FluidR3_GM vía pyfluidsynth:
   `echo "Sound three D S!" | piper -m en_US-amy-medium.onnx -f voz.wav`
   `python3 make_jingle.py voz.wav ../../cia/banner.wav`

`scripts/build_cia.sh` usa `cia/banner.cgfx` si existe (si no, la imagen plana `cia/banner.png`).
