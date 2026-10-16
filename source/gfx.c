#include "gfx.h"
#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static struct {
    bool initialized;
    C3D_RenderTarget* topTarget;
    C3D_RenderTarget* bottomTarget;
    C2D_TextBuf text_buf;
    float marquee_offset;
    u64 last_frame_time;

    // Batería: se consulta una vez por segundo para no saturar los servicios
    bool ptmu_ok;
    bool mcu_ok;
    u8 battery_percent;
    bool battery_charging;
    u64 battery_checked;
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

    // ptm:u da el nivel en 5 pasos; mcu::HWC (si el sistema lo permite) da el porcentaje exacto
    s_gfx.ptmu_ok = R_SUCCEEDED(ptmuInit());
    s_gfx.mcu_ok = R_SUCCEEDED(mcuHwcInit());

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

    if (s_gfx.mcu_ok) mcuHwcExit();
    if (s_gfx.ptmu_ok) ptmuExit();

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

static void draw_text_centered(float cx, float y, float scale, u32 color, const char* str) {
    if (!str || str[0] == '\0') return;
    C2D_Text text;
    C2D_TextParse(&text, s_gfx.text_buf, str);
    C2D_TextOptimize(&text);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&text, scale, scale, &w, &h);
    C2D_DrawText(&text, C2D_WithColor, cx - w / 2, y, 0.5f, scale, scale, color);
}

static void draw_text_fit(float x, float y, float scale, u32 color, const char* str, float max_w) {
    char buf[160];
    fit_text(str, scale, max_w, buf, sizeof(buf));
    draw_text(x, y, scale, color, buf);
}

// Colores en formato Citro2D (0xAABBGGRR)
static u32 with_alpha(u32 c, u8 a) {
    return (c & 0x00FFFFFF) | ((u32)a << 24);
}

static u32 lerp_color(u32 a, u32 b, float t) {
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= ((u32)(ca + (cb - ca) * t) & 0xFF) << sh;
    }
    return out;
}

static u32 shade(u32 c, float k) {
    return lerp_color(c, c & 0xFF000000, 1.0f - k);
}

// Círculo y elipse rellenos como abanico de triángulos. Se evita C2D_DrawCircleSolid
// porque Azahar no lo dibuja; así se ve igual en el emulador y en la consola.
static void fill_ellipse(float cx, float cy, float rx, float ry, float z, u32 color) {
    float r = rx > ry ? rx : ry;
    int segs = r < 4 ? 12 : (r < 12 ? 20 : 32);
    float px = cx + rx, py = cy;
    for (int i = 1; i <= segs; i++) {
        float a = 6.2832f * i / segs;
        float nx = cx + cosf(a) * rx, ny = cy + sinf(a) * ry;
        C2D_DrawTriangle(cx, cy, color, px, py, color, nx, ny, color, z);
        px = nx;
        py = ny;
    }
}

static void fill_circle(float cx, float cy, float z, float r, u32 color) {
    fill_ellipse(cx, cy, r, r, z, color);
}

// ---------------------------------------------------------------------------
// Íconos vectoriales dibujados con primitivas de Citro2D
// ---------------------------------------------------------------------------

#define ICON_Z 0.6f

static void draw_line(float x0, float y0, float x1, float y1, float thick, u32 color) {
    C2D_DrawLine(x0, y0, color, x1, y1, color, thick, ICON_Z);
}

static void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 color) {
    C2D_DrawTriangle(x0, y0, color, x1, y1, color, x2, y2, color, ICON_Z);
}

// Arco de circunferencia entre a0 y a1 (radianes), como tramos de línea
static void draw_arc(float cx, float cy, float r, float a0, float a1, float thick, u32 color) {
    const int segs = 14;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= segs; i++) {
        float a = a0 + (a1 - a0) * i / segs;
        float nx = cx + cosf(a) * r, ny = cy + sinf(a) * r;
        draw_line(px, py, nx, ny, thick, color);
        px = nx;
        py = ny;
    }
}

