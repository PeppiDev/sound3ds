#ifndef FAVORITES_H
#define FAVORITES_H

// Canciones locales favoritas (la estrella del reproductor, como la lista On-The-Go del
// iPod). Se guardan en sdmc:/3ds/sound3ds/favorites.txt, una ruta por línea, y forman
// el álbum virtual "Favoritos" del cover flow.

#include <stdbool.h>
#include <stdint.h>

#define FAVORITES_PATH "sdmc:/3ds/sound3ds/favorites.txt"

void favorites_init(void);
bool favorites_has(const char* path);
// Marca o desmarca; devuelve el estado nuevo y guarda el archivo
bool favorites_toggle(const char* path);
int favorites_count(void);
// Cambia con cada marca (para rehacer el álbum Favoritos)
uint32_t favorites_generation(void);

#endif // FAVORITES_H
