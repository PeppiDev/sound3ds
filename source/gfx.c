#include "gfx.h"
#include "audio.h"
#include <stdio.h>
#include <string.h>

static struct {
    bool initialized;
    C3D_RenderTarget* topTarget;
    C3D_RenderTarget* bottomTarget;
    C2D_TextBuf text_buf;
    float marquee_offset;
    u64 last_frame_time;
} s_gfx;

bool gfx_init(void) {
    if (s_gfx.initialized) return true;

    memset(&s_gfx, 0, sizeof(s_gfx));

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    s_gfx.topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_gfx.bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    fontEnsureMapped();

    s_gfx.text_buf = C2D_TextBufNew(4096);
    if (!s_gfx.text_buf) {
        gfx_exit();
        return false;
    }

    s_gfx.initialized = true;
    return true;
}

void gfx_exit(void) {
    if (!s_gfx.initialized) return;

    if (s_gfx.text_buf) {
        C2D_TextBufDelete(s_gfx.text_buf);
        s_gfx.text_buf = NULL;
    }

    C2D_Fini();
    C3D_Fini();
    gfxExit();
    s_gfx.initialized = false;
}

void gfx_frame_begin(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_gfx.text_buf);
    s_gfx.marquee_offset += 0.6f;
    if (s_gfx.marquee_offset > 300.0f) {
        s_gfx.marquee_offset = -50.0f;
    }
}

static void draw_text(float x, float y, float scale, u32 color, const char* str) {
    if (!str || str[0] == '\0') return;
    C2D_Text text;
    C2D_TextParse(&text, s_gfx.text_buf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

// Copia en `out` la parte de `src` que cabe en max_w píxeles a la escala dada,
// terminando en "..." si se recorta. Mide con la tabla de anchos de la fuente del
// sistema, sin consumir glifos del buffer de texto.
static void fit_text(const char* src, float scale, float max_w, char* out, size_t out_sz) {
    if (!src || out_sz == 0) return;
    float dots_w = 3.0f * fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, '.'))->charWidth * scale;

    float width = 0.0f;
    size_t fit_len = 0;      // bytes que caben completos
    size_t fit_dots_len = 0; // bytes que caben dejando espacio para "..."
    const uint8_t* p = (const uint8_t*)src;
    while (*p) {
        uint32_t cp;
        ssize_t units = decode_utf8(&cp, p);
        if (units <= 0) break;
        width += fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, cp))->charWidth * scale;
        if (width > max_w) break;
        p += units;
        fit_len = (size_t)(p - (const uint8_t*)src);
        if (width + dots_w <= max_w) fit_dots_len = fit_len;
    }

    if (*p == '\0') {
        snprintf(out, out_sz, "%s", src);
    } else {
        size_t n = fit_dots_len < out_sz - 4 ? fit_dots_len : out_sz - 4;
        memcpy(out, src, n);
        memcpy(out + n, "...", 4);
    }
}

static void draw_text_fit(float x, float y, float scale, u32 color, const char* str, float max_w) {
    char buf[160];
    fit_text(str, scale, max_w, buf, sizeof(buf));
    draw_text(x, y, scale, color, buf);
}

static C3D_Tex s_artwork_tex;
static bool s_artwork_tex_loaded = false;

void gfx_clear_artwork(void) {
    if (s_artwork_tex_loaded) {
        C3D_TexDelete(&s_artwork_tex);
        s_artwork_tex_loaded = false;
    }
}

