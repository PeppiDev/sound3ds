#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "audio.h"
#include "fft.h"
#include "gfx.h"
#include "theme.h"
#include "ui.h"
#include "network.h"
#include "account.h"
#include "qrscan.h"
#include "local.h"
#include "debuglog.h"
#include "game.h"
#include "library.h"
#include "settings.h"
#include "i18n.h"
#include "headset.h"
#include "subsonic.h"
#include "bandcamp.h"
#include "favorites.h"
#include "playlists.h"
#include <math.h>

// Tiempo sin tocar nada tras el cual la mascota se duerme
#define MASCOT_SLEEP_MS 10000

// De dónde viene la pista que suena
typedef enum {
    SRC_NONE = 0,
    SRC_SOUNDCLOUD,
    SRC_LOCAL,
    SRC_PLAYLIST,  // canciones de la playlist de SoundCloud abierta
    SRC_LIKES,     // likes de la cuenta conectada
    SRC_FEED       // feed de la cuenta conectada
} TrackSource;

static AppMode s_mode = MODE_MENU;
static TrackSource s_playing_src = SRC_NONE;

// Posición (ms) desde la que arrancó el audio actual tras un adelanto
static uint32_t s_seek_base_ms = 0;

// Se pidió reproducir y todavía no suena audio (para el estado "cargando" de la mascota)
static bool s_waiting_audio = false;

// Reposo con música: con la tapa cerrada (o el interruptor de reposo de la 2DS) la app
// sigue sonando con las pantallas apagadas y solo responde a pausa / anterior / siguiente
static bool s_lid_closed = false;
// Minijuego en la pantalla superior del reproductor: solo avanza mientras se ve
static GameState s_game;
static bool s_show_game = false;
// Con la consola cerrada, dos toques del mismo gatillo dentro de este tiempo cambian de
// pista; uno solo pausa
#define DOUBLE_TAP_MS 400
// Modo "Mantener": hay que dejar L o R apretado este tiempo (un roce en el bolsillo no cuenta)
#define LID_HOLD_MS 800
static bool s_ptmu_ok = false;
// Pantalla de configuración (idioma y LED RGB) abierta sobre la pantalla de abajo
static bool s_settings_open = false;
static SettingsViewModel s_settings_vm;
// Recibir desde el PC: lector de QR de Sound3DS Share (canciones o la cuenta)
#define SCAN_FOUND_ANIM_S 0.45f   // cuánto se muestra "QR detectado" antes de procesarlo
static bool s_scan_open = false;
static ScanViewModel s_scan_vm;
static char s_scan_text[1024];

// Estáticos: las listas no caben en la pila del hilo principal
static PlaybackViewModel s_playback;
static SearchViewModel s_sc_view;
static SearchViewModel s_local_view;
static SearchResultSet s_sc_results;

// SoundCloud: qué se busca (canciones o playlists) y la playlist abierta
typedef enum { SC_KIND_TRACKS = 0, SC_KIND_PLAYLISTS = 1 } ScKind;
static ScKind s_sc_kind = SC_KIND_TRACKS;
static SearchViewModel s_plr_view;          // resultados de la búsqueda de playlists
static PlaylistResultSet s_plr_results;
static bool s_in_playlist = false;          // se está viendo una playlist abierta
static bool s_pl_loading = false;
static SearchViewModel s_pl_view;           // canciones de la playlist abierta
static PlaylistDetail s_pl;
static PlaylistTopViewModel s_pl_top;
static int s_pl_marks_page = -1;            // página de la playlist con las marcas de descargada al día
static u32 s_marks_gen = 0, s_pl_marks_gen = 0;

// Cuenta conectada: pestañas Buscar / Likes / Playlists / Feed. Cada lista de la
// cuenta se carga de a 50 y pide la página siguiente al llegar al final.
typedef enum { SC_TAB_SEARCH = 0, SC_TAB_LIKES, SC_TAB_PLAYLISTS, SC_TAB_FEED } ScTab;
static ScTab s_sc_tab = SC_TAB_SEARCH;
typedef struct {
    SearchViewModel view;
    bool loading, loaded, appending;
    char next[1024];          // next_href de la API ("" = no hay más)
    int empty_pages;          // páginas seguidas sin nada útil (para no pedir sin fin)
    int marks_page;           // página con las marcas de descargada al día
    u32 marks_gen;
} AccList;
static AccList s_acc[3];      // índice = AccListKind
static SearchResultTrack s_likes_tracks[MAX_LIST_ITEMS];
static SearchResultTrack s_feed_tracks[MAX_LIST_ITEMS];
static PlaylistResult s_accpl_items[MAX_LIST_ITEMS];
static AccPage s_acc_page;
static bool s_account_was_on = false;

static AccListKind tab_list(ScTab t) {
    return t == SC_TAB_LIKES ? ACC_LIST_LIKES : (t == SC_TAB_PLAYLISTS ? ACC_LIST_PLAYLISTS : ACC_LIST_FEED);
}

// Navegador en línea: SoundCloud, el servidor propio (Subsonic) o Bandcamp. Las pestañas
// están con la cuenta de SoundCloud conectada o, en el servidor y Bandcamp, siempre.
static NetSource s_source = NET_SOURCE_SOUNDCLOUD;
#define s_server (s_source == NET_SOURCE_SERVER)
#define s_soundcloud (s_source == NET_SOURCE_SOUNDCLOUD)
static NetSource s_playing_source = NET_SOURCE_SOUNDCLOUD;   // de dónde vino lo que suena
#define s_playing_server (s_playing_source == NET_SOURCE_SERVER)
static char s_playing_id[32] = "";        // id de la canción del servidor que suena
static bool s_playing_starred = false;    // ...y si tiene estrella

static bool tabs_available(void) {
    return s_source == NET_SOURCE_BANDCAMP || (s_server ? subsonic_configured() : account_connected());
}

// Diálogo Sí / No (descargas pendientes de una sesión anterior o desconectar la cuenta)
typedef enum { DLG_PENDING_DOWNLOADS, DLG_DISCONNECT, DLG_DISCONNECT_SERVER, DLG_DISCONNECT_BANDCAMP, DLG_DELETE_PLAYLIST } DialogKind;
static DialogKind s_dialog_kind = DLG_PENDING_DOWNLOADS;
static bool s_dialog_open = false;
static DialogViewModel s_dialog;
static MenuViewModel s_menu;
static MascotViewModel s_mascot;

// Música local: todas las pistas escaneadas y la vista filtrada (índice de vista -> pista)
static LocalTrack s_local_all[LOCAL_MAX_TRACKS];
static int s_local_count = 0;
static bool s_local_scanned = false;
static bool s_local_dirty = false;        // hubo descargas: volver a escanear al entrar
static int s_local_map[LOCAL_MAX_TRACKS];   // fila de la lista -> índice en s_local_all
static int s_local_playing = -1; // índice en s_local_all de la pista local que suena
static char s_local_filter[64] = "";

// Cover flow: álbumes visibles (los que tienen pistas que coinciden con el filtro),
// el enfocado y su posición animada
static int s_vis_albums[LIB_MAX_ALBUMS + 2 + PLAYLISTS_MAX];   // + Todas, Favoritos y las playlists propias
static int s_vis_count = 0;
static int s_cf_index = 0;
static float s_cf_pos = 0.0f;

// Cola de reproducción local: las pistas del álbum desde el que se eligió la canción
static int s_local_queue[LOCAL_MAX_TRACKS];
static int s_local_queue_n = 0;
static int s_local_queue_pos = -1;
static int s_heavy_skips = 0;      // FLAC pesados saltados seguidos (corta la cadena)
static int s_local_from_all = 0;   // la cola es un álbum virtual (LIB_ALL, LIB_FAV o una playlist propia) o 0

// Menú de opciones de las playlists propias (abajo)
typedef enum { SHEET_ADD_TO, SHEET_IN_PLAYLIST } SheetKind;
static SheetViewModel s_sheet;
static bool s_sheet_open = false;
static SheetKind s_sheet_kind = SHEET_ADD_TO;
static int s_sheet_track = -1;      // canción (índice en s_local_all); -1 si no hay
static int s_sheet_playlist = -1;   // playlist enfocada en el cover flow (SHEET_IN_PLAYLIST)

// Ruta -> índice en s_local_all (las playlists guardan rutas): tabla hash abierta
#define PATH_HASH_SIZE 4096
static int16_t s_path_hash[PATH_HASH_SIZE];

