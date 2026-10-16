# Sound3DS 🎵
### Reproductor de Audio Estético para Nintendo 3DS / 2DS (Optimizado para Old 3DS)

**Sound3DS** es un reproductor MP3 y cliente de streaming para Nintendo 3DS/2DS. El objetivo de rendimiento es mantener una interfaz fluida en Old 3DS/2DS; el rendimiento y la salida de audio deben validarse en hardware real.

---

## 🌟 Características Principales

1. **Búsqueda y streaming progresivo de SoundCloud**:
   - Búsqueda de metadatos mediante el endpoint API v2 y transferencia de MP3 progresivo mediante libcurl.
   - Resolución de URL de streaming y redirecciones del proveedor.
   - Búsqueda táctil con integración del teclado virtual del sistema (`swkbd`).
2. **Arquitectura multihilo**:
   - Hilo de audio (decodificación y envío al hardware) con prioridad algo mayor que el hilo principal.
   - Búsqueda, carátula y streaming corren en hilos de red propios; la interfaz sigue animada mientras esperan y cada transferencia se puede cancelar.
   - Todos los hilos usan el núcleo por defecto de la aplicación (Core 0); `APT_SetAppCpuTimeLimit(30)` queda habilitado para pruebas futuras en el Core 1.
3. **Pipeline de audio con dos backends**:
   - Decodificación MP3 a PCM 16-bit a 44.1 kHz mediante `minimp3`.
   - **NDSP** (preferido): cola de 4 buffers sin huecos entre bloques y pausa real. Requiere `sdmc:/3ds/dspfirm.cdc`, que se genera con [DSP1](https://github.com/zoogie/DSP1).
   - **CSND** (respaldo automático si falta `dspfirm.cdc`): anillo PCM en bucle por canal, rellenado por detrás del cabezal de reproducción.
   - La pantalla superior indica qué backend está activo.
4. **Visualizador de Ondas / FFT Reactivo**:
   - Algoritmo de 128 puntos en aritmética entera con tablas de senos/cosenos precomputadas en ROM (<0.04 ms por cuadro).
   - 16 bandas logarítmicas de frecuencias y decaimiento suave de picos (*peak decay*).
5. **Motor de Temas Extensible**:
   - **Modern Glow**: Estética cyberpunk/minimalista en modo oscuro con acentos violeta/neón y barras continuas con antialiasing.
   - **Y2K Classic Tunes**: Estilo reproductores de los años 2000 (iTunes clásico / Winamp), con fondo de aluminio cepillado, visor estilo LCD retro fósforo verde y analizador de espectro con segmentos LED discretos (Verde/Amarillo/Rojo).

---

## 🎮 Controles

### Pantalla Superior (400 × 240)
- **Carátula**: Disco de vinilo estilizado o carátula de álbum.
- **Visor**: Título con desplazamiento marquesina, artista y duración.
- **Visualizador**: 16 barras de espectro reactivas al audio en tiempo real.

### Pantalla Inferior (320 × 240 Táctil)
- **Barra de búsqueda**: Toca para abrir el teclado (`swkbd`) y buscar en SoundCloud.
- **Lista de temas**: Hasta 5 resultados; toca cualquier canción para reproducirla. Al terminar una pista se pasa a la siguiente.
- **Barra de progreso**: Arrastra el punto y suéltalo para saltar a esa parte de la canción (en streams usa peticiones HTTP Range; el salto es proporcional al tamaño del archivo).
- **Transporte**: Botones táctiles `|<<` (Prev), `>` / `||` (Play/Pause), `>>|` (Next).
- **Selector de Tema**: Toca el botón `TEMA: MODERN/Y2K` para alternar la estética al vuelo.
- **Volumen**: Desliza el control de volumen.

### Atajos de Botones Físicos
- **(A)**: Play / Pause
- **(L)** o **(Cruz Izquierda)**: Pista anterior
- **(R)** o **(Cruz Derecha)**: Pista siguiente
- **(Cruz Arriba / Abajo)**: Subir / Bajar volumen
- **(SELECT)**: Alternar tema visual (Modern Glow <-> Y2K Tunes)
- **(START)**: Salir de la aplicación

---

## 🛠️ Compilación y Ejecución

El proyecto utiliza un contenedor aislado con **Podman** y el toolchain oficial `devkitpro/devkitarm`.

### 1. Compilar el binario `.3dsx`
```bash
./scripts/build.sh
```

### 2. Ejecutar en el emulador Azahar (Citra Fork)
```bash
./scripts/run_azahar.sh
```

### 3. Generar el `.cia`
```bash
./scripts/build_cia.sh
```
Necesita en el equipo `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR)) y `bannertool`; por defecto los busca en `~/.local/share/3ds-tools/` (se pueden indicar con `MAKEROM=` y `BANNERTOOL=`). El ícono, el banner y su sonido están en `cia/`.

### 4. Instalar en tu consola física (Old 3DS / 2DS / New 3DS)
- **Como `.cia`**: sirve el archivo con un servidor local (por ejemplo `serve_cia.py`) y, en FBI, usa *Remote Install → Scan QR Code*. Queda en el menú HOME.
- **Como `.3dsx`**: copia `sound3ds.3dsx` y `sound3ds.smdh` a `sdmc:/3ds/sound3ds/` y ábrelo desde el **Homebrew Launcher**.

Los binarios no se guardan en el repositorio; cada versión se publica en GitHub Releases.

---

## 📌 Estado Actual y Próximos Pasos

- v0.3.0 estabiliza audio, red e interfaz; falta validar el sonido en una Old 3DS/2DS física, en especial el respaldo CSND (Azahar no lo reproduce).
- El motor de streaming decodifica MP3 progresivo. AAC/HLS y otras fuentes requieren un decodificador y una ruta de reproducción adicionales.
- El `client_id` de SoundCloud está fijo en el código y la verificación SSL está desactivada; se revisarán al evaluar proveedores.
- La evaluación de migración a YouTube Music está en [`docs/YOUTUBE_MUSIC_EVALUATION.md`](docs/YOUTUBE_MUSIC_EVALUATION.md).

## Versionado y notas de cambios

Consulta [`CHANGELOG.md`](CHANGELOG.md) para el historial. Esta versión registra el trabajo de audio, el cambio de pista y el diagnóstico de errores; no declara compatibilidad universal de formatos ni una migración de proveedor terminada.
