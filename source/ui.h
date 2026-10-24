#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <stdbool.h>
#include "gfx.h"

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_SEARCH_REQUEST,
    UI_ACTION_SELECT_TRACK,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_PREV,
    UI_ACTION_NEXT,
    UI_ACTION_SCRUB_DRAG,   // Arrastrando la barra de progreso (vista previa)
    UI_ACTION_SEEK,         // Se soltó la barra: adelantar a seek_ratio
    UI_ACTION_CONFIRM,      // (A): reproducir la pista enfocada o pausar si ya suena
    UI_ACTION_CURSOR_UP,
    UI_ACTION_CURSOR_DOWN,
    UI_ACTION_PAGE_PREV,
    UI_ACTION_PAGE_NEXT,
    UI_ACTION_TOGGLE_THEME,
    UI_ACTION_TOGGLE_GAME,  // botón del mando: pantalla superior reproductor <-> minijuego
    UI_ACTION_DOWNLOAD,     // SoundCloud: descargar la pista enfocada a la SD
    UI_ACTION_ALBUM_PREV,   // Local: álbum anterior en el cover flow
    UI_ACTION_ALBUM_NEXT,   // Local: álbum siguiente
    UI_ACTION_OPEN_SETTINGS,  // botón del engranaje
    UI_ACTION_SETTINGS_TOGGLE, // configuración: cambiar la fila track_index (o la enfocada si es -1)
    UI_ACTION_SETTINGS_CLOSE,
    UI_ACTION_BACK,         // (B) o botón casa: volver al menú
    UI_ACTION_MENU_SELECT,  // tarjeta del menú tocada (track_index = tarjeta)
    UI_ACTION_OPEN_NOW_PLAYING, // píldora "sonando ahora" del menú
    UI_ACTION_SEARCH_KIND,  // selector de búsqueda: track_index 0 canciones, 1 playlists
    UI_ACTION_DOWNLOAD_ALL, // descargar la playlist abierta completa
    UI_ACTION_DIALOG_YES,
    UI_ACTION_DIALOG_NO,
    UI_ACTION_TAB,          // pestaña de la cuenta tocada (track_index = pestaña)
    UI_ACTION_TAB_PREV,     // ZL
    UI_ACTION_TAB_NEXT,     // ZR
    UI_ACTION_SCAN_AGAIN,   // recibir desde el PC: leer otro QR
    UI_ACTION_SCAN_CLOSE,
    UI_ACTION_SHUFFLE,      // aleatorio sí / no
    UI_ACTION_REPEAT,       // repetir: no -> la lista -> la canción
    UI_ACTION_FAVORITE,     // estrella: favorita (o no) la canción que suena
    UI_ACTION_PLAYLIST_MENU, // Local (Y o el botón de la barra): playlists propias de la canción enfocada
    UI_ACTION_SHEET_PICK,   // menú de opciones: elegida la opción track_index
    UI_ACTION_SHEET_CLOSE
} UIActionType;

typedef struct {
    UIActionType type;
    int track_index;    // fila tocada en la página visible (0..LIST_ROWS-1)
    float seek_ratio;   // 0.0f a 1.0f
    char search_text[64];
    int option;         // configuración: 0 = siguiente opción, -1 = anterior, k+1 = elegir la k
} UIEvent;

// Inicializa el gestor de interfaz de usuario táctil
void ui_init(void);

// Actualiza el estado táctil y procesa la entrada de HID
// Retorna true si ocurrió una acción de usuario
// game_on: el minijuego ocupa la pantalla superior y se queda con (X)/(Y)
// view: la lista que se muestra abajo (define qué botones hay en la cabecera)
bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, AppMode mode, bool game_on, const SearchViewModel* view,
               UIEvent* out_event);

// Entrada mientras hay un diálogo Sí / No abierto
bool ui_update_dialog(u32 kDown, touchPosition touch, UIEvent* out_event);

// Entrada mientras está abierto "Recibir desde el PC" (done: ya hay un resultado)
bool ui_update_scan(u32 kDown, touchPosition touch, bool done, UIEvent* out_event);

// Entrada mientras está abierto el menú de opciones (sh: para saber qué filas se ven)
bool ui_update_sheet(u32 kDown, touchPosition touch, const SheetViewModel* sh, UIEvent* out_event);

// Entrada mientras la pantalla de configuración está abierta
bool ui_update_settings(u32 kDown, touchPosition touch, UIEvent* out_event);

// Abre el teclado virtual nativo de la consola (swkbd)
// Retorna true si el usuario acepto el texto
// allow_empty permite aceptar texto vacío (por ejemplo para quitar un filtro)
bool ui_open_keyboard(char* out_text, size_t max_len, const char* hint, bool allow_empty);

#endif // UI_H
