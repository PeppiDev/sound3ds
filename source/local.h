#ifndef LOCAL_H
#define LOCAL_H

// Música local: busca MP3 en la tarjeta SD y los reproduce leyendo el archivo
// en un hilo, igual que el streaming de red pero desde disco.

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define LOCAL_MAX_TRACKS 300

typedef struct {
    char path[256];
    char title[128];
    char artist[64];
} LocalTrack;

// Carpetas donde se buscan canciones (subcarpetas incluidas)
#define LOCAL_MUSIC_HINT "sdmc:/music"

// Recorre las carpetas de música y llena `out`. Devuelve cuántas pistas encontró.
int local_scan(LocalTrack* out, int max);

// Duración aproximada del MP3 (cabecera Xing/Info si existe; si no, por bitrate).
uint32_t local_estimate_duration_ms(const char* path);

// Empieza a leer el archivo desde la fracción `ratio` (0 = inicio) hacia el audio
bool local_start_stream(const char* path, float ratio);

void local_stop_stream(void);

bool local_is_streaming(void);

// Motivo del último fallo (cadena vacía si no hubo)
const char* local_get_error(void);

#endif // LOCAL_H