static u32 path_hash(const char* p) {
    u32 h = 2166136261u;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static void build_path_index(void) {
    memset(s_path_hash, 0xFF, sizeof(s_path_hash));
    for (int i = 0; i < s_local_count; i++) {
        u32 h = path_hash(s_local_all[i].path) & (PATH_HASH_SIZE - 1);
        while (s_path_hash[h] >= 0) h = (h + 1) & (PATH_HASH_SIZE - 1);
        s_path_hash[h] = (int16_t)i;
    }
}

static int find_local_track(const char* path) {
    u32 h = path_hash(path) & (PATH_HASH_SIZE - 1);
    for (int n = 0; n < PATH_HASH_SIZE && s_path_hash[h] >= 0; n++, h = (h + 1) & (PATH_HASH_SIZE - 1))
        if (strcmp(s_local_all[s_path_hash[h]].path, path) == 0) return s_path_hash[h];
    return -1;
}

// Mensaje temporal en pantalla (errores de búsqueda o de stream)
static void set_status(const char* msg) {
    snprintf(s_playback.status_msg, sizeof(s_playback.status_msg), "%.95s", msg);
    s_playback.status_until = osGetTime() + 6000;
}

static SearchViewModel* view_for(TrackSource src) {
    if (src == SRC_LOCAL) return &s_local_view;
    if (src == SRC_PLAYLIST) return &s_pl_view;
    if (src == SRC_LIKES) return &s_acc[ACC_LIST_LIKES].view;
    if (src == SRC_FEED) return &s_acc[ACC_LIST_FEED].view;
    return &s_sc_view;
}

static TrackSource source_of_mode(AppMode mode) {
    if (mode == MODE_LOCAL) return SRC_LOCAL;
    if (s_in_playlist) return SRC_PLAYLIST;
    if (s_sc_tab == SC_TAB_LIKES) return SRC_LIKES;
    if (s_sc_tab == SC_TAB_FEED) return SRC_FEED;
    return SRC_SOUNDCLOUD;
}

// En SoundCloud se muestran los resultados de playlists (para abrir una, no para reproducir)
static bool showing_playlist_results(void) {
    return s_mode == MODE_SOUNDCLOUD && !s_in_playlist &&
           ((s_sc_tab == SC_TAB_SEARCH && s_sc_kind == SC_KIND_PLAYLISTS) || s_sc_tab == SC_TAB_PLAYLISTS);
}

// La lista que se ve abajo
static SearchViewModel* shown_view(void) {
    if (s_mode == MODE_LOCAL) return &s_local_view;
    if (s_in_playlist) return &s_pl_view;
    if (s_sc_tab != SC_TAB_SEARCH) return &s_acc[tab_list(s_sc_tab)].view;
    return s_sc_kind == SC_KIND_PLAYLISTS ? &s_plr_view : &s_sc_view;
}

// Pista de SoundCloud de una lista (búsqueda o playlist abierta)
static const SearchResultTrack* sc_track(TrackSource src, int idx) {
    if (src == SRC_PLAYLIST) return &s_pl.tracks[idx];
    if (src == SRC_LIKES) return &s_likes_tracks[idx];
    if (src == SRC_FEED) return &s_feed_tracks[idx];
    return &s_sc_results.tracks[idx];
}

static void clear_selection(void) {
    s_sc_view.selected_index = -1;
    s_pl_view.selected_index = -1;
    s_acc[ACC_LIST_LIKES].view.selected_index = -1;
    s_acc[ACC_LIST_FEED].view.selected_index = -1;
}

static void stop_sources(void) {
    network_stop_stream();
    local_stop_stream();
}

static bool source_streaming(void) {
    return network_is_streaming() || local_is_streaming();
}

static bool contains_ci(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static bool track_matches(const LocalTrack* t, const char* filter) {
    return contains_ci(t->title, filter) || contains_ci(t->artist, filter) || contains_ci(t->album, filter);
}

// Texto de una fila de la lista Local: se lee de la biblioteca, sin copiarlo
static void local_row_text(int idx, const char** title, const char** artist) {
    const LocalTrack* t = &s_local_all[s_local_map[idx]];
    *title = t->title;
    *artist = t->artist;
}

// Lista de abajo: las pistas del álbum enfocado que coinciden con el filtro
static void local_fill_album_view(void) {
    SearchViewModel* v = &s_local_view;
    int n = 0;
    v->selected_index = -1;
    int focus = s_vis_count > 0 ? s_vis_albums[s_cf_index] : 0;
    if (s_vis_count > 0 && LIB_IS_PL(focus)) {
        // Playlist propia: sus canciones en su orden (las que ya no están en la SD se saltan)
        int p = LIB_PL_INDEX(focus);
        for (int k = 0; k < playlists_size(p) && n < LOCAL_MAX_TRACKS; k++) {
            int i = find_local_track(playlists_song(p, k));
            if (i < 0 || !track_matches(&s_local_all[i], s_local_filter)) continue;
            s_local_map[n] = i;
            if (i == s_local_playing && s_playing_src == SRC_LOCAL && s_local_from_all == focus) v->selected_index = n;
            n++;
        }
        v->total_results = n;
        v->cursor = v->selected_index >= 0 ? v->selected_index : 0;
        v->page = v->cursor / LIST_ROWS;
        snprintf(v->empty_msg, sizeof(v->empty_msg), "%s",
                 playlists_size(p) == 0 ? "Playlist vacía: agrega canciones con Y" : "Nada coincide con el filtro");
        return;
    }
    bool all = s_vis_count > 0 && focus < 0;   // Todas las canciones o Favoritos
    bool fav = all && focus == LIB_FAV;
    const LibAlbum* a = s_vis_count > 0 && !all ? library_album(focus) : NULL;
    int first = all ? 0 : (a ? a->first : 0), end = all ? s_local_count : (a ? a->first + a->count : 0);
    for (int i = first; i < end && n < LOCAL_MAX_TRACKS; i++) {
        const LocalTrack* t = &s_local_all[i];
        if (!track_matches(t, s_local_filter) || (fav && !favorites_has(t->path))) continue;
        s_local_map[n] = i;
        if (i == s_local_playing && s_playing_src == SRC_LOCAL) v->selected_index = n;
        n++;
    }
    v->total_results = n;
    v->cursor = v->selected_index >= 0 ? v->selected_index : 0;
    v->page = v->cursor / LIST_ROWS;
    snprintf(v->empty_msg, sizeof(v->empty_msg), "%s",
             !s_local_scanned ? "Leyendo tu música..."
             : s_local_count == 0 ? "Copia tus MP3 a " LOCAL_MUSIC_HINT : "Nada coincide con el filtro");
}

// Álbumes que tienen alguna pista que coincide con el filtro
static void local_apply_filter(const char* filter) {
    int keep = s_vis_count > 0 ? s_vis_albums[s_cf_index] : -1;
    snprintf(s_local_filter, sizeof(s_local_filter), "%s", filter);
    snprintf(s_local_view.query, sizeof(s_local_view.query), "%s", filter);
    int all_n = 0, fav_n = 0;
    for (int i = 0; i < s_local_count; i++) {
        if (!track_matches(&s_local_all[i], filter)) continue;
        all_n++;
        fav_n += favorites_has(s_local_all[i].path);
    }
    gfx_set_all_songs_count(all_n);
    gfx_set_fav_songs_count(fav_n);

    // Álbumes con alguna pista que coincide, después de los virtuales: "Todas las
    // canciones" (si hay más de un álbum) y "Favoritos" (si hay alguna con estrella)
    int real = 0;
    for (int al = 0; al < library_album_count(); al++) {
        const LibAlbum* a = library_album(al);
        bool any = false;
        for (int i = a->first; i < a->first + a->count && !any; i++) any = track_matches(&s_local_all[i], filter);
        if (any) real++;
    }
    s_vis_count = 0;
    if (real > 1) s_vis_albums[s_vis_count++] = LIB_ALL;
    if (fav_n > 0) s_vis_albums[s_vis_count++] = LIB_FAV;
    // Playlists propias (las vacías también, salvo con filtro)
    for (int p = 0; p < playlists_count(); p++) {
        bool show = !filter[0];
        for (int k = 0; k < playlists_size(p) && !show; k++) {
            int i = find_local_track(playlists_song(p, k));
            show = i >= 0 && track_matches(&s_local_all[i], filter);
        }
        if (show) s_vis_albums[s_vis_count++] = LIB_PL(p);
    }
    for (int al = 0; al < library_album_count(); al++) {
        const LibAlbum* a = library_album(al);
        bool any = false;
        for (int i = a->first; i < a->first + a->count && !any; i++) any = track_matches(&s_local_all[i], filter);
        if (any) s_vis_albums[s_vis_count++] = al;
    }
    // Queda enfocado el mismo álbum si sigue visible
    s_cf_index = 0;
    for (int i = 0; i < s_vis_count; i++)
        if (s_vis_albums[i] == keep) s_cf_index = i;
    s_cf_pos = (float)s_cf_index;
    local_fill_album_view();
}

// Cambia de álbum en el cover flow; al pasar del último vuelve al primero (y al revés).
// En la vuelta la animación parte del lado de afuera, así se ve un solo paso.
static void local_change_album(int step) {
    if (s_vis_count < 2) return;
    int idx = s_cf_index + step;
    if (idx < 0) {
        idx = s_vis_count - 1;
        s_cf_pos = (float)s_vis_count;
    } else if (idx >= s_vis_count) {
        idx = 0;
        s_cf_pos = -1.0f;
    }
    s_cf_index = idx;
    local_fill_album_view();
}

// Posición en el cover flow del álbum que suena (-1 si no es visible)
static int local_playing_vis(void) {
    if (s_playing_src != SRC_LOCAL || s_local_playing < 0) return -1;
    int al = s_local_from_all ? s_local_from_all : library_album_of_track(s_local_playing);
    for (int i = 0; i < s_vis_count; i++)
        if (s_vis_albums[i] == al) return i;
    return -1;
}

// Escaneo de la biblioteca en segundo plano: el hilo llena s_scan_buf (con el índice
// de la SD solo se leen las canciones nuevas) y el hilo principal lo aplica al terminar.
// Un pedido mientras hay otro en curso se junta en uno más al final.
static LocalTrack s_scan_buf[LOCAL_MAX_TRACKS];
static Thread s_scan_thread = NULL;
static volatile bool s_scan_done = false;
static int s_scan_count = 0;
static bool s_scan_again = false;
static bool s_scan_announce = false;   // al terminar, avisar cuántas canciones hay

static void scan_worker(void* arg) {
    (void)arg;
    s_scan_count = local_scan(s_scan_buf, LOCAL_MAX_TRACKS);
    s_scan_done = true;
}

static void local_rescan_apply(void);

// Pide escanear la SD (al abrir la app o tras descargar)
static void local_rescan(void) {
    s_local_dirty = false;
    if (s_scan_thread) {
        s_scan_again = true;
        return;
    }
    s_scan_done = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Menos prioridad que la interfaz y el audio: la música y las animaciones siguen fluidas
    s_scan_thread = threadCreate(scan_worker, NULL, 64 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, false);
    if (!s_scan_thread) {
        scan_worker(NULL);
        local_rescan_apply();
    }
}

// Llamar en cada cuadro: aplica el escaneo terminado
static void local_rescan_poll(void) {
    if (!s_scan_thread || !s_scan_done) return;
    threadJoin(s_scan_thread, U64_MAX);
    threadFree(s_scan_thread);
    s_scan_thread = NULL;
    local_rescan_apply();
    if (s_scan_again) {
        s_scan_again = false;
        local_rescan();
    }
}

// Pasa el resultado del escaneo a la biblioteca conservando lo que suena
static void local_rescan_apply(void) {
    char playing_path[256] = "";
    if (s_local_playing >= 0) snprintf(playing_path, sizeof(playing_path), "%s", s_local_all[s_local_playing].path);
    library_exit();   // el hilo de carátulas no debe leer mientras se reescriben las pistas
    s_local_count = s_scan_count;
    memcpy(s_local_all, s_scan_buf, (size_t)s_scan_count * sizeof(LocalTrack));
    library_build(s_local_all, s_local_count);
    build_path_index();
    gfx_coverflow_reset();
    s_local_scanned = true;
    s_local_dirty = false;

    s_local_playing = -1;
    s_local_queue_n = 0;
    s_local_queue_pos = -1;
    for (int i = 0; playing_path[0] && i < s_local_count; i++) {
        if (strcmp(s_local_all[i].path, playing_path) != 0) continue;
        s_local_playing = i;
        // La cola vuelve a ser el álbum de la pista que suena
        const LibAlbum* a = library_album(library_album_of_track(i));
        for (int k = 0; a && k < a->count; k++) {
            if (a->first + k == i) s_local_queue_pos = k;
            s_local_queue[s_local_queue_n++] = a->first + k;
        }
        break;
    }
    local_apply_filter(s_local_filter);
    gfx_set_library_scanning(-1);
    if (s_scan_announce) {
        s_scan_announce = false;
        char msg[64];
        snprintf(msg, sizeof(msg), L("%d canciones en %d álbumes"), s_local_count, library_album_count());
        set_status(msg);
    }
}

static void enter_mode(AppMode mode) {
    s_mode = mode;
    if (mode == MODE_LOCAL && (!s_local_scanned || s_local_dirty)) {
        // Primera vez: si el escaneo del inicio sigue en curso se muestra el avance
        if (!s_local_scanned) s_scan_announce = true;
        if (s_local_dirty || !s_scan_thread) local_rescan();
    }
}

// Marca en la lista de SoundCloud las pistas que ya están en la SD
static void refresh_saved_marks(void) {
    if (s_source == NET_SOURCE_BANDCAMP) return;
    for (int i = 0; i < s_sc_view.total_results; i++)
        s_sc_view.result_saved[i] = network_download_exists(&s_sc_results.tracks[i]);
}

static void finish_start(bool started) {
    if (!started) {
        s_playback.is_playing = false;
        s_playback.stream_error = true;
        set_status("No se pudo iniciar la pista");
        return;
    }
    audio_play();
    s_playback.is_playing = true;
    s_waiting_audio = true;
}

// Reproduce la posición `pos` de la cola local
// Carátula de la canción local que suena (la del álbum, preparada por la biblioteca):
// se pide hasta que esté lista
static int s_local_art_album = -1;
static u32 s_local_art_gen = 0;

static void update_local_artwork(void) {
    if (s_local_art_album < 0) return;
    if (s_local_art_gen != library_generation()) {
        // Se reconstruyó la biblioteca: el álbum puede tener otro índice
        s_local_art_album = s_local_playing >= 0 ? library_album_of_track(s_local_playing) : -1;
        s_local_art_gen = library_generation();
        if (s_local_art_album < 0) return;
    }
    if (gfx_set_artwork_library(s_local_art_album)) s_local_art_album = -1;
}

// ---------------------------------------------------------------------------
// Aleatorio y repetir
// ---------------------------------------------------------------------------

// Aleatorio: se recorren las posiciones de la lista sin repetir hasta agotarlas, y el
// historial permite volver atrás por el mismo camino
#define SHUF_HIST 256
static u8 s_shuf_played[LOCAL_MAX_TRACKS];
static int s_shuf_hist[SHUF_HIST];
static int s_shuf_hist_n = 0;
static bool s_auto_pick = false;   // la pista la eligió play_step, no el usuario
static bool s_list_ended = false;  // se terminó la lista sin repetir: Play vuelve a empezar

static void shuffle_reset(void) {
    memset(s_shuf_played, 0, sizeof(s_shuf_played));
    s_shuf_hist_n = 0;
}

static void shuffle_note(int pos) {
    if (pos < 0 || pos >= LOCAL_MAX_TRACKS) return;
    s_shuf_played[pos] = 1;
    if (s_shuf_hist_n == SHUF_HIST) {
        memmove(s_shuf_hist, s_shuf_hist + 1, (SHUF_HIST - 1) * sizeof(int));
        s_shuf_hist_n--;
    }
    s_shuf_hist[s_shuf_hist_n++] = pos;
}

// Llamado al empezar cualquier pista: si la eligió el usuario, el aleatorio arranca de nuevo
static void play_started(int pos) {
    s_list_ended = false;
    if (s_auto_pick) return;
    shuffle_reset();
    shuffle_note(pos);
}

// Una posición al azar que no haya sonado (cur excluida). Si ya sonaron todas y se
// permite, empieza otra vuelta; si no, -1.
static int shuffle_pick(int n, int cur, bool allow_restart) {
    if (n > LOCAL_MAX_TRACKS) n = LOCAL_MAX_TRACKS;
    int left = 0;
    for (int i = 0; i < n; i++)
        if (!s_shuf_played[i] && i != cur) left++;
    if (left == 0) {
        if (!allow_restart) return -1;
        if (n == 1) return 0;
        memset(s_shuf_played, 0, n);
        left = n - (cur >= 0 && cur < n ? 1 : 0);
        if (left <= 0) return -1;
    }
    int r = rand() % left;
    for (int i = 0; i < n; i++)
        if (!s_shuf_played[i] && i != cur && r-- == 0) return i;
    return -1;
}

// Cola local con toda la biblioteca que coincide con el filtro; devuelve la posición de track
static int local_queue_library(int track, bool only_fav) {
    int n = 0, pos = 0;
    for (int i = 0; i < s_local_count && n < LOCAL_MAX_TRACKS; i++) {
        if (!track_matches(&s_local_all[i], s_local_filter)) continue;
        if (only_fav && !favorites_has(s_local_all[i].path) && i != track) continue;
        if (i == track) pos = n;
        s_local_queue[n++] = i;
    }
    s_local_queue_n = n;
    return pos;
}

static void play_local(int pos) {
    if (pos < 0 || pos >= s_local_queue_n) return;
    play_started(pos);
    stop_sources();
    audio_clear_buffers();

    clear_selection();
    s_local_queue_pos = pos;
    s_local_playing = s_local_queue[pos];
    const LocalTrack* t = &s_local_all[s_local_playing];
    s_playing_src = SRC_LOCAL;
    s_seek_base_ms = 0;
    s_playback.stream_error = false;
    snprintf(s_playback.title, sizeof(s_playback.title), "%s", t->title);
    snprintf(s_playback.artist, sizeof(s_playback.artist), "%s", t->artist);
    s_playback.elapsed_ms = 0;
    gfx_clear_artwork();
    network_artwork_async(NULL);
    s_playback.artwork_loading = false;
    s_local_art_album = library_album_of_track(s_local_playing);
    s_local_art_gen = library_generation();
    update_local_artwork();
    s_playback.duration_ms = t->duration_ms ? t->duration_ms : local_estimate_duration_ms(t->path);
    audio_set_codec(local_codec(t));
    bool started = local_start_stream(t->path, 0.0f);

    // Resaltar la fila si el álbum que suena es el que se está viendo
    SearchViewModel* v = &s_local_view;
    v->selected_index = -1;
    for (int r = 0; r < v->total_results; r++)
        if (s_local_map[r] == s_local_playing) {
            v->selected_index = r;
            v->cursor = r;
            v->page = r / LIST_ROWS;
        }
    finish_start(started);
}

static void play_track(TrackSource src, int idx) {
    SearchViewModel* v = view_for(src);
    if (idx < 0 || idx >= v->total_results) return;
    if (src == SRC_LOCAL) {
        // La cola pasa a ser el álbum enfocado; en "Todas las canciones", toda la
        // biblioteca que coincide con el filtro
        int focus = s_vis_count > 0 ? s_vis_albums[s_cf_index] : 0;
        s_local_from_all = s_vis_count > 0 && focus < 0 ? focus : 0;
        if (focus == LIB_ALL || focus == LIB_FAV) {
            play_local(local_queue_library(s_local_map[idx], focus == LIB_FAV));
            return;
        }
        s_local_queue_n = v->total_results;
        for (int i = 0; i < v->total_results; i++) s_local_queue[i] = s_local_map[i];
        play_local(idx);
        return;
    }

    play_started(idx);
    // Detener lo que suene y vaciar el audio antes de preparar la nueva pista
    stop_sources();
    audio_clear_buffers();

    clear_selection();
    s_local_view.selected_index = -1;
    v->selected_index = idx;
    v->cursor = idx;
    v->page = idx / LIST_ROWS;
    s_playing_src = src;
    s_seek_base_ms = 0;
    s_playback.stream_error = false;
    snprintf(s_playback.title, sizeof(s_playback.title), "%s", v->result_titles[idx]);
    snprintf(s_playback.artist, sizeof(s_playback.artist), "%s", v->result_artists[idx]);
    s_playback.elapsed_ms = 0;
    gfx_clear_artwork();
    s_local_art_album = -1;

    const SearchResultTrack* t = sc_track(src, idx);
    s_playing_source = s_source;
    snprintf(s_playing_id, sizeof(s_playing_id), "%s", t->id);
    s_playing_starred = t->starred;
    s_playback.duration_ms = t->is_preview ? 30 * 1000 : t->duration_ms; // las previews duran 30 s
    // La carátula se descarga en segundo plano; mientras tanto se ve el vinilo
    s_playback.artwork_loading = network_artwork_async(t->artwork_url);
    finish_start(network_start_stream(t->stream_endpoint, t->kind));
}

static int list_count(TrackSource src) {
    return src == SRC_LOCAL ? s_local_queue_n : view_for(src)->total_results;
}

static int list_current(TrackSource src) {
    return src == SRC_LOCAL ? s_local_queue_pos : view_for(src)->selected_index;
}

static void play_at(TrackSource src, int pos) {
    s_auto_pick = true;
    if (src == SRC_LOCAL) play_local(pos);
    else play_track(src, pos);
    s_auto_pick = false;
}

// Fin de la lista sin repetir: se detiene; Play vuelve a empezar
static void stop_at_end(void) {
    audio_pause();
    s_playback.is_playing = false;
    s_list_ended = true;
    set_status("Fin de la lista");
}

// Pista siguiente (step 1) o anterior (-1). at_end: la pista terminó sola (ahí cuentan
// repetir la canción y detenerse al final de la lista)
static void play_step(int step, bool at_end) {
    TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
    int n = list_count(src);
    if (n <= 0) return;
    int cur = list_current(src);
    int pos;
    if (at_end && g_settings.repeat == REPEAT_ONE && cur >= 0) {
        pos = cur;
    } else if (g_settings.shuffle) {
        if (step < 0) {
            // Volver por el historial; sin historial, repetir la actual
            if (s_shuf_hist_n >= 2) {
                s_shuf_hist_n--;
                pos = s_shuf_hist[s_shuf_hist_n - 1];
            } else {
                pos = cur >= 0 ? cur : 0;
            }
        } else {
            pos = shuffle_pick(n, cur, !at_end || g_settings.repeat == REPEAT_ALL);
            if (pos < 0) {
                stop_at_end();
                return;
            }
            shuffle_note(pos);
        }
    } else {
        pos = cur < 0 ? 0 : cur + step;
        if (pos >= n) {
            if (at_end && g_settings.repeat == REPEAT_OFF) {
                stop_at_end();
                return;
            }
            pos = 0;
        }
        if (pos < 0) pos = n - 1;
    }
    play_at(src, pos);
}

static void play_relative(int step) {
    play_step(step, false);
}

static void toggle_shuffle(void) {
    g_settings.shuffle = !g_settings.shuffle;
    settings_save();
    set_status(g_settings.shuffle ? "Aleatorio activado" : "Aleatorio desactivado");
    shuffle_reset();
    TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
    shuffle_note(list_current(src));
}

static void cycle_repeat(void) {
    g_settings.repeat = (RepeatMode)((g_settings.repeat + 1) % 3);
    settings_save();
    static const char* const msgs[3] = { "Repetir: no", "Repetir: la lista", "Repetir: la canción" };
    set_status(msgs[g_settings.repeat]);
}

static void toggle_play_pause(void) {
    dlog("play/pausa (sonando=%d)", (int)audio_is_playing());
    if (s_list_ended && !audio_is_playing() && s_playing_src != SRC_NONE) {
        // Se terminó la lista: empezar otra vez (desde el principio o al azar)
        TrackSource src = s_playing_src;
        int n = list_count(src);
        if (n > 0) {
            shuffle_reset();
            int pos = g_settings.shuffle ? rand() % n : 0;
            shuffle_note(pos);
            play_at(src, pos);
        }
        return;
    }
    if (audio_is_playing()) {
        audio_pause();
        s_playback.is_playing = false;
    } else if (s_playing_src == SRC_NONE || s_playback.stream_error) {
        // Nada cargado (o falló): reproducir la pista enfocada de la lista actual
        if (s_playing_src == SRC_NONE && showing_playlist_results()) return;
        TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
        SearchViewModel* v = view_for(src);
        if (src == SRC_LOCAL && s_playback.stream_error && s_local_queue_pos >= 0) play_local(s_local_queue_pos);
        else play_track(src, s_playback.stream_error && v->selected_index >= 0 ? v->selected_index : v->cursor);
    } else {
        audio_play();
        s_playback.is_playing = true;
    }
}

static void seek_to(float ratio) {
    s_playback.scrub_preview = -1.0f;
    if (s_playing_src == SRC_NONE || s_playback.duration_ms == 0) return;
    bool was_playing = s_playback.is_playing;
    uint32_t start_ms = (uint32_t)((float)s_playback.duration_ms * ratio);
    bool ok;
    if (s_playing_src == SRC_LOCAL) {
        local_stop_stream();
        audio_clear_buffers();
        audio_set_codec(local_codec(&s_local_all[s_local_playing]));
        ok = local_start_stream(s_local_all[s_local_playing].path, ratio);
    } else {
        ok = network_seek_stream(ratio, s_playback.duration_ms, &start_ms);
    }
    if (ok) {
        s_seek_base_ms = start_ms;
        s_waiting_audio = true;
        if (was_playing) audio_play();
    }
}

static void move_cursor(SearchViewModel* v, int step) {
    // Recorre la lista; al pasar el borde de la página cambia de página
    if (v->total_results <= 0) return;
    int c = v->cursor + step;
    if (c < 0) c = 0;
    if (c > v->total_results - 1) c = v->total_results - 1;
    v->cursor = c;
    v->page = c / LIST_ROWS;
}

static void change_page(SearchViewModel* v, int step) {
    int pages = (v->total_results + LIST_ROWS - 1) / LIST_ROWS;
    int p = v->page + step;
    if (pages <= 0 || p < 0 || p >= pages) return;
    v->page = p;
    v->cursor = p * LIST_ROWS;
}

// Abre la playlist idx de la lista de playlists visible (búsqueda o biblioteca de la
// cuenta): se carga en segundo plano. Las de la biblioteca se piden con la cuenta,
// así también se abren las privadas.
static void open_playlist(int idx) {
    bool library = s_sc_tab == SC_TAB_PLAYLISTS;
    SearchViewModel* lv = library ? &s_acc[ACC_LIST_PLAYLISTS].view : &s_plr_view;
    if (idx < 0 || idx >= lv->total_results) return;
    const PlaylistResult* r = library ? &s_accpl_items[idx] : &s_plr_results.items[idx];
    lv->cursor = idx;
    s_in_playlist = true;
    s_pl_loading = network_playlist_async_ex(r->id, library);
    gfx_clear_playlist_cover();
    memset(&s_pl_top, 0, sizeof(s_pl_top));
    snprintf(s_pl_top.title, sizeof(s_pl_top.title), "%s", r->title);
    snprintf(s_pl_top.creator, sizeof(s_pl_top.creator), "%s", r->creator);
    s_pl_top.album = strncmp(r->id, "al:", 3) == 0 || strncmp(r->id, "bc:a:", 5) == 0;
    s_pl_top.loading = s_pl_loading;
    snprintf(s_pl_view.header_title, sizeof(s_pl_view.header_title), "%s", r->title);
    s_pl_view.total_results = 0;
    s_pl_view.page = 0;
    s_pl_view.cursor = 0;
    s_pl_view.show_dl_all = false;
    snprintf(s_pl_view.empty_msg, sizeof(s_pl_view.empty_msg), "%s",
             s_pl_loading ? "Cargando playlist..." : "No se pudo abrir la playlist");
    // Si suena una canción de la playlist anterior, deja de estar resaltada aquí
    s_pl_view.selected_index = -1;
    s_pl_marks_page = -1;
}

// Marcas de "ya descargada" de la página visible de la playlist (leen la SD: solo 5 filas)
static void refresh_playlist_marks(void) {
    if (s_source == NET_SOURCE_BANDCAMP) return;
    if (!s_in_playlist || s_pl_loading) return;
    if (s_pl_marks_page == s_pl_view.page && s_pl_marks_gen == s_marks_gen) return;
    s_pl_marks_page = s_pl_view.page;
    s_pl_marks_gen = s_marks_gen;
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = s_pl_view.page * LIST_ROWS + r;
        if (i < s_pl_view.total_results) s_pl_view.result_saved[i] = network_download_exists_in_playlist(&s_pl, i);
    }
}

static void download_message(DownloadResult r, const char* title, const char* album) {
    char msg[96];
    switch (r) {
        case DL_QUEUED:         snprintf(msg, sizeof(msg), L("Descargando: %.70s"), title); break;
        case DL_ALREADY_QUEUED: snprintf(msg, sizeof(msg), "Ya está en la cola de descargas"); break;
        case DL_ALREADY_THERE:  snprintf(msg, sizeof(msg), L("Ya está en el álbum %.60s"), album); break;
        case DL_PREVIEW_ONLY:   snprintf(msg, sizeof(msg), "Solo hay una preview de 30 s: no se descarga"); break;
        default:                snprintf(msg, sizeof(msg), "No se pudo poner en la cola"); break;
    }
    set_status(msg);
}

static void download_whole_playlist(void) {
    if (!s_in_playlist || s_pl_loading || s_pl.count == 0) return;
    int skipped = 0, done = 0, total = 0;
    int queued = network_download_enqueue_playlist(&s_pl, &skipped);
    char msg[96];
    if (queued > 0 && skipped > 0)
        snprintf(msg, sizeof(msg), L("Descargando %d canciones (%d omitidas)"), queued, skipped);
    else if (queued > 0)
        snprintf(msg, sizeof(msg), L("Descargando %d canciones de la playlist"), queued);
    else if (network_download_group_progress(s_pl.id, &done, &total))
        snprintf(msg, sizeof(msg), "La playlist ya se está descargando");
    else
        snprintf(msg, sizeof(msg), "Ya estaba todo descargado");
    set_status(msg);
}

// ---------------------------------------------------------------------------
// Listas de la cuenta
// ---------------------------------------------------------------------------

static void acc_header_info(AccList* L) {
    SearchViewModel* v = &L->view;
    if (L->loading && !L->appending) snprintf(v->header_info, sizeof(v->header_info), "Cargando...");
    else if (!L->loaded) v->header_info[0] = '\0';
    else snprintf(v->header_info, sizeof(v->header_info), "%d%s", v->total_results, L->next[0] ? "+" : "");
}

// Pide la primera página (more = false) o la siguiente
static void acc_list_request(AccListKind k, bool more) {
    AccList* L = &s_acc[k];
    if (!tabs_available() || (more && !L->next[0])) return;
    if (!network_account_list_async(k, more ? L->next : NULL)) return;
    // Solo hay un pedido a la vez: el de otra lista queda cancelado
    for (int o = 0; o < 3; o++) {
        if (o == (int)k || !s_acc[o].loading) continue;
        s_acc[o].loading = false;
        if (!s_acc[o].loaded) snprintf(s_acc[o].view.empty_msg, sizeof(s_acc[o].view.empty_msg), "Toca el título para cargar");
        acc_header_info(&s_acc[o]);
    }
    L->loading = true;
    L->appending = more;
    if (!more) {
        L->empty_pages = 0;
        if (!L->loaded) snprintf(L->view.empty_msg, sizeof(L->view.empty_msg), "Cargando...");
    }
    acc_header_info(L);
}

static void poll_account_list(void) {
    AccListKind k = ACC_LIST_LIKES;
    NetJobStatus st = network_poll_account_list(&k, &s_acc_page);
    if (st == NET_JOB_RUNNING || st == NET_JOB_IDLE) return;
    AccList* L = &s_acc[k];
    SearchViewModel* v = &L->view;
    if (!L->loading) return;   // se cambió de lista mientras tanto
    L->loading = false;
    if (st != NET_JOB_DONE) {
        const char* err = network_get_account_error();
        if (!L->loaded) snprintf(v->empty_msg, sizeof(v->empty_msg), "%s", err[0] ? err : "No se pudo cargar");
        set_status(err[0] ? err : "No se pudo cargar");
        acc_header_info(L);
        return;
    }
    const AccPage* pg = &s_acc_page;
    int n = L->appending ? v->total_results : 0;
    if (!L->appending) {
        // Lista nueva: si sonaba una canción de esta lista deja de estar resaltada
        v->selected_index = -1;
        v->page = 0;
        v->cursor = 0;
        L->marks_page = -1;
    }
    for (int i = 0; i < pg->count && n < MAX_LIST_ITEMS; i++, n++) {
        if (k == ACC_LIST_PLAYLISTS) {
            const PlaylistResult* r = &pg->playlists[i];
            s_accpl_items[n] = *r;
            snprintf(v->result_titles[n], sizeof(v->result_titles[n]), "%s", r->title);
            snprintf(v->result_artists[n], sizeof(v->result_artists[n]), "%s", r->creator);
            snprintf(v->result_badge[n], sizeof(v->result_badge[n]), "%d", r->track_count > 999 ? 999 : r->track_count);
            v->result_preview[n] = false;
            v->result_repost[n] = false;
        } else {
            SearchResultTrack* dst = k == ACC_LIST_LIKES ? &s_likes_tracks[n] : &s_feed_tracks[n];
            *dst = pg->tracks[i];
            snprintf(v->result_titles[n], sizeof(v->result_titles[n]), "%s", dst->title);
            snprintf(v->result_artists[n], sizeof(v->result_artists[n]), "%s", dst->artist);
            v->result_badge[n][0] = '\0';
            v->result_preview[n] = dst->is_preview;
            v->result_repost[n] = pg->repost[i];
        }
        v->result_saved[n] = false;
    }
    v->total_results = n;
    L->empty_pages = pg->count == 0 ? L->empty_pages + 1 : 0;
    snprintf(L->next, sizeof(L->next), "%s", n < MAX_LIST_ITEMS ? pg->next_href : "");
    L->loaded = true;
    L->marks_page = -1;
    if (n == 0)
        snprintf(v->empty_msg, sizeof(v->empty_msg), "%s",
                 s_server ? (k == ACC_LIST_LIKES ? "No tienes canciones con estrella"
                             : k == ACC_LIST_PLAYLISTS ? "El servidor no tiene álbumes" : "El servidor no tiene canciones")
                 : !s_soundcloud ? "Bandcamp no devolvió nada"
                 : k == ACC_LIST_LIKES ? "Todavía no tienes likes"
                 : k == ACC_LIST_PLAYLISTS ? "No tienes playlists guardadas" : "Tu feed no tiene canciones nuevas");
    acc_header_info(L);
    dlog("cuenta: lista %d -> %d elementos (+%d, saltados %d)%s", (int)k, n, pg->count, pg->skipped,
         L->next[0] ? ", hay más" : "");
}

// Al llegar a la última página de una lista de la cuenta se pide la siguiente
static void acc_list_autoload(void) {
    if (s_mode != MODE_SOUNDCLOUD || s_in_playlist || s_sc_tab == SC_TAB_SEARCH) return;
    AccListKind k = tab_list(s_sc_tab);
    AccList* L = &s_acc[k];
    SearchViewModel* v = &L->view;
    if (!L->loaded || L->loading || !L->next[0] || L->empty_pages >= 4) return;
    int last_page = v->total_results > 0 ? (v->total_results - 1) / LIST_ROWS : 0;
    if (v->page >= last_page) acc_list_request(k, true);
}

// Marcas de "ya descargada" de la página visible de likes y feed (leen la SD: 5 filas)
static void acc_list_marks(void) {
    if (s_source == NET_SOURCE_BANDCAMP) return;
    if (s_mode != MODE_SOUNDCLOUD || s_in_playlist || (s_sc_tab != SC_TAB_LIKES && s_sc_tab != SC_TAB_FEED)) return;
    AccListKind k = tab_list(s_sc_tab);
    AccList* L = &s_acc[k];
    SearchViewModel* v = &L->view;
    if (L->loading && !L->appending) return;
    if (L->marks_page == v->page && L->marks_gen == s_marks_gen) return;
    L->marks_page = v->page;
    L->marks_gen = s_marks_gen;
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = v->page * LIST_ROWS + r;
        if (i < v->total_results) v->result_saved[i] = network_download_exists(sc_track(k == ACC_LIST_LIKES ? SRC_LIKES : SRC_FEED, i));
    }
}

