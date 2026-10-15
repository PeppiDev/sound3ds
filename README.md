# Sound3DS 🎵
### Reproductor de Audio Estético para Nintendo 3DS / 2DS (Optimizado para Old 3DS)

**Sound3DS** es un reproductor MP3 y cliente de streaming autónomo (*standalone*) para la familia Nintendo 3DS, diseñado específicamente para garantizar fluidez absoluta a **60 FPS** en el hardware más limitado (**Old 3DS y Old 2DS** con CPU ARM11 a 268 MHz).

---

## 🌟 Características Principales

1. **Streaming Nativo SoundCloud API v2**:
   - Conexión HTTPS directa mediante `httpc` con bypass de certificados SSL anticuados (`SSLCOPT_DisableVerify`).
   - Seguimiento automático de redirecciones HTTP 302 a CloudFront (`cf-media.sndcdn.com`).
   - Búsqueda táctil con integración del teclado virtual del sistema (`swkbd`).
2. **Arquitectura Multihilo en Core 1 (Syscore)**:
   - Desbloqueo de CPU secundaria con `APT_SetAppCpuTimeLimit(30)`.
   - Streaming y decodificación `minimp3` en Core 1; Core 0 queda 100% libre para la interfaz a 60 FPS sin tirones.
3. **Pipeline de Audio DSP por Hardware**:
   - Salida estéreo PCM 16-bit a 44.1 kHz mediante `ndsp` con triple buffer `ndspWaveBuf` en memoria física lineal (`linearAlloc`) y coherencia de caché DMA (`DSP_FlushDataCache`).
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

- [x] Reproducción de audio por hardware DSP (`ndsp` + `minimp3`) funcional y verificada.
- [x] Descarga y renderizado de carátulas a 100x100 con conversión de textura PICA200 Morton 8x8.
- [x] Conexión y streaming con la API de SoundCloud (resolución de streams progresivos MP3 a 128 kbps).
- [ ] **[PENDIENTE / BUG CONOCIDO] Sobreposición de canciones**: Falta pulir el diseño de las filas de canciones en la pantalla táctil inferior para evitar que títulos y subtítulos de artista se sobrepongan, implementando cálculo de ancho en píxeles (`C2D_TextGetDimensions`) o disposición apilada en 2 renglones. Continuación en la próxima sesión.
