# Historial de cambios

## [0.3.0] - 2026-10-16

### Añadido
- Backend de audio NDSP con cola de buffers sin huecos y pausa real; CSND queda como respaldo automático cuando falta `sdmc:/3ds/dspfirm.cdc`.
- Indicador del backend activo, "Buscando..." durante la búsqueda y "Cargando..." mientras llega la carátula.
- Paso automático a la siguiente pista al terminar un stream; si una pista no entrega audio se muestra "SIN STREAM" y (A) la reintenta.

### Corregido
- Cortes periódicos de audio: CSND ahora reproduce un anillo PCM en bucle en lugar de bloques sueltos con silencio entre ellos.
- La búsqueda y la descarga de carátulas ya no congelan la interfaz; las transferencias se cancelan al cambiar de pista y el stream se corta si la conexión queda colgada.
- Colores de la carátula (orden de bytes de `GPU_RGBA8`).
- La lista muestra los 5 resultados de búsqueda; títulos y artistas se recortan por ancho real y ya no se superponen.
- El tiempo transcurrido sigue al audio reproducido en lugar del reloj.
- Lectura del buffer MP3 con `memcpy` y salto directo de etiquetas ID3.

### Cambiado
- La barra de progreso es solo indicador hasta que exista búsqueda dentro de la pista.
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
