#ifndef GFX_H
#define GFX_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include "theme.h"

#define SCREEN_TOP_W    400
#define SCREEN_TOP_H    240
#define SCREEN_BOT_W    320
#define SCREEN_BOT_H    240

// Lista de resultados en la pantalla inferior (compartido con ui.c)
#define LIST_ROWS       5
#define LIST_Y          42
#define LIST_ROW_STEP   21
#define LIST_ROW_H      19

// Controles inferiores (compartido con ui.c)
#define TRANSPORT_Y     186
#define PREV_X          100
#define PLAY_X          160
#define NEXT_X          220
#define VOL_ICON_X      76
#define VOL_BAR_X       96
#define VOL_BAR_W       204

typedef struct {
    char title[128];
    char artist[64];
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    bool is_playing;
    bool is_buffering;
    bool artwork_loading;
    bool stream_error;
    float scrub_preview; // < 0: sin arrastre; 0..1 posición que se está arrastrando
    char status_msg[64]; // aviso temporal (errores de búsqueda o stream)
    u64 status_until;
    int buffer_percent;
    float volume;
} PlaybackViewModel;

typedef struct {
    char query[64];
    bool is_searching;
    int selected_index;
    int total_results;
    char result_titles[LIST_ROWS][128];
    char result_artists[LIST_ROWS][64];
    bool result_preview[LIST_ROWS];
} SearchViewModel;

// Inicializa subsistemas gráficos de 3DS y buffers de texto
bool gfx_init(void);

// Libera recursos gráficos
void gfx_exit(void);

// Inicia un nuevo cuadro sincronizado con VBlank
void gfx_frame_begin(void);

// Renderiza la pantalla superior completa (Carátula, metadatos y visualizador FFT)
void gfx_render_top(const PlaybackViewModel* playback, const uint8_t bands[16], const uint8_t peaks[16]);

// Renderiza la pantalla inferior táctil (Búsqueda, lista, controles, volumen y tema)
void gfx_render_bottom(const PlaybackViewModel* playback, const SearchViewModel* search, bool touch_down, int touch_x, int touch_y);

// Finaliza el cuadro actual
void gfx_frame_end(void);

// Carga la carátula decodificada como textura dinámica para Citro2D
void gfx_set_artwork_rgba(const uint8_t* rgba, int w, int h);

// Limpia la carátula actual y vuelve al vinilo retro
void gfx_clear_artwork(void);

#endif // GFX_H
