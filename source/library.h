#ifndef LIBRARY_H
#define LIBRARY_H

// Biblioteca local agrupada por álbum (etiqueta TALB) para el cover flow.
// Las carátulas se decodifican en un hilo de baja prioridad, empezando por el
// álbum enfocado, y se guardan en sdmc:/3ds/sound3ds/covers para abrir rápido
// la próxima vez. Cada carátula queda como textura RGB565 de 128x128 ya en el
// orden de mosaico de la GPU.

#include <3ds.h>
#include <stdbool.h>
#include "local.h"

#define LIB_COVER_SIZE 128
#define LIB_MAX_ALBUMS LOCAL_MAX_TRACKS

typedef struct {
    char name[96];       // "Sin álbum" para las pistas sin etiqueta
    char artist[64];     // artista del álbum, o "Varios artistas"
    int first, count;    // rango de sus pistas en el arreglo de pistas (contiguo)
    bool is_soundcloud;  // álbum de descargas de SoundCloud: carátula propia
    bool is_unknown;     // pistas sin álbum
    bool has_art;        // alguna pista trae carátula embebida
} LibAlbum;

// Agrupa las pistas (ya ordenadas por local_scan) y arranca la carga de carátulas.
// Detiene y descarta la biblioteca anterior.
void library_build(const LocalTrack* tracks, int count);

// Detiene el hilo y libera las carátulas
void library_exit(void);

int library_album_count(void);
const LibAlbum* library_album(int i);

// Álbum que contiene la pista (índice en el arreglo de pistas), -1 si ninguno
int library_album_of_track(int track_idx);

// Álbum enfocado en el cover flow: sus carátulas (y las vecinas) se cargan primero
void library_set_focus(int album);

// Cambia cada vez que se reconstruye la biblioteca (para invalidar texturas)
u32 library_generation(void);

// Copia la carátula lista (128x128 RGB565 en mosaico) a dst. false si aún no está.
bool library_copy_cover(int album, u16* dst);

#endif // LIBRARY_H