static void draw_ring(float cx, float cy, float r, float thick, u32 color) {
    draw_arc(cx, cy, r, 0.0f, 6.2832f, thick, color);
}

static void fill_round_rect(float x, float y, float w, float h, float r, float z, u32 color) {
    C2D_DrawRectSolid(x + r, y, z, w - 2 * r, h, color);
    C2D_DrawRectSolid(x, y + r, z, r, h - 2 * r, color);
    C2D_DrawRectSolid(x + w - r, y + r, z, r, h - 2 * r, color);
    fill_circle(x + r, y + r, z, r, color);
    fill_circle(x + w - r, y + r, z, r, color);
    fill_circle(x + r, y + h - r, z, r, color);
    fill_circle(x + w - r, y + h - r, z, r, color);
}

static void icon_play(float cx, float cy, float size, u32 color) {
    // Triángulo desplazado a la derecha para que se vea centrado ópticamente
    float h = size, w = size * 0.9f, ox = size * 0.12f;
    draw_tri(cx - w / 2 + ox, cy - h / 2, cx - w / 2 + ox, cy + h / 2, cx + w / 2 + ox, cy, color);
}

static void icon_pause(float cx, float cy, float size, u32 color) {
    float bw = size * 0.3f, gap = size * 0.22f;
    fill_round_rect(cx - gap / 2 - bw, cy - size / 2, bw, size, bw * 0.3f, ICON_Z, color);
    fill_round_rect(cx + gap / 2, cy - size / 2, bw, size, bw * 0.3f, ICON_Z, color);
}

// Anterior / siguiente: dos triángulos y una barra en el extremo hacia el que apuntan
static void icon_skip(float cx, float cy, float size, bool next, u32 color) {
    float h = size * 0.8f, w = size * 0.42f, bar = size * 0.14f;
    float left = cx - (2 * w + bar) / 2, top = cy - h / 2, bottom = cy + h / 2;
    if (next) {
        draw_tri(left, top, left, bottom, left + w, cy, color);
        draw_tri(left + w, top, left + w, bottom, left + 2 * w, cy, color);
        C2D_DrawRectSolid(left + 2 * w, top, ICON_Z, bar, h, color);
    } else {
        C2D_DrawRectSolid(left, top, ICON_Z, bar, h, color);
        draw_tri(left + bar + w, top, left + bar + w, bottom, left + bar, cy, color);
        draw_tri(left + bar + 2 * w, top, left + bar + 2 * w, bottom, left + bar + w, cy, color);
    }
}

// Lupa: anillo con mango diagonal
static void icon_search(float cx, float cy, float size, u32 color) {
    float r = size * 0.32f;
    float lx = cx - size * 0.08f, ly = cy - size * 0.08f;
    draw_ring(lx, ly, r, size * 0.13f, color);
    float a = 0.785f; // 45°
    draw_line(lx + cosf(a) * r, ly + sinf(a) * r, cx + size * 0.45f, cy + size * 0.45f, size * 0.17f, color);
}

// Flecha de paginador: "<" o ">" de dos trazos
static void icon_chevron(float cx, float cy, float size, bool right, u32 color) {
    float d = right ? 1.0f : -1.0f, w = size * 0.28f, h = size * 0.5f;
    draw_line(cx - d * w, cy - h, cx + d * w, cy, size * 0.22f, color);
    draw_line(cx + d * w, cy, cx - d * w, cy + h, size * 0.22f, color);
}

// Paleta de pintor: óvalo claro con agujero para el pulgar y cuatro gotas de color
static void icon_palette(float cx, float cy, float size, u32 hole_color) {
    u32 wood = C2D_Color32(236, 216, 186, 255);
    fill_ellipse(cx, cy, size * 0.5f, size * 0.4f, ICON_Z, wood);
    fill_circle(cx + size * 0.2f, cy + size * 0.14f, ICON_Z + 0.01f, size * 0.11f, hole_color);
    const u32 dots[4] = {
        C2D_Color32(239, 68, 68, 255), C2D_Color32(250, 204, 21, 255),
        C2D_Color32(34, 197, 94, 255), C2D_Color32(59, 130, 246, 255)
    };
    const float pos[4][2] = { {-0.24f, 0.08f}, {-0.16f, -0.18f}, {0.06f, -0.22f}, {0.26f, -0.1f} };
    for (int i = 0; i < 4; i++) {
        fill_circle(cx + pos[i][0] * size, cy + pos[i][1] * size, ICON_Z + 0.01f, size * 0.09f, dots[i]);
    }
}

