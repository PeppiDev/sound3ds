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
    UI_ACTION_BACK,         // (B) o botón casa: volver al menú
    UI_ACTION_MENU_SELECT,  // tarjeta del menú tocada (track_index = tarjeta)
    UI_ACTION_OPEN_NOW_PLAYING // píldora "sonando ahora" del menú
} UIActionType;

typedef struct {
    UIActionType type;
    int track_index;    // fila tocada en la página visible (0..LIST_ROWS-1)
    float seek_ratio;   // 0.0f a 1.0f
    char search_text[64];
} UIEvent;

// Inicializa el gestor de interfaz de usuario táctil
void ui_init(void);

// Actualiza el estado táctil y procesa la entrada de HID
// Retorna true si ocurrió una acción de usuario
bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, AppMode mode, UIEvent* out_event);

// Abre el teclado virtual nativo de la consola (swkbd)
// Retorna true si el usuario acepto el texto
// allow_empty permite aceptar texto vacío (por ejemplo para quitar un filtro)
bool ui_open_keyboard(char* out_text, size_t max_len, const char* hint, bool allow_empty);

#endif // UI_H