void gfx_set_artwork_rgba(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;

    gfx_clear_artwork();

    int tex_w = 128;
    int tex_h = 128;

    if (!C3D_TexInit(&s_artwork_tex, tex_w, tex_h, GPU_RGBA8)) {
        return;
    }

    uint32_t* dst = (uint32_t*)s_artwork_tex.data;
    memset(dst, 0, tex_w * tex_h * sizeof(uint32_t));

    static const uint8_t morton_table[8][8] = {
        { 0,  1,  4,  5, 16, 17, 20, 21 },
        { 2,  3,  6,  7, 18, 19, 22, 23 },
        { 8,  9, 12, 13, 24, 25, 28, 29 },
        {10, 11, 14, 15, 26, 27, 30, 31 },
        {32, 33, 36, 37, 48, 49, 52, 53 },
        {34, 35, 38, 39, 50, 51, 54, 55 },
        {40, 41, 44, 45, 56, 57, 60, 61 },
        {42, 43, 46, 47, 58, 59, 62, 63 }
    };

    int tiles_x = tex_w / 8;
    const uint32_t* src32 = (const uint32_t*)rgba;

    for (int y = 0; y < tex_h; y++) {
        int src_y = (y * h) / tex_h;
        for (int x = 0; x < tex_w; x++) {
            int src_x = (x * w) / tex_w;
            int tx = x / 8;
            int ty = y / 8;
            int tile_idx = ty * tiles_x + tx;
            int in_x = x % 8;
            int in_y = y % 8;
            int p_idx = tile_idx * 64 + morton_table[in_y][in_x];

            uint32_t col = src32[src_y * w + src_x];
            uint8_t r = (col) & 0xFF;
            uint8_t g = (col >> 8) & 0xFF;
            uint8_t b = (col >> 16) & 0xFF;
            uint8_t a = (col >> 24) & 0xFF;
            // GPU_RGBA8 espera R en el byte alto: 0xRRGGBBAA
            dst[p_idx] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
        }
    }

    C3D_TexFlush(&s_artwork_tex);
    s_artwork_tex_loaded = true;
}

// Dibuja la carátula o el disco de vinilo estilizado
static void draw_artwork(float x, float y, float size, const UITheme* theme) {
    C2D_DrawRectSolid(x - 2, y - 2, 0.5f, size + 4, size + 4, theme->panel_border);

    if (s_artwork_tex_loaded) {
        Tex3DS_SubTexture subtex = {
            .width = 128,
            .height = 128,
            .left = 0.0f,
            .top = 1.0f,
            .right = 1.0f,
            .bottom = 0.0f
        };
        C2D_Image img = { &s_artwork_tex, &subtex };
        C2D_DrawImageAt(img, x, y, 0.5f, NULL, size / 128.0f, size / 128.0f);
        return;
    }

    // Marco exterior con sombra/borde
    C2D_DrawRectSolid(x, y, 0.5f, size, size, C2D_Color32(18, 20, 26, 255));

    // Disco de vinilo interior con ranuras
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r = size * 0.44f;

    C2D_DrawCircleSolid(cx, cy, 0.5f, r, C2D_Color32(28, 30, 36, 255));
    C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.85f, C2D_Color32(20, 22, 28, 255));
    C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.70f, C2D_Color32(32, 34, 42, 255));
    C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.55f, C2D_Color32(24, 26, 32, 255));

    // Galleta central del vinilo con color de acento
    C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.35f, theme->accent);
    C2D_DrawCircleSolid(cx, cy, 0.5f, 4.0f, C2D_Color32(12, 14, 18, 255));
}

