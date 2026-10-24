#include "ui.h"
#include "gfx.h"
#include <string.h>
#include <stdio.h>
#include "i18n.h"

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
    swkbdSetHintText(&swkbd, L(hint));
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
    if (kDown & KEY_DLEFT)  { out_event->type = UI_ACTION_PAGE_PREV; return true; }
    if (kDown & KEY_DRIGHT) { out_event->type = UI_ACTION_PAGE_NEXT; return true; }
    if (kDown & KEY_A)      { out_event->type = UI_ACTION_CONFIRM; return true; }
    if (kDown & KEY_SELECT) { out_event->type = UI_ACTION_TOGGLE_THEME; return true; }

    if (kDown & KEY_TOUCH) {
        int x = touch.px, y = touch.py;
        for (int i = 0; i < MENU_ITEMS; i++) {
            int cx = MENU_CARD_X(i), cy = MENU_CARD_Y(i);
            if (x >= cx && x <= cx + MENU_CARD_W && y >= cy && y <= cy + MENU_CARD_H) {
                out_event->type = UI_ACTION_MENU_SELECT;
                out_event->track_index = i;
                return true;
            }
        }
        if (x >= THEME_BTN_X - 2 && x <= THEME_BTN_X + BAR_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_TOGGLE_THEME;
            return true;
        }
        if (x >= CFG_MENU_BTN_X - 2 && x <= CFG_MENU_BTN_X + BAR_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_OPEN_SETTINGS;
            return true;
        }
        if (x >= NOWPLAY_X && x <= SCREEN_BOT_W - 10 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_OPEN_NOW_PLAYING;
            return true;
        }
    }
    return false;
}

