#ifndef SUBSONIC_H
#define SUBSONIC_H

// Servidor propio de música con la API de Subsonic (Navidrome, Airsonic, Gonic...).
//
// La conexión llega desde Sound3DS Share (QR) y se guarda en
// sdmc:/3ds/sound3ds/server.json: la URL, el usuario y un token de Subsonic
// (md5(contraseña + sal)) con su sal. La contraseña nunca llega a la consola.
// Las canciones se piden convertidas a MP3, así que se reproducen como un MP3 de
// SoundCloud. Los tokens nunca se escriben en el registro.
//
// Las funciones de carga son síncronas: network.c las llama desde sus hilos.

#include <stdbool.h>
#include <stddef.h>
#include "network.h"

#define SUBSONIC_PATH "sdmc:/3ds/sound3ds/server.json"

void subsonic_init(void);
bool subsonic_configured(void);
// Nombre para mostrar ("usuario @ servidor")
void subsonic_label(char* out, size_t out_sz);

// Conecta el servidor que entrega Sound3DS Share (JSON con url, u, t, s): lo prueba
// con ping, lo guarda y queda en uso. Llamar desde un hilo de red. false con el motivo.
bool subsonic_import(const char* json_text, char* label_out, size_t label_sz, char* err, size_t err_sz);
void subsonic_disconnect(void);

// Búsquedas: canciones o álbumes
bool subsonic_search_songs(const char* query, SearchResultSet* out, volatile bool* cancel, char* err, size_t err_sz);
bool subsonic_search_albums(const char* query, PlaylistResultSet* out, volatile bool* cancel, char* err, size_t err_sz);

// Abre un álbum ("al:<id>") o una playlist del servidor ("pl:<id>") como lista de canciones
bool subsonic_load_list(const char* id, PlaylistDetail* d, volatile bool* cancel, char* err, size_t err_sz);

// Listas de las pestañas: ACC_LIST_LIKES = favoritos (canciones con estrella),
// ACC_LIST_PLAYLISTS = playlists del servidor y álbumes (de a 50), ACC_LIST_FEED =
// canciones al azar. next: "" la primera página, o el next_href de la anterior.
bool subsonic_account_list(AccListKind kind, const char* next, AccPage* out, volatile bool* cancel, char* err,
                           size_t err_sz);

// Si la URL es un stream de este servidor: la misma URL empezando en `seconds`
bool subsonic_stream_at(const char* stream_url, unsigned seconds, char* out, size_t out_sz);

#endif // SUBSONIC_H