// Dibuja el visualizador de 16 bandas
static void draw_visualizer(float start_x, float base_y, float total_w, float max_h,
                            const uint8_t bands[16], const uint8_t peaks[16], const UITheme* theme) {
    float bar_w = (total_w / 16.0f) - 3.0f;
    if (bar_w < 4.0f) bar_w = 4.0f;

    for (int i = 0; i < 16; i++) {
        float bx = start_x + i * (bar_w + 3.0f);
        float norm = (float)bands[i] / 255.0f;
        float pnorm = (float)peaks[i] / 255.0f;

        if (theme->is_segmented) {
            // Estilo Y2K / Winamp: 10 bloques LED discretos apilados
            int total_leds = 10;
            int active_leds = (int)(norm * total_leds);
            int peak_led = (int)(pnorm * total_leds);
            float led_h = (max_h / total_leds) - 2.0f;

            for (int l = 0; l < total_leds; l++) {
                float ly = base_y - (l + 1) * (led_h + 2.0f);
                u32 led_color;

                if (l < 5) {
                    led_color = theme->viz_low;   // Verde base
                } else if (l < 8) {
                    led_color = theme->viz_mid;   // Amarillo medio
                } else {
                    led_color = theme->viz_high;  // Rojo pico
                }

                if (l <= active_leds || l == peak_led) {
                    C2D_DrawRectSolid(bx, ly, 0.5f, bar_w, led_h, led_color);
                } else {
                    // LED apagado (sombra tenue)
                    C2D_DrawRectSolid(bx, ly, 0.5f, bar_w, led_h, C2D_Color32(40, 48, 45, 120));
                }
            }
        } else {
            // Estilo Moderno: Barras continuas con gradiente
            float bar_h = norm * max_h;
            if (bar_h < 3.0f) bar_h = 3.0f;
            float by = base_y - bar_h;

            u32 bar_color = (i < 5) ? theme->viz_low : ((i < 11) ? theme->viz_mid : theme->viz_high);
            C2D_DrawRectSolid(bx, by, 0.5f, bar_w, bar_h, bar_color);

            // Pico flotante (Peak Cap)
            float peak_y = base_y - (pnorm * max_h) - 2.0f;
            if (peak_y < base_y - max_h) peak_y = base_y - max_h;
            C2D_DrawRectSolid(bx, peak_y, 0.5f, bar_w, 2.0f, theme->viz_peak);
        }
    }
}

void gfx_render_top(const PlaybackViewModel* playback, const uint8_t bands[16], const uint8_t peaks[16]) {
    const UITheme* theme = theme_get_current();

    C2D_TargetClear(s_gfx.topTarget, theme->bg_top);
    C2D_SceneBegin(s_gfx.topTarget);

    // Textura sutil de metal cepillado si el tema lo requiere
    if (theme->has_metal_texture) {
        for (int y = 0; y < SCREEN_TOP_H; y += 4) {
            C2D_DrawRectSolid(0, y, 0.1f, SCREEN_TOP_W, 1, C2D_Color32(255, 255, 255, 20));
        }
    }

    // Barra de encabezado
    C2D_DrawRectSolid(0, 0, 0.5f, SCREEN_TOP_W, 22, theme->header_bg);
    draw_text(12, 4, 0.45f, theme->text_primary, "Sound3DS");
    draw_text(110, 4, 0.40f, theme->accent, theme->name);
    draw_text(250, 4, 0.40f, theme->text_muted, audio_get_backend_name());

    if (!audio_is_initialized()) {
        draw_text(300, 4, 0.40f, C2D_Color32(239, 68, 68, 255), "AUD ERR");
        char audio_error[80];
        snprintf(audio_error, sizeof(audio_error), "Audio init: %s", audio_get_init_error());
        draw_text(12, 23, 0.32f, C2D_Color32(239, 68, 68, 255), audio_error);
    } else if (playback->is_buffering) {
        char buf_msg[32];
        snprintf(buf_msg, sizeof(buf_msg), "Buffer %d%%", playback->buffer_percent);
        draw_text(300, 4, 0.40f, C2D_Color32(255, 200, 0, 255), buf_msg);
    } else if (playback->stream_error) {
        draw_text(300, 4, 0.40f, C2D_Color32(239, 68, 68, 255), "SIN STREAM");
    } else if (playback->is_playing) {
        draw_text(320, 4, 0.40f, C2D_Color32(34, 197, 94, 255), "ONLINE");
    } else {
        draw_text(330, 4, 0.40f, theme->text_muted, "PAUSA");
    }

    // 1. Caratula del album (100x100)
    draw_artwork(16, 32, 100, theme);
    if (playback->artwork_loading) {
        draw_text(36, 120, 0.35f, theme->text_muted, "Cargando...");
    }

    // 2. Visor de Metadatos
    float meta_x = 126;
    float meta_y = 32;
    float meta_w = SCREEN_TOP_W - meta_x - 14;

    if (theme->is_lcd_box) {
        // Caja LCD estilo retro
        C2D_DrawRectSolid(meta_x - 2, meta_y - 2, 0.5f, meta_w + 4, 64, C2D_Color32(100, 110, 120, 255));
        C2D_DrawRectSolid(meta_x, meta_y, 0.5f, meta_w, 60, C2D_Color32(20, 30, 18, 255));
    } else {
        // Panel moderno
        C2D_DrawRectSolid(meta_x, meta_y, 0.5f, meta_w, 60, theme->panel_bg);
        C2D_DrawRectSolid(meta_x, meta_y, 0.5f, meta_w, 1, theme->panel_border);
    }

    // Titulo de la cancion
    u32 title_color = theme->is_lcd_box ? C2D_Color32(120, 255, 30, 255) : theme->text_primary;
    u32 artist_color = theme->is_lcd_box ? C2D_Color32(80, 200, 20, 255) : theme->text_secondary;

    draw_text_fit(meta_x + 8, meta_y + 8, 0.55f, title_color, playback->title, meta_w - 16);
    draw_text_fit(meta_x + 8, meta_y + 26, 0.45f, artist_color, playback->artist, meta_w - 16);

    // Tiempos (02:14 / 03:45)
    char time_str[32];
    uint32_t cur_s = playback->elapsed_ms / 1000;
    uint32_t tot_s = playback->duration_ms / 1000;
    snprintf(time_str, sizeof(time_str), "%02lu:%02lu / %02lu:%02lu",
             (unsigned long)(cur_s / 60), (unsigned long)(cur_s % 60),
             (unsigned long)(tot_s / 60), (unsigned long)(tot_s % 60));
    draw_text(meta_x + 8, meta_y + 42, 0.40f, theme->is_lcd_box ? title_color : theme->accent, time_str);

    // 3. Visualizador FFT en la seccion inferior de la pantalla superior
    float viz_y = 228.0f;
    float viz_w = SCREEN_TOP_W - 32.0f;
    float viz_h = 95.0f;

    draw_visualizer(16.0f, viz_y, viz_w, viz_h, bands, peaks, theme);
}

