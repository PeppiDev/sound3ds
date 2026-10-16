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
#include "local.h"

// Tiempo sin tocar nada tras el cual la mascota se duerme
#define MASCOT_SLEEP_MS 10000

// De dónde viene la pista que suena
typedef enum {
    SRC_NONE = 0,
    SRC_SOUNDCLOUD,
    SRC_LOCAL
} TrackSource;

static AppMode s_mode = MODE_MENU;
static TrackSource s_playing_src = SRC_NONE;

// Posición (ms) desde la que arrancó el audio actual tras un adelanto
static uint32_t s_seek_base_ms = 0;

// Estáticos: las listas no caben en la pila del hilo principal
static PlaybackViewModel s_playback;
static SearchViewModel s_sc_view;
static SearchViewModel s_local_view;
static SearchResultSet s_sc_results;
static MenuViewModel s_menu;
static MascotViewModel s_mascot;

// Música local: todas las pistas escaneadas y la vista filtrada (índice de vista -> pista)
static LocalTrack s_local_all[LOCAL_MAX_TRACKS];
static int s_local_count = 0;
static bool s_local_scanned = false;
static int s_local_map[MAX_LIST_ITEMS];
static int s_local_playing = -1; // índice en s_local_all de la pista local que suena

// Mensaje temporal en pantalla (errores de búsqueda o de stream)
static void set_status(const char* msg) {
    snprintf(s_playback.status_msg, sizeof(s_playback.status_msg), "%s", msg);
    s_playback.status_until = osGetTime() + 6000;
}

static SearchViewModel* view_for(TrackSource src) {
    return src == SRC_LOCAL ? &s_local_view : &s_sc_view;
}