static void set_sc_tab(ScTab t, bool tapped) {
    if (t != SC_TAB_SEARCH && !tabs_available()) return;
    if (t == s_sc_tab && !s_in_playlist) {
        // Tocar la pestaña activa vuelve a cargar su lista
        if (tapped && t != SC_TAB_SEARCH && !s_acc[tab_list(t)].loading) acc_list_request(tab_list(t), false);
        return;
    }
    s_in_playlist = false;
    s_sc_tab = t;
    if (t != SC_TAB_SEARCH) {
        AccList* L = &s_acc[tab_list(t)];
        if (!L->loaded && !L->loading) acc_list_request(tab_list(t), false);
    }
}

// Pestañas visibles solo con la cuenta conectada; al desconectarla se olvidan sus listas
static void sync_account_tabs(void) {
    bool on = tabs_available();
    if (!on && s_account_was_on) {
        for (int k = 0; k < 3; k++) {
            AccList* L = &s_acc[k];
            L->loaded = L->loading = false;
            L->next[0] = '\0';
            L->view.total_results = 0;
            L->view.header_info[0] = '\0';
            snprintf(L->view.empty_msg, sizeof(L->view.empty_msg), "Toca el título para cargar");
        }
        if (s_sc_tab != SC_TAB_SEARCH) {
            s_sc_tab = SC_TAB_SEARCH;
            s_in_playlist = false;
        }
    }
    s_account_was_on = on;
    SearchViewModel* views[6] = { &s_sc_view, &s_plr_view, &s_pl_view, &s_acc[0].view, &s_acc[1].view, &s_acc[2].view };
    for (int i = 0; i < 6; i++) {
        views[i]->show_tabs = on;
        views[i]->source = s_source == NET_SOURCE_BANDCAMP && bandcamp_connected() ? 3 : (int)s_source;
        views[i]->active_tab = (int)s_sc_tab;
    }
}

// Estrella del reproductor: favorita (o no) la canción que suena. Local: en la SD (álbum
// Favoritos). Servidor propio: con estrella en el servidor (pestaña Favoritos).
static int playing_favorite_state(void) {
    if (s_playing_src == SRC_LOCAL && s_local_playing >= 0) return favorites_has(s_local_all[s_local_playing].path);
    if (s_playing_src != SRC_NONE && s_playing_src != SRC_LOCAL && s_playing_server) return s_playing_starred;
    return -1;
}

static void toggle_favorite(void) {
    if (s_playing_src == SRC_NONE) {
        set_status("Reproduce una canción para marcarla");
    } else if (s_playing_src == SRC_LOCAL) {
        if (s_local_playing < 0) return;
        bool on = favorites_toggle(s_local_all[s_local_playing].path);
        set_status(on ? "Agregada a Favoritos" : "Quitada de Favoritos");
        if (s_local_scanned) local_apply_filter(s_local_filter);
    } else if (s_playing_server) {
        if (!network_star_async(s_playing_id, !s_playing_starred)) return;
        s_playing_starred = !s_playing_starred;   // se ve al instante; si falla se revierte
        set_status(s_playing_starred ? "Agregada a Favoritos" : "Quitada de Favoritos");
    } else if (s_playing_source == NET_SOURCE_BANDCAMP) {
        set_status("Bandcamp no tiene favoritos sin cuenta");
    } else {
        set_status("En SoundCloud usa los Likes");
    }
}