void gfx_render_bottom(const PlaybackViewModel* playback, const SearchViewModel* search,
                       bool touch_down, int touch_x, int touch_y) {
    const UITheme* theme = theme_get_current();

    C2D_TargetClear(s_gfx.bottomTarget, theme->bg_bottom);
    C2D_SceneBegin(s_gfx.bottomTarget);

    // 1. Barra de busqueda (Top del Bottom Screen)
    C2D_DrawRectSolid(10, 8, 0.5f, SCREEN_BOT_W - 20, 28, theme->panel_bg);
    C2D_DrawRectSolid(10, 8, 0.5f, SCREEN_BOT_W - 20, 1, theme->panel_border);
    C2D_DrawRectSolid(10, 35, 0.5f, SCREEN_BOT_W - 20, 1, theme->panel_border);

    // Icono lupa o etiqueta
    draw_text(18, 14, 0.42f, theme->accent, "[LUPA]");
    if (search->is_searching) {
        draw_text(58, 14, 0.45f, theme->accent, "Buscando...");
    } else if (search->query[0] != '\0') {
        draw_text_fit(58, 14, 0.45f, theme->text_primary, search->query, SCREEN_BOT_W - 78);
    } else {
        draw_text(58, 14, 0.42f, theme->text_muted, "Toca para buscar en SoundCloud...");
    }

    // 2. Lista de canciones / Resultados (5 filas de 19 px)
    for (int i = 0; i < LIST_ROWS; i++) {
        float iy = LIST_Y + i * LIST_ROW_STEP;
        bool is_sel = (i == search->selected_index);

        C2D_DrawRectSolid(10, iy, 0.5f, SCREEN_BOT_W - 20, LIST_ROW_H, is_sel ? theme->accent : theme->panel_bg);

        u32 item_text_clr = is_sel ? C2D_Color32(255, 255, 255, 255) : theme->text_primary;
        u32 item_art_clr  = is_sel ? C2D_Color32(240, 240, 240, 255) : theme->text_muted;

        if (i < search->total_results) {
            char num_label[8];
            snprintf(num_label, sizeof(num_label), "%d.", i + 1);
            draw_text(16, iy + 3, 0.40f, item_text_clr, num_label);
            // Título en x 30..194 y artista en x 200..306: el recorte por ancho evita que se encimen
            draw_text_fit(30, iy + 3, 0.40f, item_text_clr, search->result_titles[i], 164);
            draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 106);
        } else {
            draw_text(16, iy + 3, 0.40f, theme->text_muted, "-");
        }
    }

    // 3. Barra de progreso (Scrubber)
    float scrub_y = 152;
    float scrub_w = SCREEN_BOT_W - 40;
    C2D_DrawRectSolid(20, scrub_y, 0.5f, scrub_w, 6, theme->progress_track);

    float progress_ratio = 0.0f;
    if (playback->duration_ms > 0) {
        progress_ratio = (float)playback->elapsed_ms / (float)playback->duration_ms;
        if (progress_ratio > 1.0f) progress_ratio = 1.0f;
    }
    float fill_w = scrub_w * progress_ratio;
    C2D_DrawRectSolid(20, scrub_y, 0.5f, fill_w, 6, theme->progress_fill);
    // Puntero / Knob circular
    C2D_DrawCircleSolid(20 + fill_w, scrub_y + 3, 0.5f, 6.0f, theme->progress_knob);

    // 4. Botones de Control de Transporte
    // Prev: (70, 168)
    bool prev_active = touch_down && (touch_x >= 60 && touch_x <= 100 && touch_y >= 165 && touch_y <= 205);
    C2D_DrawRectSolid(60, 168, 0.5f, 40, 36, prev_active ? theme->accent_active : theme->control_btn_bg);
    C2D_DrawRectSolid(60, 168, 0.5f, 40, 1, theme->control_btn_border);
    draw_text(71, 178, 0.50f, theme->control_icon, "|<<");

    // Play/Pause: (135, 164)
    bool play_active = touch_down && (touch_x >= 135 && touch_x <= 185 && touch_y >= 162 && touch_y <= 210);
    C2D_DrawRectSolid(135, 164, 0.5f, 50, 44, play_active ? theme->accent_active : theme->accent);
    if (playback->is_playing) {
        draw_text(150, 177, 0.60f, C2D_Color32(255, 255, 255, 255), "||");
    } else {
        draw_text(152, 177, 0.60f, C2D_Color32(255, 255, 255, 255), ">");
    }

    // Next: (210, 168)
    bool next_active = touch_down && (touch_x >= 210 && touch_x <= 250 && touch_y >= 165 && touch_y <= 205);
    C2D_DrawRectSolid(210, 168, 0.5f, 40, 36, next_active ? theme->accent_active : theme->control_btn_bg);
    C2D_DrawRectSolid(210, 168, 0.5f, 40, 1, theme->control_btn_border);
    draw_text(220, 178, 0.50f, theme->control_icon, ">>|");

    // 5. Boton de Cambio de Tema y Volumen
    // Boton Tema: (14, 212)
    bool theme_active = touch_down && (touch_x >= 14 && touch_x <= 110 && touch_y >= 210 && touch_y <= 236);
    C2D_DrawRectSolid(14, 212, 0.5f, 96, 22, theme_active ? theme->accent_active : theme->control_btn_bg);
    C2D_DrawRectSolid(14, 212, 0.5f, 96, 1, theme->control_btn_border);
    draw_text(20, 216, 0.38f, theme->text_primary, "TEMA: ");
    draw_text(60, 216, 0.38f, theme->accent, (theme == theme_get_current() && theme_get_current_id() == THEME_MODERN_GLOW) ? "MODERN" : "Y2K");

    // Barra de volumen: (180, 216)
    draw_text(150, 216, 0.40f, theme->text_secondary, "VOL");
    C2D_DrawRectSolid(184, 220, 0.5f, 116, 8, theme->progress_track);
    float vol_w = 116.0f * playback->volume;
    C2D_DrawRectSolid(184, 220, 0.5f, vol_w, 8, theme->accent);
}

void gfx_frame_end(void) {
    C3D_FrameEnd(0);
}
