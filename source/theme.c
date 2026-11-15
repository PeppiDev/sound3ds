#include "theme.h"

// Color constante (mismo formato que C2D_Color32: ABGR)
#define RGB(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)

static const UITheme s_themes[THEME_COUNT] = {
    // Minimalista oscuro con neón
    [THEME_MODERN_GLOW] = {
        .name = "Modern Glow", .subtitle = "Dark Cyber Minimalist",
        .bg_top = RGB(13, 15, 24), .bg_bottom = RGB(18, 20, 32),
        .panel_bg = RGB(25, 28, 45), .panel_border = RGB(50, 58, 88), .header_bg = RGB(20, 23, 38),
        .text_primary = RGB(245, 247, 250), .text_secondary = RGB(160, 168, 190), .text_muted = RGB(95, 105, 130),
        .accent = RGB(168, 85, 247), .accent_active = RGB(217, 70, 239),
        .control_btn_bg = RGB(32, 38, 62), .control_btn_border = RGB(65, 75, 115), .control_icon = RGB(240, 245, 255),
        .progress_track = RGB(38, 44, 70), .progress_fill = RGB(6, 182, 212), .progress_knob = RGB(255, 255, 255),
        .viz_low = RGB(6, 182, 212), .viz_mid = RGB(168, 85, 247), .viz_high = RGB(244, 63, 94), .viz_peak = RGB(255, 255, 255),
        .mascot_body = RGB(168, 85, 247), .mascot_light = RGB(221, 190, 252),
    },
    // iTunes de los 2000 / Winamp: metal cepillado y LCD verde
    [THEME_Y2K_TUNES] = {
        .name = "Y2K Tunes", .subtitle = "2000s Brushed Metal & LCD",
        .bg_top = RGB(195, 202, 212), .bg_bottom = RGB(185, 192, 202),
        .panel_bg = RGB(220, 226, 235), .panel_border = RGB(140, 148, 160), .header_bg = RGB(160, 170, 185),
        .text_primary = RGB(20, 25, 35), .text_secondary = RGB(60, 70, 85), .text_muted = RGB(100, 110, 125),
        .accent = RGB(30, 115, 230), .accent_active = RGB(20, 90, 190),
        .control_btn_bg = RGB(230, 235, 245), .control_btn_border = RGB(120, 130, 145), .control_icon = RGB(40, 50, 70),
        .progress_track = RGB(150, 158, 170), .progress_fill = RGB(40, 130, 240), .progress_knob = RGB(245, 250, 255),
        .viz_low = RGB(34, 197, 94), .viz_mid = RGB(234, 179, 8), .viz_high = RGB(239, 68, 68), .viz_peak = RGB(255, 255, 180),
        .mascot_body = RGB(34, 160, 72), .mascot_light = RGB(190, 236, 196),
        .lcd_frame = RGB(100, 110, 120), .lcd_bg = RGB(20, 30, 18), .lcd_text = RGB(120, 255, 30), .lcd_text_dim = RGB(80, 200, 20),
        .is_segmented = true, .is_lcd_box = true, .has_metal_texture = true,
    },
    // Negro puro (bonito y sin brillo en la pantalla de noche)
    [THEME_MIDNIGHT] = {
        .name = "Midnight", .subtitle = "Pure Black",
        .bg_top = RGB(0, 0, 0), .bg_bottom = RGB(0, 0, 0),
        .panel_bg = RGB(16, 17, 22), .panel_border = RGB(40, 42, 54), .header_bg = RGB(10, 10, 13),
        .text_primary = RGB(236, 238, 244), .text_secondary = RGB(150, 155, 170), .text_muted = RGB(88, 92, 108),
        .accent = RGB(56, 189, 248), .accent_active = RGB(14, 165, 233),
        .control_btn_bg = RGB(24, 26, 34), .control_btn_border = RGB(52, 56, 72), .control_icon = RGB(230, 236, 245),
        .progress_track = RGB(30, 32, 42), .progress_fill = RGB(56, 189, 248), .progress_knob = RGB(255, 255, 255),
        .viz_low = RGB(56, 189, 248), .viz_mid = RGB(99, 102, 241), .viz_high = RGB(236, 72, 153), .viz_peak = RGB(255, 255, 255),
        .mascot_body = RGB(56, 189, 248), .mascot_light = RGB(186, 230, 253),
    },
    // Rosa claro
    [THEME_SAKURA] = {
        .name = "Sakura", .subtitle = "Soft Cherry Blossom",
        .bg_top = RGB(255, 238, 244), .bg_bottom = RGB(253, 230, 238),
        .panel_bg = RGB(255, 249, 251), .panel_border = RGB(240, 190, 208), .header_bg = RGB(249, 215, 228),
        .text_primary = RGB(82, 30, 52), .text_secondary = RGB(140, 80, 105), .text_muted = RGB(186, 135, 156),
        .accent = RGB(236, 72, 153), .accent_active = RGB(219, 39, 119),
        .control_btn_bg = RGB(255, 240, 246), .control_btn_border = RGB(240, 180, 200), .control_icon = RGB(130, 40, 82),
        .progress_track = RGB(245, 205, 220), .progress_fill = RGB(244, 114, 182), .progress_knob = RGB(255, 255, 255),
        .viz_low = RGB(249, 168, 212), .viz_mid = RGB(244, 114, 182), .viz_high = RGB(190, 24, 93), .viz_peak = RGB(157, 23, 77),
        .mascot_body = RGB(244, 114, 182), .mascot_light = RGB(253, 214, 229),
    },
    // Azul profundo y turquesa
    [THEME_OCEAN] = {
        .name = "Ocean", .subtitle = "Deep Blue & Teal",
        .bg_top = RGB(6, 26, 40), .bg_bottom = RGB(8, 32, 48),
        .panel_bg = RGB(12, 45, 66), .panel_border = RGB(30, 85, 115), .header_bg = RGB(8, 36, 54),
        .text_primary = RGB(230, 248, 255), .text_secondary = RGB(140, 190, 210), .text_muted = RGB(80, 125, 150),
        .accent = RGB(20, 184, 166), .accent_active = RGB(45, 212, 191),
        .control_btn_bg = RGB(14, 52, 76), .control_btn_border = RGB(35, 95, 128), .control_icon = RGB(220, 245, 255),
        .progress_track = RGB(20, 60, 85), .progress_fill = RGB(56, 189, 248), .progress_knob = RGB(255, 255, 255),
        .viz_low = RGB(45, 212, 191), .viz_mid = RGB(56, 189, 248), .viz_high = RGB(129, 140, 248), .viz_peak = RGB(255, 255, 255),
        .mascot_body = RGB(20, 184, 166), .mascot_light = RGB(153, 246, 228),
    },
    // Ochentas: morado, rosa y cian, con líneas de escaneo
    [THEME_SYNTHWAVE] = {
        .name = "Synthwave", .subtitle = "80s Neon Sunset",
        .bg_top = RGB(26, 10, 46), .bg_bottom = RGB(30, 12, 52),
        .panel_bg = RGB(45, 18, 75), .panel_border = RGB(110, 40, 140), .header_bg = RGB(36, 14, 60),
        .text_primary = RGB(255, 240, 250), .text_secondary = RGB(230, 150, 220), .text_muted = RGB(150, 90, 160),
        .accent = RGB(255, 41, 117), .accent_active = RGB(255, 110, 199),
        .control_btn_bg = RGB(55, 22, 90), .control_btn_border = RGB(130, 50, 160), .control_icon = RGB(255, 230, 250),
        .progress_track = RGB(60, 25, 95), .progress_fill = RGB(0, 229, 255), .progress_knob = RGB(255, 255, 255),
        .viz_low = RGB(0, 229, 255), .viz_mid = RGB(255, 41, 117), .viz_high = RGB(255, 190, 11), .viz_peak = RGB(255, 255, 255),
        .mascot_body = RGB(255, 41, 117), .mascot_light = RGB(255, 180, 220),
        .has_metal_texture = true,
    },
    // Consola de bolsillo: cuatro verdes y LCD
    [THEME_POCKET_LCD] = {
        .name = "Pocket LCD", .subtitle = "4-Shade Handheld",
        .bg_top = RGB(160, 190, 40), .bg_bottom = RGB(150, 180, 35),
        .panel_bg = RGB(139, 172, 15), .panel_border = RGB(48, 98, 48), .header_bg = RGB(125, 156, 20),
        .text_primary = RGB(15, 56, 15), .text_secondary = RGB(48, 98, 48), .text_muted = RGB(80, 122, 40),
        .accent = RGB(48, 98, 48), .accent_active = RGB(15, 56, 15),
        .control_btn_bg = RGB(139, 172, 15), .control_btn_border = RGB(48, 98, 48), .control_icon = RGB(15, 56, 15),
        .progress_track = RGB(120, 150, 20), .progress_fill = RGB(15, 56, 15), .progress_knob = RGB(48, 98, 48),
        .viz_low = RGB(48, 98, 48), .viz_mid = RGB(32, 78, 32), .viz_high = RGB(15, 56, 15), .viz_peak = RGB(15, 56, 15),
        .mascot_body = RGB(48, 98, 48), .mascot_light = RGB(139, 172, 15),
        .lcd_frame = RGB(48, 98, 48), .lcd_bg = RGB(139, 172, 15), .lcd_text = RGB(15, 56, 15), .lcd_text_dim = RGB(48, 98, 48),
        .is_segmented = true, .is_lcd_box = true,
    },
};

static ThemeId s_current_theme_id = THEME_MODERN_GLOW;

void theme_init(void) {}

const UITheme* theme_get_current(void) {
    return &s_themes[s_current_theme_id];
}

const UITheme* theme_get(ThemeId id) {
    return (unsigned)id < THEME_COUNT ? &s_themes[id] : NULL;
}

void theme_toggle_next(void) {
    s_current_theme_id = (ThemeId)((s_current_theme_id + 1) % THEME_COUNT);
}

void theme_set(ThemeId id) {
    if ((unsigned)id < THEME_COUNT) s_current_theme_id = id;
}

ThemeId theme_get_current_id(void) {
    return s_current_theme_id;
}
