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
    bool starred;     // servidor propio: marcada con estrella (favorita)
} SearchResultTrack;

typedef struct {
    int count;
    SearchResultTrack tracks[MAX_SEARCH_RESULTS];
} SearchResultSet;

// --- Playlists ---

#define MAX_PLAYLIST_TRACKS 500

typedef struct {
    char id[48];        // SoundCloud: número; servidor propio: "al:<id>" o "pl:<id>"
    char title[128];
    char creator[64];
    int track_count;
    char artwork_url[512];
} PlaylistResult;

typedef struct {
    int count;
    PlaylistResult items[MAX_SEARCH_RESULTS];
} PlaylistResultSet;

// Playlist abierta: sus canciones reproducibles en orden
typedef struct {
    char id[48];
    char title[128];
    char creator[64];
    char artwork_url[512];      // carátula de la playlist (o de su primera canción)
    int total;                  // canciones que tiene en SoundCloud
    int count;                  // canciones reproducibles cargadas en tracks
    int unavailable;            // sin stream reproducible o que no se pudieron cargar
    uint32_t total_ms;
    SearchResultTrack tracks[MAX_PLAYLIST_TRACKS];
    int position[MAX_PLAYLIST_TRACKS];   // posición en la playlist (1..total), para el número de pista
} PlaylistDetail;

typedef enum {
    NET_JOB_IDLE = 0,  // No hay trabajo pendiente
    NET_JOB_RUNNING,   // En curso
    NET_JOB_DONE,      // Terminó bien; el resultado se entregó en esta llamada
    NET_JOB_FAILED     // Terminó con error o sin resultados
} NetJobStatus;

// Inicializa el subsistema de red httpc / socInit
bool network_init(void);

// Fuente de las búsquedas, playlists y listas de las pestañas: SoundCloud, el servidor
// propio con API de Subsonic (Navidrome...) o Bandcamp. Cambiarla cancela lo que esté
// cargando.
typedef enum { NET_SOURCE_SOUNDCLOUD = 0, NET_SOURCE_SERVER, NET_SOURCE_BANDCAMP } NetSource;
void network_set_source(NetSource source);
NetSource network_get_source(void);

// Duración que se supo al resolver el stream (Bandcamp no la da en las búsquedas); 0 si no
uint32_t network_stream_duration_ms(void);

// Servidor propio: marca o desmarca una canción con estrella en segundo plano.
// network_poll_star entrega el resultado una vez (NET_JOB_DONE / FAILED) con el id.
bool network_star_async(const char* song_id, bool star);
NetJobStatus network_poll_star(char* id_out, size_t id_sz, bool* starred_out);

// Cierra el subsistema de red
void network_exit(void);

// Motivo del último fallo de búsqueda o de stream (cadena vacía si no hubo)
const char* network_get_search_error(void);
const char* network_get_stream_error(void);

// Inicia una búsqueda en SoundCloud API v2 en segundo plano (false si ya hay una en curso)
bool network_search_async(const char* query);

// Consulta la búsqueda; con NET_JOB_DONE copia los resultados en out_results
NetJobStatus network_poll_search(SearchResultSet* out_results);

// Búsqueda de playlists de usuarios (sin álbumes oficiales) en segundo plano
bool network_search_playlists_async(const char* query);
NetJobStatus network_poll_playlist_search(PlaylistResultSet* out);
const char* network_get_playlist_error(void);

// Carga una playlist con todas sus canciones y su carátula en segundo plano
bool network_playlist_async(const char* id);
// Con NET_JOB_DONE copia la playlist en out y entrega la carátula decodificada
// (liberar con network_free_artwork; puede ser NULL)
NetJobStatus network_poll_playlist(PlaylistDetail* out, uint8_t** cover_rgba, int* cover_w, int* cover_h);

// Con auth la pide con la cuenta conectada (playlists privadas de la biblioteca)
bool network_playlist_async_ex(const char* id, bool auth);

// --- Cuenta conectada: likes, playlists de la biblioteca y feed ---

#define ACC_PAGE_SIZE 50

typedef enum { ACC_LIST_LIKES = 0, ACC_LIST_PLAYLISTS, ACC_LIST_FEED } AccListKind;

