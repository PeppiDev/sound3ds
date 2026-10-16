# Historial de cambios

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
