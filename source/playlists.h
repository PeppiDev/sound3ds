#ifndef PLAYLISTS_H
#define PLAYLISTS_H

// Playlists propias de la música local. Cada una es un archivo .m3u en
// sdmc:/3ds/sound3ds/playlists/ con las rutas de sus canciones (solo referencias: no se
// copia ningún audio). Se ven en el cover flow de Local como álbumes virtuales, después
// de "Todas las canciones" y "Favoritos". El formato es estándar: también se pueden
// armar o editar en el PC.

#include <stdbool.h>
#include <stdint.h>

#define PLAYLISTS_DIR      "sdmc:/3ds/sound3ds/playlists"
#define PLAYLISTS_MAX      32
#define PLAYLIST_MAX_SONGS 500
#define PLAYLIST_NAME_LEN  48

void playlists_init(void);
int playlists_count(void);
const char* playlists_name(int i);
int playlists_size(int i);
// Ruta de la canción k de la playlist i
const char* playlists_song(int i, int k);
bool playlists_contains(int i, const char* path);

// Crea una playlist vacía (el nombre se limpia para que sirva como nombre de archivo).
// Devuelve su índice, o -1 si ya hay demasiadas, el nombre queda vacío o ya existe.
int playlists_create(const char* name);
// Agrega al final (false si ya estaba o está llena)
bool playlists_add(int i, const char* path);
bool playlists_remove(int i, const char* path);
bool playlists_rename(int i, const char* name);
bool playlists_delete(int i);

// Cambia con cada modificación (para rehacer el cover flow)
uint32_t playlists_generation(void);

#endif // PLAYLISTS_H
