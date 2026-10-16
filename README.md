# Sound3DS 🎵
### Reproductor de Audio Estético para Nintendo 3DS / 2DS (Optimizado para Old 3DS)

**Sound3DS** es un reproductor MP3 y cliente de streaming para Nintendo 3DS/2DS. El objetivo de rendimiento es mantener una interfaz fluida en Old 3DS/2DS; el rendimiento y la salida de audio deben validarse en hardware real.

---

## 🌟 Características Principales

1. **Búsqueda y streaming progresivo de SoundCloud**:
   - Búsqueda de metadatos mediante el endpoint API v2 y transferencia de MP3 progresivo mediante libcurl.
   - Resolución de URL de streaming y redirecciones del proveedor.
   - Búsqueda táctil con integración del teclado virtual del sistema (`swkbd`).
2. **Arquitectura Multihilo en Core 1 (Syscore)**:
   - Desbloqueo de CPU secundaria con `APT_SetAppCpuTimeLimit(30)`.
   - Hilo separado para red/decodificación; afinidad y rendimiento dependen de los permisos disponibles en el sistema.
3. **Pipeline de Audio sin firmware DSP externo**:
   - Decodificación MP3 a PCM 16-bit a 44.1 kHz mediante `minimp3`; salida estéreo por dos canales CSND y buffers lineales.
   - No requiere `sdmc:/3ds/dspfirm.cdc`. El backend CSND está integrado, pero requiere validación de reproducción continua en Azahar y en una 2DS Old.
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
- **Lista de temas**: Toca cualquier canción para reproducir inmediatamente.
- **Scrubber de progreso**: Arrastra el punto para avanzar/retroceder.
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

### 3. Instalar en tu consola física (Old 3DS / 2DS / New 3DS)
1. Copia `sound3ds.3dsx` y `sound3ds.smdh` a la carpeta `sdmc:/3ds/sound3ds/` en tu tarjeta SD.
2. Ábrelo desde el **Homebrew Launcher**.

---

## 📌 Estado Actual y Próximos Pasos

- Backend CSND integrado y mensajes visibles para diagnosticar fallos de inicialización.
- El motor de streaming actual decodifica MP3 progresivo. AAC/HLS y otras fuentes requieren un decodificador y una ruta de reproducción adicionales.
- El cambio de pista detiene el productor anterior y vacía los buffers para reducir la mezcla de audio entre canciones.
- La búsqueda y reproducción de fuentes remotas necesita validación con canciones variadas; CSND necesita validación en hardware Old 3DS/2DS.
- La evaluación de migración a YouTube Music está en [`docs/YOUTUBE_MUSIC_EVALUATION.md`](docs/YOUTUBE_MUSIC_EVALUATION.md).

## Versionado y notas de cambios

Consulta [`CHANGELOG.md`](CHANGELOG.md) para el historial. Esta versión registra el trabajo de audio, el cambio de pista y el diagnóstico de errores; no declara compatibilidad universal de formatos ni una migración de proveedor terminada.