static void update_battery(void) {
    u64 now = osGetTime();
    if (s_gfx.battery_checked && now - s_gfx.battery_checked < 1000) return;
    s_gfx.battery_checked = now;

    u8 level = 0;
    if (s_gfx.mcu_ok && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level))) {
        s_gfx.battery_percent = level > 100 ? 100 : level;
    } else if (s_gfx.ptmu_ok && R_SUCCEEDED(PTMU_GetBatteryLevel(&level))) {
        s_gfx.battery_percent = level * 20; // 0..5
    }
    u8 charging = 0;
    if (s_gfx.ptmu_ok && R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging))) {
        s_gfx.battery_charging = charging != 0;
    }
}

// Pila horizontal con nivel de carga; rayo cuando está cargando
static void icon_battery(float x, float y, u32 outline) {
    update_battery();
    const float w = 22, h = 11;
    // Contorno
    C2D_DrawRectSolid(x, y, ICON_Z, w, 1.5f, outline);
    C2D_DrawRectSolid(x, y + h - 1.5f, ICON_Z, w, 1.5f, outline);
    C2D_DrawRectSolid(x, y, ICON_Z, 1.5f, h, outline);
    C2D_DrawRectSolid(x + w - 1.5f, y, ICON_Z, 1.5f, h, outline);
    C2D_DrawRectSolid(x + w, y + 3, ICON_Z, 2, h - 6, outline); // borne

    int pct = s_gfx.battery_percent;
    u32 fill = s_gfx.battery_charging ? C2D_Color32(34, 197, 94, 255)
             : (pct <= 20 ? C2D_Color32(239, 68, 68, 255) : outline);
    float inner = (w - 5) * pct / 100.0f;
    if (pct > 0 && inner < 2) inner = 2;
    C2D_DrawRectSolid(x + 2.5f, y + 2.5f, ICON_Z, inner, h - 5, fill);

    if (s_gfx.battery_charging) {
        u32 bolt = C2D_Color32(255, 255, 255, 255);
        float cx = x + w / 2, cy = y + h / 2;
        draw_tri(cx + 1, cy - 5, cx - 3, cy + 1, cx, cy + 1, bolt);
        draw_tri(cx - 1, cy + 5, cx + 3, cy - 1, cx, cy - 1, bolt);
    }
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

    fill_circle(cx, cy, 0.5f, r, C2D_Color32(28, 30, 36, 255));
    fill_circle(cx, cy, 0.5f, r * 0.85f, C2D_Color32(20, 22, 28, 255));
    fill_circle(cx, cy, 0.5f, r * 0.70f, C2D_Color32(32, 34, 42, 255));
    fill_circle(cx, cy, 0.5f, r * 0.55f, C2D_Color32(24, 26, 32, 255));

    // Galleta central del vinilo con color de acento
    fill_circle(cx, cy, 0.5f, r * 0.35f, theme->accent);
    fill_circle(cx, cy, 0.5f, 4.0f, C2D_Color32(12, 14, 18, 255));
}