static TrackSource source_of_mode(AppMode mode) {
    return mode == MODE_LOCAL ? SRC_LOCAL : SRC_SOUNDCLOUD;
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

// Rellena la vista local con las pistas que coinciden con el filtro
static void local_apply_filter(const char* filter) {
    SearchViewModel* v = &s_local_view;
    int n = 0;
    v->selected_index = -1;
    for (int i = 0; i < s_local_count && n < MAX_LIST_ITEMS; i++) {
        const LocalTrack* t = &s_local_all[i];
        if (!contains_ci(t->title, filter) && !contains_ci(t->artist, filter)) continue;
        s_local_map[n] = i;
        snprintf(v->result_titles[n], sizeof(v->result_titles[n]), "%s", t->title);
        snprintf(v->result_artists[n], sizeof(v->result_artists[n]), "%s", t->artist);
        v->result_preview[n] = false;
        if (i == s_local_playing && s_playing_src == SRC_LOCAL) v->selected_index = n;
        n++;
    }
    v->total_results = n;
    v->page = 0;
    v->cursor = 0;
    snprintf(v->query, sizeof(v->query), "%s", filter);
    snprintf(v->empty_msg, sizeof(v->empty_msg), "%s",
             s_local_count == 0 ? "Copia tus MP3 a " LOCAL_MUSIC_HINT : "Nada coincide con el filtro");
}

static void enter_mode(AppMode mode) {
    s_mode = mode;
    if (mode == MODE_LOCAL && !s_local_scanned) {
        s_local_count = local_scan(s_local_all, LOCAL_MAX_TRACKS);
        s_local_scanned = true;
        local_apply_filter("");
        char msg[64];
        snprintf(msg, sizeof(msg), "%d canciones en la tarjeta SD", s_local_count);
        set_status(msg);
    }
}

static void play_track(TrackSource src, int idx) {
    SearchViewModel* v = view_for(src);
    if (idx < 0 || idx >= v->total_results) return;

    // Detener lo que suene y vaciar el audio antes de preparar la nueva pista
    stop_sources();
    audio_clear_buffers();

    view_for(src == SRC_LOCAL ? SRC_SOUNDCLOUD : SRC_LOCAL)->selected_index = -1;
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

    bool started;
    if (src == SRC_SOUNDCLOUD) {
        const SearchResultTrack* t = &s_sc_results.tracks[idx];
        s_playback.duration_ms = t->is_preview ? 30 * 1000 : t->duration_ms; // las previews duran 30 s
        // La carátula se descarga en segundo plano; mientras tanto se ve el vinilo
        s_playback.artwork_loading = network_artwork_async(t->artwork_url);
        started = network_start_stream(t->stream_endpoint, t->kind);
    } else {
        s_local_playing = s_local_map[idx];
        const char* path = s_local_all[s_local_playing].path;
        network_artwork_async(NULL);
        s_playback.artwork_loading = false;
        s_playback.duration_ms = local_estimate_duration_ms(path);
        audio_set_codec(AUDIO_CODEC_MP3);
        started = local_start_stream(path, 0.0f);
    }

    if (!started) {
        s_playback.is_playing = false;
        s_playback.stream_error = true;
        set_status("No se pudo iniciar la pista");
        return;
    }
    audio_play();
    s_playback.is_playing = true;
}

static void play_relative(int step) {
    TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
    SearchViewModel* v = view_for(src);
    if (v->total_results <= 0) return;
    int idx = v->selected_index < 0 ? 0 : v->selected_index + step;
    if (idx < 0) idx = v->total_results - 1;
    if (idx >= v->total_results) idx = 0;
    play_track(src, idx);
}

static void toggle_play_pause(void) {
    if (audio_is_playing()) {
        audio_pause();
        s_playback.is_playing = false;
    } else if (s_playing_src == SRC_NONE || s_playback.stream_error) {
        // Nada cargado (o falló): reproducir la pista enfocada de la lista actual
        TrackSource src = s_playing_src != SRC_NONE ? s_playing_src : source_of_mode(s_mode);
        SearchViewModel* v = view_for(src);
        play_track(src, s_playback.stream_error && v->selected_index >= 0 ? v->selected_index : v->cursor);
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
        audio_set_codec(AUDIO_CODEC_MP3);
        ok = local_start_stream(s_local_all[s_local_playing].path, ratio);
    } else {
        ok = network_seek_stream(ratio, s_playback.duration_ms, &start_ms);
    }
    if (ok) {
        s_seek_base_ms = start_ms;
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
                break;
            case UI_ACTION_TOGGLE_THEME:
                theme_toggle_next();
                break;
            default:
                break;
        }
        return;
    }

    TrackSource src = source_of_mode(s_mode);
    SearchViewModel* v = view_for(src);
    switch (ev->type) {
        case UI_ACTION_SEARCH_REQUEST: {
            char text[64] = {0};
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
        case UI_ACTION_BACK:       s_mode = MODE_MENU; break;
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
    if (previews > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d de %d son previews de 30 s", previews, v->total_results);
        set_status(msg);
    }
}

// Qué hace la mascota según lo que pasa en la app
static void update_mascot(u64 last_input, const uint8_t bands[16]) {
    u64 now = osGetTime();
    MascotState st;
    if (s_sc_view.is_searching) st = MASCOT_SEARCH;
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
    if (!gfx_init()) return 1;
    theme_init();
    ui_init();
    fft_init();
    audio_init();
    network_init();

    // Volumen de la app al máximo: el nivel lo fija el deslizador físico de la consola
    s_playback.volume = 1.0f;
    audio_set_volume(1.0f);
    s_playback.scrub_preview = -1.0f;
    snprintf(s_playback.title, sizeof(s_playback.title), "Nada sonando");

    snprintf(s_sc_view.placeholder, sizeof(s_sc_view.placeholder), "search");
    snprintf(s_sc_view.empty_msg, sizeof(s_sc_view.empty_msg), "Toca la lupa para buscar");
    s_sc_view.selected_index = -1;
    snprintf(s_local_view.placeholder, sizeof(s_local_view.placeholder), "filtrar");
    s_local_view.selected_index = -1;

    uint8_t bands[16] = {0};
    uint8_t peaks[16] = {0};
    int16_t pcm_samples[256] = {0};
    u64 last_input = osGetTime();

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        if (kDown & KEY_START) break;
        if (kDown || (kHeld & KEY_TOUCH)) last_input = osGetTime();

        UIEvent event;
        if (ui_update(kDown, kHeld, touch, s_mode, &event)) handle_event(&event);

        poll_search();

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

        update_mascot(last_input, bands);
        s_menu.has_track = s_playing_src != SRC_NONE;
        s_menu.is_playing = s_playback.is_playing;
        snprintf(s_menu.now_playing, sizeof(s_menu.now_playing), "%s - %s", s_playback.title, s_playback.artist);

        gfx_frame_begin();
        bool touching = (kHeld & KEY_TOUCH) != 0;
        if (s_mode == MODE_MENU) {
            gfx_render_menu_top(&s_playback, &s_mascot);
            gfx_render_menu_bottom(&s_menu, touching, touch.px, touch.py);
        } else {
            gfx_render_top(&s_playback, bands, peaks, &s_mascot);
            gfx_render_bottom(&s_playback, view_for(source_of_mode(s_mode)), touching, touch.px, touch.py);
        }
        gfx_frame_end();
    }

    stop_sources();
    network_exit();
    audio_exit();
    gfx_exit();
    return 0;
}
