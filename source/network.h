#ifndef NETWORK_H
#define NETWORK_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SEARCH_RESULTS 5

typedef struct {
    char id[32];
    char title[128];
    char artist[64];
    uint32_t duration_ms;
    char stream_endpoint[512];
    char artwork_url[512];
    bool has_progressive;
} SearchResultTrack;

typedef struct {
    int count;
    SearchResultTrack tracks[MAX_SEARCH_RESULTS];
} SearchResultSet;

// Inicializa el subsistema de red httpc / socInit
bool network_init(void);

// Cierra el subsistema de red
void network_exit(void);

// Busca canciones en SoundCloud API v2
bool network_search_tracks(const char* query, SearchResultSet* out_results);

// Resuelve la URL firmada directa de audio progresivo y comienza el streaming
bool network_start_stream(const char* stream_endpoint);

// Detiene el streaming activo
void network_stop_stream(void);

// Retorna true si hay un streaming en curso
bool network_is_streaming(void);

// Obtiene el client_id actual
const char* network_get_client_id(void);

// Descarga y decodifica la carátula de álbum (JPEG/PNG a RGBA32)
bool network_download_artwork(const char* url, uint8_t** out_rgba, int* out_w, int* out_h);

// Libera los pixeles RGBA decodificados
void network_free_artwork(uint8_t* rgba);

#endif // NETWORK_H
