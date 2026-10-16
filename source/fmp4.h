#ifndef FMP4_H
#define FMP4_H

// Demuxer mínimo de MP4 fragmentado (CMAF) para audio AAC, como el que sirve
// SoundCloud por HLS: un init.mp4 con la configuración y segmentos moof+mdat.
// Sin dependencias de la 3DS para poder probarlo en el PC.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int object_type;      // 2 = AAC-LC
    int sample_rate_index;
    int sample_rate;
    int channels;
    uint32_t timescale;
    uint32_t default_sample_size;     // de trex (0 si no viene)
    uint32_t default_sample_duration; // de trex (0 si no viene)
} Fmp4AudioConfig;

// Recibe cada trama AAC cruda; devuelve false para detener el recorrido.
typedef bool (*Fmp4FrameCallback)(const uint8_t* frame, size_t size, void* user);

// Lee la configuración de audio del segmento de inicialización.
bool fmp4_parse_init(const uint8_t* data, size_t len, Fmp4AudioConfig* out);

// Recorre las tramas de un segmento multimedia. Devuelve cuántas entregó,
// o -1 si el segmento está mal formado.
int fmp4_parse_segment(const uint8_t* data, size_t len, const Fmp4AudioConfig* cfg,
                       Fmp4FrameCallback cb, void* user);

// Escribe la cabecera ADTS de 7 bytes para una trama de `frame_len` bytes.
void fmp4_adts_header(uint8_t out[7], const Fmp4AudioConfig* cfg, size_t frame_len);

#endif // FMP4_H