// Resultado de la estrella en el servidor: actualizar las listas abiertas
static void poll_star(void) {
    char id[32];
    bool starred;
    NetJobStatus st = network_poll_star(id, sizeof(id), &starred);
    if (st != NET_JOB_DONE && st != NET_JOB_FAILED) return;
    if (st == NET_JOB_FAILED) {
        if (strcmp(id, s_playing_id) == 0) s_playing_starred = !starred;
        set_status("El servidor no guardó la estrella");
        return;
    }
    for (int i = 0; i < s_sc_results.count; i++)
        if (strcmp(s_sc_results.tracks[i].id, id) == 0) s_sc_results.tracks[i].starred = starred;
    for (int i = 0; i < s_pl.count; i++)
        if (strcmp(s_pl.tracks[i].id, id) == 0) s_pl.tracks[i].starred = starred;
    for (int i = 0; i < s_acc[ACC_LIST_FEED].view.total_results; i++)
        if (strcmp(s_feed_tracks[i].id, id) == 0) s_feed_tracks[i].starred = starred;
    for (int i = 0; i < s_acc[ACC_LIST_LIKES].view.total_results; i++)
        if (strcmp(s_likes_tracks[i].id, id) == 0) s_likes_tracks[i].starred = starred;
    // La pestaña Favoritos se vuelve a pedir la próxima vez que se abra
    if (s_sc_tab != SC_TAB_LIKES) s_acc[ACC_LIST_LIKES].loaded = false;
}

// Cambia el navegador en línea entre SoundCloud, el servidor propio y Bandcamp: cada uno
// tiene sus propias búsquedas y listas, así que al cambiar se vacían
static void set_online_source(NetSource src) {
    if (src == s_source) return;
    network_set_source(src);
    s_source = src;
    bool server = src == NET_SOURCE_SERVER, bandcamp = src == NET_SOURCE_BANDCAMP;
    s_in_playlist = false;
    s_pl_loading = false;
    s_sc_tab = SC_TAB_SEARCH;
    s_sc_kind = SC_KIND_TRACKS;
    s_sc_results.count = 0;
    s_sc_view.total_results = 0;
    s_sc_view.query[0] = '\0';
    s_plr_results.count = 0;
    s_plr_view.total_results = 0;
    s_plr_view.query[0] = '\0';
    s_pl_view.total_results = 0;
    s_pl_view.show_dl_all = false;
    clear_selection();
    snprintf(s_sc_view.empty_msg, sizeof(s_sc_view.empty_msg), "%s",
             server ? "Toca la lupa para buscar en tu servidor"
             : bandcamp ? "Toca la lupa para buscar en Bandcamp" : "Toca la lupa para buscar");
    snprintf(s_plr_view.placeholder, sizeof(s_plr_view.placeholder), "%s", server || bandcamp ? "álbumes" : "playlists");
    snprintf(s_plr_view.empty_msg, sizeof(s_plr_view.empty_msg), "%s",
             server || bandcamp ? "Toca la lupa para buscar álbumes" : "Toca la lupa para buscar playlists");
    static const char* sc_titles[3] = { "Tus likes", "Tus playlists", "Feed" };
    static const char* srv_titles[3] = { "Favoritos", "Álbumes", "Al azar" };
    static const char* bc_titles[3] = { "Populares", "Destacados", "Novedades" };
    static const char* bc_acc_titles[3] = { "Populares", "Tu colección", "Novedades" };
    const char* const* titles = server ? srv_titles : bandcamp ? (bandcamp_connected() ? bc_acc_titles : bc_titles) : sc_titles;
    for (int k = 0; k < 3; k++) {
        AccList* L = &s_acc[k];
        L->loaded = L->loading = L->appending = false;
        L->next[0] = '\0';
        L->view.total_results = 0;
        L->view.header_info[0] = '\0';
        L->view.show_download = src != NET_SOURCE_BANDCAMP && k != ACC_LIST_PLAYLISTS;
        snprintf(L->view.header_title, sizeof(L->view.header_title), "%s", titles[k]);
        snprintf(L->view.empty_msg, sizeof(L->view.empty_msg), "Toca el título para cargar");
    }
    s_sc_view.show_download = src != NET_SOURCE_BANDCAMP;   // Bandcamp es para escuchar
    s_pl_view.show_download = src != NET_SOURCE_BANDCAMP;
    dlog("fuente en línea: %s", server ? "servidor" : bandcamp ? "Bandcamp" : "SoundCloud");
}

// La cuenta de Bandcamp cambió: la pestaña del medio pasa de Destacados a Tu colección (o al revés)
static void bandcamp_account_changed(void) {
    if (s_source != NET_SOURCE_BANDCAMP) return;
    AccList* L = &s_acc[ACC_LIST_PLAYLISTS];
    L->loaded = L->loading = L->appending = false;
    L->next[0] = '\0';
    L->view.total_results = 0;
    L->view.header_info[0] = '\0';
    snprintf(L->view.header_title, sizeof(L->view.header_title), "%s", bandcamp_connected() ? "Tu colección" : "Destacados");
    if (s_sc_tab == SC_TAB_PLAYLISTS && !s_in_playlist) acc_list_request(ACC_LIST_PLAYLISTS, false);
}

// Abre una tarjeta del menú: 0 SoundCloud, 1 Local, 2 servidor propio, 3 Bandcamp
static void open_menu_item(int i) {
    s_menu.cursor = i;
    if (i == 1) {
        enter_mode(MODE_LOCAL);
    } else if (i == 2) {
        if (!subsonic_configured()) {
            set_status("Conecta tu servidor desde Sound3DS Share");
            return;
        }
        set_online_source(NET_SOURCE_SERVER);
        enter_mode(MODE_SOUNDCLOUD);
    } else if (i == 3) {
        set_online_source(NET_SOURCE_BANDCAMP);
        enter_mode(MODE_SOUNDCLOUD);
    } else {
        set_online_source(NET_SOURCE_SOUNDCLOUD);
        enter_mode(MODE_SOUNDCLOUD);
    }
}

// ---------------------------------------------------------------------------
// Recibir desde el PC
// ---------------------------------------------------------------------------

static void scan_reset_vm(void) {
    memset(&s_scan_vm, 0, sizeof(s_scan_vm));
    s_scan_vm.state = SCAN_SEARCHING;
    s_scan_vm.found_t = -1;
}

static void open_scan(void) {
    s_settings_open = false;
    if (s_scan_open) return;
    scan_reset_vm();
    gfx_scan_begin();
    qrscan_open();
    s_scan_open = true;
}

static void close_scan(void) {
    if (!s_scan_open) return;
    qrscan_close();
    gfx_scan_end();
    s_scan_open = false;
}

// Avanza el lector: QR leído -> breve animación -> se procesa en segundo plano -> resultado
static void update_scan(float dt) {
    if (!s_scan_open) return;
    ScanViewModel* vm = &s_scan_vm;
    qrscan_tick();
    vm->has_frame = qrscan_has_frame();
    vm->cam_failed = qrscan_failed();
    if (vm->state == SCAN_SEARCHING) {
        if (vm->found_t < 0) {
            if (qrscan_take(s_scan_text, sizeof(s_scan_text))) vm->found_t = 0;
        } else if ((vm->found_t += dt) > SCAN_FOUND_ANIM_S) {
            vm->server = network_receive_is_server(s_scan_text);
            vm->account = vm->server || network_receive_is_account(s_scan_text) || network_receive_is_bandcamp(s_scan_text);
            if (network_receive_async(s_scan_text)) {
                vm->state = SCAN_WORKING;
            } else {
                vm->state = SCAN_ERROR;
                snprintf(vm->title, sizeof(vm->title), "No se pudo recibir");
                snprintf(vm->line1, sizeof(vm->line1), "Inténtalo de nuevo");
            }
        }
    } else if (vm->state == SCAN_WORKING) {
        ReceiveResult r;
        NetJobStatus st = network_poll_receive(&r);
        if (st == NET_JOB_RUNNING || st == NET_JOB_IDLE) return;
        vm->state = st == NET_JOB_DONE ? SCAN_DONE : SCAN_ERROR;
        vm->line2[0] = '\0';
        if (r.kind == RECV_SERVER) {
            if (st == NET_JOB_DONE) {
                snprintf(vm->title, sizeof(vm->title), "Servidor conectado");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.username);
                snprintf(vm->line2, sizeof(vm->line2), "Ábrelo desde Servidor en el menú");
            } else {
                snprintf(vm->title, sizeof(vm->title), "No se pudo conectar el servidor");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.error);
            }
        } else if (r.kind == RECV_BANDCAMP) {
            if (st == NET_JOB_DONE) {
                snprintf(vm->title, sizeof(vm->title), "Bandcamp conectado");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.username);
                snprintf(vm->line2, sizeof(vm->line2), "Tu colección está en Bandcamp, en el menú");
                bandcamp_account_changed();
            } else {
                snprintf(vm->title, sizeof(vm->title), "No se pudo conectar Bandcamp");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.error);
            }
        } else if (r.kind == RECV_ACCOUNT) {
            if (st == NET_JOB_DONE) {
                snprintf(vm->title, sizeof(vm->title), "Cuenta conectada");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.username);
                snprintf(vm->line2, sizeof(vm->line2), "Tus Likes, Playlists y Feed ya están en SoundCloud");
            } else {
                snprintf(vm->title, sizeof(vm->title), "No se pudo conectar la cuenta");
                snprintf(vm->line1, sizeof(vm->line1), "%s", r.error);
            }
        } else if (st == NET_JOB_DONE) {
            if (r.queued > 0) snprintf(vm->title, sizeof(vm->title), L("%d canciones en camino"), r.queued);
            else snprintf(vm->title, sizeof(vm->title), "Ya estaban todas en la SD");
            if (r.existing > 0) snprintf(vm->line1, sizeof(vm->line1), L("%d ya estaban en la SD"), r.existing);
            else snprintf(vm->line1, sizeof(vm->line1), "Se guardan en sdmc:/music");
            snprintf(vm->line2, sizeof(vm->line2), "Aparecen en Local, cada una en su álbum");
        } else {
            snprintf(vm->title, sizeof(vm->title), "No se pudo recibir");
            snprintf(vm->line1, sizeof(vm->line1), "%s", r.error);
        }
    }
}

static void handle_scan_event(const UIEvent* ev) {
    if (ev->type == UI_ACTION_SCAN_CLOSE) {
        // Mientras se procesa el QR se espera a que termine (es corto)
        if (s_scan_vm.state != SCAN_WORKING) close_scan();
    } else if (ev->type == UI_ACTION_SCAN_AGAIN) {
        scan_reset_vm();
        qrscan_rearm();
    }
}

