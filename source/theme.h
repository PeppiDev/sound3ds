#ifndef THEME_H
#define THEME_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

typedef enum {
    THEME_MODERN_GLOW = 0,
    THEME_Y2K_TUNES   = 1,
    THEME_COUNT       = 2
} ThemeId;

typedef struct {
    char name[32];
    char subtitle[32];

    // Colores de pantallas y contenedores
    u32 bg_top;
    u32 bg_bottom;
    u32 panel_bg;
    u32 panel_border;
    u32 header_bg;

    // Textos
    u32 text_primary;
    u32 text_secondary;
    u32 text_muted;

    // Acentos y controles
    u32 accent;
    u32 accent_active;
    u32 control_btn_bg;
    u32 control_btn_border;
    u32 control_icon;

    // Barras de progreso y volumen
    u32 progress_track;
    u32 progress_fill;
    u32 progress_knob;

    // Visualizador de frecuencias (FFT)
    u32 viz_low;
    u32 viz_mid;
    u32 viz_high;
    u32 viz_peak;
    // Mascota (cubo): cara frontal y cara superior iluminada
    u32 mascot_body;
    u32 mascot_light;

    bool is_segmented;     // Bloques tipo LED discretos (Winamp/Estéreo)
    bool is_lcd_box;        // Visor LCD retro fósforo verde
    bool has_metal_texture; // Textura de metal cepillado skeuomórfico
} UITheme;

// Inicializa el registro de temas predeterminados
void theme_init(void);

// Obtiene el tema activo actualmente
const UITheme* theme_get_current(void);

// Alterna al siguiente tema disponible
void theme_toggle_next(void);

// Selecciona un tema por su ID
void theme_set(ThemeId id);

// Devuelve el ID del tema actual
ThemeId theme_get_current_id(void);

#endif // THEME_H
