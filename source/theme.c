#include "theme.h"
#include <string.h>

static UITheme s_themes[THEME_COUNT];
static ThemeId s_current_theme_id = THEME_MODERN_GLOW;

void theme_init(void) {
    // ----------------------------------------------------
    // Tema 0: Modern Glow (Minimalista Oscuro / Neón)
    // ----------------------------------------------------
    strncpy(s_themes[THEME_MODERN_GLOW].name, "Modern Glow", 31);
    strncpy(s_themes[THEME_MODERN_GLOW].subtitle, "Dark Cyber Minimalist", 31);

    s_themes[THEME_MODERN_GLOW].bg_top             = C2D_Color32(13, 15, 24, 255);
    s_themes[THEME_MODERN_GLOW].bg_bottom          = C2D_Color32(18, 20, 32, 255);
    s_themes[THEME_MODERN_GLOW].panel_bg           = C2D_Color32(25, 28, 45, 255);
    s_themes[THEME_MODERN_GLOW].panel_border       = C2D_Color32(50, 58, 88, 255);
    s_themes[THEME_MODERN_GLOW].header_bg          = C2D_Color32(20, 23, 38, 255);

    s_themes[THEME_MODERN_GLOW].text_primary       = C2D_Color32(245, 247, 250, 255);
    s_themes[THEME_MODERN_GLOW].text_secondary     = C2D_Color32(160, 168, 190, 255);
    s_themes[THEME_MODERN_GLOW].text_muted         = C2D_Color32(95, 105, 130, 255);

    s_themes[THEME_MODERN_GLOW].accent             = C2D_Color32(168, 85, 247, 255); // Violeta neón
    s_themes[THEME_MODERN_GLOW].accent_active      = C2D_Color32(217, 70, 239, 255); // Fucsia neón
    s_themes[THEME_MODERN_GLOW].control_btn_bg     = C2D_Color32(32, 38, 62, 255);
    s_themes[THEME_MODERN_GLOW].control_btn_border = C2D_Color32(65, 75, 115, 255);
    s_themes[THEME_MODERN_GLOW].control_icon       = C2D_Color32(240, 245, 255, 255);

    s_themes[THEME_MODERN_GLOW].progress_track     = C2D_Color32(38, 44, 70, 255);
    s_themes[THEME_MODERN_GLOW].progress_fill      = C2D_Color32(6, 182, 212, 255);  // Cian brillante
    s_themes[THEME_MODERN_GLOW].progress_knob      = C2D_Color32(255, 255, 255, 255);

    s_themes[THEME_MODERN_GLOW].viz_low            = C2D_Color32(6, 182, 212, 255);  // Cian
    s_themes[THEME_MODERN_GLOW].viz_mid            = C2D_Color32(168, 85, 247, 255); // Violeta
    s_themes[THEME_MODERN_GLOW].viz_high           = C2D_Color32(244, 63, 94, 255);  // Rosa fuego
    s_themes[THEME_MODERN_GLOW].viz_peak           = C2D_Color32(255, 255, 255, 255);
    s_themes[THEME_MODERN_GLOW].mascot_body        = C2D_Color32(168, 85, 247, 255); // Morado
    s_themes[THEME_MODERN_GLOW].mascot_light       = C2D_Color32(221, 190, 252, 255);
    s_themes[THEME_MODERN_GLOW].is_segmented       = false;
    s_themes[THEME_MODERN_GLOW].is_lcd_box         = false;
    s_themes[THEME_MODERN_GLOW].has_metal_texture  = false;

    // ----------------------------------------------------
    // Tema 1: Y2K Tunes (iTunes 2000s / Winamp Metal)
    // ----------------------------------------------------
    strncpy(s_themes[THEME_Y2K_TUNES].name, "Y2K Classic Tunes", 31);
    strncpy(s_themes[THEME_Y2K_TUNES].subtitle, "2000s Brushed Metal & LCD", 31);

    s_themes[THEME_Y2K_TUNES].bg_top             = C2D_Color32(195, 202, 212, 255); // Aluminio
    s_themes[THEME_Y2K_TUNES].bg_bottom          = C2D_Color32(185, 192, 202, 255);
    s_themes[THEME_Y2K_TUNES].panel_bg           = C2D_Color32(220, 226, 235, 255);
    s_themes[THEME_Y2K_TUNES].panel_border       = C2D_Color32(140, 148, 160, 255);
    s_themes[THEME_Y2K_TUNES].header_bg          = C2D_Color32(160, 170, 185, 255);

    s_themes[THEME_Y2K_TUNES].text_primary       = C2D_Color32(20, 25, 35, 255);
    s_themes[THEME_Y2K_TUNES].text_secondary     = C2D_Color32(60, 70, 85, 255);
    s_themes[THEME_Y2K_TUNES].text_muted         = C2D_Color32(100, 110, 125, 255);

    s_themes[THEME_Y2K_TUNES].accent             = C2D_Color32(30, 115, 230, 255); // Azul Aqua Apple
    s_themes[THEME_Y2K_TUNES].accent_active      = C2D_Color32(20, 90, 190, 255);
    s_themes[THEME_Y2K_TUNES].control_btn_bg     = C2D_Color32(230, 235, 245, 255);
    s_themes[THEME_Y2K_TUNES].control_btn_border = C2D_Color32(120, 130, 145, 255);
    s_themes[THEME_Y2K_TUNES].control_icon       = C2D_Color32(40, 50, 70, 255);

    s_themes[THEME_Y2K_TUNES].progress_track     = C2D_Color32(150, 158, 170, 255);
    s_themes[THEME_Y2K_TUNES].progress_fill      = C2D_Color32(40, 130, 240, 255);
    s_themes[THEME_Y2K_TUNES].progress_knob      = C2D_Color32(245, 250, 255, 255);

    // Visualizador de LED clásico (Verde, Amarillo, Rojo)
    s_themes[THEME_Y2K_TUNES].viz_low            = C2D_Color32(34, 197, 94, 255);  // Verde LED
    s_themes[THEME_Y2K_TUNES].viz_mid            = C2D_Color32(234, 179, 8, 255);  // Amarillo ámbar
    s_themes[THEME_Y2K_TUNES].viz_high           = C2D_Color32(239, 68, 68, 255);  // Rojo pico
    s_themes[THEME_Y2K_TUNES].viz_peak           = C2D_Color32(255, 255, 180, 255);
    s_themes[THEME_Y2K_TUNES].mascot_body        = C2D_Color32(34, 160, 72, 255);  // Verde
    s_themes[THEME_Y2K_TUNES].mascot_light       = C2D_Color32(190, 236, 196, 255);
    s_themes[THEME_Y2K_TUNES].is_segmented       = true;
    s_themes[THEME_Y2K_TUNES].is_lcd_box         = true;
    s_themes[THEME_Y2K_TUNES].has_metal_texture  = true;
}

const UITheme* theme_get_current(void) {
    return &s_themes[s_current_theme_id];
}

void theme_toggle_next(void) {
    s_current_theme_id = (ThemeId)((s_current_theme_id + 1) % THEME_COUNT);
}

void theme_set(ThemeId id) {
    if (id < THEME_COUNT) {
        s_current_theme_id = id;
    }
}

ThemeId theme_get_current_id(void) {
    return s_current_theme_id;
}