static void handle_settings_event(const UIEvent* ev) {
    SettingsViewModel* vm = &s_settings_vm;
    switch (ev->type) {
        case UI_ACTION_CURSOR_UP:   vm->cursor = (vm->cursor + SETTINGS_ROWS - 1) % SETTINGS_ROWS; break;
        case UI_ACTION_CURSOR_DOWN: vm->cursor = (vm->cursor + 1) % SETTINGS_ROWS; break;
        case UI_ACTION_SETTINGS_CLOSE: s_settings_open = false; break;
        case UI_ACTION_SETTINGS_TOGGLE: {
            int row = ev->track_index >= 0 ? ev->track_index : vm->cursor;
            vm->cursor = row;
            if (row == SETTINGS_ROW_LANG) {
                Lang l = ev->option > 0 ? (ev->option == 2 ? LANG_EN : LANG_ES)
                                        : (g_settings.lang == LANG_EN ? LANG_ES : LANG_EN);
                g_settings.lang = l;
                i18n_set(g_settings.lang);
                settings_save();
            } else if (row == SETTINGS_ROW_LED) {
                g_settings.led_rgb = !g_settings.led_rgb;
                settings_save();
                set_status(g_settings.led_rgb ? "LED RGB activado" : "LED RGB desactivado");
            } else if (row == SETTINGS_ROW_LID) {
                int v = g_settings.lid_keys;
                v = ev->option > 0 ? ev->option - 1 : (ev->option < 0 ? (v + 2) % 3 : (v + 1) % 3);
                g_settings.lid_keys = (LidKeys)v;
                settings_save();
            } else if (row == SETTINGS_ROW_ACCOUNT) {
                // Tres líneas: SoundCloud, servidor y Bandcamp (con A, la primera conectada)
                int line = ev->option > 0 ? ev->option - 1
                         : account_connected() ? 0 : subsonic_configured() ? 1 : bandcamp_connected() ? 2 : 0;
                if (line == 2) {
                    if (bandcamp_connected()) {
                        s_dialog_kind = DLG_DISCONNECT_BANDCAMP;
                        snprintf(s_dialog.title, sizeof(s_dialog.title), "¿Desconectar Bandcamp?");
                        snprintf(s_dialog.body, sizeof(s_dialog.body), "Se borrará la sesión guardada en la tarjeta SD.");
                        s_dialog.account_icon = true;
                        snprintf(s_dialog.yes, sizeof(s_dialog.yes), "Desconectar");
                        snprintf(s_dialog.no, sizeof(s_dialog.no), "Cancelar");
                        s_dialog_open = true;
                    } else {
                        set_status("Conecta Bandcamp desde Sound3DS Share");
                    }
                } else if (line == 1) {
                    if (subsonic_configured()) {
                        s_dialog_kind = DLG_DISCONNECT_SERVER;
                        snprintf(s_dialog.title, sizeof(s_dialog.title), "¿Desconectar el servidor?");
                        snprintf(s_dialog.body, sizeof(s_dialog.body), "Se borrará el token guardado en la tarjeta SD.");
                        s_dialog.account_icon = true;
                        snprintf(s_dialog.yes, sizeof(s_dialog.yes), "Desconectar");
                        snprintf(s_dialog.no, sizeof(s_dialog.no), "Cancelar");
                        s_dialog_open = true;
                    } else {
                        set_status("Conecta tu servidor desde Sound3DS Share");
                    }
                } else if (account_connected()) {
                    s_dialog_kind = DLG_DISCONNECT;
                    snprintf(s_dialog.title, sizeof(s_dialog.title), "¿Desconectar la cuenta?");
                    snprintf(s_dialog.body, sizeof(s_dialog.body), "Se borrará la sesión guardada en la tarjeta SD.");
                    s_dialog.account_icon = true;
                    snprintf(s_dialog.yes, sizeof(s_dialog.yes), "Desconectar");
                    snprintf(s_dialog.no, sizeof(s_dialog.no), "Cancelar");
                    s_dialog_open = true;
                } else {
                    set_status("Conéctala desde Sound3DS Share en el PC");
                }
            } else if (row == SETTINGS_ROW_RECEIVE) {
                open_scan();
            }
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Playlists propias (Local)
// ---------------------------------------------------------------------------

// Rehace el cover flow y deja enfocada la playlist p (o el mismo álbum si p < 0)
static void playlists_refresh(int focus_playlist) {
    local_apply_filter(s_local_filter);
    if (focus_playlist < 0) return;
    for (int i = 0; i < s_vis_count; i++)
        if (s_vis_albums[i] == LIB_PL(focus_playlist)) {
            s_cf_index = i;
            s_cf_pos = (float)i;
            local_fill_album_view();
        }
}

static void sheet_add_items(void) {
    s_sheet_kind = SHEET_ADD_TO;
    const char* title = s_sheet_track >= 0 ? s_local_all[s_sheet_track].title : "";
    snprintf(s_sheet.title, sizeof(s_sheet.title), "%s %s", L("Agregar a:"), title);
    s_sheet.count = 0;
    snprintf(s_sheet.items[s_sheet.count], sizeof(s_sheet.items[0]), "%s", L("+ Nueva playlist..."));
    s_sheet.checked[s_sheet.count++] = false;
    for (int p = 0; p < playlists_count() && s_sheet.count < SHEET_MAX; p++) {
        snprintf(s_sheet.items[s_sheet.count], sizeof(s_sheet.items[0]), "%s", playlists_name(p));
        s_sheet.checked[s_sheet.count++] = s_sheet_track >= 0 && playlists_contains(p, s_local_all[s_sheet_track].path);
    }
    s_sheet.cursor = 0;
}

// Y (o el botón de la barra) en Local: dentro de una playlist propia, opciones de la
// playlist; en cualquier otro álbum, agregar la canción enfocada a una playlist
static void open_playlist_sheet(void) {
    if (!s_local_scanned) return;
    SearchViewModel* v = &s_local_view;
    int focus = s_vis_count > 0 ? s_vis_albums[s_cf_index] : 0;
    s_sheet_track = v->total_results > 0 ? s_local_map[v->cursor] : -1;
    memset(&s_sheet, 0, sizeof(s_sheet));
    if (s_vis_count > 0 && LIB_IS_PL(focus)) {
        s_sheet_kind = SHEET_IN_PLAYLIST;
        s_sheet_playlist = LIB_PL_INDEX(focus);
        snprintf(s_sheet.title, sizeof(s_sheet.title), "%s", playlists_name(s_sheet_playlist));
        static const char* opts[4] = { "Quitar la canción de la playlist", "Agregar la canción a otra playlist",
                                       "Renombrar la playlist", "Borrar la playlist" };
        for (int k = s_sheet_track >= 0 ? 0 : 2; k < 4; k++)
            snprintf(s_sheet.items[s_sheet.count++], sizeof(s_sheet.items[0]), "%s", L(opts[k]));
    } else {
        if (s_sheet_track < 0) {
            set_status("Elige una canción para agregarla a una playlist");
            return;
        }
        s_sheet_playlist = -1;
        sheet_add_items();
    }
    s_sheet_open = true;
}

static void sheet_new_playlist(void) {
    char name[PLAYLIST_NAME_LEN] = "";
    s_sheet_open = false;
    if (!ui_open_keyboard(name, sizeof(name), L("Nombre de la playlist"), false) || !name[0]) return;
    int p = playlists_create(name);
    if (p < 0) {
        set_status(playlists_count() >= PLAYLISTS_MAX ? "Ya tienes el máximo de playlists" : "Ya hay una playlist con ese nombre");
        return;
    }
    if (s_sheet_track >= 0) playlists_add(p, s_local_all[s_sheet_track].path);
    char msg[96];
    snprintf(msg, sizeof(msg), L("Playlist creada: %s"), playlists_name(p));
    set_status(msg);
    playlists_refresh(-1);
}

static void handle_sheet_event(const UIEvent* ev) {
    SheetViewModel* sh = &s_sheet;
    switch (ev->type) {
        case UI_ACTION_SHEET_CLOSE: s_sheet_open = false; return;
        case UI_ACTION_CURSOR_UP: sh->cursor = sh->cursor > 0 ? sh->cursor - 1 : sh->count - 1; return;
        case UI_ACTION_CURSOR_DOWN: sh->cursor = sh->cursor + 1 < sh->count ? sh->cursor + 1 : 0; return;
        case UI_ACTION_PAGE_PREV: sh->cursor = sh->cursor >= SHEET_ROWS ? sh->cursor - SHEET_ROWS : 0; return;
        case UI_ACTION_PAGE_NEXT:
            sh->cursor = sh->cursor + SHEET_ROWS < sh->count ? sh->cursor + SHEET_ROWS : sh->count - 1;
            return;
        case UI_ACTION_SHEET_PICK: break;
        default: return;
    }
    int pick = ev->track_index;
    if (pick < 0 || pick >= sh->count) return;
    char msg[128];
    if (s_sheet_kind == SHEET_ADD_TO) {
        if (pick == 0) {
            sheet_new_playlist();
            return;
        }
        int p = pick - 1;
        s_sheet_open = false;
        if (s_sheet_track < 0) return;
        const char* path = s_local_all[s_sheet_track].path;
        if (playlists_contains(p, path)) snprintf(msg, sizeof(msg), L("Ya estaba en %s"), playlists_name(p));
        else if (playlists_add(p, path)) snprintf(msg, sizeof(msg), L("Agregada a %s"), playlists_name(p));
        else snprintf(msg, sizeof(msg), "%s", L("No se pudo guardar la playlist"));
        set_status(msg);
        playlists_refresh(-1);
        return;
    }
    // Opciones de la playlist enfocada (sin canción elegida faltan las dos primeras)
    int opt = s_sheet_track >= 0 ? pick : pick + 2;
    int p = s_sheet_playlist;
    if (opt == 0) {
        s_sheet_open = false;
        if (playlists_remove(p, s_local_all[s_sheet_track].path)) {
            snprintf(msg, sizeof(msg), L("Quitada de %s"), playlists_name(p));
            set_status(msg);
        }
        playlists_refresh(p);
    } else if (opt == 1) {
        sheet_add_items();
    } else if (opt == 2) {
        s_sheet_open = false;
        char name[PLAYLIST_NAME_LEN];
        snprintf(name, sizeof(name), "%s", playlists_name(p));
        if (!ui_open_keyboard(name, sizeof(name), L("Nombre de la playlist"), false) || !name[0]) return;
        if (!playlists_rename(p, name)) {
            set_status("No se pudo renombrar (¿ya existe ese nombre?)");
            return;
        }
        // El orden es alfabético: buscar dónde quedó
        int np = -1;
        for (int i = 0; i < playlists_count(); i++)
            if (strcasecmp(playlists_name(i), name) == 0) np = i;
        playlists_refresh(np);
    } else if (opt == 3) {
        s_sheet_open = false;
        s_dialog_kind = DLG_DELETE_PLAYLIST;
        snprintf(s_dialog.title, sizeof(s_dialog.title), "%s", L("¿Borrar la playlist?"));
        snprintf(s_dialog.body, sizeof(s_dialog.body), L("Se borra la lista \"%s\". Las canciones siguen en la SD."),
                 playlists_name(p));
        s_dialog.account_icon = false;
        snprintf(s_dialog.yes, sizeof(s_dialog.yes), "Borrar");
        snprintf(s_dialog.no, sizeof(s_dialog.no), "Cancelar");
        s_dialog_open = true;
    }
}

static void handle_event(const UIEvent* ev) {
    if (s_mode == MODE_MENU) {
        switch (ev->type) {
            // Cuadrícula de 2x2: arriba/abajo cambian de fila, izquierda/derecha de columna
            case UI_ACTION_CURSOR_UP:
            case UI_ACTION_CURSOR_DOWN:
                s_menu.cursor = (s_menu.cursor + 2) % MENU_ITEMS;
                break;
            case UI_ACTION_PAGE_PREV:
            case UI_ACTION_PAGE_NEXT:
                s_menu.cursor ^= 1;
                break;
            case UI_ACTION_CONFIRM:
                open_menu_item(s_menu.cursor);
                break;
            case UI_ACTION_MENU_SELECT:
                open_menu_item(ev->track_index);
                break;
            case UI_ACTION_OPEN_NOW_PLAYING:
                if (s_playing_src != SRC_NONE && s_playing_src != SRC_LOCAL) set_online_source(s_playing_source);
                if (s_playing_src != SRC_NONE) enter_mode(s_playing_src == SRC_LOCAL ? MODE_LOCAL : MODE_SOUNDCLOUD);
                // Volver a la pestaña de lo que suena
                if (s_playing_src == SRC_LIKES || s_playing_src == SRC_FEED) {
                    s_in_playlist = false;
                    s_sc_tab = s_playing_src == SRC_LIKES ? SC_TAB_LIKES : SC_TAB_FEED;
                } else if (s_playing_src == SRC_SOUNDCLOUD) {
                    s_in_playlist = false;
                    s_sc_tab = SC_TAB_SEARCH;
                    s_sc_kind = SC_KIND_TRACKS;
                }
                break;
            case UI_ACTION_TOGGLE_THEME:
                theme_toggle_next();
                break;
            case UI_ACTION_OPEN_SETTINGS:
                s_settings_open = true;
                s_settings_vm.cursor = 0;
                break;
            default:
                break;
        }
        return;
    }

    // Pestañas de la cuenta (tocar la activa vuelve a cargar su lista)
    if (ev->type == UI_ACTION_TAB || ev->type == UI_ACTION_TAB_PREV || ev->type == UI_ACTION_TAB_NEXT) {
        int t = ev->type == UI_ACTION_TAB ? ev->track_index : ((int)s_sc_tab + (ev->type == UI_ACTION_TAB_NEXT ? 1 : 3)) % 4;
        set_sc_tab((ScTab)t, ev->type == UI_ACTION_TAB);
        return;
    }
    // Selector Canciones / Playlists
    if (ev->type == UI_ACTION_SEARCH_KIND) {
        s_sc_kind = ev->track_index == 1 ? SC_KIND_PLAYLISTS : SC_KIND_TRACKS;
        return;
    }
    if (ev->type == UI_ACTION_BACK && s_mode == MODE_SOUNDCLOUD && s_in_playlist) {
        s_in_playlist = false;   // de la playlist a los resultados
        return;
    }
    if (ev->type == UI_ACTION_DOWNLOAD_ALL || (ev->type == UI_ACTION_DOWNLOAD && s_source == NET_SOURCE_BANDCAMP)) {
        if (s_source == NET_SOURCE_BANDCAMP) set_status("Bandcamp es para escuchar: compra el álbum para tenerlo");
        else download_whole_playlist();
        return;
    }
    // Resultados de playlists: elegir una la abre
    if (showing_playlist_results()) {
        // La lista visible: resultados de la búsqueda o las playlists de la cuenta
        SearchViewModel* pv = shown_view();
        switch (ev->type) {
            case UI_ACTION_SEARCH_REQUEST: {
                char text[64] = {0};
                if (s_sc_tab == SC_TAB_PLAYLISTS) {
                    acc_list_request(ACC_LIST_PLAYLISTS, false);
                    return;
                }
                if (!pv->is_searching && ui_open_keyboard(text, sizeof(text), "Buscar playlists en SoundCloud", false)) {
                    snprintf(pv->query, sizeof(pv->query), "%s", text);
                    pv->is_searching = network_search_playlists_async(text);
                }
                return;
            }
            case UI_ACTION_SELECT_TRACK: open_playlist(pv->page * LIST_ROWS + ev->track_index); return;
            case UI_ACTION_CONFIRM:      open_playlist(pv->cursor); return;
            case UI_ACTION_CURSOR_UP:    move_cursor(pv, -1); return;
            case UI_ACTION_CURSOR_DOWN:  move_cursor(pv, 1); return;
            case UI_ACTION_PAGE_PREV:    change_page(pv, -1); return;
            case UI_ACTION_PAGE_NEXT:    change_page(pv, 1); return;
            case UI_ACTION_DOWNLOAD:     set_status("Abre la playlist para descargarla"); return;
            default: break;   // el resto (transporte, tema, volver...) como siempre
        }
    }

    TrackSource src = source_of_mode(s_mode);
    SearchViewModel* v = view_for(src);
    switch (ev->type) {
        case UI_ACTION_SEARCH_REQUEST: {
            char text[64] = {0};
            if (src == SRC_PLAYLIST) break;
            if (src == SRC_LIKES || src == SRC_FEED) {
                acc_list_request(src == SRC_LIKES ? ACC_LIST_LIKES : ACC_LIST_FEED, false);
                break;
            }
            if (src == SRC_SOUNDCLOUD) {
                if (!v->is_searching && ui_open_keyboard(text, sizeof(text), "Buscar en SoundCloud", false)) {
                    snprintf(v->query, sizeof(v->query), "%s", text);
                    v->is_searching = network_search_async(text);
                }
            } else if (ui_open_keyboard(text, sizeof(text), "Filtrar (vacio = todas)", true)) {
                local_apply_filter(text);
            }
            break;
        }

        case UI_ACTION_SELECT_TRACK:
            play_track(src, v->page * LIST_ROWS + ev->track_index);
            break;

        case UI_ACTION_CONFIRM:
            // (A) sobre otra pista la reproduce; sobre la que suena hace play/pausa
            if (v->cursor >= 0 && v->cursor < v->total_results &&
                !(s_playing_src == src && v->cursor == v->selected_index)) {
                play_track(src, v->cursor);
                break;
            }
            toggle_play_pause();
            break;

        case UI_ACTION_PLAY_PAUSE: toggle_play_pause(); break;
        case UI_ACTION_PREV:       play_relative(-1); break;
        case UI_ACTION_NEXT:       play_relative(1); break;
        case UI_ACTION_SHUFFLE:    toggle_shuffle(); break;
        case UI_ACTION_REPEAT:     cycle_repeat(); break;
        case UI_ACTION_FAVORITE:   toggle_favorite(); break;
        case UI_ACTION_SCRUB_DRAG: s_playback.scrub_preview = ev->seek_ratio; break;
        case UI_ACTION_SEEK:       seek_to(ev->seek_ratio); break;
        case UI_ACTION_CURSOR_UP:  move_cursor(v, -1); break;
        case UI_ACTION_CURSOR_DOWN: move_cursor(v, 1); break;
        case UI_ACTION_PAGE_PREV:  change_page(v, -1); break;
        case UI_ACTION_PAGE_NEXT:  change_page(v, 1); break;
        case UI_ACTION_TOGGLE_THEME: theme_toggle_next(); break;
        case UI_ACTION_OPEN_SETTINGS:
            s_settings_open = true;
            s_settings_vm.cursor = 0;
            break;
        case UI_ACTION_TOGGLE_GAME:
            s_show_game = !s_show_game;
            if (s_show_game) game_resume(&s_game);
            break;
        case UI_ACTION_BACK:       s_mode = MODE_MENU; break;
        case UI_ACTION_ALBUM_PREV: local_change_album(-1); break;
        case UI_ACTION_ALBUM_NEXT: local_change_album(1); break;
        case UI_ACTION_PLAYLIST_MENU: open_playlist_sheet(); break;
        case UI_ACTION_DOWNLOAD: {
            if (v->total_results <= 0) break;
            if (src == SRC_PLAYLIST) {
                if (s_pl_loading) break;
                download_message(network_download_enqueue_from_playlist(&s_pl, v->cursor), s_pl.tracks[v->cursor].title,
                                 s_pl.title);
            } else if (src == SRC_SOUNDCLOUD || src == SRC_LIKES || src == SRC_FEED) {
                const SearchResultTrack* t = sc_track(src, v->cursor);
                char album[48];
                if (subsonic_is_stream_url(t->stream_endpoint)) subsonic_download_name(album, sizeof(album));
                else snprintf(album, sizeof(album), "%s", LOCAL_SC_ALBUM);
                download_message(network_download_enqueue(t), t->title, album);
            }
            break;
        }
        default: break;
    }
}

// Resultados de la búsqueda de SoundCloud en segundo plano
static void poll_search(void) {
    SearchViewModel* v = &s_sc_view;
    if (!v->is_searching) return;
    NetJobStatus st = network_poll_search(&s_sc_results);
    if (st == NET_JOB_RUNNING) return;

    v->is_searching = false;
    if (st != NET_JOB_DONE) {
        const char* err = network_get_search_error();
        set_status(err[0] ? err : "La busqueda fallo");
        return;
    }
    // Solo se muestra la lista; la reproducción empieza cuando el usuario elige una pista.
    // Lo que suena sigue sonando; siguiente/anterior pasan a usar la lista nueva.
    v->total_results = s_sc_results.count < MAX_LIST_ITEMS ? s_sc_results.count : MAX_LIST_ITEMS;
    v->selected_index = -1;
    v->page = 0;
    v->cursor = 0;
    int previews = 0;
    for (int i = 0; i < v->total_results; i++) {
        snprintf(v->result_titles[i], sizeof(v->result_titles[i]), "%s", s_sc_results.tracks[i].title);
        snprintf(v->result_artists[i], sizeof(v->result_artists[i]), "%s", s_sc_results.tracks[i].artist);
        v->result_preview[i] = s_sc_results.tracks[i].is_preview;
        if (s_sc_results.tracks[i].is_preview) previews++;
    }
    refresh_saved_marks();
    if (previews > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), L("%d de %d son previews de 30 s"), previews, v->total_results);
        set_status(msg);
    }
}

static void poll_playlist_search(void) {
    SearchViewModel* v = &s_plr_view;
    if (!v->is_searching) return;
    NetJobStatus st = network_poll_playlist_search(&s_plr_results);
    if (st == NET_JOB_RUNNING) return;
    v->is_searching = false;
    if (st != NET_JOB_DONE) {
        const char* err = network_get_playlist_error();
        set_status(err[0] ? err : "La busqueda fallo");
        return;
    }
    v->total_results = s_plr_results.count < MAX_LIST_ITEMS ? s_plr_results.count : MAX_LIST_ITEMS;
    v->selected_index = -1;
    v->page = 0;
    v->cursor = 0;
    for (int i = 0; i < v->total_results; i++) {
        const PlaylistResult* r = &s_plr_results.items[i];
        snprintf(v->result_titles[i], sizeof(v->result_titles[i]), "%s", r->title);
        snprintf(v->result_artists[i], sizeof(v->result_artists[i]), "%s", r->creator);
        snprintf(v->result_badge[i], sizeof(v->result_badge[i]), "%d", (r->track_count > 999 ? 999 : r->track_count) % 1000);
        v->result_preview[i] = false;
        v->result_saved[i] = false;
    }
}

static void poll_playlist(void) {
    if (!s_pl_loading) return;
    uint8_t* rgba = NULL;
    int w = 0, h = 0;
    NetJobStatus st = network_poll_playlist(&s_pl, &rgba, &w, &h);
    if (st == NET_JOB_RUNNING) return;
    s_pl_loading = false;
    s_pl_top.loading = false;
    SearchViewModel* v = &s_pl_view;
    if (st != NET_JOB_DONE) {
        const char* err = network_get_playlist_error();
        snprintf(v->empty_msg, sizeof(v->empty_msg), "%s", err[0] ? err : "No se pudo abrir la playlist");
        set_status(v->empty_msg);
        return;
    }
    if (rgba) {
        gfx_set_playlist_cover_rgba(rgba, w, h);
        network_free_artwork(rgba);
    }
    snprintf(s_pl_top.title, sizeof(s_pl_top.title), "%s", s_pl.title);
    snprintf(s_pl_top.creator, sizeof(s_pl_top.creator), "%s", s_pl.creator);
    s_pl_top.track_count = s_pl.count;
    s_pl_top.unavailable = s_pl.unavailable;
    s_pl_top.total_ms = s_pl.total_ms;
    v->total_results = s_pl.count < MAX_LIST_ITEMS ? s_pl.count : MAX_LIST_ITEMS;
    v->selected_index = -1;
    v->page = 0;
    v->cursor = 0;
    v->show_dl_all = v->total_results > 0 && s_source != NET_SOURCE_BANDCAMP;
    int previews = 0;
    for (int i = 0; i < v->total_results; i++) {
        snprintf(v->result_titles[i], sizeof(v->result_titles[i]), "%s", s_pl.tracks[i].title);
        snprintf(v->result_artists[i], sizeof(v->result_artists[i]), "%s", s_pl.tracks[i].artist);
        v->result_preview[i] = s_pl.tracks[i].is_preview;
        v->result_saved[i] = false;
        v->result_badge[i][0] = '\0';
        if (s_pl.tracks[i].is_preview) previews++;
    }
    s_pl_marks_page = -1;
    if (previews > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), L("%d de %d son previews de 30 s"), previews, v->total_results);
        set_status(msg);
    }
}

