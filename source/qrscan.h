#ifndef QRSCAN_H
#define QRSCAN_H

// Lector de QR con la cámara exterior (lo que antes era la app Sound3DS QR): la
// cámara captura en su hilo (camera.c) y otro hilo de baja prioridad busca un QR en
// cada cuadro con quirc, sin frenar la interfaz ni la música.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

bool qrscan_open(void);     // enciende la cámara y el decodificador
void qrscan_close(void);    // apaga todo y libera la memoria
bool qrscan_is_open(void);

// Al ir a HOME o dormir se apaga la cámara; al volver se enciende de nuevo
void qrscan_suspend(void);
void qrscan_resume(void);

// Vuelve a buscar (tras leer un código)
void qrscan_rearm(void);

// Hay imagen de la cámara desde que se abrió; la cámara no responde
bool qrscan_has_frame(void);
bool qrscan_failed(void);

// Llamar en cada cuadro: relanza la cámara si se cuelga
void qrscan_tick(void);

// Cuadro nuevo para dibujar (400x240 RGB565): true y lo bloquea; liberar con qrscan_unlock_frame
bool qrscan_lock_new_frame(const u16** frame);
void qrscan_unlock_frame(void);

// Texto del QR leído (una vez); false si todavía no hay
bool qrscan_take(char* out, size_t out_sz);

#ifdef SOUND3DS_AUTOTEST
// Prueba en el emulador (sin cámara): simula la lectura de un QR
void qrscan_inject(const char* text);
#endif

#endif
