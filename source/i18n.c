#include "i18n.h"
#include <stdint.h>
#include <string.h>

static volatile Lang s_lang = LANG_EN;

static const char* const s_pairs[][2] = {
    // Barra superior y reproductor
    {"PAUSA", "PAUSED"},
    {"SIN STREAM", "NO STREAM"},
    {"Cargando...", "Loading..."},
    {"Buscando...", "Searching..."},
    {"filtrar", "filter"},
    {"Nada sonando", "Nothing playing"},
    {"Toca la lupa para buscar", "Tap the magnifier to search"},
    {"Copia tus MP3 a sdmc:/music", "Copy your MP3s to sdmc:/music"},
    {"Nada coincide con el filtro", "Nothing matches the filter"},
    // Cover flow
    {"Ningún álbum coincide con el filtro", "No album matches the filter"},
    {"No hay música en la tarjeta SD", "No music on the SD card"},
    {"Copia tus MP3 con etiquetas a sdmc:/music", "Copy your tagged MP3s to sdmc:/music"},
    {"o descárgalos desde SoundCloud", "or download them from SoundCloud"},
    {"canción", "song"},
    {"canciones", "songs"},
    {"D-pad izq./der.: álbum - arriba/abajo: canción - A: reproducir", "D-pad left/right: album - up/down: song - A: play"},
    {"Sin álbum", "No album"},
    {"Varios artistas", "Various artists"},
    {"Descargas", "Downloads"},
    // Minijuego
    {"Récord %d", "Best %d"},
    {"¿Listo?", "Ready?"},
    {"Analógico mover - X impulso - Y onda", "Circle Pad move - X dash - Y wave"},
    {"Fin de la partida", "Game over"},
    {"Puntos %d", "Score %d"},
    {"Nuevo récord", "New record"},
    {"Sigue intentando", "Keep trying"},
    {"X para jugar otra vez", "X to play again"},
    // Mascota y menú
    {"Zzz... toca algo para despertarlo", "Zzz... touch something to wake it up"},
    {"Streaming desde SoundCloud", "Streaming from SoundCloud"},
    {"Música de tu tarjeta SD", "Music from your SD card"},
    {"Cargando la canción...", "Loading the song..."},
    {"¡Qué temazo!", "What a banger!"},
    {"Elige qué quieres escuchar", "Choose what to listen to"},
    {"¿Qué quieres escuchar?", "What do you want to hear?"},
    {"Busca y escucha en streaming", "Search and stream"},
    {"MP3 en sdmc:/music", "MP3s in sdmc:/music"},
    // Avisos
    {"%d canciones en %d álbumes", "%d songs in %d albums"},
    {"No se pudo iniciar la pista", "Couldn't start the track"},
    {"Buscar en SoundCloud", "Search SoundCloud"},
    {"Filtrar (vacio = todas)", "Filter (empty = all)"},
    {"Descargando: %.70s", "Downloading: %.70s"},
    {"Ya está en la cola de descargas", "Already in the download queue"},
    {"Ya está en tu álbum SoundCloud", "Already in your SoundCloud album"},
    {"Solo hay una preview de 30 s: no se descarga", "Only a 30 s preview: can't download"},
    {"No se pudo poner en la cola", "Couldn't queue it"},
    {"La busqueda fallo", "Search failed"},
    {"%d de %d son previews de 30 s", "%d of %d are 30 s previews"},
    {" (%d en cola)", " (%d queued)"},
    {"No llego audio", "No audio arrived"},
    // Red y descargas
    {"SoundCloud respondio HTTP %ld", "SoundCloud returned HTTP %ld"},
    {"Red: %s", "Network: %s"},
    {"Respuesta invalida (linea %d)", "Invalid response (line %d)"},
    {"%d resultados, ninguno reproducible", "%d results, none playable"},
    {"Sin resultados", "No results"},
    {"Lista HLS: %s", "HLS playlist: %s"},
    {"Lista HLS no compatible", "Unsupported HLS playlist"},
    {"HLS sin segmento de inicio", "HLS without an init segment"},
    {"Formato de audio no compatible", "Unsupported audio format"},
    {"Segmento %d danado", "Segment %d is damaged"},
    {"Segmento %d dañado", "Segment %d is damaged"},
    {"No se pudo obtener el stream", "Couldn't get the stream"},
    {"No se pudo escribir en la SD", "Couldn't write to the SD card"},
    {"Ya estaba descargada: %.60s", "Already downloaded: %.60s"},
    {"No se pudo obtener: %.60s", "Couldn't fetch: %.60s"},
    {"No se pudo crear el archivo en la SD", "Couldn't create the file on the SD card"},
    {"Descargada: %.70s", "Downloaded: %.70s"},
    {"Descarga cancelada", "Download cancelled"},
    {"Falló la descarga%s%s", "Download failed%s%s"},
    {"No se pudo abrir el archivo", "Couldn't open the file"},
    {"No se pudo crear el hilo de lectura", "Couldn't create the reader thread"},
    // Configuración
    {"Configuración", "Settings"},
    {"Idioma", "Language"},
    {"LED RGB al reproducir", "RGB LED while playing"},
    {"El LED de notificación brilla en arcoíris", "The notification LED glows in a rainbow"},
    {"Activado", "On"},
    {"Desactivado", "Off"},
    {"Cerrar", "Close"},
    {"Toca una opción o usa la cruceta y A - B: volver", "Tap an option or use the D-pad and A - B: back"},
    {"Idioma: Español", "Language: English"},
    {"LED RGB activado", "RGB LED on"},
    {"LED RGB desactivado", "RGB LED off"},
    // Playlists y cola de descargas
    {"Cargando playlist...", "Loading playlist..."},
    {"%d canciones - %lu min", "%d songs - %lu min"},
    {"%d no disponibles", "%d unavailable"},
    {"Descargar", "Download"},
    {"Reanudar", "Resume"},
    {"Descartar", "Discard"},
    {"Descargas pendientes", "Pending downloads"},
    {"Hay una descarga sin terminar: %s. ¿Quieres reanudarla?", "There's an unfinished download: %s. Resume it?"},
    {"%.60s (%d de %d)", "%.60s (%d of %d)"},
    {"%d canciones en cola", "%d songs queued"},
    {"Playlist %.60s: %d canciones descargadas", "Playlist %.60s: %d songs downloaded"},
    {"Playlist %.50s: %d de %d descargadas", "Playlist %.50s: %d of %d downloaded"},
    {"Sin espacio en la SD: descargas en pausa", "SD card is full: downloads paused"},
    {"Sin conexión: descargas en pausa", "No connection: downloads paused"},
    {"Ya está en el álbum %.60s", "Already in the album %.60s"},
    {"Descargando %d canciones (%d omitidas)", "Downloading %d songs (%d skipped)"},
    {"Descargando %d canciones de la playlist", "Downloading %d songs from the playlist"},
    {"La playlist ya se está descargando", "The playlist is already downloading"},
    {"Ya estaba todo descargado", "Everything was already downloaded"},
    {"Abre la playlist para descargarla", "Open the playlist to download it"},
    {"Buscar playlists en SoundCloud", "Search playlists on SoundCloud"},
    {"Toca la lupa para buscar playlists", "Tap the magnifier to search playlists"},
    {"No se pudo abrir la playlist", "Couldn't open the playlist"},
    {"La playlist no tiene canciones reproducibles", "The playlist has no playable songs"},
    {"Descargando %d de %d", "Downloading %d of %d"},
    {"Reanudando descargas", "Resuming downloads"},
    {"Descargas descartadas", "Downloads discarded"},
};

#define NPAIRS (sizeof(s_pairs) / sizeof(s_pairs[0]))
#define HSIZE 256   // tabla hash por contenido (potencia de 2, más del doble de entradas)

static int16_t s_hash[HSIZE];
static int s_ready = 0;

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void build(void) {
    for (int i = 0; i < HSIZE; i++) s_hash[i] = -1;
    for (int i = 0; i < (int)NPAIRS; i++) {
        uint32_t k = fnv(s_pairs[i][0]) & (HSIZE - 1);
        while (s_hash[k] >= 0) k = (k + 1) & (HSIZE - 1);
        s_hash[k] = (int16_t)i;
    }
    s_ready = 1;
}

void i18n_set(Lang lang) {
    s_lang = lang;
}

Lang i18n_get(void) {
    return s_lang;
}

const char* L(const char* es) {
    if (s_lang == LANG_ES || !es || !*es) return es;
    if (!s_ready) build();
    uint32_t k = fnv(es) & (HSIZE - 1);
    while (s_hash[k] >= 0) {
        if (strcmp(s_pairs[s_hash[k]][0], es) == 0) return s_pairs[s_hash[k]][1];
        k = (k + 1) & (HSIZE - 1);
    }
    return es;
}
