#ifndef HLS_H
#define HLS_H

// Lector de listas HLS (m3u8) de solo audio. Portable para poder probarlo en el PC.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char* uri;
    uint32_t start_ms;    // inicio del segmento dentro de la pista
    uint32_t duration_ms;
} HlsSegment;

typedef struct {
    bool is_master;       // lista maestra: hay que descargar variant_uri
    char* variant_uri;
    char* init_uri;       // EXT-X-MAP (NULL si los segmentos son autocontenidos)
    HlsSegment* segments;
    int count;
    uint32_t total_ms;
} HlsPlaylist;

// Interpreta el texto de una lista. Las URIs relativas se resuelven contra base_url.
// Falla si la lista está cifrada (EXT-X-KEY distinto de NONE) o no tiene segmentos.
bool hls_parse(const char* text, const char* base_url, HlsPlaylist* out);

void hls_free(HlsPlaylist* pl);

// Índice del segmento que contiene `ms` (el último si se pasa del final).
int hls_segment_at(const HlsPlaylist* pl, uint32_t ms);

#endif // HLS_H
