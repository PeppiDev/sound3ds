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

bool ui_open_keyboard(char* out_text, size_t max_len, const char* hint, bool allow_empty) {
    if (!out_text || max_len == 0) return false;

    SwkbdState swkbd;
    char buffer[128] = {0};

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetValidation(&swkbd, allow_empty ? SWKBD_ANYTHING : SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

    SwkbdButton button = swkbdInputText(&swkbd, buffer, sizeof(buffer));

    if (button != SWKBD_BUTTON_NONE && button != SWKBD_BUTTON_LEFT) {
        // Usuario pulso OK o Enter
        strncpy(out_text, buffer, max_len - 1);
        out_text[max_len - 1] = '\0';
        return true;
    }

    return false;
}

// Menú principal: cruceta + (A) o toque sobre las tarjetas
static bool ui_update_menu(u32 kDown, touchPosition touch, UIEvent* out_event) {
    if (kDown & KEY_DUP)    { out_event->type = UI_ACTION_CURSOR_UP; return true; }
    if (kDown & KEY_DDOWN)  { out_event->type = UI_ACTION_CURSOR_DOWN; return true; }
    if (kDown & KEY_A)      { out_event->type = UI_ACTION_CONFIRM; return true; }
    if (kDown & KEY_SELECT) { out_event->type = UI_ACTION_TOGGLE_THEME; return true; }

    if (kDown & KEY_TOUCH) {
        int x = touch.px, y = touch.py;
        for (int i = 0; i < MENU_ITEMS; i++) {
            int cy = i == 0 ? MENU_CARD_Y0 : MENU_CARD_Y1;
            if (x >= MENU_CARD_X && x <= MENU_CARD_X + MENU_CARD_W && y >= cy && y <= cy + MENU_CARD_H) {
                out_event->type = UI_ACTION_MENU_SELECT;
                out_event->track_index = i;
                return true;
            }
        }
        if (x >= 12 && x <= 52 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_TOGGLE_THEME;
            return true;
        }
        if (x >= NOWPLAY_X && x <= SCREEN_BOT_W - 10 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_OPEN_NOW_PLAYING;
            return true;
        }
    }
    return false;
}

bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, AppMode mode, bool game_on, UIEvent* out_event) {
    if (!out_event) return false;
    memset(out_event, 0, sizeof(UIEvent));
    if (mode == MODE_MENU) return ui_update_menu(kDown, touch, out_event);

    if (kDown & KEY_B) {
        out_event->type = UI_ACTION_BACK;
        return true;
    }

    // --- Atajos con botones fisicos de la consola ---
    // (A) reproduce la pista enfocada (o pausa si ya es la que suena); L/R cambian de pista;
    // la cruceta recorre la lista: arriba/abajo entre canciones, izquierda/derecha entre páginas.
    // El volumen se controla con el deslizador físico de la consola.
    if (kDown & KEY_A) {
        out_event->type = UI_ACTION_CONFIRM;
        return true;
    }
    if (kDown & KEY_L) {
        out_event->type = UI_ACTION_PREV;
        return true;
    }
    if (kDown & KEY_R) {
        out_event->type = UI_ACTION_NEXT;
        return true;
    }
    if (kDown & KEY_DUP) {
        out_event->type = UI_ACTION_CURSOR_UP;
        return true;
    }
    if (kDown & KEY_DDOWN) {
        out_event->type = UI_ACTION_CURSOR_DOWN;
        return true;
    }
    // En Local la cruceta izquierda/derecha recorre los álbumes del cover flow;
    // en SoundCloud cambia de página
    if (kDown & KEY_DLEFT) {
        out_event->type = mode == MODE_LOCAL ? UI_ACTION_ALBUM_PREV : UI_ACTION_PAGE_PREV;
        return true;
    }
    if (kDown & KEY_DRIGHT) {
        out_event->type = mode == MODE_LOCAL ? UI_ACTION_ALBUM_NEXT : UI_ACTION_PAGE_NEXT;
        return true;
    }
    if ((kDown & KEY_Y) && mode == MODE_SOUNDCLOUD && !game_on) {
        out_event->type = UI_ACTION_DOWNLOAD;
        return true;
    }
    if (kDown & KEY_SELECT) {
        out_event->type = UI_ACTION_TOGGLE_THEME;
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
        // Prev, Play/Pause y Next: botones circulares centrados en PREV_X, PLAY_X y NEXT_X
        if (x >= PREV_X - 20 && x <= PREV_X + 20 && y >= 165 && y <= 207) {
            out_event->type = UI_ACTION_PREV;
            return true;
        }

        if (x >= PLAY_X - 24 && x <= PLAY_X + 24 && y >= 162 && y <= 210) {
            out_event->type = UI_ACTION_PLAY_PAUSE;
            return true;
        }

        if (x >= NEXT_X - 20 && x <= NEXT_X + 20 && y >= 165 && y <= 207) {
            out_event->type = UI_ACTION_NEXT;
            return true;
        }

        // 5. Paginador
        if (y >= PAGER_Y - 14 && y <= PAGER_Y + 14) {
            if (x >= PAGE_PREV_X - 18 && x <= PAGE_PREV_X + 18) {
                out_event->type = UI_ACTION_PAGE_PREV;
                return true;
            }
            if (x >= PAGE_NEXT_X - 18 && x <= PAGE_NEXT_X + 18) {
                out_event->type = UI_ACTION_PAGE_NEXT;
                return true;
            }
        }

        // 6. Volver al menú (botón casa)
        if (x >= HOME_X - 16 && x <= HOME_X + 16 && y >= HOME_Y - 14 && y <= HOME_Y + 14) {
            out_event->type = UI_ACTION_BACK;
            return true;
        }

        // 7. Boton de Cambio de Tema (paleta): x: 12..52, y: 210..238
        if (x >= 12 && x <= 52 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_TOGGLE_THEME;
            return true;
        }

        // 8. Descargar la pista enfocada (solo SoundCloud)
        if (mode == MODE_SOUNDCLOUD && x >= DL_BTN_X - 2 && x <= DL_BTN_X + DL_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_DOWNLOAD;
            return true;
        }

        // 9. Minijuego en la pantalla superior (mando, junto a la paleta)
        if (x >= GAME_BTN_X - 2 && x <= GAME_BTN_X + GAME_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_TOGGLE_GAME;
            return true;
        }
    }

    s_was_touching = is_touching;
    return false;
}
