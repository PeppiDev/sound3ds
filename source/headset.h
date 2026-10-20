#ifndef HEADSET_H
#define HEADSET_H

// Botón de los audífonos con micrófono (el del cable, como en los celulares).
//
// El botón deja en corto la línea del micrófono y el códec del 3DS lo detecta por
// hardware (página 100, registro 0x2C). Se lee con cdc:CHK, el mismo servicio que
// fuerza los parlantes con la tapa cerrada.
//
// Gestos, como en los celulares: 1 toque = pausa/reanuda, 2 = siguiente, 3 = anterior.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    HEADSET_NONE = 0,
    HEADSET_TOGGLE,
    HEADSET_NEXT,
    HEADSET_PREV
} HeadsetAction;

void headset_init(void);
void headset_exit(void);

// Llamar en cada cuadro (con la tapa abierta o cerrada). Devuelve el gesto terminado.
HeadsetAction headset_poll(u64 now_ms);

// Línea de diagnóstico (build de prueba)
void headset_diag(char* out, size_t out_sz);

#endif // HEADSET_H
