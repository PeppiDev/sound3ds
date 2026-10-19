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

// Diálogo Sí / No (descargas pendientes de una sesión anterior o desconectar la cuenta)
typedef enum { DLG_PENDING_DOWNLOADS, DLG_DISCONNECT } DialogKind;
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
static int s_local_map[MAX_LIST_ITEMS];   // fila de la lista -> índice en s_local_all
static int s_local_playing = -1; // índice en s_local_all de la pista local que suena
static char s_local_filter[64] = "";

// Cover flow: álbumes visibles (los que tienen pistas que coinciden con el filtro),
// el enfocado y su posición animada
static int s_vis_albums[LIB_MAX_ALBUMS];
static int s_vis_count = 0;
static int s_cf_index = 0;
static float s_cf_pos = 0.0f;

// Cola de reproducción local: las pistas del álbum desde el que se eligió la canción
static int s_local_queue[LOCAL_MAX_TRACKS];
static int s_local_queue_n = 0;
static int s_local_queue_pos = -1;

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

// Lista de abajo: las pistas del álbum enfocado que coinciden con el filtro
static void local_fill_album_view(void) {
    SearchViewModel* v = &s_local_view;
    int n = 0;
    v->selected_index = -1;
    const LibAlbum* a = s_vis_count > 0 ? library_album(s_vis_albums[s_cf_index]) : NULL;
    for (int i = a ? a->first : 0; a && i < a->first + a->count && n < MAX_LIST_ITEMS; i++) {
        const LocalTrack* t = &s_local_all[i];
        if (!track_matches(t, s_local_filter)) continue;
        s_local_map[n] = i;
        snprintf(v->result_titles[n], sizeof(v->result_titles[n]), "%s", t->title);
        snprintf(v->result_artists[n], sizeof(v->result_artists[n]), "%s", t->artist);
        v->result_preview[n] = false;
        v->result_saved[n] = false;
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
    s_vis_count = 0;
    s_cf_index = 0;
    for (int al = 0; al < library_album_count(); al++) {
        const LibAlbum* a = library_album(al);
        bool any = false;
        for (int i = a->first; i < a->first + a->count && !any; i++) any = track_matches(&s_local_all[i], filter);
        if (!any) continue;
        if (al == keep) s_cf_index = s_vis_count;
        s_vis_albums[s_vis_count++] = al;
    }
    s_cf_pos = (float)s_cf_index;
    local_fill_album_view();
}

static void local_change_album(int step) {
    if (s_vis_count == 0) return;
    int idx = s_cf_index + step;
    if (idx < 0) idx = 0;
    if (idx >= s_vis_count) idx = s_vis_count - 1;
    if (idx == s_cf_index) return;
    s_cf_index = idx;
    local_fill_album_view();
}

// Posición en el cover flow del álbum que suena (-1 si no es visible)
static int local_playing_vis(void) {
    if (s_playing_src != SRC_LOCAL || s_local_playing < 0) return -1;
    int al = library_album_of_track(s_local_playing);
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

static void play_local(int pos) {
    if (pos < 0 || pos >= s_local_queue_n) return;
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
    audio_set_codec(t->is_aac ? AUDIO_CODEC_AAC : AUDIO_CODEC_MP3);
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
        // La cola pasa a ser la lista visible (el álbum enfocado)
        s_local_queue_n = v->total_results;
        for (int i = 0; i < v->total_results; i++) s_local_queue[i] = s_local_map[i];
        play_local(idx);
        return;
    }

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
    s_playback.duration_ms = t->is_preview ? 30 * 1000 : t->duration_ms; // las previews duran 30 s
    // La carátula se descarga en segundo plano; mientras tanto se ve el vinilo
    s_playback.artwork_loading = network_artwork_async(t->artwork_url);
    finish_start(network_start_stream(t->stream_endpoint, t->kind));
}

static void play_relative(int step) {
    TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
    if (src == SRC_LOCAL && s_local_queue_n > 0) {
        int pos = s_local_queue_pos + step;
        if (pos < 0) pos = s_local_queue_n - 1;
        if (pos >= s_local_queue_n) pos = 0;
        play_local(pos);
        return;
    }
    SearchViewModel* v = view_for(src);
    if (v->total_results <= 0) return;
    int idx = v->selected_index < 0 ? 0 : v->selected_index + step;
    if (idx < 0) idx = v->total_results - 1;
    if (idx >= v->total_results) idx = 0;
    play_track(src, idx);
}

static void toggle_play_pause(void) {
    dlog("play/pausa (sonando=%d)", (int)audio_is_playing());
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
        audio_set_codec(s_local_all[s_local_playing].is_aac ? AUDIO_CODEC_AAC : AUDIO_CODEC_MP3);
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
    if (!account_connected() || (more && !L->next[0])) return;
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
                 k == ACC_LIST_LIKES ? "Todavía no tienes likes"
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
    if (t != SC_TAB_SEARCH && !account_connected()) return;
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
    bool on = account_connected();
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
        views[i]->active_tab = (int)s_sc_tab;
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
            vm->account = network_receive_is_account(s_scan_text);
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
        if (r.kind == RECV_ACCOUNT) {
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
            if (row == 0) {
                g_settings.lang = g_settings.lang == LANG_EN ? LANG_ES : LANG_EN;
                i18n_set(g_settings.lang);
                settings_save();
            } else if (row == 1) {
                g_settings.led_rgb = !g_settings.led_rgb;
                settings_save();
                set_status(g_settings.led_rgb ? "LED RGB activado" : "LED RGB desactivado");
            } else if (row == SETTINGS_ROW_ACCOUNT) {
                if (account_connected()) {
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

static void handle_event(const UIEvent* ev) {
    if (s_mode == MODE_MENU) {
        switch (ev->type) {
            case UI_ACTION_CURSOR_UP:
            case UI_ACTION_CURSOR_DOWN:
                s_menu.cursor = (s_menu.cursor + 1) % MENU_ITEMS;
                break;
            case UI_ACTION_CONFIRM:
                enter_mode(s_menu.cursor == 0 ? MODE_SOUNDCLOUD : MODE_LOCAL);
                break;
            case UI_ACTION_MENU_SELECT:
                s_menu.cursor = ev->track_index;
                enter_mode(ev->track_index == 0 ? MODE_SOUNDCLOUD : MODE_LOCAL);
                break;
            case UI_ACTION_OPEN_NOW_PLAYING:
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
    if (ev->type == UI_ACTION_DOWNLOAD_ALL) {
        download_whole_playlist();
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
        case UI_ACTION_DOWNLOAD: {
            if (v->total_results <= 0) break;
            if (src == SRC_PLAYLIST) {
                if (s_pl_loading) break;
                download_message(network_download_enqueue_from_playlist(&s_pl, v->cursor), s_pl.tracks[v->cursor].title,
                                 s_pl.title);
            } else if (src == SRC_SOUNDCLOUD || src == SRC_LIKES || src == SRC_FEED) {
                const SearchResultTrack* t = sc_track(src, v->cursor);
                download_message(network_download_enqueue(t), t->title, LOCAL_SC_ALBUM);
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
    v->show_dl_all = v->total_results > 0;
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
                if (strcmp(library_album(s_vis_albums[i])->name, "Lofi Chill Vietnam") == 0) s_cf_index = i;
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
    else if (s_mode == MODE_MENU) st = s_menu.cursor == 0 ? MASCOT_CLOUD : MASCOT_LOCAL;
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

        if (s_lid_closed) {
            audio_keep_dsp_awake();
            // En reposo: un toque de L o R = pausa/reanuda; doble toque de L = anterior,
            // de R = siguiente. La pausa espera a que pase la ventana del doble toque.
            // (A) también pausa si el sistema la entrega con la consola cerrada.
            u32 kUp = hidKeysUp();
            if (kDown || kUp) dlog("reposo teclas down=%08lX up=%08lX held=%08lX",
                                   (unsigned long)kDown, (unsigned long)kUp, (unsigned long)kHeld);
            if (kDown & KEY_A) toggle_play_pause();
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
        } else {
            if (kDown & KEY_START) break;
            if (kDown || (kHeld & KEY_TOUCH)) last_input = osGetTime();

            UIEvent event;
            sync_account_tabs();
            if (s_dialog_open) {
                if (ui_update_dialog(kDown, touch, &event)) handle_dialog_event(&event);
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

        // Tiempo transcurrido segun las muestras realmente enviadas al hardware
        uint32_t pos_ms = s_playing_src != SRC_NONE ? s_seek_base_ms + audio_get_position_ms() : 0;
        s_playback.elapsed_ms = pos_ms;

        // Fin de la pista: la fuente terminó y el decodificador ya no tiene datos
        if (s_playback.is_playing && !source_streaming() && audio_is_starved()) {
            if (pos_ms < 1000) {
                // No llegó audio: no saltar en cadena por toda la lista
                audio_pause();
                s_playback.is_playing = false;
                s_playback.stream_error = true;
                const char* err = s_playing_src == SRC_LOCAL ? local_get_error() : network_get_stream_error();
                set_status(err[0] ? err : "No llego audio");
            } else {
                play_relative(1);
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
