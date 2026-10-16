#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "audio.h"
#include "fft.h"
#include "gfx.h"
#include "theme.h"
#include "ui.h"
#include "network.h"

// Datos binarios del MP3 demo incrustados directamente en memoria física (.rodata)
extern const u8 demo_bin[];
extern const u8 demo_bin_end[];

static bool s_is_demo_mode = true;

static void load_demo_audio(bool clear_existing) {
    if (clear_existing) {
        network_stop_stream();
        audio_clear_buffers();
    }
    s_is_demo_mode = true;
    size_t size = (size_t)(demo_bin_end - demo_bin);
    audio_feed_mp3_data(demo_bin, size);
}

static void switch_to_track(int idx, PlaybackViewModel* playback, SearchViewModel* search, const SearchResultSet* live_results) {
    if (idx < 0 || idx >= search->total_results) return;

    // Detener cualquier stream anterior y limpiar buffers de audio inmediatamente.
    // Esto evita que el audio anterior continúe sonando o se sobreponga
    // mientras se descarga la carátula o se inicializa la nueva pista.
    network_stop_stream();
    audio_clear_buffers();

    search->selected_index = idx;
    strncpy(playback->title, search->result_titles[idx], sizeof(playback->title) - 1);
    playback->title[sizeof(playback->title) - 1] = '\0';
    strncpy(playback->artist, search->result_artists[idx], sizeof(playback->artist) - 1);
    playback->artist[sizeof(playback->artist) - 1] = '\0';
    playback->elapsed_ms = 0;

    if (live_results && live_results->count > idx) {
        if (live_results->tracks[idx].duration_ms > 0) {
            playback->duration_ms = live_results->tracks[idx].duration_ms;
        }

        // La carátula se descarga en segundo plano; mientras tanto se ve el vinilo
        gfx_clear_artwork();
        playback->artwork_loading = network_artwork_async(live_results->tracks[idx].artwork_url);

        // Iniciar streaming si tiene endpoint progressive
        if (live_results->tracks[idx].has_progressive && live_results->tracks[idx].stream_endpoint[0] != '\0') {
            s_is_demo_mode = false;
            network_start_stream(live_results->tracks[idx].stream_endpoint);
        } else {
            load_demo_audio(true);
        }
    } else {
        playback->duration_ms = 12 * 1000;
        network_artwork_async(NULL);
        playback->artwork_loading = false;
        gfx_clear_artwork();
        load_demo_audio(true);
    }

    audio_play();
    playback->is_playing = true;
}

