#include "ui.h"
#include "gfx.h"
#include <string.h>
#include <stdio.h>

static bool s_was_touching = false;

// Arrastre de la barra de progreso: empieza al tocarla y se confirma al soltar
static bool s_scrubbing = false;
static float s_scrub_ratio = 0.0f;

#define SCRUB_X      20
#define SCRUB_W      (320 - 40)
#define SCRUB_TOP    147
#define SCRUB_BOTTOM 163

static float scrub_ratio_at(int x) {
    float ratio = (float)(x - SCRUB_X) / (float)SCRUB_W;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return ratio;
}

void ui_init(void) {
    s_was_touching = false;
    s_scrubbing = false;
}

bool ui_open_keyboard(char* out_text, size_t max_len) {
    if (!out_text || max_len == 0) return false;

    SwkbdState swkbd;
    char buffer[128] = {0};

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Buscar cancion en SoundCloud");
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    SwkbdButton button = swkbdInputText(&swkbd, buffer, sizeof(buffer));

    if (button != SWKBD_BUTTON_NONE && button != SWKBD_BUTTON_LEFT) {
        // Usuario pulso OK o Enter
        strncpy(out_text, buffer, max_len - 1);
        out_text[max_len - 1] = '\0';
        return true;
    }

    return false;
}

bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, UIEvent* out_event) {
    if (!out_event) return false;
    memset(out_event, 0, sizeof(UIEvent));

    // --- Atajos con botones fisicos de la consola ---
    if (kDown & KEY_A) {
        out_event->type = UI_ACTION_PLAY_PAUSE;
        return true;
    }
    if ((kDown & KEY_L) || (kDown & KEY_DLEFT)) {
        out_event->type = UI_ACTION_PREV;
        return true;
    }
    if ((kDown & KEY_R) || (kDown & KEY_DRIGHT)) {
        out_event->type = UI_ACTION_NEXT;
        return true;
    }
    if (kDown & KEY_SELECT) {
        out_event->type = UI_ACTION_TOGGLE_THEME;
        return true;
    }
    if (kHeld & KEY_DUP) {
        out_event->type = UI_ACTION_SET_VOLUME;
        out_event->volume_ratio = 1.0f; // Sube volumen
        return true;
    }
    if (kHeld & KEY_DDOWN) {
        out_event->type = UI_ACTION_SET_VOLUME;
        out_event->volume_ratio = -1.0f; // Baja volumen
        return true;
    }

    // --- Interaccion Tactil en Pantalla Inferior (320x240) ---
    bool is_touching = (kHeld & KEY_TOUCH);
    bool just_touched = (kDown & KEY_TOUCH);

    // Barra de progreso: vista previa mientras se arrastra, adelanto al soltar
    if (s_scrubbing) {
        if (is_touching) {
            s_scrub_ratio = scrub_ratio_at(touch.px);
            out_event->type = UI_ACTION_SCRUB_DRAG;
        } else {
            s_scrubbing = false;
            out_event->type = UI_ACTION_SEEK;
        }
        out_event->seek_ratio = s_scrub_ratio;
        s_was_touching = is_touching;
        return true;
    }

    if (just_touched) {
        int x = touch.px;
        int y = touch.py;

        // 1. Barra de busqueda: x: 10..310, y: 8..36
        if (x >= 10 && x <= 310 && y >= 8 && y <= 36) {
            out_event->type = UI_ACTION_SEARCH_REQUEST;
            return true;
        }

        // 2. Elementos de la lista de resultados
        for (int i = 0; i < LIST_ROWS; i++) {
            int iy = LIST_Y + i * LIST_ROW_STEP;
            if (x >= 10 && x <= 310 && y >= iy && y <= (iy + LIST_ROW_H)) {
                out_event->type = UI_ACTION_SELECT_TRACK;
                out_event->track_index = i;
                return true;
            }
        }

        // 3. Barra de progreso
        if (x >= 15 && x <= 305 && y >= SCRUB_TOP && y <= SCRUB_BOTTOM) {
            s_scrubbing = true;
            s_scrub_ratio = scrub_ratio_at(x);
            out_event->type = UI_ACTION_SCRUB_DRAG;
            out_event->seek_ratio = s_scrub_ratio;
            return true;
        }

        // 4. Botones de Transporte
        // Prev: x: 60..100, y: 165..205
        if (x >= 60 && x <= 100 && y >= 165 && y <= 205) {
            out_event->type = UI_ACTION_PREV;
            return true;
        }

        // Play/Pause: x: 135..185, y: 162..210
        if (x >= 135 && x <= 185 && y >= 162 && y <= 210) {
            out_event->type = UI_ACTION_PLAY_PAUSE;
            return true;
        }

        // Next: x: 210..250, y: 165..205
        if (x >= 210 && x <= 250 && y >= 165 && y <= 205) {
            out_event->type = UI_ACTION_NEXT;
            return true;
        }

        // 4. Boton de Cambio de Tema: x: 14..110, y: 210..236
        if (x >= 14 && x <= 110 && y >= 210 && y <= 236) {
            out_event->type = UI_ACTION_TOGGLE_THEME;
            return true;
        }
    }

    // 6. Arrastre en la barra de volumen: x: 184..300, y: 212..232
    if (is_touching && touch.px >= 180 && touch.px <= 305 && touch.py >= 212 && touch.py <= 232) {
        float ratio = (float)(touch.px - 184) / 116.0f;
        if (ratio < 0.0f) ratio = 0.0f;
        if (ratio > 1.0f) ratio = 1.0f;
        out_event->type = UI_ACTION_SET_VOLUME;
        out_event->volume_ratio = ratio;
        return true;
    }

    s_was_touching = is_touching;
    return false;
}