// Dibuja el visualizador de 16 bandas
static void draw_visualizer(float start_x, float base_y, float total_w, float max_h,
                            const uint8_t bands[16], const uint8_t peaks[16], const UITheme* theme) {
    const float gap = 5.0f;
    float slot = total_w / 16.0f;
    float bar_w = slot - gap;

    if (theme->is_segmented) {
        // Y2K: analizador LED dentro de un visor oscuro empotrado
        C2D_DrawRectSolid(start_x - 6, base_y - max_h - 6, 0.45f, total_w + 7, max_h + 12, C2D_Color32(100, 110, 120, 255));
        C2D_DrawRectSolid(start_x - 4, base_y - max_h - 4, 0.46f, total_w + 3, max_h + 8, C2D_Color32(16, 22, 18, 255));

        const int total_leds = 12;
        float led_h = max_h / total_leds - 1.5f;
        for (int i = 0; i < 16; i++) {
            float bx = start_x + i * slot;
            int active = (int)((float)bands[i] / 255.0f * total_leds + 0.5f);
            int peak = (int)((float)peaks[i] / 255.0f * total_leds + 0.5f) - 1;
            for (int l = 0; l < total_leds; l++) {
                float ly = base_y - (l + 1) * (led_h + 1.5f);
                u32 zone = l < 7 ? theme->viz_low : (l < 10 ? theme->viz_mid : theme->viz_high);
                bool lit = l < active || (l == peak && peak >= active);
                C2D_DrawRectSolid(bx, ly, 0.5f, bar_w, led_h, lit ? zone : with_alpha(zone, 38));
            }
        }
        return;
    }

    // Modern Glow: panel sutil, barras redondeadas con degradado y reflejo bajo la base
    fill_round_rect(start_x - 8, base_y - max_h - 8, total_w + 11, max_h + 18, 6, 0.45f, with_alpha(theme->panel_bg, 170));
    C2D_DrawRectSolid(start_x - 2, base_y + 0.5f, 0.47f, total_w - 1, 1, with_alpha(theme->panel_border, 200));

    for (int i = 0; i < 16; i++) {
        float bx = start_x + i * slot;
        float t = i / 15.0f;
        // Color continuo de graves a agudos: low -> mid -> high
        u32 top = t < 0.5f ? lerp_color(theme->viz_low, theme->viz_mid, t * 2.0f)
                           : lerp_color(theme->viz_mid, theme->viz_high, (t - 0.5f) * 2.0f);
        u32 bottom = shade(top, 0.45f);

        float bar_h = (float)bands[i] / 255.0f * max_h;
        if (bar_h < 4.0f) bar_h = 4.0f;
        float by = base_y - bar_h;
        float r = 3.0f;

        fill_round_rect(bx, by, bar_w, 2 * r, r, 0.5f, top);
        C2D_DrawRectangle(bx, by + r, 0.5f, bar_w, bar_h - r, top, top, bottom, bottom);

        // Reflejo tenue bajo la línea base
        float refl = bar_h * 0.18f;
        if (refl > 8.0f) refl = 8.0f;
        C2D_DrawRectangle(bx, base_y + 2, 0.5f, bar_w, refl, with_alpha(top, 70), with_alpha(top, 70),
                          with_alpha(top, 0), with_alpha(top, 0));

        // Pico flotante en forma de píldora
        float py = base_y - (float)peaks[i] / 255.0f * max_h - 5.0f;
        if (py < base_y - max_h - 4.0f) py = base_y - max_h - 4.0f;
        fill_round_rect(bx + 1, py, bar_w - 2, 3, 1.5f, 0.55f, with_alpha(theme->viz_peak, 220));
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
    // Backend, códec y carga del decodificador (para medir el rendimiento en consola)
    char dsp_info[32];
    snprintf(dsp_info, sizeof(dsp_info), "%s %s %d%%", audio_get_backend_name(),
             audio_get_codec() == AUDIO_CODEC_AAC ? "AAC" : "MP3", audio_get_decode_load_percent());
    draw_text(206, 5, 0.34f, theme->text_muted, dsp_info);

    icon_battery(SCREEN_TOP_W - 32, 6, theme->text_primary);

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
        draw_text(312, 4, 0.40f, C2D_Color32(34, 197, 94, 255), "ONLINE");
    } else {
        draw_text(318, 4, 0.40f, theme->text_muted, "PAUSA");
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

    // Aviso temporal bajo el visor
    if (playback->status_msg[0]) {
        draw_text_fit(meta_x, meta_y + 66, 0.38f, C2D_Color32(250, 180, 60, 255), playback->status_msg, meta_w);
    }

    // 3. Visualizador FFT en la seccion inferior de la pantalla superior
    // Más bajo que la carátula (que termina en y=134) para que no choquen
    draw_visualizer(22.0f, 222.0f, SCREEN_TOP_W - 44.0f, 60.0f, bands, peaks, theme);
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

    // Lupa + ":" y el texto de la búsqueda (o el placeholder)
    icon_search(28, 22, 14, theme->accent);
    draw_text(39, 13, 0.45f, theme->accent, ":");
    if (search->is_searching) {
        draw_text(50, 14, 0.45f, theme->accent, "Buscando...");
    } else if (search->query[0] != '\0') {
        draw_text_fit(50, 14, 0.45f, theme->text_primary, search->query, SCREEN_BOT_W - 70);
    } else {
        draw_text(50, 14, 0.45f, theme->text_muted, "search");
    }

    // 2. Lista de canciones / Resultados (5 filas de 19 px)
    for (int row = 0; row < LIST_ROWS; row++) {
        int i = search->page * LIST_ROWS + row;
        float iy = LIST_Y + row * LIST_ROW_STEP;
        bool is_sel = (i == search->selected_index);
        bool is_cursor = (i == search->cursor) && i < search->total_results;

        C2D_DrawRectSolid(10, iy, 0.5f, SCREEN_BOT_W - 20, LIST_ROW_H, is_sel ? theme->accent : theme->panel_bg);
        if (is_cursor) {
            // Marco de la fila enfocada con la cruceta
            u32 frame = is_sel ? C2D_Color32(255, 255, 255, 230) : theme->accent;
            C2D_DrawRectSolid(10, iy, 0.52f, SCREEN_BOT_W - 20, 1.5f, frame);
            C2D_DrawRectSolid(10, iy + LIST_ROW_H - 1.5f, 0.52f, SCREEN_BOT_W - 20, 1.5f, frame);
            C2D_DrawRectSolid(10, iy, 0.52f, 1.5f, LIST_ROW_H, frame);
            C2D_DrawRectSolid(SCREEN_BOT_W - 11.5f, iy, 0.52f, 1.5f, LIST_ROW_H, frame);
        }

        u32 item_text_clr = is_sel ? C2D_Color32(255, 255, 255, 255) : theme->text_primary;
        u32 item_art_clr  = is_sel ? C2D_Color32(240, 240, 240, 255) : theme->text_muted;

        if (i < search->total_results) {
            char num_label[12];
            snprintf(num_label, sizeof(num_label), "%d", i + 1);
            draw_text_centered(22, iy + 3, 0.40f, item_text_clr, num_label);
            // Título en x 34..194 y artista en x 200..306: el recorte por ancho evita que se encimen
            draw_text_fit(34, iy + 3, 0.40f, item_text_clr, search->result_titles[i], 160);
            if (search->result_preview[i]) {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 80);
                draw_text(284, iy + 4, 0.34f, is_sel ? item_text_clr : theme->accent, "30s");
            } else {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 106);
            }
        } else {
            draw_text(16, iy + 3, 0.40f, theme->text_muted, "-");
        }
    }

    // 3. Barra de progreso (Scrubber)
    float scrub_y = 152;
    float scrub_w = SCREEN_BOT_W - 40;
    C2D_DrawRectSolid(20, scrub_y, 0.5f, scrub_w, 6, theme->progress_track);

    float progress_ratio = 0.0f;
    if (playback->scrub_preview >= 0.0f) {
        progress_ratio = playback->scrub_preview;
    } else if (playback->duration_ms > 0) {
        progress_ratio = (float)playback->elapsed_ms / (float)playback->duration_ms;
        if (progress_ratio > 1.0f) progress_ratio = 1.0f;
    }
    float fill_w = scrub_w * progress_ratio;
    C2D_DrawRectSolid(20, scrub_y, 0.5f, fill_w, 6, theme->progress_fill);
    // Puntero / Knob circular
    fill_circle(20 + fill_w, scrub_y + 3, 0.5f, 6.0f, theme->progress_knob);

    // 4. Transporte: botones circulares con íconos
    bool prev_active = touch_down && touch_x >= PREV_X - 20 && touch_x <= PREV_X + 20 && touch_y >= 165 && touch_y <= 207;
    fill_circle(PREV_X, TRANSPORT_Y, 0.5f, 18, prev_active ? theme->accent_active : theme->control_btn_bg);
    icon_skip(PREV_X, TRANSPORT_Y, 16, false, theme->control_icon);

    bool play_active = touch_down && touch_x >= PLAY_X - 24 && touch_x <= PLAY_X + 24 && touch_y >= 162 && touch_y <= 210;
    fill_circle(PLAY_X, TRANSPORT_Y, 0.5f, 23, play_active ? theme->accent_active : theme->accent);
    if (playback->is_playing) {
        icon_pause(PLAY_X, TRANSPORT_Y, 18, C2D_Color32(255, 255, 255, 255));
    } else {
        icon_play(PLAY_X, TRANSPORT_Y, 18, C2D_Color32(255, 255, 255, 255));
    }

    bool next_active = touch_down && touch_x >= NEXT_X - 20 && touch_x <= NEXT_X + 20 && touch_y >= 165 && touch_y <= 207;
    fill_circle(NEXT_X, TRANSPORT_Y, 0.5f, 18, next_active ? theme->accent_active : theme->control_btn_bg);
    icon_skip(NEXT_X, TRANSPORT_Y, 16, true, theme->control_icon);

    // 5. Tema: botón con ícono de paleta
    bool theme_active = touch_down && touch_x >= 12 && touch_x <= 52 && touch_y >= 210 && touch_y <= 238;
    u32 theme_bg = theme_active ? theme->accent_active : theme->control_btn_bg;
    fill_round_rect(14, 212, 36, 24, 7, 0.5f, theme_bg);
    icon_palette(32, 224, 22, theme_bg);

    // 6. Paginador de resultados
    int pages = (search->total_results + LIST_ROWS - 1) / LIST_ROWS;
    if (pages < 1) pages = 1;
    bool can_prev = search->page > 0;
    bool can_next = search->page < pages - 1;
    bool pprev_active = touch_down && can_prev && touch_x >= PAGE_PREV_X - 18 && touch_x <= PAGE_PREV_X + 18 &&
                        touch_y >= PAGER_Y - 14 && touch_y <= PAGER_Y + 14;
    bool pnext_active = touch_down && can_next && touch_x >= PAGE_NEXT_X - 18 && touch_x <= PAGE_NEXT_X + 18 &&
                        touch_y >= PAGER_Y - 14 && touch_y <= PAGER_Y + 14;

    fill_circle(PAGE_PREV_X, PAGER_Y, 0.5f, PAGER_BTN_R, pprev_active ? theme->accent_active : theme->control_btn_bg);
    icon_chevron(PAGE_PREV_X - 1, PAGER_Y, 12, false, can_prev ? theme->control_icon : with_alpha(theme->control_icon, 60));
    fill_circle(PAGE_NEXT_X, PAGER_Y, 0.5f, PAGER_BTN_R, pnext_active ? theme->accent_active : theme->control_btn_bg);
    icon_chevron(PAGE_NEXT_X + 1, PAGER_Y, 12, true, can_next ? theme->control_icon : with_alpha(theme->control_icon, 60));

    char page_label[24];
    snprintf(page_label, sizeof(page_label), "%d / %d", search->page + 1, pages);
    draw_text_centered((PAGE_PREV_X + PAGE_NEXT_X) / 2.0f, PAGER_Y - 7, 0.45f, theme->text_secondary, page_label);
}

void gfx_frame_end(void) {
    C3D_FrameEnd(0);
}