static void handle_dialog_event(const UIEvent* ev) {
    if (s_dialog_kind == DLG_DELETE_PLAYLIST) {
        if (ev->type == UI_ACTION_DIALOG_YES && playlists_delete(s_sheet_playlist)) {
            set_status("Playlist borrada");
            playlists_refresh(-1);
        }
        if (ev->type == UI_ACTION_DIALOG_YES || ev->type == UI_ACTION_DIALOG_NO) s_dialog_open = false;
        return;
    }
    if (s_dialog_kind == DLG_DISCONNECT_BANDCAMP) {
        if (ev->type == UI_ACTION_DIALOG_YES) {
            bandcamp_disconnect();
            bandcamp_account_changed();
            set_status("Bandcamp desconectado");
        }
        if (ev->type == UI_ACTION_DIALOG_YES || ev->type == UI_ACTION_DIALOG_NO) s_dialog_open = false;
        return;
    }
    if (s_dialog_kind == DLG_DISCONNECT_SERVER) {
        if (ev->type == UI_ACTION_DIALOG_YES) {
            subsonic_disconnect();
            if (s_server) {
                set_online_source(NET_SOURCE_SOUNDCLOUD);
                if (s_mode == MODE_SOUNDCLOUD) s_mode = MODE_MENU;
            }
            set_status("Servidor desconectado");
        }
        if (ev->type == UI_ACTION_DIALOG_YES || ev->type == UI_ACTION_DIALOG_NO) s_dialog_open = false;
        return;
    }
    if (s_dialog_kind == DLG_DISCONNECT) {
        if (ev->type == UI_ACTION_DIALOG_YES) {
            account_disconnect();
            set_status("Cuenta de SoundCloud desconectada");
        }
        if (ev->type == UI_ACTION_DIALOG_YES || ev->type == UI_ACTION_DIALOG_NO) s_dialog_open = false;
        return;
    }
    if (ev->type == UI_ACTION_DIALOG_YES) {
        network_download_resume();
        set_status("Reanudando descargas");
        s_dialog_open = false;
    } else if (ev->type == UI_ACTION_DIALOG_NO) {
        network_download_discard();
        set_status("Descargas descartadas");
        s_dialog_open = false;
    }
}

#ifdef SOUND3DS_AUTOTEST
// Prueba automática en el emulador (solo en la compilación de prueba): lee
// sdmc:/3ds/sound3ds/autotest.txt = "consulta|índice de resultado|descargar(0/1)"
// y recorre búsqueda de playlists -> abrir -> descargar sin tocar la pantalla.
static void autotest_step(void) {
    static int stage = 0;
    static u64 t0 = 0, t_stage = 0;
    static char query[64] = "";
    static int pick = 0, dl = 0;
    u64 now = osGetTime();
    if (stage == 0) {
        FILE* f = fopen("sdmc:/3ds/sound3ds/autotest.txt", "r");
        char line[128] = "";
        if (!f) { stage = 99; return; }
        if (!fgets(line, sizeof(line), f)) line[0] = 0;
        fclose(f);
        char* a = strtok(line, "|\r\n");
        char* b = strtok(NULL, "|\r\n");
        char* c = strtok(NULL, "|\r\n");
        snprintf(query, sizeof(query), "%s", a ? a : "lofi");
        pick = b ? atoi(b) : 0;
        dl = c ? atoi(c) : 0;
        t0 = now;
        stage = 1;
        dlog("autotest: %s|%d|%d", query, pick, dl);
        return;
    }
    // "local": abre Local en el álbum de la playlist descargada
    if (strcmp(query, "local") == 0) {
        if (stage == 1 && s_dialog_open && now - t0 > 2000) {
            UIEvent ev = { .type = UI_ACTION_DIALOG_NO };
            handle_dialog_event(&ev);
        }
        if (stage == 1 && !s_dialog_open && now - t0 > 3000) {
            enter_mode(MODE_LOCAL);
            for (int i = 0; i < s_vis_count; i++)
                if (s_vis_albums[i] >= 0 && strcmp(library_album(s_vis_albums[i])->name, "Lofi Chill Vietnam") == 0) s_cf_index = i;
            local_fill_album_view();
            stage = 99;
        }
        return;
    }
    // "localart": reproduce la primera canción de Local y pasa a SoundCloud (carátula)
    if (strcmp(query, "localart") == 0) {
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            play_track(SRC_LOCAL, 0);
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && now - t_stage > 3000) {
            enter_mode(MODE_SOUNDCLOUD);
            stage = 99;
        }
        return;
    }
    // "biglist": Local, "Todas las canciones" en la última página (cuando termina el escaneo)
    if (strcmp(query, "biglist") == 0) {
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            stage = 2;
        } else if (stage == 2 && s_local_scanned && s_local_count > 500) {
            s_cf_index = 0;
            local_fill_album_view();
            SearchViewModel* v = &s_local_view;
            v->cursor = v->total_results - 1;
            v->page = v->cursor / LIST_ROWS;
            dlog("autotest biglist: filas=%d", v->total_results);
            stage = 99;
        }
        return;
    }
    // "server": servidor propio -> buscar `pick`... (query fija "Alice"), reproducir, abrir Álbumes y un álbum
    if (strcmp(query, "server") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            open_menu_item(2);
            dlog("autotest server: configurado=%d modo=%d", (int)subsonic_configured(), (int)s_mode);
            network_search_async("Alice");
            s_sc_view.is_searching = true;
            snprintf(s_sc_view.query, sizeof(s_sc_view.query), "Alice");
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && now - t_stage > 1500 && step == 0) {
            dlog("autotest server: resultados=%d", s_sc_view.total_results);
            if (s_sc_view.total_results > 0) play_track(SRC_SOUNDCLOUD, 0);
            step = 1;
            t_stage = now;
        } else if (stage == 2 && step == 1 && now - t_stage > 5000) {
            dlog("autotest server: suena=%d pos=%lu", (int)audio_is_playing(), (unsigned long)s_playback.elapsed_ms);
            set_sc_tab(SC_TAB_PLAYLISTS, false);
            step = 2;
            t_stage = now;
        } else if (stage == 2 && step == 2 && now - t_stage > 2500) {
            dlog("autotest server: albumes=%d", s_acc[ACC_LIST_PLAYLISTS].view.total_results);
            if (s_acc[ACC_LIST_PLAYLISTS].view.total_results > 1) open_playlist(1);
            step = 3;
            t_stage = now;
        } else if (stage == 2 && step == 3 && now - t_stage > 3000) {
            dlog("autotest server: album abierto=%d canciones=%d", (int)s_in_playlist, s_pl_view.total_results);
            stage = 99;
        }
        return;
    }
    // "bandcamp": buscar y reproducir, adelantar, abrir un álbum destacado y Populares
    if (strcmp(query, "bandcamp") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            open_menu_item(3);
            network_search_async("lofi");
            s_sc_view.is_searching = true;
            snprintf(s_sc_view.query, sizeof(s_sc_view.query), "lofi");
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && step == 0 && now - t_stage > 3000) {
            dlog("autotest bandcamp: resultados=%d primero=%s", s_sc_view.total_results,
                 s_sc_view.total_results ? s_sc_results.tracks[0].stream_endpoint : "-");
            if (s_sc_view.total_results > 0) play_track(SRC_SOUNDCLOUD, 0);
            step = 1;
            t_stage = now;
        } else if (stage == 2 && step == 1 && now - t_stage > 6000) {
            dlog("autotest bandcamp: suena=%d pos=%lu dur=%lu err=%s", (int)audio_is_playing(),
                 (unsigned long)s_playback.elapsed_ms, (unsigned long)s_playback.duration_ms, network_get_stream_error());
            seek_to(0.5f);
            step = 2;
            t_stage = now;
        } else if (stage == 2 && step == 2 && now - t_stage > 5000) {
            dlog("autotest bandcamp: tras adelantar pos=%lu suena=%d", (unsigned long)s_playback.elapsed_ms,
                 (int)audio_is_playing());
            set_sc_tab(SC_TAB_PLAYLISTS, false);
            step = 3;
            t_stage = now;
        } else if (stage == 2 && step == 3 && now - t_stage > 4000) {
            dlog("autotest bandcamp: destacados=%d", s_acc[ACC_LIST_PLAYLISTS].view.total_results);
            if (s_acc[ACC_LIST_PLAYLISTS].view.total_results > 0) open_playlist(0);
            step = 4;
            t_stage = now;
        } else if (stage == 2 && step == 4 && now - t_stage > 4000) {
            dlog("autotest bandcamp: album abierto=%d canciones=%d", (int)s_in_playlist, s_pl_view.total_results);
            if (s_pl_view.total_results > 0) play_track(SRC_PLAYLIST, 0);
            step = 5;
            t_stage = now;
        } else if (stage == 2 && step == 5 && now - t_stage > 5000) {
            dlog("autotest bandcamp: album suena=%d pos=%lu dur=%lu", (int)audio_is_playing(),
                 (unsigned long)s_playback.elapsed_ms, (unsigned long)s_playback.duration_ms);
            set_sc_tab(SC_TAB_LIKES, false);
            step = 6;
            t_stage = now;
        } else if (stage == 2 && step == 6 && now - t_stage > 4000) {
            dlog("autotest bandcamp: populares=%d", s_acc[ACC_LIST_LIKES].view.total_results);
            stage = 99;
        }
        return;
    }
    // "bccol": pestaña Tu colección de Bandcamp (con bandcamp.json en la SD) y Configuración
    if (strcmp(query, "bccol") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            open_menu_item(3);
            set_sc_tab(SC_TAB_PLAYLISTS, false);
            dlog("autotest bccol: conectada=%d", (int)bandcamp_connected());
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && step == 0 && now - t_stage > 4000) {
            AccList* L = &s_acc[ACC_LIST_PLAYLISTS];
            dlog("autotest bccol: coleccion=%d titulo=%s siguiente=%d", L->view.total_results, L->view.header_title,
                 (int)(L->next[0] != 0));
            if (L->view.total_results > 0) open_playlist(0);
            step = 1;
            t_stage = now;
        } else if (stage == 2 && step == 1 && now - t_stage > 4000) {
            dlog("autotest bccol: abierto=%d canciones=%d", (int)s_in_playlist, s_pl_view.total_results);
            if (s_pl_view.total_results > 0) play_track(SRC_PLAYLIST, 0);
            step = 2;
            t_stage = now;
        } else if (stage == 2 && step == 2 && now - t_stage > 5000) {
            dlog("autotest bccol: suena=%d pos=%lu", (int)audio_is_playing(), (unsigned long)s_playback.elapsed_ms);
            s_settings_open = true;
            stage = 99;
        }
        return;
    }
    // "srvdl": descargar una canción suelta y un álbum entero del servidor
    if (strcmp(query, "srvdl") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            open_menu_item(2);
            network_search_async("Alice");
            s_sc_view.is_searching = true;
            snprintf(s_sc_view.query, sizeof(s_sc_view.query), "Alice");
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && step == 0 && now - t_stage > 2000) {
            if (s_sc_view.total_results > 0)
                download_message(network_download_enqueue(&s_sc_results.tracks[0]), s_sc_results.tracks[0].title, "srv");
            set_sc_tab(SC_TAB_PLAYLISTS, false);
            step = 1;
            t_stage = now;
        } else if (stage == 2 && step == 1 && now - t_stage > 2500) {
            if (s_acc[ACC_LIST_PLAYLISTS].view.total_results > 1) open_playlist(1);
            step = 2;
            t_stage = now;
        } else if (stage == 2 && step == 2 && now - t_stage > 3000) {
            int skipped = 0;
            int n = s_in_playlist ? network_download_enqueue_playlist(&s_pl, &skipped) : -1;
            dlog("autotest srvdl: album=%s en cola=%d saltadas=%d", s_pl.title, n, skipped);
            step = 3;
            t_stage = now;
        } else if (stage == 2 && step == 3 && now - t_stage > 15000) {
            char title[96];
            int pct = 0, queued = 0;
            bool busy = network_download_status(title, sizeof(title), &pct, &queued);
            dlog("autotest srvdl: descargando=%d pendientes=%d", (int)busy, queued);
            stage = 99;
        }
        return;
    }
    // "plist": playlist propia "Prueba" con dos canciones; se reproduce y se abre su menú
    if (strcmp(query, "plist") == 0) {
        if (stage == 1 && now - t0 > 3000 && s_local_scanned) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            for (int i = 0; i < playlists_count(); i++)
                if (strcmp(playlists_name(i), "Prueba") == 0) playlists_delete(i);
            int p = playlists_create("Prueba");
            bool a1 = playlists_add(p, s_local_all[s_local_count - 1].path);
            bool a2 = playlists_add(p, s_local_all[0].path);
            bool dup = playlists_add(p, s_local_all[0].path);
            playlists_refresh(p);
            dlog("autotest plist: p=%d add=%d,%d repetida=%d vis=%d foco=%d filas=%d", p, a1, a2, dup, s_vis_count,
                 s_vis_albums[s_cf_index], s_local_view.total_results);
            play_track(SRC_LOCAL, 0);
            dlog("autotest plist: cola=%d desde=%d suena=%s", s_local_queue_n, s_local_from_all,
                 s_local_all[s_local_playing].title);
            open_playlist_sheet();
            dlog("autotest plist: menu abierto=%d opciones=%d", (int)s_sheet_open, s_sheet.count);
            // En el álbum de la primera canción: el menú de agregar, con la marca de "ya está"
            s_sheet_open = false;
            int al = library_album_of_track(0);
            for (int i = 0; i < s_vis_count; i++)
                if (s_vis_albums[i] == al) s_cf_index = i;
            s_cf_pos = (float)s_cf_index;
            local_fill_album_view();
            s_local_view.cursor = 0;
            open_playlist_sheet();
            dlog("autotest plist: agregar opciones=%d marcada=%d", s_sheet.count, (int)s_sheet.checked[1]);
            stage = 99;
        }
        return;
    }
    // "fav": estrella en Local (álbum Favoritos) y en el servidor; vuelta del cover flow
    if (strcmp(query, "fav") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 3000 && s_local_scanned) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            int before = s_vis_count;
            local_change_album(-1);
            dlog("autotest fav: vuelta del cover flow -> %d de %d", s_cf_index, before);
            play_track(SRC_LOCAL, 0);
            toggle_favorite();
            dlog("autotest fav: local favorita=%d albumes=%d primero=%d segundo=%d", playing_favorite_state(),
                 s_vis_count, s_vis_albums[0], s_vis_albums[1]);
            stage = 2;
            t_stage = now;
        } else if (stage == 2 && step == 0 && now - t_stage > 1500) {
            open_menu_item(2);
            network_search_async("Alice");
            s_sc_view.is_searching = true;
            snprintf(s_sc_view.query, sizeof(s_sc_view.query), "Alice");
            step = 1;
            t_stage = now;
        } else if (stage == 2 && step == 1 && now - t_stage > 2000) {
            if (s_sc_view.total_results > 0) play_track(SRC_SOUNDCLOUD, 0);
            toggle_favorite();
            dlog("autotest fav: servidor favorita=%d id=%s", playing_favorite_state(), s_playing_id);
            step = 2;
            t_stage = now;
        } else if (stage == 2 && step == 2 && now - t_stage > 2500) {
            dlog("autotest fav: lista marcada=%d", (int)s_sc_results.tracks[0].starred);
            stage = 99;
        }
        return;
    }
    // "flac": Local, álbum "Prueba FLAC", canción <índice>; adelanta a la mitad (resultado en debug.log)
    if (strcmp(query, "flac") == 0) {
        if (stage == 1 && now - t0 > 3000 && s_local_scanned) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            for (int i = 0; i < s_vis_count; i++)
                if (s_vis_albums[i] >= 0 && strcmp(library_album(s_vis_albums[i])->name, "Prueba FLAC") == 0) s_cf_index = i;
            local_fill_album_view();
            play_track(SRC_LOCAL, pick);
            dlog("autotest flac: canciones=%d pista=%d dur=%lu", s_local_view.total_results, pick,
                 (unsigned long)s_playback.duration_ms);
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && now - t_stage > 5000) {
            dlog("autotest flac: suena=%d pos=%lu carga=%d%% buffer=%d%% err=%s", (int)audio_is_playing(),
                 (unsigned long)s_playback.elapsed_ms, audio_get_decode_load_percent(), audio_get_buffer_fill_percent(),
                 local_get_error());
            seek_to(0.5f);
            t_stage = now;
            stage = 3;
        } else if (stage == 3 && now - t_stage > 4000) {
            dlog("autotest flac: tras adelantar pos=%lu suena=%d carga=%d%%", (unsigned long)s_playback.elapsed_ms,
                 (int)audio_is_playing(), audio_get_decode_load_percent());
            stage = 99;
        }
        return;
    }
    // "settings": abre Configuración
    if (strcmp(query, "settings") == 0) {
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            s_settings_open = true;
            stage = 99;
        }
        return;
    }
    // "modes": aleatorio y repetir en Local (resultado en debug.log)
    if (strcmp(query, "modes") == 0) {
        static int step = 0;
        if (stage == 1 && now - t0 > 3000) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_LOCAL);
            g_settings.shuffle = true;
            g_settings.repeat = REPEAT_ALL;
            play_track(SRC_LOCAL, 0);
            dlog("autotest modes: cola=%d pos=%d", s_local_queue_n, s_local_queue_pos);
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && now - t_stage > 700) {
            t_stage = now;
            step++;
            if (step <= 6) {
                play_relative(1);
                dlog("autotest modes: aleatorio +1 -> pos=%d", s_local_queue_pos);
            } else if (step <= 8) {
                play_relative(-1);
                dlog("autotest modes: aleatorio -1 -> pos=%d", s_local_queue_pos);
            } else if (step == 9) {
                g_settings.repeat = REPEAT_ONE;
                int before = s_local_queue_pos;
                play_step(1, true);
                dlog("autotest modes: repetir cancion %d -> %d", before, s_local_queue_pos);
            } else if (step == 10) {
                toggle_shuffle();
                g_settings.repeat = REPEAT_OFF;
                dlog("autotest modes: sin aleatorio cola=%d pos=%d", s_local_queue_n, s_local_queue_pos);
                play_at(SRC_LOCAL, s_local_queue_n - 1);
                play_step(1, true);
                dlog("autotest modes: fin de lista ended=%d playing=%d", (int)s_list_ended,
                     (int)s_playback.is_playing);
            } else if (step == 11) {
                toggle_play_pause();
                dlog("autotest modes: play tras el fin -> pos=%d playing=%d", s_local_queue_pos,
                     (int)s_playback.is_playing);
                g_settings.shuffle = false;
                g_settings.repeat = REPEAT_OFF;
                stage = 99;
            }
        }
        return;
    }
    // "scan": abre Recibir desde el PC y simula leer el QR de qr_autotest.txt
    if (strcmp(query, "scan") == 0) {
        if (stage == 1 && now - t0 > 2500) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            open_scan();
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && now - t_stage > (u64)(pick > 0 ? pick * 1000 : 2500)) {
            char qr[1024] = "";
            FILE* f = fopen("sdmc:/3ds/sound3ds/qr_autotest.txt", "r");
            if (f) {
                if (!fgets(qr, sizeof(qr), f)) qr[0] = 0;
                fclose(f);
            }
            qr[strcspn(qr, "\r\n")] = 0;
            qrscan_inject(qr);
            stage = 99;
        }
        return;
    }
    // "tab": pestaña <índice> de la cuenta; acción 1 = abrir/reproducir el primero,
    // 2 = descargar el primero
    if (strcmp(query, "tab") == 0) {
        if (stage == 1 && now - t0 > 3000) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            enter_mode(MODE_SOUNDCLOUD);
            set_sc_tab((ScTab)pick, false);
            stage = 2;
        } else if (stage == 2 && s_acc[tab_list((ScTab)pick)].loaded && !s_acc[tab_list((ScTab)pick)].loading) {
            t_stage = now;
            stage = 3;
        } else if (stage == 3 && now - t_stage > 2000) {
            if (dl == 1 && pick == SC_TAB_PLAYLISTS) open_playlist(0);
            else if (dl == 1) play_track(pick == SC_TAB_LIKES ? SRC_LIKES : SRC_FEED, 0);
            else if (dl == 3) s_acc[tab_list((ScTab)pick)].view.result_repost[0] = true;   // ver la marca
            else if (dl == 2) {
                UIEvent ev = { .type = UI_ACTION_DOWNLOAD };
                handle_event(&ev);
            }
            stage = 99;
        }
        return;
    }
    // "account": abre Configuración en la fila de la cuenta; "disconnect" además
    // abre el diálogo de Desconectar y lo acepta
    if (strcmp(query, "account") == 0 || strcmp(query, "disconnect") == 0) {
        if (stage == 1 && now - t0 > 3000) {
            if (s_dialog_open && s_dialog_kind == DLG_PENDING_DOWNLOADS) s_dialog_open = false;
            s_settings_open = true;
            s_settings_vm.cursor = SETTINGS_ROW_ACCOUNT;
            t_stage = now;
            stage = 2;
        } else if (stage == 2 && strcmp(query, "disconnect") == 0 && now - t_stage > 6000) {
            UIEvent ev = { .type = UI_ACTION_SETTINGS_TOGGLE, .track_index = SETTINGS_ROW_ACCOUNT };
            handle_settings_event(&ev);
            t_stage = now;
            stage = 3;
        } else if (stage == 3 && now - t_stage > 5000) {
            UIEvent ev = { .type = UI_ACTION_DIALOG_YES };
            handle_dialog_event(&ev);
            stage = 99;
        }
        return;
    }
    // "resume": solo acepta el diálogo de descargas pendientes tras unos segundos
    if (strcmp(query, "resume") == 0) {
        if (stage == 1 && s_dialog_open && now - t0 > 5000) {
            UIEvent ev = { .type = UI_ACTION_DIALOG_YES };
            handle_dialog_event(&ev);
            enter_mode(MODE_SOUNDCLOUD);
            stage = 99;
        }
        return;
    }
    if (stage == 1 && now - t0 > 2500 && !s_dialog_open) {
        enter_mode(MODE_SOUNDCLOUD);
        s_sc_kind = SC_KIND_PLAYLISTS;
        snprintf(s_plr_view.query, sizeof(s_plr_view.query), "%s", query);
        s_plr_view.is_searching = network_search_playlists_async(query);
        stage = 2;
    } else if (stage == 2 && !s_plr_view.is_searching && s_plr_view.total_results > 0) {
        t_stage = now;
        stage = 3;
    } else if (stage == 3 && now - t_stage > 3000) {
        open_playlist(pick < s_plr_view.total_results ? pick : 0);
        stage = 4;
    } else if (stage == 4 && !s_pl_loading) {
        t_stage = now;
        stage = 5;
    } else if (stage == 5 && now - t_stage > 4000) {
        if (dl == 1) download_whole_playlist();
        if (dl == 2) play_track(SRC_PLAYLIST, 1);   // reproducir la segunda canción
        stage = 6;
    }
}
#endif

