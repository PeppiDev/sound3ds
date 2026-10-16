# Historial de cambios

## [0.4.0] - 2026-10-16

### Añadido
- Reproducción de canciones completas de SoundCloud en HLS + AAC, decodificadas en la consola: lista m3u8, segmentos fMP4 (`fmp4.c`, `hls.c`) y decodificador Helix AAC en punto fijo (`source/helix-aac`, RPSL) con el camino ARMv6 en ensamblador.
- La búsqueda elige por pista la mejor transcodificación: MP3 completo, luego HLS AAC, luego la preview MP3 de 30 s.
- La barra de progreso en HLS salta al segmento que contiene el instante elegido.
- Indicador de carga del decodificador en la pantalla superior (por ejemplo `NDSP AAC 6%`).
- Prueba en el PC del camino HLS/AAC (`tests/host/run.sh`), comparada contra ffmpeg.
- Ícono de batería en la cabecera: porcentaje exacto vía `mcu::HWC` (o 5 niveles vía `ptm:u`), rojo bajo 20 % y con rayo al cargar.
- Resultados paginados: hasta 50 por búsqueda en páginas de 5, con paginador táctil y la cruceta (arriba/abajo recorre canciones, izquierda/derecha cambia de página; (A) reproduce la enfocada).
- Íconos vectoriales: anterior/siguiente y reproducir/pausa en botones circulares, paleta de colores para cambiar el tema y lupa con `: search` en la barra de búsqueda.

### Cambiado
- La búsqueda usa jansson para leer el JSON de SoundCloud en vez de buscar texto.
- La salida de audio sigue la frecuencia de la pista (44,1 o 48 kHz).
- Nada se reproduce solo: ni al abrir la app ni al terminar una búsqueda.
- Se quitó el control de volumen de la app: queda al máximo y el nivel lo fija el deslizador físico de la consola. Su lugar lo ocupa el paginador.
- El botón de tema pasa a ser solo el ícono de paleta.
- Ecualizador rediseñado y más bajo para no chocar con la carátula: barras redondeadas con degradado de graves a agudos, picos en píldora y reflejo (Modern); visor LED empotrado de 12 segmentos con LEDs apagados tenues (Y2K).
- Los círculos se dibujan como abanicos de triángulos: Azahar no dibuja `C2D_DrawCircleSolid` (el vinilo de la carátula tampoco se veía).

### Corregido
- Elegir un resultado sin stream MP3 reproducía el audio demo en su lugar.
- Los errores de búsqueda y de stream se muestran en pantalla.

## [0.3.0] - 2026-10-16

### Añadido
- Backend de audio NDSP con cola de buffers sin huecos y pausa real; CSND queda como respaldo automático cuando falta `sdmc:/3ds/dspfirm.cdc`.
- Indicador del backend activo, "Buscando..." durante la búsqueda y "Cargando..." mientras llega la carátula.
- Adelantar con la barra de progreso: al soltarla, el stream se reinicia desde el byte proporcional con HTTP Range (reutiliza la URL resuelta del CDN y la vuelve a resolver si caducó); en el demo se salta dentro del MP3 incrustado.
- Empaquetado `.cia` con `scripts/build_cia.sh`, ícono, banner y sonido propios (título `000400000F5D3D00`).
- Paso automático a la siguiente pista al terminar un stream; si una pista no entrega audio se muestra "SIN STREAM" y (A) la reintenta.

### Corregido
- Cortes periódicos de audio: CSND ahora reproduce un anillo PCM en bucle en lugar de bloques sueltos con silencio entre ellos.
- La búsqueda y la descarga de carátulas ya no congelan la interfaz; las transferencias se cancelan al cambiar de pista y el stream se corta si la conexión queda colgada.
- Colores de la carátula (orden de bytes de `GPU_RGBA8`).
- La lista muestra los 5 resultados de búsqueda; títulos y artistas se recortan por ancho real y ya no se superponen.
- El tiempo transcurrido sigue al audio reproducido en lugar del reloj.
- Lectura del buffer MP3 con `memcpy` y salto directo de etiquetas ID3.

### Cambiado
- `sound3ds.3dsx` y `sound3ds.smdh` dejan de versionarse; los binarios se publican en GitHub Releases.
- La búsqueda pide 20 pistas en un buffer de 256 KB (antes 35 en 128 KB).
- Se quitó `romfs/demo.mp3`, que se empaquetaba sin usarse; el demo sigue incrustado desde `data/demo.bin`.

### Pendiente de validar
- Reproducción continua con NDSP y CSND en una Old 3DS/2DS física.
- Orientación de la carátula descargada.

## [0.2.0] - 2026-10-16

### Añadido
- Diagnóstico en pantalla cuando CSND no puede inicializarse, con el código o causa informada por el servicio.
- Buffers estéreo separados para emitir audio PCM mediante dos canales CSND y limpieza de caché antes de reproducirlos.
- Manejo de muestras MP3 restantes entre bloques para evitar descartar el final de una trama al llenar un buffer.

### Corregido
- Al elegir otra pista se detiene el flujo anterior y se descartan los buffers pendientes, evitando que el audio de la canción previa se mezcle con la nueva.
- El audio de demostración queda reservado al modo de demostración y no reemplaza silenciosamente una búsqueda sin stream compatible.
- La transferencia de red reintenta alimentar el ring buffer cuando está lleno y se detiene limpiamente al cancelar el flujo.
- Se restableció la terminación nula de títulos y artistas copiados a los modelos de reproducción.

### Documentación
- Se actualizó el README para describir el backend y los límites de validación sin afirmar compatibilidad universal ni 60 FPS garantizados.
- Se añadió una evaluación de la alternativa YouTube Music y de su encaje con la plataforma 3DS.

### Pendiente de validar
- Reproducción continua y cambio entre canciones en Azahar y en una Old 3DS/2DS física.
- Diferentes respuestas de streaming, cortes de red, pausado y reanudación.
- El backend actual acepta MP3 progresivo; AAC/HLS no está implementado en esta versión.