int main(int argc, char* argv[]) {
    // 1. Inicializar subsistemas CTR
    if (!gfx_init()) return 1;
    theme_init();
    ui_init();
    fft_init();
    audio_init();
    network_init();

    // Estado del reproductor y busqueda
    PlaybackViewModel playback;
    memset(&playback, 0, sizeof(playback));
    strncpy(playback.title, "Lofi Hip Hop Stream", sizeof(playback.title) - 1);
    strncpy(playback.artist, "ChilledCow / Lofi Girl", sizeof(playback.artist) - 1);
    playback.duration_ms = 180 * 1000;
    playback.elapsed_ms = 0;
    playback.volume = 0.85f;
    playback.is_playing = false;

    SearchViewModel search;
    memset(&search, 0, sizeof(search));
    strncpy(search.query, "daft punk", sizeof(search.query) - 1);
    search.total_results = 4;
    search.selected_index = 0;

    // Canciones demo precargadas para arranque instantaneo
    strncpy(search.result_titles[0], "Lofi Hip Hop Stream", 127);
    strncpy(search.result_artists[0], "ChilledCow / Lofi Girl", 63);

    strncpy(search.result_titles[1], "Synthwave Dreams 2000", 127);
    strncpy(search.result_artists[1], "RetroWave Records", 63);

    strncpy(search.result_titles[2], "Midnight Tokyo Beats", 127);
    strncpy(search.result_artists[2], "Nujabes Tribute", 63);

    strncpy(search.result_titles[3], "Classic Y2K MP3 Groove", 127);
    strncpy(search.result_artists[3], "Winamp Era", 63);

    SearchResultSet live_results;
    memset(&live_results, 0, sizeof(live_results));

    // Precargar audio demo directamente desde memoria embebida y arrancar reproducción
    load_demo_audio(true);
    audio_play();
    playback.is_playing = true;
    playback.duration_ms = 12 * 1000;

    // Arrays para visualizador FFT
    uint8_t bands[16] = {0};
    uint8_t peaks[16] = {0};
    int16_t pcm_samples[256] = {0};

    u64 last_tick = osGetTime();

    // Bucle principal CTR
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        // Salir al pulsar START
        if (kDown & KEY_START) {
            break;
        }

        // Procesar eventos de interfaz de usuario
        UIEvent event;
        if (ui_update(kDown, kHeld, touch, &event)) {
            switch (event.type) {
                case UI_ACTION_SEARCH_REQUEST: {
                    char query_in[64] = {0};
                    if (!search.is_searching && ui_open_keyboard(query_in, sizeof(query_in))) {
                        snprintf(search.query, sizeof(search.query), "%s", query_in);
                        search.is_searching = network_search_async(query_in);
                    }
                    break;
                }

                case UI_ACTION_SELECT_TRACK: {
                    switch_to_track(event.track_index, &playback, &search, &live_results);
                    break;
                }

                case UI_ACTION_PLAY_PAUSE: {
                    if (audio_is_playing()) {
                        audio_pause();
                        playback.is_playing = false;
                    } else {
                        if (s_is_demo_mode && audio_get_buffer_fill_percent() < 5 && !network_is_streaming()) {
                            load_demo_audio(false);
                        }
                        audio_play();
                        playback.is_playing = true;
                    }
                    break;
                }

                case UI_ACTION_PREV: {
                    int prev_idx = (search.selected_index > 0) ? search.selected_index - 1 : search.total_results - 1;
                    switch_to_track(prev_idx, &playback, &search, &live_results);
                    break;
                }

                case UI_ACTION_NEXT: {
                    int next_idx = (search.selected_index < search.total_results - 1) ? search.selected_index + 1 : 0;
                    switch_to_track(next_idx, &playback, &search, &live_results);
                    break;
                }

                case UI_ACTION_SEEK: {
                    playback.elapsed_ms = (uint32_t)(playback.duration_ms * event.seek_ratio);
                    break;
                }

                case UI_ACTION_SET_VOLUME: {
                    if (event.volume_ratio == 1.0f) {
                        playback.volume += 0.02f;
                    } else if (event.volume_ratio == -1.0f) {
                        playback.volume -= 0.02f;
                    } else {
                        playback.volume = event.volume_ratio;
                    }
                    if (playback.volume > 1.0f) playback.volume = 1.0f;
                    if (playback.volume < 0.0f) playback.volume = 0.0f;
                    audio_set_volume(playback.volume);
                    break;
                }

                case UI_ACTION_TOGGLE_THEME: {
                    theme_toggle_next();
                    break;
                }

                default:
                    break;
            }
        }

        // Resultados de la búsqueda en segundo plano
        if (search.is_searching) {
            NetJobStatus st = network_poll_search(&live_results);
            if (st != NET_JOB_RUNNING) {
                search.is_searching = false;
                if (st == NET_JOB_DONE) {
                    search.total_results = live_results.count;
                    for (int i = 0; i < live_results.count && i < 4; i++) {
                        strncpy(search.result_titles[i], live_results.tracks[i].title, 127);
                        strncpy(search.result_artists[i], live_results.tracks[i].artist, 63);
                    }
                    switch_to_track(0, &playback, &search, &live_results);
                }
            }
        }

        // Carátula descargada en segundo plano: subirla a la GPU desde el hilo principal
        if (playback.artwork_loading) {
            uint8_t* rgba = NULL;
            int w = 0, h = 0;
            NetJobStatus st = network_poll_artwork(&rgba, &w, &h);
            if (st != NET_JOB_RUNNING) {
                playback.artwork_loading = false;
                if (st == NET_JOB_DONE) {
                    gfx_set_artwork_rgba(rgba, w, h);
                    network_free_artwork(rgba);
                }
            }
        }

        // Actualizar temporizador de reproduccion
        u64 now = osGetTime();
        u64 delta = now - last_tick;
        last_tick = now;

        if (playback.is_playing) {
            playback.elapsed_ms += (uint32_t)delta;
            if (playback.elapsed_ms > playback.duration_ms) {
                playback.elapsed_ms = 0;
            }

            // Si el buffer de audio se vacía y estamos en modo demo local, alimentar siguiente ciclo sin cortar
            if (s_is_demo_mode && audio_get_buffer_fill_percent() < 10 && !network_is_streaming()) {
                load_demo_audio(false);
            }

            // Alimentar y calcular visualizador FFT
            audio_get_recent_samples(pcm_samples, 128);
            fft_process(pcm_samples, 128);
        } else {
            // Decaimiento natural de barras hacia cero
            fft_process(pcm_samples, 0);
        }

        fft_get_bands(bands);
        fft_get_peaks(peaks);

        playback.buffer_percent = audio_get_buffer_fill_percent();
        playback.is_buffering = network_is_streaming() && (playback.buffer_percent < 20);

        // --- Renderizado Gráfico a 60 FPS con Citro2D ---
        gfx_frame_begin();
        gfx_render_top(&playback, bands, peaks);
        gfx_render_bottom(&playback, &search, (kHeld & KEY_TOUCH), touch.px, touch.py);
        gfx_frame_end();
    }

    // Salida limpia de todos los modulos
    network_exit();
    audio_exit();
    gfx_exit();

    return 0;
}
