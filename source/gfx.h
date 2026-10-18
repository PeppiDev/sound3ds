#ifndef GFX_H
#define GFX_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include "theme.h"
#include "game.h"

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
#define PAGE_PREV_X     172
#define PAGE_NEXT_X     250
#define PAGER_Y         224
#define PAGER_BTN_R     11
#define HOME_X          290   // botón para volver al menú
#define HOME_Y          224

// Menú principal: dos tarjetas en la pantalla inferior (compartido con ui.c)
#define MENU_CARD_X     20
#define MENU_CARD_W     280
#define MENU_CARD_H     70
#define MENU_CARD_Y0    34
#define MENU_CARD_Y1    114
#define MENU_ITEMS      2
// Fila de botones de abajo: paleta, minijuego, descarga (SoundCloud) y configuración
#define BAR_BTN_W       30
#define THEME_BTN_X     8
#define GAME_BTN_X      42
#define GAME_BTN_W      BAR_BTN_W
#define DL_BTN_X        76
#define DL_BTN_W        BAR_BTN_W
#define CFG_BTN_X       110   // en el reproductor
#define CFG_MENU_BTN_X  42    // en el menú, junto a la paleta
#define NOWPLAY_X       78    // píldora "sonando ahora" junto a paleta y configuración

// Resultados que caben en la lista paginada (páginas de LIST_ROWS)
#define MAX_LIST_ITEMS  300

// Pantallas de la app
typedef enum {
    MODE_MENU = 0,
    MODE_SOUNDCLOUD,
    MODE_LOCAL
} AppMode;

// Lo que está haciendo la mascota (un cubo de ojos cuadrados)
typedef enum {
    MASCOT_AWAKE = 0,  // despierto, parpadea y mira alrededor
    MASCOT_SLEEP,      // sin interacción: duerme y le salen ZzZ
    MASCOT_CLOUD,      // menú sobre SoundCloud: mira una nube
    MASCOT_LOCAL,      // menú sobre Local: mira una tarjeta SD
    MASCOT_SEARCH,     // buscando: mueve los ojos de lado a lado
    MASCOT_LOADING,    // cargando la pista: se queda pensando con un spinner
    MASCOT_DANCE       // suena música: se balancea con el ritmo
} MascotState;

typedef struct {
    MascotState state;
    float energy;      // 0..1, intensidad de los graves para el baile
    u64 state_since;   // cuándo empezó el estado (para animaciones)
} MascotViewModel;

typedef struct {
    int cursor;              // tarjeta enfocada (0 SoundCloud, 1 Local)
    bool has_track;          // hay una pista cargada para la píldora "sonando ahora"
    bool is_playing;
    char now_playing[200];
} MenuViewModel;

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
    char status_msg[96]; // aviso temporal (errores de búsqueda o stream)
    u64 status_until;
    int buffer_percent;
    float volume;
} PlaybackViewModel;

typedef struct {
    char query[64];
    char placeholder[32];    // texto de la barra cuando no hay consulta
    char empty_msg[64];      // aviso cuando la lista está vacía
    bool is_searching;
    int selected_index;
    int total_results;
    int page;            // página visible (LIST_ROWS resultados por página)
    int cursor;          // resultado enfocado con la cruceta (índice absoluto)
    char result_titles[MAX_LIST_ITEMS][128];
    char result_artists[MAX_LIST_ITEMS][64];
    bool result_preview[MAX_LIST_ITEMS];
    bool result_saved[MAX_LIST_ITEMS];   // ya descargada en la SD
    bool show_download;      // botón de descarga (solo SoundCloud)
    bool download_active;    // hay una descarga en curso
    int download_percent;
} SearchViewModel;

// Pantalla de configuración (abajo)
#define SETTINGS_ROWS 3   // idioma, LED, cerrar
typedef struct {
    int cursor;          // fila enfocada con la cruceta
    bool lang_en;
    bool led_rgb;
} SettingsViewModel;

// Inicializa subsistemas gráficos de 3DS y buffers de texto
bool gfx_init(void);

// Libera recursos gráficos
void gfx_exit(void);

// Inicia un nuevo cuadro sincronizado con VBlank
void gfx_frame_begin(void);

// Pantalla superior del reproductor (carátula, metadatos, visualizador y la mascota pequeña)
void gfx_render_top(const PlaybackViewModel* playback, const uint8_t bands[16], const uint8_t peaks[16],
                    const MascotViewModel* mascot);

// Pantalla inferior del reproductor (búsqueda, lista, controles, tema, paginador y volver)
// game_on resalta el botón del minijuego mientras se muestra arriba
void gfx_render_bottom(const PlaybackViewModel* playback, const SearchViewModel* search, bool game_on,
                       bool touch_down, int touch_x, int touch_y);

// Pantalla superior del modo Local: cover flow de álbumes. albums son los índices de
// biblioteca visibles (tras el filtro), pos la posición animada en esa lista y
// playing la posición del álbum que suena (-1 si ninguno).
void gfx_render_coverflow_top(const PlaybackViewModel* playback, const int* albums, int count, float pos, int playing);

// Olvida las texturas de carátula (tras reconstruir la biblioteca)
void gfx_coverflow_reset(void);

// Pantalla superior con el minijuego (energy 0..1: graves de la música)
void gfx_render_game_top(const PlaybackViewModel* playback, const GameState* g, float energy);

// Pantalla superior del menú: barra de estado y la mascota en grande
void gfx_render_menu_top(const PlaybackViewModel* playback, const MascotViewModel* mascot);

// Pantalla inferior del menú: tarjetas SoundCloud / Local, tema y "sonando ahora"
void gfx_render_menu_bottom(const MenuViewModel* menu, bool touch_down, int touch_x, int touch_y);

// Pantalla de abajo con la configuración (idioma y LED RGB)
void gfx_render_settings_bottom(const SettingsViewModel* s, bool touch_down, int touch_x, int touch_y);

// Fila de la configuración bajo el punto tocado (-1 si ninguna)
int gfx_settings_row_at(int x, int y);

// Finaliza el cuadro actual
void gfx_frame_end(void);

// Carga la carátula decodificada como textura dinámica para Citro2D
void gfx_set_artwork_rgba(const uint8_t* rgba, int w, int h);

// Limpia la carátula actual y vuelve al vinilo retro
void gfx_clear_artwork(void);

#endif // GFX_H
