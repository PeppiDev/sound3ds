#ifndef LOCAL_H
#define LOCAL_H

// Música local: busca MP3 en la tarjeta SD y los reproduce leyendo el archivo
// en un hilo, igual que el streaming de red pero desde disco.

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LOCAL_MAX_TRACKS 2000

typedef struct {
    char path[256];
    char title[128];
    char artist[64];
    char album[96];          // TALB (vacío si no tiene)
    char album_artist[64];   // TPE2 (vacío si no tiene)
    int track_no;            // TRCK (0 si no tiene)
    uint32_t duration_ms;    // TLEN (0 si no tiene: se estima al reproducir)
    uint32_t cover_off;      // posición de la imagen embebida (APIC) en el archivo
    uint32_t cover_len;      // tamaño de la imagen (0 = sin carátula)
    uint32_t file_size;
    bool is_aac;             // .aac con tramas ADTS (descargas de SoundCloud en AAC)
} LocalTrack;

// Metadatos que se escriben en una etiqueta ID3v2.3 nueva
typedef struct {
    const char* title;
    const char* artist;
    const char* album;
    const char* album_artist;
    int track_no;            // 0 = no escribir
    uint32_t duration_ms;    // 0 = no escribir
    const uint8_t* jpeg;     // carátula (puede ser NULL)
    size_t jpeg_len;
    const char* sc_id;       // ID de la pista en SoundCloud (marco TXXX SOUNDCLOUD_ID); NULL = no escribir
} LocalTagInfo;

// Carpetas donde se buscan canciones (subcarpetas incluidas)
#define LOCAL_MUSIC_HINT "sdmc:/music"
// Donde se guardan las descargas de SoundCloud (forman el álbum "SoundCloud")
#define LOCAL_SC_DIR     "sdmc:/music/SoundCloud"
#define LOCAL_SC_ALBUM   "SoundCloud"
// Descripción del marco TXXX con el ID de la pista en SoundCloud
#define LOCAL_SC_ID_DESC "SOUNDCLOUD_ID"

// Recorre las carpetas de música y llena `out` ordenado por álbum (SoundCloud primero,
// sin álbum al final), número de pista y título. Devuelve cuántas pistas encontró.
int local_scan(LocalTrack* out, int max);

// Lee la imagen embebida de la pista (malloc; liberar con free). NULL si no tiene.
uint8_t* local_read_cover(const LocalTrack* t, size_t* out_len);

// Escribe una etiqueta ID3v2.3 completa al inicio de `f` (posición actual)
bool local_write_id3(FILE* f, const LocalTagInfo* info);

// Lee el ID de SoundCloud guardado en la etiqueta (TXXX SOUNDCLOUD_ID). false si no lo tiene.
bool local_read_sc_id(const char* path, char* out, size_t out_sz);

// Duración aproximada del MP3 (cabecera Xing/Info si existe; si no, por bitrate).
uint32_t local_estimate_duration_ms(const char* path);

// Empieza a leer el archivo desde la fracción `ratio` (0 = inicio) hacia el audio
bool local_start_stream(const char* path, float ratio);

void local_stop_stream(void);

bool local_is_streaming(void);

// Motivo del último fallo (cadena vacía si no hubo)
const char* local_get_error(void);

#endif // LOCAL_H
