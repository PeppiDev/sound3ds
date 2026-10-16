#ifndef NETWORK_H
#define NETWORK_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SEARCH_RESULTS 50

// Formato del stream elegido para una pista
typedef enum {
    STREAM_NONE = 0,
    STREAM_MP3,      // MP3 progresivo (descarga continua)
    STREAM_HLS_AAC   // Lista HLS con segmentos fMP4 y audio AAC
} StreamKind;

typedef struct {
    char id[32];
    char title[128];
    char artist[64];
    uint32_t duration_ms;
    char stream_endpoint[512];
    char artwork_url[512];
    StreamKind kind;
    bool is_preview;  // SoundCloud solo entrega un fragmento de 30 s
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

// Motivo del último fallo de búsqueda o de stream (cadena vacía si no hubo)
const char* network_get_search_error(void);
const char* network_get_stream_error(void);

// Inicia una búsqueda en SoundCloud API v2 en segundo plano (false si ya hay una en curso)
bool network_search_async(const char* query);

// Consulta la búsqueda; con NET_JOB_DONE copia los resultados en out_results
NetJobStatus network_poll_search(SearchResultSet* out_results);

// Resuelve el endpoint de la pista y comienza el streaming con el decodificador
// que corresponde al formato
bool network_start_stream(const char* stream_endpoint, StreamKind kind);

// Reinicia el stream actual en la fracción `ratio` (0..1) de la pista: en MP3 con
// HTTP Range, en HLS desde el segmento que contiene ese instante. Devuelve en
// start_ms el instante real desde el que sonará. Falla si aún no se conoce el
// tamaño (MP3) o la lista de segmentos (HLS).
bool network_seek_stream(float ratio, uint32_t duration_ms, uint32_t* start_ms);

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
