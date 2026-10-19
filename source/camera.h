#ifndef CAMERA_H
#define CAMERA_H

// Cámara exterior a 400x240 RGB565 capturando sin parar en su propio hilo.
// El hilo atiende la recepción de cuadros y el aviso de búfer lleno de la
// cámara (que la detiene); así la imagen nunca se queda pegada aunque el resto
// de la app tarde en decodificar o dibujar.

#include <3ds.h>
#include <stdbool.h>

#define CAM_W 400
#define CAM_H 240

bool camera_start(void);
void camera_stop(void);
bool camera_running(void);
// Libera el cuadro guardado (con la cámara detenida)
void camera_free(void);

// Contador de cuadros recibidos (cambia con cada cuadro nuevo)
u32 camera_frame_seq(void);
// Milisegundos desde el último cuadro (para detectar una captura colgada)
u64 camera_ms_since_frame(void);

// Acceso al último cuadro completo; mantener el bloqueo lo menos posible
const u16* camera_lock_frame(void);
void camera_unlock_frame(void);

// Se señala con cada cuadro nuevo (un solo hilo debe esperarlo)
LightEvent* camera_frame_event(void);

#endif