static bool shell_is_closed(void) {
    u8 state = 1;
    return s_ptmu_ok && R_SUCCEEDED(PTMU_GetShellState(&state)) && state == 0;
}

// gsp::Lcd se abre y cierra en cada uso (como ftpd): el menú HOME también lo necesita
static void set_backlight(bool on) {
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
}

static const char* apt_hook_name(APT_HookType h) {
    switch (h) {
        case APTHOOK_ONSUSPEND: return "suspend";
        case APTHOOK_ONRESTORE: return "restore";
        case APTHOOK_ONSLEEP:   return "sleep";
        case APTHOOK_ONWAKEUP:  return "wakeup";
        case APTHOOK_ONEXIT:    return "exit";
        default:                return "?";
    }
}

static void apt_hook(APT_HookType hook, void* param) {
    (void)param;
    dlog("apt %s", apt_hook_name(hook));
    // Si la app deja el primer plano con las pantallas apagadas, encenderlas o el
    // menú HOME quedaría a oscuras
    if ((hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) && s_lid_closed) set_backlight(true);
    // Recibir desde el PC: la cámara se apaga en HOME o en reposo y vuelve al regresar
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) qrscan_suspend();
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) qrscan_resume();
}

// Si la consola puede suspenderse: no mientras suena música ni mientras esté cerrada
// con la app en reposo (para poder reanudar con (A) sin abrirla)
static void update_sleep_policy(void) {
    bool allowed = !s_playback.is_playing && !s_lid_closed;
    if (aptIsSleepAllowed() != allowed) aptSetSleepAllowed(allowed);
}

// Qué hace la mascota según lo que pasa en la app
static void update_mascot(u64 last_input, const uint8_t bands[16]) {
    u64 now = osGetTime();
    MascotState st;
    // Cargando: desde que se elige la pista (o se adelanta) hasta el primer audio, y si el
    // stream se queda sin datos a mitad de la canción
    if (s_waiting_audio && audio_get_position_ms() > 0) s_waiting_audio = false;
    bool loading = s_playback.is_playing && !s_playback.stream_error &&
                   (s_waiting_audio || (source_streaming() && audio_is_starved()));

    if (s_sc_view.is_searching || s_plr_view.is_searching || s_pl_loading) st = MASCOT_SEARCH;
    else if (loading) st = MASCOT_LOADING;
    else if (s_playback.is_playing) st = MASCOT_DANCE;
    else if (now - last_input > MASCOT_SLEEP_MS) st = MASCOT_SLEEP;
    else if (s_mode == MODE_MENU) {
        static const MascotState by_card[MENU_ITEMS] = { MASCOT_CLOUD, MASCOT_LOCAL, MASCOT_SERVER, MASCOT_BANDCAMP };
        st = by_card[s_menu.cursor];
    }
    else st = MASCOT_AWAKE;

    if (st != s_mascot.state) {
        s_mascot.state = st;
        s_mascot.state_since = now;
    }
    float bass = (bands[0] + bands[1] + bands[2] + bands[3]) / (4.0f * 255.0f);
    s_mascot.energy = s_mascot.energy * 0.8f + bass * 0.2f;
}

