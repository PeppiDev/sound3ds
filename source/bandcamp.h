#ifndef BANDCAMP_H
#define BANDCAMP_H

// Bandcamp sin cuenta: búsqueda, álbumes y Descubrir, con las mismas rutas que usa la
// web y la app oficial (no hay API pública para escuchar). Se escucha el MP3 de 128 kbps
// que Bandcamp ofrece para escuchar gratis; no se descarga nada.
//
// Las canciones guardan como stream_endpoint "bc:t:<band_id>:<track_id>": la URL del MP3
// se pide al reproducir (bandcamp_resolve), porque las que entrega la API vencen.
// Los álbumes se abren con el id "bc:a:<band_id>:<album_id>".
//
// Las funciones son síncronas: network.c las llama desde sus hilos.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "network.h"

bool bandcamp_search_tracks(const char* query, SearchResultSet* out, volatile bool* cancel, char* err, size_t err_sz);
bool bandcamp_search_albums(const char* query, PlaylistResultSet* out, volatile bool* cancel, char* err, size_t err_sz);

// Abre un álbum ("bc:a:<band_id>:<album_id>") como lista de canciones
bool bandcamp_load_album(const char* id, PlaylistDetail* d, volatile bool* cancel, char* err, size_t err_sz);

// Pestañas (Descubrir de Bandcamp): ACC_LIST_LIKES = canciones populares,
// ACC_LIST_PLAYLISTS = álbumes destacados, ACC_LIST_FEED = canciones nuevas.
// next: "" la primera página, o el cursor de la anterior.
bool bandcamp_discover(AccListKind kind, const char* next, AccPage* out, volatile bool* cancel, char* err,
                       size_t err_sz);

// "bc:t:..." -> URL del MP3 y su duración
bool bandcamp_resolve(const char* endpoint, char* url_out, size_t url_sz, uint32_t* duration_ms, volatile bool* cancel,
                      char* err, size_t err_sz);

#endif // BANDCAMP_H