typedef struct {
    int count;                                // elementos útiles en tracks o playlists
    int seen;                                 // elementos que trajo la página
    int skipped;                              // no reproducibles u otros tipos
    SearchResultTrack tracks[ACC_PAGE_SIZE];  // likes y feed
    bool repost[ACC_PAGE_SIZE];               // feed: la canción es un repost
    PlaylistResult playlists[ACC_PAGE_SIZE];  // playlists
    char next_href[1024];                     // "" = no hay más
} AccPage;

// Pide una página (next_href NULL = la primera) en segundo plano; cancela la anterior
bool network_account_list_async(AccListKind kind, const char* next_href);
// Con NET_JOB_DONE copia la página en out; kind dice de qué lista era
NetJobStatus network_poll_account_list(AccListKind* kind, AccPage* out);
const char* network_get_account_error(void);

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

// --- Descargas ---
// Las sueltas van a sdmc:/music/SoundCloud (álbum "SoundCloud"); las de una playlist a
// sdmc:/music/SoundCloud/<playlist> con el nombre y la carátula de la playlist como álbum.
// La cola se guarda en la SD tras cada cambio: si se corta (salir, HOME, apagar, sin red
// o sin espacio) se puede reanudar al volver a abrir la app.

typedef enum {
    DL_QUEUED = 0,        // en cola
    DL_ALREADY_QUEUED,    // ya estaba en la cola o descargándose
    DL_ALREADY_THERE,     // la misma pista ya está en ese álbum
    DL_PREVIEW_ONLY,      // SoundCloud solo da 30 s: no se descarga
    DL_REJECTED           // sin red o sin memoria
} DownloadResult;

// Pone la pista en la cola de descargas (un hilo en segundo plano las baja de a una)
// --- Recibir desde el PC (QR de Sound3DS Share) ---

typedef enum { RECV_SONGS = 0, RECV_ACCOUNT, RECV_SERVER } ReceiveKind;

typedef struct {
    ReceiveKind kind;
    bool ok;
    int found;           // canciones en el QR
    int queued;          // puestas en la cola de descargas
    int existing;        // ya estaban en la SD
    char username[96];   // cuenta conectada (o "usuario @ servidor")
    char error[96];
} ReceiveResult;

// Procesa el texto de un QR: la lista de canciones (van a la cola de descargas, a
// sdmc:/music), el enlace de la cuenta de SoundCloud o el de un servidor propio (quedan
// conectados al instante)
bool network_receive_async(const char* qr_text);
bool network_receive_is_account(const char* qr_text);
bool network_receive_is_server(const char* qr_text);
NetJobStatus network_poll_receive(ReceiveResult* out);

DownloadResult network_download_enqueue(const SearchResultTrack* t);

// Pone en la cola una canción de la playlist (index en pl->tracks) o la playlist completa
DownloadResult network_download_enqueue_from_playlist(const PlaylistDetail* pl, int index);
// Devuelve cuántas se pusieron en la cola; *skipped = avances de 30 s y las que ya estaban
int network_download_enqueue_playlist(const PlaylistDetail* pl, int* skipped);

// Avance de la descarga de una playlist: cuántas terminaron de cuántas. false si no se está bajando.
bool network_download_group_progress(const char* playlist_id, int* done, int* total);

// La cola quedó en pausa (sin red o sin espacio) o pendiente de una sesión anterior
bool network_download_paused(void);
// Pendientes guardadas en la SD de una sesión anterior: cuántas y un resumen para mostrar
int network_download_pending(char* summary, size_t summary_sz);
// Reanuda la cola en pausa; descartar la vacía y borra lo guardado
void network_download_resume(void);
void network_download_discard(void);

// Si la pista ya está en la carpeta de la playlist
bool network_download_exists_in_playlist(const PlaylistDetail* pl, int index);

// Descarga en curso: título, porcentaje y cuántas esperan. false si no hay nada.
bool network_download_status(char* title, size_t title_sz, int* percent, int* queued);

// Mensaje de la última descarga terminada (una vez por mensaje)
bool network_download_take_message(char* out, size_t out_sz);

// true una vez tras descargar algo nuevo (hay que volver a escanear la biblioteca)
bool network_download_take_library_changed(void);

// Ruta donde queda (o quedaría) la pista descargada y si ya existe
void network_download_path(const SearchResultTrack* t, char* out, size_t out_sz);
bool network_download_exists(const SearchResultTrack* t);

// Detiene la descarga en curso conservando la cola en la SD (al salir de la app)
void network_download_stop(void);

#endif // NETWORK_H