int main(int argc, char* argv[]) {
    dlog_init();
    dlog("inicio");
    settings_load();
    if (!gfx_init()) return 1;
    theme_init();
    ui_init();
    fft_init();
    audio_init();
    network_init();
    account_init();
    // La biblioteca local se prepara en segundo plano desde el inicio
    gfx_set_library_scanning(0);
    local_rescan();
    s_ptmu_ok = R_SUCCEEDED(ptmuInit());
    static aptHookCookie apt_cookie;
    aptHook(&apt_cookie, apt_hook, NULL);
    audio_remember_output_route();
    headset_init();
    favorites_init();
    playlists_init();
    build_path_index();
    srand((unsigned)osGetTime());
    game_init(&s_game);

    // Volumen de la app al máximo: el nivel lo fija el deslizador físico de la consola
    s_playback.volume = 1.0f;
    audio_set_volume(1.0f);
    s_playback.scrub_preview = -1.0f;
    snprintf(s_playback.title, sizeof(s_playback.title), "Nada sonando");

    snprintf(s_sc_view.placeholder, sizeof(s_sc_view.placeholder), "search");
    snprintf(s_sc_view.empty_msg, sizeof(s_sc_view.empty_msg), "Toca la lupa para buscar");
    s_sc_view.selected_index = -1;
    snprintf(s_local_view.placeholder, sizeof(s_local_view.placeholder), "filtrar");
    s_local_view.row_text = local_row_text;
    s_local_view.show_playlist_btn = true;
    s_sc_view.show_download = true;
    s_sc_view.show_selector = true;
    s_sc_view.search_kind = SC_KIND_TRACKS;
    s_local_view.selected_index = -1;
    snprintf(s_plr_view.placeholder, sizeof(s_plr_view.placeholder), "playlists");
    snprintf(s_plr_view.empty_msg, sizeof(s_plr_view.empty_msg), "Toca la lupa para buscar playlists");
    s_plr_view.selected_index = -1;
    s_plr_view.show_selector = true;
    s_plr_view.search_kind = SC_KIND_PLAYLISTS;
    s_pl_view.selected_index = -1;
    s_pl_view.playlist_header = true;
    {
        static const char* titles[3] = { "Tus likes", "Tus playlists", "Feed" };
        for (int k = 0; k < 3; k++) {
            SearchViewModel* av = &s_acc[k].view;
            av->selected_index = -1;
            av->title_header = true;
            av->show_download = k != ACC_LIST_PLAYLISTS;
            s_acc[k].marks_page = -1;
            snprintf(av->header_title, sizeof(av->header_title), "%s", titles[k]);
            snprintf(av->empty_msg, sizeof(av->empty_msg), "Toca el título para cargar");
        }
    }
    s_pl_view.show_download = true;

    // Descargas que quedaron pendientes de una sesión anterior
    {
        char summary[96];
        if (network_download_pending(summary, sizeof(summary)) > 0) {
            snprintf(s_dialog.title, sizeof(s_dialog.title), "Descargas pendientes");
            snprintf(s_dialog.body, sizeof(s_dialog.body), L("Hay una descarga sin terminar: %s. ¿Quieres reanudarla?"), summary);
            snprintf(s_dialog.yes, sizeof(s_dialog.yes), "Reanudar");
            snprintf(s_dialog.no, sizeof(s_dialog.no), "Descartar");
            s_dialog_kind = DLG_PENDING_DOWNLOADS;
            s_dialog.account_icon = false;
            s_dialog_open = true;
        }
    }

    uint8_t bands[16] = {0};
    uint8_t peaks[16] = {0};
    int16_t pcm_samples[256] = {0};
    u64 last_input = osGetTime();
    u64 lid_changed_at = 0, last_route_check = 0;
    u64 tap_at = 0;
    u64 last_game_ms = osGetTime();
    u64 last_cpad_step = 0;
    bool cpad_held = false;
    bool game_was_visible = false;
    u32 tap_key = 0;
    u32 hold_keys = 0;
    u64 hold_at = 0;
    bool hold_done = false;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        // Tapa cerrada: si la consola no se suspendió es porque hay música; apagar pantallas
        bool closed = shell_is_closed();
        if (closed != s_lid_closed) {
            s_lid_closed = closed;
            dlog("tapa %s", closed ? "cerrada" : "abierta");
            set_backlight(!closed);
            if (!closed) last_input = osGetTime();
            lid_changed_at = osGetTime();
            tap_key = 0;
        }

        // Salida de audio: con la tapa cerrada el códec silencia los parlantes; se
        // revisa cada medio segundo mientras está cerrada y un rato después de abrirla
        u64 now_ms = osGetTime();
        if ((s_lid_closed || now_ms - lid_changed_at < 3000) && now_ms - last_route_check >= 500) {
            last_route_check = now_ms;
            audio_route_for_lid(s_lid_closed);
        }

        switch (headset_poll(now_ms)) {
            case HEADSET_TOGGLE: toggle_play_pause(); break;
            case HEADSET_NEXT:   play_relative(1); break;
            case HEADSET_PREV:   play_relative(-1); break;
            default: break;
        }

        if (s_lid_closed) {
            audio_keep_dsp_awake();
            // En reposo: un toque de L o R = pausa/reanuda; doble toque de L = anterior,
            // de R = siguiente. La pausa espera a que pase la ventana del doble toque.
            // (A) también pausa si el sistema la entrega con la consola cerrada.
            u32 kUp = hidKeysUp();
            if (kDown || kUp) dlog("reposo teclas down=%08lX up=%08lX held=%08lX",
                                   (unsigned long)kDown, (unsigned long)kUp, (unsigned long)kHeld);
            if (kDown & KEY_A) toggle_play_pause();
            if (g_settings.lid_keys == LID_KEYS_TAP) {
                u32 tap = kDown & (KEY_L | KEY_R);
                if (tap == (KEY_L | KEY_R)) tap = KEY_R; // ambos en el mismo cuadro: uno solo
                if (tap) {
                    if (tap_key == tap && now_ms - tap_at <= DOUBLE_TAP_MS) {
                        play_relative(tap == KEY_L ? -1 : 1);
                        tap_key = 0;
                    } else {
                        if (tap_key) toggle_play_pause(); // toque previo del otro gatillo
                        tap_key = tap;
                        tap_at = now_ms;
                    }
                } else if (tap_key && now_ms - tap_at > DOUBLE_TAP_MS) {
                    toggle_play_pause();
                    tap_key = 0;
                }
            } else if (g_settings.lid_keys == LID_KEYS_HOLD) {
                // Mantener: L anterior, R siguiente, L+R juntos pausa. Un roce no hace nada.
                u32 lr = kHeld & (KEY_L | KEY_R);
                if (lr != hold_keys) {
                    hold_keys = lr;
                    hold_at = now_ms;
                    hold_done = false;
                } else if (lr && !hold_done && now_ms - hold_at >= LID_HOLD_MS) {
                    hold_done = true;
                    if (lr == (KEY_L | KEY_R)) toggle_play_pause();
                    else play_relative(lr == KEY_L ? -1 : 1);
                }
            }
        } else {
            if (kDown & KEY_START) break;
            if (kDown || (kHeld & KEY_TOUCH)) last_input = osGetTime();

            UIEvent event;
            sync_account_tabs();
            if (s_dialog_open) {
                if (ui_update_dialog(kDown, touch, &event)) handle_dialog_event(&event);
            } else if (s_sheet_open) {
                if (ui_update_sheet(kDown, touch, &s_sheet, &event)) handle_sheet_event(&event);
            } else if (s_scan_open) {
                bool done = s_scan_vm.state == SCAN_DONE || s_scan_vm.state == SCAN_ERROR;
                if (ui_update_scan(kDown, touch, done, &event)) handle_scan_event(&event);
            } else if (s_settings_open) {
                if (ui_update_settings(kDown, touch, &event)) handle_settings_event(&event);
            } else if (ui_update(kDown, kHeld, touch, s_mode, s_show_game, shown_view(), &event)) {
                handle_event(&event);
            }

            // Local: el analógico también recorre los álbumes (con repetición al mantenerlo)
            if (s_mode == MODE_LOCAL && !s_show_game) {
                circlePosition cp;
                hidCircleRead(&cp);
                int dir = cp.dx > 90 ? 1 : (cp.dx < -90 ? -1 : 0);
                if (dir != 0 && osGetTime() - last_cpad_step > (cpad_held ? 160u : 0u)) {
                    local_change_album(dir);
                    last_cpad_step = osGetTime();
                    last_input = last_cpad_step;
                }
                cpad_held = dir != 0;
            }
        }

#ifdef SOUND3DS_AUTOTEST
        autotest_step();
#endif
        // Cuenta: renovación de la sesión en segundo plano y avisos (sesión vencida)
        account_poll();
        {
            char acc_msg[96];
            if (account_take_message(acc_msg, sizeof(acc_msg))) set_status(acc_msg);
        }
        {
            static u64 last_scan_ms = 0;
            u64 t = osGetTime();
            update_scan(last_scan_ms ? (t - last_scan_ms) / 1000.0f : 0.0f);
            last_scan_ms = t;
        }
        local_rescan_poll();
        if (s_scan_thread && !s_local_scanned) {
            int found = 0;
            local_scan_progress(&found, NULL);
            gfx_set_library_scanning(found);
        }
        update_local_artwork();
        poll_search();
        poll_account_list();
        poll_star();
        acc_list_autoload();
        acc_list_marks();
        poll_playlist_search();
        poll_playlist();
        refresh_playlist_marks();

        // Descargas: avance en el botón, avisos al terminar y biblioteca desactualizada
        {
            int pct = 0, queued = 0;
            bool dl_active = network_download_status(NULL, 0, &pct, &queued);
            s_sc_view.download_active = s_pl_view.download_active = dl_active;
            s_sc_view.download_percent = s_pl_view.download_percent = pct;
            // Avance de la playlist abierta si se está bajando
            int done = 0, total = 0;
            if (s_in_playlist && network_download_group_progress(s_pl.id, &done, &total)) {
                snprintf(s_pl_view.dl_all_label, sizeof(s_pl_view.dl_all_label), "%d/%d", done, total);
                snprintf(s_pl_top.download_status, sizeof(s_pl_top.download_status), L("Descargando %d de %d"), done, total);
            } else {
                s_pl_view.dl_all_label[0] = '\0';
                s_pl_top.download_status[0] = '\0';
            }
            char dl_msg[96];
            if (network_download_take_message(dl_msg, sizeof(dl_msg))) {
                if (queued > 0) {
                    size_t len = strlen(dl_msg);
                    snprintf(dl_msg + len, sizeof(dl_msg) - len, L(" (%d en cola)"), queued);
                }
                set_status(dl_msg);
            }
            if (network_download_take_library_changed()) {
                s_marks_gen++;
                refresh_saved_marks();
                s_local_dirty = true;
                if (s_mode == MODE_LOCAL) local_rescan();   // en segundo plano; varias seguidas se juntan
            }
        }

        // Carátula descargada en segundo plano: subirla a la GPU desde el hilo principal
        if (s_playback.artwork_loading) {
            uint8_t* rgba = NULL;
            int w = 0, h = 0;
            NetJobStatus st = network_poll_artwork(&rgba, &w, &h);
            if (st != NET_JOB_RUNNING) {
                s_playback.artwork_loading = false;
                if (st == NET_JOB_DONE) {
                    gfx_set_artwork_rgba(rgba, w, h);
                    network_free_artwork(rgba);
                }
            }
        }

        // Bandcamp no da la duración en las búsquedas: llega al resolver el stream
        if (s_playing_src != SRC_NONE && s_playing_src != SRC_LOCAL && s_playback.duration_ms == 0)
            s_playback.duration_ms = network_stream_duration_ms();

        // Tiempo transcurrido segun las muestras realmente enviadas al hardware
        uint32_t pos_ms = s_playing_src != SRC_NONE ? s_seek_base_ms + audio_get_position_ms() : 0;
        s_playback.elapsed_ms = pos_ms;

        // Fin de la pista: la fuente terminó y el decodificador ya no tiene datos
        if (s_playback.is_playing && !source_streaming() && audio_is_starved()) {
            if (pos_ms < 1000 && s_playing_src == SRC_LOCAL && local_last_too_heavy() &&
                s_heavy_skips < s_local_queue_n) {
                // FLAC demasiado pesado: se salta con aviso (sin dar vueltas si la lista entera lo es)
                s_heavy_skips++;
                char msg[64];
                snprintf(msg, sizeof(msg), "%s", local_get_error());
                play_step(1, true);
                set_status(msg);
            } else if (pos_ms < 1000) {
                // No llegó audio: no saltar en cadena por toda la lista
                audio_pause();
                s_playback.is_playing = false;
                s_playback.stream_error = true;
                const char* err = s_playing_src == SRC_LOCAL ? local_get_error() : network_get_stream_error();
                set_status(err[0] ? err : "No llego audio");
            } else {
                s_heavy_skips = 0;
                play_step(1, true);
            }
        }

        if (s_playback.is_playing) {
            audio_get_recent_samples(pcm_samples, 128);
            fft_process(pcm_samples, 128);
        } else {
            fft_process(pcm_samples, 0); // las barras caen a cero
        }
        fft_get_bands(bands);
        fft_get_peaks(peaks);

        if (s_playback.status_msg[0] && osGetTime() > s_playback.status_until) s_playback.status_msg[0] = '\0';
        s_playback.buffer_percent = audio_get_buffer_fill_percent();
        s_playback.is_buffering = network_is_streaming() && (s_playback.buffer_percent < 20);

        // Minijuego: solo se actualiza si se está viendo (reproductor, consola abierta)
        bool game_visible = s_show_game && s_mode != MODE_MENU && !s_lid_closed;
        if (game_visible) {
            circlePosition cp;
            hidCircleRead(&cp);
            float dt = (float)(now_ms - last_game_ms) / 1000.0f;
            game_update(&s_game, dt, cp.dx / 150.0f, -cp.dy / 150.0f, kDown, s_mascot.energy);
        } else if (game_was_visible) {
            game_resume(&s_game); // al volver a verlo, cuenta corta antes de seguir
        }
        game_was_visible = game_visible;
        // El cover flow se desliza hacia el álbum enfocado
        {
            float dt = (float)(now_ms - last_game_ms) / 1000.0f;
            float k = dt * 11.0f;
            if (k > 1.0f) k = 1.0f;
            s_cf_pos += ((float)s_cf_index - s_cf_pos) * k;
            if (fabsf((float)s_cf_index - s_cf_pos) < 0.002f) s_cf_pos = (float)s_cf_index;
        }
        last_game_ms = now_ms;

        // LED de notificación en arcoíris mientras suena música (si está activado)
        led_set_rainbow(g_settings.led_rgb && s_playback.is_playing);

        update_sleep_policy();
        update_mascot(last_input, bands);
        s_menu.has_track = s_playing_src != SRC_NONE;
        s_menu.is_playing = s_playback.is_playing;
        snprintf(s_menu.now_playing, sizeof(s_menu.now_playing), "%s - %s", s_playback.title, s_playback.artist);

        // Con las pantallas apagadas no se dibuja: el bucle solo atiende botones.
        // svcSleepThread y no gspWaitForVBlank, que podría no llegar con la tapa cerrada.
        if (s_lid_closed) {
            svcSleepThread(16 * 1000 * 1000LL);
            continue;
        }

        gfx_frame_begin();
        bool touching = (kHeld & KEY_TOUCH) != 0;
        s_settings_vm.lang_en = g_settings.lang == LANG_EN;
        s_settings_vm.led_rgb = g_settings.led_rgb;
        s_settings_vm.lid_keys = g_settings.lid_keys;
        subsonic_label(s_menu.server, sizeof(s_menu.server));
        snprintf(s_settings_vm.server, sizeof(s_settings_vm.server), "%s", s_menu.server);
        bandcamp_label(s_settings_vm.bandcamp, sizeof(s_settings_vm.bandcamp));
        s_playback.favorite = playing_favorite_state();
        s_playback.shuffle = g_settings.shuffle;
        s_playback.repeat = g_settings.repeat;
        account_username(s_settings_vm.account, sizeof(s_settings_vm.account));
        s_settings_vm.account_expired = account_expired();
        snprintf(s_playback.account, sizeof(s_playback.account), "%s", s_settings_vm.account);
        if (s_scan_open) {
            const u16* frame;
            if (qrscan_lock_new_frame(&frame)) {
                gfx_scan_upload(frame);
                qrscan_unlock_frame();
            }
            gfx_render_scan_top(&s_scan_vm, &s_playback);
            if (s_dialog_open) gfx_render_dialog_bottom(&s_dialog, touching, touch.px, touch.py);
            else gfx_render_scan_bottom(&s_scan_vm, touching, touch.px, touch.py);
        } else if (s_mode == MODE_MENU) {
            gfx_render_menu_top(&s_playback, &s_mascot);
            if (s_dialog_open) gfx_render_dialog_bottom(&s_dialog, touching, touch.px, touch.py);
            else if (s_settings_open) gfx_render_settings_bottom(&s_settings_vm, touching, touch.px, touch.py);
            else gfx_render_menu_bottom(&s_menu, touching, touch.px, touch.py);
        } else {
            if (s_show_game) gfx_render_game_top(&s_playback, &s_game, s_mascot.energy);
            else if (s_mode == MODE_SOUNDCLOUD && s_in_playlist) gfx_render_playlist_top(&s_pl_top, &s_playback, &s_mascot);
            else if (s_mode == MODE_LOCAL) gfx_render_coverflow_top(&s_playback, s_vis_albums, s_vis_count, s_cf_pos,
                                                                  local_playing_vis());
            else gfx_render_top(&s_playback, bands, peaks, &s_mascot);
            if (s_dialog_open) gfx_render_dialog_bottom(&s_dialog, touching, touch.px, touch.py);
            else if (s_sheet_open) gfx_render_sheet_bottom(&s_sheet, touching, touch.px, touch.py);
            else if (s_settings_open) gfx_render_settings_bottom(&s_settings_vm, touching, touch.px, touch.py);
            else gfx_render_bottom(&s_playback, shown_view(), s_show_game, touching, touch.px, touch.py);
        }
        gfx_frame_end();
    }

    dlog("salida: fuentes");
    stop_sources();
    aptSetSleepAllowed(true);
    set_backlight(true);
    if (s_ptmu_ok) ptmuExit();
    library_exit();
    led_exit();
    headset_exit();
    close_scan();
    if (s_scan_thread) {
        threadJoin(s_scan_thread, U64_MAX);
        threadFree(s_scan_thread);
        s_scan_thread = NULL;
    }
    dlog("salida: red");
    account_exit();
    network_exit();
    dlog("salida: audio");
    audio_exit();
    dlog("salida: gfx");
    gfx_exit();
    dlog("salida: fin");
    return 0;
}
