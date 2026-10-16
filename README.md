# Sound3DS 🎵
### Reproductor de Audio Estético para Nintendo 3DS / 2DS (Optimizado para Old 3DS)

**Sound3DS** es un reproductor MP3 y cliente de streaming para Nintendo 3DS/2DS. El objetivo de rendimiento es mantener una interfaz fluida en Old 3DS/2DS; el rendimiento y la salida de audio deben validarse en hardware real.

---

## 🌟 Características Principales

1. **Búsqueda y streaming de SoundCloud**:
   - Búsqueda mediante la API v2 y canciones completas en HLS + AAC (segmentos fMP4), o MP3 progresivo cuando existe.
   - AAC decodificado en la propia consola con Helix AAC (punto fijo, optimizado para ARM); las previews de 30 s se marcan con "30s".
   - Resolución de URL de streaming y redirecciones del proveedor.
   - Búsqueda táctil con integración del teclado virtual del sistema (`swkbd`).
2. **Arquitectura multihilo**:
   - Hilo de audio (decodificación y envío al hardware) con prioridad algo mayor que el hilo principal.
   - Búsqueda, carátula y streaming corren en hilos de red propios; la interfaz sigue animada mientras esperan y cada transferencia se puede cancelar.
   - Todos los hilos usan el núcleo por defecto de la aplicación (Core 0); `APT_SetAppCpuTimeLimit(30)` queda habilitado para pruebas futuras en el Core 1.
3. **Pipeline de audio con dos backends**:
   - Decodificación MP3 con `minimp3` y AAC con Helix AAC, a PCM 16-bit a la frecuencia de la pista.
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

### Menú principal
- **Pantalla superior**: la mascota, un cubo con ojitos cuadrados que reacciona a lo que haces (y se duerme si no haces nada).
- **Pantalla inferior**: elige **SoundCloud** o **Local** con la cruceta + (A) o tocando la tarjeta. La paleta cambia el tema y la píldora "sonando ahora" lleva a la lista de lo que suena.

### Reproductor (SoundCloud y Local)
- **Pantalla superior**: carátula, título, artista, tiempo, ecualizador y la mascota pequeña.
- **Barra de búsqueda**: en SoundCloud busca; en Local filtra tus canciones (vacío = todas).
- **Lista**: hasta 50 resultados (SoundCloud) o 300 canciones (Local) en páginas de 5.
- **Barra de progreso**: arrastra y suelta para saltar.
- **Transporte**: anterior, reproducir/pausa y siguiente.
- **Abajo**: paleta de tema, paginador y casa para volver al menú.

### Botones físicos
- **Cruceta arriba / abajo**: recorrer canciones (o tarjetas en el menú)
- **Cruceta izquierda / derecha**: página anterior / siguiente
- **(A)**: reproducir la canción enfocada, o pausar/reanudar la que suena
- **(B)**: volver al menú
- **(L) / (R)**: pista anterior / siguiente
- **(SELECT)**: alternar tema visual
- **(START)**: salir
- **Volumen**: deslizador físico de la consola

### En reposo
Si cierras la tapa (o activas el interruptor de reposo de la 2DS) con música sonando, la app sigue reproduciendo con las pantallas apagadas. El sonido sale por los parlantes, o por los audífonos si están conectados. Los controles son un toque de **(L)** o **(R)** para pausar/reanudar, doble toque de **(L)** para la anterior y doble toque de **(R)** para la siguiente; al abrirla todo vuelve a la normalidad. Sin música, la consola se suspende como siempre.

### Música local
Copia tus MP3 a `sdmc:/music/` (se admiten subcarpetas). El título y el artista salen de las etiquetas ID3 o del nombre `Artista - Título.mp3`.

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

### 3. Probar en el PC el camino HLS/AAC
```bash
tests/host/run.sh <carpeta con playlist.m3u8, init.mp4 y seg0.m4s, seg1.m4s...>
```
Compila el demuxer y Helix AAC para el PC, decodifica los segmentos y compara el resultado con ffmpeg (debe superar 40 dB de SNR).

### 4. Generar el `.cia`
```bash
./scripts/build_cia.sh
```
Necesita en el equipo `makerom` ([Project_CTR](https://github.com/3DSGuy/Project_CTR)) y `bannertool`; por defecto los busca en `~/.local/share/3ds-tools/` (se pueden indicar con `MAKEROM=` y `BANNERTOOL=`). El ícono, el banner y su sonido están en `cia/`.

### 5. Instalar en tu consola física (Old 3DS / 2DS / New 3DS)
- **Como `.cia`**: sirve el archivo con un servidor local (por ejemplo `serve_cia.py`) y, en FBI, usa *Remote Install → Scan QR Code*. Queda en el menú HOME.
- **Como `.3dsx`**: copia `sound3ds.3dsx` y `sound3ds.smdh` a `sdmc:/3ds/sound3ds/` y ábrelo desde el **Homebrew Launcher**.

Los binarios no se guardan en el repositorio; cada versión se publica en GitHub Releases.

---

## 📌 Estado Actual y Próximos Pasos

- v0.3.0 estabiliza audio, red e interfaz; falta validar el sonido en una Old 3DS/2DS física, en especial el respaldo CSND (Azahar no lo reproduce).
- El streaming soporta MP3 progresivo y HLS con AAC-LC en fMP4. Las pistas cifradas (DRM) de SoundCloud no se pueden reproducir.
- La carga del decodificador AAC en una Old 3DS/2DS real está por medir; la pantalla superior la muestra en porcentaje.
- El `client_id` de SoundCloud está fijo en el código y la verificación SSL está desactivada; se revisarán al evaluar proveedores.
- La evaluación de migración a YouTube Music está en [`docs/YOUTUBE_MUSIC_EVALUATION.md`](docs/YOUTUBE_MUSIC_EVALUATION.md).

## Versionado y notas de cambios

Consulta [`CHANGELOG.md`](CHANGELOG.md) para el historial. Esta versión registra el trabajo de audio, el cambio de pista y el diagnóstico de errores; no declara compatibilidad universal de formatos ni una migración de proveedor terminada.
