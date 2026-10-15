#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_SEARCH_REQUEST,
    UI_ACTION_SELECT_TRACK,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_PREV,
    UI_ACTION_NEXT,
    UI_ACTION_SEEK,
    UI_ACTION_SET_VOLUME,
    UI_ACTION_TOGGLE_THEME
} UIActionType;

typedef struct {
    UIActionType type;
    int track_index;
    float seek_ratio;   // 0.0f a 1.0f
    float volume_ratio; // 0.0f a 1.0f
    char search_text[64];
} UIEvent;

// Inicializa el gestor de interfaz de usuario táctil
void ui_init(void);

// Actualiza el estado táctil y procesa la entrada de HID
// Retorna true si ocurrió una acción de usuario
bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, UIEvent* out_event);

// Abre el teclado virtual nativo de la consola (swkbd)
// Retorna true si el usuario acepto el texto
bool ui_open_keyboard(char* out_text, size_t max_len);

#endif // UI_H