bool ui_update(u32 kDown, u32 kHeld, touchPosition touch, AppMode mode, bool game_on, const SearchViewModel* view,
               UIEvent* out_event) {
    if (!out_event) return false;
    memset(out_event, 0, sizeof(UIEvent));
    if (mode == MODE_MENU) return ui_update_menu(kDown, touch, out_event);

    if (kDown & KEY_B) {
        out_event->type = UI_ACTION_BACK;
        return true;
    }
    // ZL / ZR (New 3DS y New 2DS XL): pestañas de la cuenta
    if (view && view->show_tabs && (kDown & (KEY_ZL | KEY_ZR))) {
        out_event->type = (kDown & KEY_ZL) ? UI_ACTION_TAB_PREV : UI_ACTION_TAB_NEXT;
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
    if ((kDown & KEY_X) && !game_on) {
        out_event->type = UI_ACTION_FAVORITE;
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

        // 1. Cabecera: playlist abierta (volver / descargar todo) o barra de búsqueda (+ selector)
        bool in_header = y >= SEARCH_BAR_Y && y <= SEARCH_BAR_Y + SEARCH_BAR_H;
        if (view && view->show_tabs && y >= TABS_Y - 2 && y <= TABS_Y + TABS_H + 2) {
            for (int k = 0; k < TAB_COUNT; k++)
                if (x >= TAB_X(k) && x <= TAB_X(k) + TAB_W) {
                    out_event->type = UI_ACTION_TAB;
                    out_event->track_index = k;
                    return true;
                }
        }
        if (in_header && view && view->title_header) {
            // Título de Likes / Playlists / Feed: volver a cargar la lista
            if (x >= SEARCH_BAR_X && x <= 310) {
                out_event->type = UI_ACTION_SEARCH_REQUEST;
                return true;
            }
        } else if (in_header && view && view->playlist_header) {
            if (x >= SEARCH_BAR_X && x <= SEARCH_BAR_X + BACK_CHIP_W) {
                out_event->type = UI_ACTION_BACK;
                return true;
            }
            if (view->show_dl_all && x >= DLALL_X && x <= DLALL_X + DLALL_W) {
                out_event->type = UI_ACTION_DOWNLOAD_ALL;
                return true;
            }
        } else if (in_header) {
            bool selector = view && view->show_selector;
            int bar_end = selector ? SEARCH_BAR_X + SEARCH_BAR_SEL_W : 310;
            if (x >= SEARCH_BAR_X && x <= bar_end) {
                out_event->type = UI_ACTION_SEARCH_REQUEST;
                return true;
            }
            if (selector && x >= KIND_BTN_X0 - 2 && x <= KIND_BTN_X0 + KIND_BTN_W + 2) {
                out_event->type = UI_ACTION_SEARCH_KIND;
                out_event->track_index = 0;
                return true;
            }
            if (selector && x >= KIND_BTN_X1 - 2 && x <= KIND_BTN_X1 + KIND_BTN_W + 2) {
                out_event->type = UI_ACTION_SEARCH_KIND;
                out_event->track_index = 1;
                return true;
            }
        }

        // 2. Elementos de la lista de resultados
        for (int i = 0; i < LIST_ROWS; i++) {
            int iy = gfx_list_y(view) + i * gfx_list_step(view);
            if (x >= 10 && x <= 310 && y >= iy && y <= (iy + gfx_list_row_h(view))) {
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
        // Aleatorio y repetir, a los lados
        if (y >= 168 && y <= 204 && x >= SHUFFLE_X - 18 && x <= SHUFFLE_X + 18) {
            out_event->type = UI_ACTION_SHUFFLE;
            return true;
        }
        if (y >= 168 && y <= 204 && x >= REPEAT_X - 18 && x <= REPEAT_X + 18) {
            out_event->type = UI_ACTION_REPEAT;
            return true;
        }
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
        if (x >= THEME_BTN_X - 2 && x <= THEME_BTN_X + BAR_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_TOGGLE_THEME;
            return true;
        }

        // 7b. Configuración (engranaje)
        if (x >= STAR_BTN_X - 2 && x <= STAR_BTN_X + BAR_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_FAVORITE;
            return true;
        }
        if (x >= CFG_BTN_X - 2 && x <= CFG_BTN_X + BAR_BTN_W + 2 && y >= 210 && y <= 238) {
            out_event->type = UI_ACTION_OPEN_SETTINGS;
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

bool ui_update_dialog(u32 kDown, touchPosition touch, UIEvent* out_event) {
    memset(out_event, 0, sizeof(UIEvent));
    if (kDown & KEY_A) { out_event->type = UI_ACTION_DIALOG_YES; return true; }
    if (kDown & KEY_B) { out_event->type = UI_ACTION_DIALOG_NO; return true; }
    if (kDown & KEY_TOUCH) {
        int x = touch.px, y = touch.py;
        if (y >= DIALOG_BTN_Y && y <= DIALOG_BTN_Y + DIALOG_BTN_H) {
            if (x >= DIALOG_YES_X && x <= DIALOG_YES_X + DIALOG_BTN_W) { out_event->type = UI_ACTION_DIALOG_YES; return true; }
            if (x >= DIALOG_NO_X && x <= DIALOG_NO_X + DIALOG_BTN_W) { out_event->type = UI_ACTION_DIALOG_NO; return true; }
        }
    }
    return false;
}

bool ui_update_scan(u32 kDown, touchPosition touch, bool done, UIEvent* out_event) {
    memset(out_event, 0, sizeof(UIEvent));
    if (kDown & KEY_B) { out_event->type = UI_ACTION_SCAN_CLOSE; return true; }
    if (done && (kDown & KEY_A)) { out_event->type = UI_ACTION_SCAN_AGAIN; return true; }
    if (kDown & KEY_TOUCH) {
        int x = touch.px, y = touch.py;
        if (y >= SCAN_BTN_Y - 3 && y <= SCAN_BTN_Y + SCAN_BTN_H + 3) {
            if (x >= SCAN_BACK_X && x <= SCAN_BACK_X + SCAN_BACK_W) { out_event->type = UI_ACTION_SCAN_CLOSE; return true; }
            if (done && x >= SCAN_AGAIN_X && x <= SCAN_AGAIN_X + SCAN_AGAIN_W) {
                out_event->type = UI_ACTION_SCAN_AGAIN;
                return true;
            }
        }
    }
    return false;
}

bool ui_update_settings(u32 kDown, touchPosition touch, UIEvent* out_event) {
    memset(out_event, 0, sizeof(UIEvent));
    out_event->track_index = -1;
    if (kDown & (KEY_B | KEY_START)) { out_event->type = UI_ACTION_SETTINGS_CLOSE; return true; }
    if (kDown & KEY_DUP)   { out_event->type = UI_ACTION_CURSOR_UP; return true; }
    if (kDown & KEY_DDOWN) { out_event->type = UI_ACTION_CURSOR_DOWN; return true; }
    if (kDown & (KEY_A | KEY_DLEFT | KEY_DRIGHT)) {
        out_event->type = UI_ACTION_SETTINGS_TOGGLE;
        out_event->option = (kDown & KEY_DLEFT) ? -1 : 0;
        return true;
    }
    if (kDown & KEY_TOUCH) {
        if (touch.px >= SETTINGS_CLOSE_X && touch.px <= SETTINGS_CLOSE_X + SETTINGS_CLOSE_W && touch.py <= 31) {
            out_event->type = UI_ACTION_SETTINGS_CLOSE;
            return true;
        }
        int row = gfx_settings_row_at(touch.px, touch.py);
        if (row >= 0) {
            out_event->type = UI_ACTION_SETTINGS_TOGGLE;
            out_event->track_index = row;
            int k = gfx_settings_option_at(row, touch.px, touch.py);
            if (k >= 0) out_event->option = k + 1;
            return true;
        }
    }
    return false;
}
