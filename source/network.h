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

typedef enum {
    NET_JOB_IDLE = 0,  // No hay trabajo pendiente
    NET_JOB_RUNNING,   // En curso
    NET_JOB_DONE,      // Terminó bien; el resultado se entregó en esta llamada
    NET_JOB_FAILED     // Terminó con error o sin resultados
} NetJobStatus;

// Inicializa el subsistema de red httpc / socInit
bool network_init(void);

// Cierra el subsistema de red
void network_exit(void);

// Inicia una búsqueda en SoundCloud API v2 en segundo plano (false si ya hay una en curso)
bool network_search_async(const char* query);

// Consulta la búsqueda; con NET_JOB_DONE copia los resultados en out_results
NetJobStatus network_poll_search(SearchResultSet* out_results);

// Resuelve la URL firmada directa de audio progresivo y comienza el streaming
bool network_start_stream(const char* stream_endpoint);

// Reinicia el stream actual desde la fracción `ratio` (0..1) del archivo mediante
// HTTP Range. Falla si aún no se conoce el tamaño del MP3.
bool network_seek_stream(float ratio);

// Detiene el streaming activo
void network_stop_stream(void);

// Retorna true si hay un streaming en curso
bool network_is_streaming(void);

// Obtiene el client_id actual
const char* network_get_client_id(void);

// Descarga y decodifica la carátula (JPEG/PNG a RGBA32) en segundo plano,
// cancelando la descarga anterior si seguía en curso
bool network_artwork_async(const char* url);

// Consulta la carátula; con NET_JOB_DONE entrega los píxeles (liberar con network_free_artwork)
NetJobStatus network_poll_artwork(uint8_t** out_rgba, int* out_w, int* out_h);

// Libera los pixeles RGBA decodificados
void network_free_artwork(uint8_t* rgba);

#endif // NETWORK_H
