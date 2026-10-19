#include "gfx.h"
#include "audio.h"
#include "library.h"
#include "local.h"
#include "i18n.h"
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

    u64 start_ms; // origen del reloj de animaciones
} s_gfx;

// Segundos desde que arrancó la app, acotados a una hora: osGetTime() cuenta
// milisegundos desde 1900 y en float perdería toda la parte fraccionaria.
static float anim_time(void) {
    return (float)((osGetTime() - s_gfx.start_ms) % 3600000ULL) / 1000.0f;
}

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
    s_gfx.start_ms = osGetTime();

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

static void coverflow_free(void);

void gfx_exit(void) {
    coverflow_free();
    gfx_clear_playlist_cover();
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

static void draw_text_z(float x, float y, float z, float scale, u32 color, const char* str) {
    if (!str || str[0] == '\0') return;
    str = L(str);
    C2D_Text text;
    C2D_TextParse(&text, s_gfx.text_buf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, z, scale, scale, color);
}

static void draw_text(float x, float y, float scale, u32 color, const char* str) {
    draw_text_z(x, y, 0.5f, scale, color, str);
}

// Copia en `out` la parte de `src` que cabe en max_w píxeles a la escala dada,
// terminando en "..." si se recorta. Mide con la tabla de anchos de la fuente del
// sistema, sin consumir glifos del buffer de texto.
static void fit_text(const char* src, float scale, float max_w, char* out, size_t out_sz) {
    if (!src || out_sz == 0) return;
    src = L(src);
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
    str = L(str);
    C2D_Text text;
    C2D_TextParse(&text, s_gfx.text_buf, str);
    C2D_TextOptimize(&text);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&text, scale, scale, &w, &h);
    C2D_DrawText(&text, C2D_WithColor, cx - w / 2, y, 0.5f, scale, scale, color);
}

static void draw_text_centered_z(float cx, float y, float z, float scale, u32 color, const char* str) {
    if (!str || str[0] == '\0') return;
    str = L(str);
    C2D_Text text;
    C2D_TextParse(&text, s_gfx.text_buf, str);
    C2D_TextOptimize(&text);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&text, scale, scale, &w, &h);
    C2D_DrawText(&text, C2D_WithColor, cx - w / 2, y, z, scale, scale, color);
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

static void draw_line_z(float x0, float y0, float x1, float y1, float thick, float z, u32 color) {
    C2D_DrawLine(x0, y0, color, x1, y1, color, thick, z);
}

static void draw_line(float x0, float y0, float x1, float y1, float thick, u32 color) {
    draw_line_z(x0, y0, x1, y1, thick, ICON_Z, color);
}

static void draw_tri_z(float x0, float y0, float x1, float y1, float x2, float y2, float z, u32 color) {
    C2D_DrawTriangle(x0, y0, color, x1, y1, color, x2, y2, color, z);
}

static void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 color) {
    draw_tri_z(x0, y0, x1, y1, x2, y2, ICON_Z, color);
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

// Nube (SoundCloud): tres círculos y base plana
static void icon_cloud(float cx, float cy, float size, u32 color) {
    fill_circle(cx - size * 0.24f, cy + size * 0.06f, ICON_Z, size * 0.22f, color);
    fill_circle(cx + size * 0.04f, cy - size * 0.1f, ICON_Z, size * 0.3f, color);
    fill_circle(cx + size * 0.3f, cy + size * 0.08f, ICON_Z, size * 0.2f, color);
    C2D_DrawRectSolid(cx - size * 0.24f, cy + size * 0.06f, ICON_Z, size * 0.54f, size * 0.22f, color);
}

// Tarjeta SD con la esquina cortada, contactos y etiqueta
static void icon_sdcard(float cx, float cy, float size, u32 color, u32 detail) {
    float w = size * 0.68f, h = size * 0.88f, bev = size * 0.22f;
    float x0 = cx - w / 2, y0 = cy - h / 2;
    C2D_DrawRectSolid(x0, y0 + bev, ICON_Z, w, h - bev, color);
    C2D_DrawRectSolid(x0 + bev, y0, ICON_Z, w - bev, bev, color);
    draw_tri(x0, y0 + bev, x0 + bev, y0, x0 + bev, y0 + bev, color);
    for (int i = 0; i < 4; i++) {
        C2D_DrawRectSolid(x0 + bev + 1 + i * (w - bev - 2) / 4.0f, y0 + size * 0.06f, ICON_Z + 0.01f,
                          (w - bev - 2) / 4.0f - 1.5f, size * 0.16f, detail);
    }
    C2D_DrawRectSolid(x0 + w * 0.15f, y0 + h * 0.55f, ICON_Z + 0.01f, w * 0.7f, h * 0.3f, detail);
}

// Casa: volver al menú
static void icon_home(float cx, float cy, float size, u32 color) {
    draw_tri(cx - size * 0.5f, cy - size * 0.02f, cx + size * 0.5f, cy - size * 0.02f, cx, cy - size * 0.5f, color);
    C2D_DrawRectSolid(cx - size * 0.34f, cy - size * 0.04f, ICON_Z, size * 0.68f, size * 0.48f, color);
}

// Mando: cuerpo con empuñaduras, cruceta a la izquierda y dos botones a la derecha
static void icon_gamepad(float cx, float cy, float size, u32 color, u32 detail) {
    float w = size, h = size * 0.5f;
    fill_round_rect(cx - w / 2, cy - h / 2, w, h, h * 0.45f, ICON_Z, color);
    fill_circle(cx - w * 0.3f, cy + h * 0.32f, ICON_Z, h * 0.42f, color);
    fill_circle(cx + w * 0.3f, cy + h * 0.32f, ICON_Z, h * 0.42f, color);
    float dx = cx - w * 0.24f, t = size * 0.08f, l = size * 0.26f;
    C2D_DrawRectSolid(dx - l / 2, cy - t / 2, ICON_Z + 0.01f, l, t, detail);
    C2D_DrawRectSolid(dx - t / 2, cy - l / 2, ICON_Z + 0.01f, t, l, detail);
    fill_circle(cx + w * 0.18f, cy + h * 0.1f, ICON_Z + 0.01f, size * 0.06f, detail);
    fill_circle(cx + w * 0.3f, cy - h * 0.12f, ICON_Z + 0.01f, size * 0.06f, detail);
}

// Descarga: flecha hacia abajo sobre una bandeja
static void icon_download(float cx, float cy, float size, u32 color) {
    float t = size * 0.13f;
    C2D_DrawRectSolid(cx - t / 2, cy - size * 0.45f, ICON_Z, t, size * 0.5f, color);
    draw_tri(cx - size * 0.3f, cy - size * 0.02f, cx + size * 0.3f, cy - size * 0.02f, cx, cy + size * 0.28f, color);
    C2D_DrawRectSolid(cx - size * 0.42f, cy + size * 0.36f, ICON_Z, size * 0.84f, t, color);
    C2D_DrawRectSolid(cx - size * 0.42f, cy + size * 0.18f, ICON_Z, t, size * 0.2f, color);
    C2D_DrawRectSolid(cx + size * 0.42f - t, cy + size * 0.18f, ICON_Z, t, size * 0.2f, color);
}

static void fill_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                      float z, u32 color);

// Engranaje: dientes alrededor de un aro con hueco central
static void icon_gear(float cx, float cy, float size, u32 color, u32 hole) {
    float r = size * 0.32f, tooth = size * 0.17f;
    for (int i = 0; i < 8; i++) {
        float a = i * 6.2832f / 8, c = cosf(a), s = sinf(a), w = size * 0.11f;
        float x0 = cx + c * r, y0 = cy + s * r, x1 = cx + c * (r + tooth), y1 = cy + s * (r + tooth);
        fill_quad(x0 - s * w, y0 + c * w, x1 - s * w, y1 + c * w, x1 + s * w, y1 - c * w, x0 + s * w, y0 - c * w,
                  ICON_Z, color);
    }
    fill_circle(cx, cy, ICON_Z, r + size * 0.04f, color);
    fill_circle(cx, cy, ICON_Z + 0.01f, size * 0.13f, hole);
}

static void draw_cfg_button(float x, bool touch_down, int touch_x, int touch_y, const UITheme* theme) {
    bool active = touch_down && touch_x >= x - 2 && touch_x <= x + BAR_BTN_W + 2 && touch_y >= 210 && touch_y <= 238;
    u32 bg = active ? theme->accent_active : theme->control_btn_bg;
    fill_round_rect(x, 212, BAR_BTN_W, 24, 7, 0.5f, bg);
    icon_gear(x + BAR_BTN_W / 2.0f, 224, 18, theme->control_icon, bg);
}

// Visto bueno (pista ya descargada)
static void icon_check(float cx, float cy, float size, u32 color) {
    draw_line(cx - size * 0.4f, cy, cx - size * 0.1f, cy + size * 0.3f, size * 0.18f, color);
    draw_line(cx - size * 0.1f, cy + size * 0.3f, cx + size * 0.45f, cy - size * 0.35f, size * 0.18f, color);
}

// Corchea: cabeza, plica y bandera
static void icon_note(float x, float y, float size, u32 color) {
    fill_ellipse(x, y, size * 0.3f, size * 0.22f, ICON_Z, color);
    draw_line(x + size * 0.26f, y, x + size * 0.26f, y - size * 0.85f, size * 0.12f, color);
    draw_line(x + size * 0.26f, y - size * 0.85f, x + size * 0.6f, y - size * 0.55f, size * 0.12f, color);
}

// Playlist: tres renglones y una corchea pequeña
static void icon_playlist(float cx, float cy, float size, u32 color) {
    float t = size * 0.12f, x0 = cx - size * 0.45f;
    C2D_DrawRectSolid(x0, cy - size * 0.32f, ICON_Z, size * 0.62f, t, color);
    C2D_DrawRectSolid(x0, cy - size * 0.06f, ICON_Z, size * 0.62f, t, color);
    C2D_DrawRectSolid(x0, cy + size * 0.20f, ICON_Z, size * 0.36f, t, color);
    icon_note(cx + size * 0.18f, cy + size * 0.30f, size * 0.5f, color);
}

// ---------------------------------------------------------------------------
// Mascota: un cubo con dos ojitos cuadrados, dibujado en pseudo-3D (cara frontal,
// superior y lateral). (cx, base_y) es el centro de su base; s escala el dibujo
// (1.0 ≈ 50 px de alto).
// ---------------------------------------------------------------------------

static void fill_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                      float z, u32 color) {
    draw_tri_z(x0, y0, x1, y1, x2, y2, z, color);
    draw_tri_z(x0, y0, x2, y2, x3, y3, z, color);
}

static void draw_mascot(float cx, float base_y, float s, const MascotViewModel* m, const UITheme* th) {
    float t = anim_time();
    u32 front = th->mascot_body;
    u32 top = th->mascot_light;
    u32 side = shade(front, 0.68f);
    u32 eye_white = C2D_Color32(250, 250, 250, 255);
    u32 ink = C2D_Color32(24, 24, 32, 255);
    MascotState st = m->state;
    // Capas propias, de atrás hacia adelante (Citro2D dibuja encima lo de mayor z)
    const float Z_SHADOW = 0.69f, Z_FACE = 0.72f, Z_EYE = 0.75f, Z_PUPIL = 0.76f;
    const float Z_SHINE = 0.77f, Z_FX = 0.80f;

    // Movimiento: respira, se balancea o rebota con los graves (estirar y aplastar)
    float hop = 0.0f, sway = 0.0f, squash = 0.0f;
    if (st == MASCOT_SLEEP) squash = sinf(t * 1.6f) * 0.03f;
    else if (st == MASCOT_DANCE) {
        float beat = fabsf(sinf(t * 5.0f));
        hop = beat * (3.0f + 6.0f * m->energy) * s;
        squash = (1.0f - beat) * 0.12f - 0.04f;   // aplasta al tocar el piso, se estira en el aire
        sway = sinf(t * 2.5f) * 3.0f * s;
    } else squash = sinf(t * 1.2f) * 0.015f;

    float W = 44 * s * (1.0f + squash), H = 40 * s * (1.0f - squash), d = 10 * s;
    float x0 = cx - (W + d) / 2 + sway, y0 = base_y - H - hop;  // esquina superior izquierda del frente

    // Sombra en el piso: más chica cuanto más alto salta
    float sh = 1.0f - hop / (12 * s + 1);
    fill_ellipse(cx + d / 2, base_y + 1.5f * s, (W * 0.55f + d) * sh, 3.5f * s * sh, Z_SHADOW,
                 C2D_Color32(0, 0, 0, 70));

    // Caras del cubo
    fill_quad(x0, y0, x0 + W, y0, x0 + W + d, y0 - d, x0 + d, y0 - d, Z_FACE, top);
    fill_quad(x0 + W, y0, x0 + W + d, y0 - d, x0 + W + d, y0 + H - d, x0 + W, y0 + H, Z_FACE, side);
    C2D_DrawRectSolid(x0, y0, Z_FACE, W, H, front);
    // Brillo en la arista superior del frente
    C2D_DrawRectSolid(x0, y0, Z_FACE + 0.005f, W, 1.5f * s, with_alpha(top, 200));

    // Hacia dónde mira según lo que hace
    float lx = sinf(t * 0.7f) * 0.5f, ly = 0.0f;
    if (st == MASCOT_CLOUD) { lx = 0.75f; ly = -0.75f; }
    if (st == MASCOT_LOCAL) { lx = -0.7f; ly = 0.7f; }
    if (st == MASCOT_SEARCH) { lx = sinf(t * 5.0f); ly = 0.0f; }
    if (st == MASCOT_LOADING) { lx = 0.6f; ly = -0.85f; }  // mira hacia arriba, pensando
    bool blink = (st == MASCOT_AWAKE || st == MASCOT_CLOUD || st == MASCOT_LOCAL) && fmodf(t, 4.0f) < 0.13f;

    // Dos ojitos cuadrados
    float es = 11 * s, ey = y0 + H * 0.42f, fcx = x0 + W / 2;
    for (int sd = -1; sd <= 1; sd += 2) {
        float ex = fcx + sd * 10 * s;
        if (st == MASCOT_SLEEP || blink) {
            draw_line_z(ex - es / 2, ey + 1 * s, ex + es / 2, ey + 1 * s, 2.2f * s, Z_PUPIL, ink);
        } else if (st == MASCOT_DANCE) {
            draw_line_z(ex - es / 2, ey + 2 * s, ex, ey - 3 * s, 2.2f * s, Z_PUPIL, ink);   // ojos felices ^ ^
            draw_line_z(ex, ey - 3 * s, ex + es / 2, ey + 2 * s, 2.2f * s, Z_PUPIL, ink);
        } else {
            C2D_DrawRectSolid(ex - es / 2, ey - es / 2, Z_EYE, es, es, eye_white);
            float px = ex - 2.5f * s + lx * 2.8f * s, py = ey - 2.5f * s + ly * 2.8f * s;
            C2D_DrawRectSolid(px, py, Z_PUPIL, 5 * s, 5 * s, ink);
            C2D_DrawRectSolid(px + 0.8f * s, py + 0.8f * s, Z_SHINE, 1.5f * s, 1.5f * s, eye_white);
        }
    }

    // Mejillas y boca
    u32 blush = C2D_Color32(255, 120, 150, 110);
    fill_ellipse(fcx - 15 * s, ey + 8 * s, 3.5f * s, 2 * s, Z_EYE, blush);
    fill_ellipse(fcx + 15 * s, ey + 8 * s, 3.5f * s, 2 * s, Z_EYE, blush);
    float my = y0 + H * 0.72f;
    if (st == MASCOT_DANCE) {
        C2D_DrawRectSolid(fcx - 3 * s, my - 1.5f * s, Z_PUPIL, 6 * s, 4 * s, ink);     // boca abierta
    } else if (st == MASCOT_SLEEP) {
        fill_ellipse(fcx, my, 1.8f * s, 1.5f * s, Z_PUPIL, ink);                    // "o" dormida
    } else if (st == MASCOT_LOADING) {
        draw_line_z(fcx - 3 * s, my, fcx + 3 * s, my, 1.6f * s, Z_PUPIL, ink);      // boca recta, pensando
    } else {
        draw_line_z(fcx - 3.5f * s, my - 1 * s, fcx, my + 1 * s, 1.6f * s, Z_PUPIL, ink); // sonrisa
        draw_line_z(fcx, my + 1 * s, fcx + 3.5f * s, my - 1 * s, 1.6f * s, Z_PUPIL, ink);
    }

    // Detalles de cada estado, alrededor del cubo
    float ox = x0 + W + d, oy = y0 - d;  // esquina superior derecha del cubo
    if (st == MASCOT_SLEEP) {
        // ZzZ que suben y se desvanecen
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * 0.55f + i / 3.0f, 1.0f);
            u8 alpha = (u8)(255 * (ph < 0.15f ? ph / 0.15f : 1.0f - (ph - 0.15f) / 0.85f));
            draw_text_z(ox + (2 + ph * 16) * s, oy - (8 + ph * 26) * s, Z_FX, (0.32f + ph * 0.3f) * s,
                        with_alpha(th->text_primary, alpha), i == 1 ? "z" : "Z");
        }
    } else if (st == MASCOT_CLOUD) {
        icon_cloud(ox + 14 * s, oy - 12 * s + sinf(t * 2.0f) * 2 * s, 22 * s, C2D_Color32(255, 119, 0, 255));
    } else if (st == MASCOT_LOCAL) {
        icon_sdcard(x0 - 16 * s, y0 + H * 0.55f + sinf(t * 2.0f) * 2 * s, 20 * s, th->accent,
                    with_alpha(C2D_Color32(255, 255, 255, 255), 200));
    } else if (st == MASCOT_SEARCH) {
        icon_search(ox + 12 * s, oy - 6 * s, 16 * s, th->accent);
    } else if (st == MASCOT_LOADING) {
        float scx = ox + 12 * s, scy = oy - 8 * s, r = 8 * s;
        if (th->is_segmented) {
            // Y2K: spinner clásico de 8 puntos que avanza a saltos; el líder es el más
            // grande y oscuro y los demás se van aclarando detrás de él
            int lead = (int)(t * 10.0f) % 8;
            for (int i = 0; i < 8; i++) {
                int age = (lead - i + 8) % 8;          // 0 = líder
                float k = 1.0f - age / 8.0f;
                float a = i * 0.7854f - 1.5708f;       // empieza arriba, sentido horario
                fill_circle(scx + cosf(a) * r, scy + sinf(a) * r, Z_FX, (1.0f + 1.2f * k) * s,
                            with_alpha(th->text_secondary, (u8)(40 + 215 * k)));
            }
        } else {
            // Modern: una sola pieza tipo cometa, del color de la barra de progreso,
            // que se desvanece hacia la cola y termina en una cabeza redonda
            // Banda continua de triángulos con el degradado por vértice (sin uniones visibles)
            const int segs = 20;
            float a0 = t * 6.0f, span = 4.4f, thick = 2.6f * s;
            float ri = r - thick / 2, ro = r + thick / 2;
            for (int i = 0; i < segs; i++) {
                float a = a0 + span * i / segs, b = a0 + span * (i + 1) / segs;
                u32 ca = with_alpha(th->progress_fill, (u8)(255.0f * i / segs));
                u32 cb = with_alpha(th->progress_fill, (u8)(255.0f * (i + 1) / segs));
                float aix = scx + cosf(a) * ri, aiy = scy + sinf(a) * ri, aox = scx + cosf(a) * ro, aoy = scy + sinf(a) * ro;
                float bix = scx + cosf(b) * ri, biy = scy + sinf(b) * ri, box = scx + cosf(b) * ro, boy = scy + sinf(b) * ro;
                C2D_DrawTriangle(aix, aiy, ca, aox, aoy, ca, box, boy, cb, Z_FX);
                C2D_DrawTriangle(aix, aiy, ca, box, boy, cb, bix, biy, cb, Z_FX);
            }
            float ah = a0 + span;
            fill_circle(scx + cosf(ah) * r, scy + sinf(ah) * r, Z_FX, thick / 2, th->progress_fill);
        }
    } else if (st == MASCOT_DANCE) {
        for (int i = 0; i < 2; i++) {
            float ph = fmodf(t * 0.5f + i * 0.5f, 1.0f);
            u8 alpha = (u8)(255 * (1.0f - ph));
            float nx = (i ? ox + 8 * s : x0 - 10 * s) + sinf(ph * 6.0f) * 4 * s;
            icon_note(nx, y0 + 4 * s - ph * 30 * s, 11 * s, with_alpha(i ? th->viz_mid : th->viz_low, alpha));
        }
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

// Sube una imagen RGBA a una textura de 128x128 (escalada) en el orden de mosaico de la GPU
static bool upload_cover(C3D_Tex* tex, bool* loaded, const uint8_t* rgba, int w, int h) {
    if (*loaded) {
        C3D_TexDelete(tex);
        *loaded = false;
    }
    if (!rgba || w <= 0 || h <= 0) return false;
    const int tex_w = 128, tex_h = 128;
    if (!C3D_TexInit(tex, tex_w, tex_h, GPU_RGBA8)) return false;

    uint32_t* dst = (uint32_t*)tex->data;
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
            int p_idx = ((y / 8) * tiles_x + x / 8) * 64 + morton_table[y % 8][x % 8];
            uint32_t col = src32[src_y * w + src_x];
            // GPU_RGBA8 espera R en el byte alto: 0xRRGGBBAA
            dst[p_idx] = ((col & 0xFF) << 24) | (((col >> 8) & 0xFF) << 16) | (((col >> 16) & 0xFF) << 8) | (col >> 24);
        }
    }
    C3D_TexFlush(tex);
    *loaded = true;
    return true;
}

static const Tex3DS_SubTexture s_cover_sub = { .width = 128, .height = 128, .left = 0.0f, .top = 1.0f, .right = 1.0f,
                                               .bottom = 0.0f };

void gfx_set_artwork_rgba(const uint8_t* rgba, int w, int h) {
    upload_cover(&s_artwork_tex, &s_artwork_tex_loaded, rgba, w, h);
}

static C3D_Tex s_pl_tex;
static bool s_pl_tex_loaded = false;

void gfx_set_playlist_cover_rgba(const uint8_t* rgba, int w, int h) {
    upload_cover(&s_pl_tex, &s_pl_tex_loaded, rgba, w, h);
}

void gfx_clear_playlist_cover(void) {
    if (s_pl_tex_loaded) {
        C3D_TexDelete(&s_pl_tex);
        s_pl_tex_loaded = false;
    }
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

// Fondo y barra superior (estado, batería) comunes al menú y al reproductor
static void begin_top(const UITheme* theme, const PlaybackViewModel* playback) {
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
    // Cuenta de SoundCloud conectada (si no hay, el nombre del tema)
    if (playback && playback->account[0]) {
        fill_circle(114, 11, 0.55f, 3.0f, C2D_Color32(255, 119, 0, 255));
        draw_text_fit(121, 4, 0.40f, theme->accent, playback->account, 82);
    } else {
        draw_text(110, 4, 0.40f, theme->accent, theme->name);
    }
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
}

void gfx_render_top(const PlaybackViewModel* playback, const uint8_t bands[16], const uint8_t peaks[16],
                    const MascotViewModel* mascot) {
    const UITheme* theme = theme_get_current();
    begin_top(theme, playback);

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
        draw_text_fit(meta_x, meta_y + 66, 0.38f, C2D_Color32(250, 180, 60, 255), playback->status_msg, 200);
    }

    // Cubo pequeño en el hueco entre el visor y el ecualizador
    draw_mascot(360, 148, 0.8f, mascot, theme);

    // 3. Visualizador FFT en la seccion inferior de la pantalla superior
    // Más bajo que la carátula (que termina en y=134) para que no choquen
    draw_visualizer(22.0f, 222.0f, SCREEN_TOP_W - 44.0f, 60.0f, bands, peaks, theme);
}

void gfx_render_bottom(const PlaybackViewModel* playback, const SearchViewModel* search, bool game_on,
                       bool touch_down, int touch_x, int touch_y) {
    const UITheme* theme = theme_get_current();

    C2D_TargetClear(s_gfx.bottomTarget, theme->bg_bottom);
    C2D_SceneBegin(s_gfx.bottomTarget);

    // 1. Cabecera: playlist abierta (volver + nombre + descargar todo) o barra de búsqueda
    if (search->playlist_header) {
        bool back_active = touch_down && touch_x >= SEARCH_BAR_X && touch_x <= SEARCH_BAR_X + BACK_CHIP_W &&
                           touch_y >= SEARCH_BAR_Y && touch_y <= SEARCH_BAR_Y + SEARCH_BAR_H;
        fill_round_rect(SEARCH_BAR_X, SEARCH_BAR_Y, BACK_CHIP_W, SEARCH_BAR_H, 7, 0.5f,
                        back_active ? theme->accent_active : theme->control_btn_bg);
        icon_chevron(SEARCH_BAR_X + BACK_CHIP_W / 2.0f - 1, SEARCH_BAR_Y + SEARCH_BAR_H / 2.0f, 12, false,
                     theme->control_icon);
        float title_x = SEARCH_BAR_X + BACK_CHIP_W + 8;
        float title_w = (search->show_dl_all ? DLALL_X - 8 : SCREEN_BOT_W - 10) - title_x;
        icon_playlist(title_x + 7, SEARCH_BAR_Y + SEARCH_BAR_H / 2.0f, 14, theme->accent);
        draw_text_fit(title_x + 18, 14, 0.45f, theme->text_primary, search->header_title, title_w - 18);
        if (search->show_dl_all) {
            bool dl_active = touch_down && touch_x >= DLALL_X && touch_x <= DLALL_X + DLALL_W &&
                             touch_y >= SEARCH_BAR_Y && touch_y <= SEARCH_BAR_Y + SEARCH_BAR_H;
            fill_round_rect(DLALL_X, SEARCH_BAR_Y, DLALL_W, SEARCH_BAR_H, 7, 0.5f,
                            dl_active ? theme->accent_active : theme->accent);
            u32 white = C2D_Color32(255, 255, 255, 255);
            if (search->dl_all_label[0]) {
                draw_text_centered(DLALL_X + DLALL_W / 2.0f, 15, 0.42f, white, search->dl_all_label);
            } else {
                icon_download(DLALL_X + 16, SEARCH_BAR_Y + SEARCH_BAR_H / 2.0f - 1, 14, white);
                draw_text(DLALL_X + 28, 15, 0.42f, white, "Descargar");
            }
        }
    } else {
        float bar_w = search->show_selector ? SEARCH_BAR_SEL_W : SCREEN_BOT_W - 20;
        C2D_DrawRectSolid(SEARCH_BAR_X, SEARCH_BAR_Y, 0.5f, bar_w, SEARCH_BAR_H, theme->panel_bg);
        C2D_DrawRectSolid(SEARCH_BAR_X, SEARCH_BAR_Y, 0.5f, bar_w, 1, theme->panel_border);
        C2D_DrawRectSolid(SEARCH_BAR_X, SEARCH_BAR_Y + SEARCH_BAR_H - 1, 0.5f, bar_w, 1, theme->panel_border);

        // Lupa + ":" y el texto de la búsqueda (o el placeholder)
        icon_search(28, 22, 14, theme->accent);
        draw_text(39, 13, 0.45f, theme->accent, ":");
        if (search->is_searching) {
            draw_text(50, 14, 0.45f, theme->accent, "Buscando...");
        } else if (search->query[0] != '\0') {
            draw_text_fit(50, 14, 0.45f, theme->text_primary, search->query, bar_w - 50);
        } else {
            draw_text(50, 14, 0.45f, theme->text_muted, search->placeholder[0] ? search->placeholder : "search");
        }

        // Selector: canciones (corchea) o playlists
        if (search->show_selector) {
            for (int k = 0; k < 2; k++) {
                float bx = k == 0 ? KIND_BTN_X0 : KIND_BTN_X1;
                bool on = search->search_kind == k;
                bool pressed = touch_down && touch_x >= bx && touch_x <= bx + KIND_BTN_W && touch_y >= SEARCH_BAR_Y &&
                               touch_y <= SEARCH_BAR_Y + SEARCH_BAR_H;
                u32 bg = pressed ? theme->accent_active : (on ? theme->accent : theme->control_btn_bg);
                u32 fg = on ? C2D_Color32(255, 255, 255, 255) : theme->control_icon;
                fill_round_rect(bx, SEARCH_BAR_Y, KIND_BTN_W, SEARCH_BAR_H, 7, 0.5f, bg);
                if (k == 0) icon_note(bx + KIND_BTN_W / 2.0f - 3, SEARCH_BAR_Y + 20, 14, fg);
                else icon_playlist(bx + KIND_BTN_W / 2.0f, SEARCH_BAR_Y + SEARCH_BAR_H / 2.0f, 18, fg);
            }
        }
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
            if (search->result_badge[i][0]) {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 78);
                icon_playlist(287, iy + 9, 10, is_sel ? item_text_clr : theme->accent);
                draw_text(294, iy + 4, 0.34f, is_sel ? item_text_clr : theme->accent, search->result_badge[i]);
            } else if (search->result_preview[i]) {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 80);
                draw_text(284, iy + 4, 0.34f, is_sel ? item_text_clr : theme->accent, "30s");
            } else if (search->result_saved[i]) {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 90);
                icon_check(299, iy + 9, 9, is_sel ? item_text_clr : C2D_Color32(34, 197, 94, 255));
            } else {
                draw_text_fit(200, iy + 4, 0.36f, item_art_clr, search->result_artists[i], 106);
            }
        } else if (search->total_results == 0 && row == 2 && search->empty_msg[0]) {
            draw_text_centered(SCREEN_BOT_W / 2.0f, iy + 3, 0.40f, theme->text_muted, search->empty_msg);
        } else if (search->total_results > 0) {
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
    bool theme_active = touch_down && touch_x >= THEME_BTN_X - 2 && touch_x <= THEME_BTN_X + BAR_BTN_W + 2 &&
                        touch_y >= 210 && touch_y <= 238;
    u32 theme_bg = theme_active ? theme->accent_active : theme->control_btn_bg;
    fill_round_rect(THEME_BTN_X, 212, BAR_BTN_W, 24, 7, 0.5f, theme_bg);
    icon_palette(THEME_BTN_X + BAR_BTN_W / 2.0f, 224, 20, theme_bg);

    // 5b. Minijuego en la pantalla superior: resaltado mientras se muestra
    bool game_active = touch_down && touch_x >= GAME_BTN_X - 2 && touch_x <= GAME_BTN_X + GAME_BTN_W + 2 &&
                       touch_y >= 210 && touch_y <= 238;
    u32 game_bg = game_active ? theme->accent_active : (game_on ? theme->accent : theme->control_btn_bg);
    fill_round_rect(GAME_BTN_X, 212, GAME_BTN_W, 24, 7, 0.5f, game_bg);
    icon_gamepad(GAME_BTN_X + GAME_BTN_W / 2.0f, 224, 22, game_on ? C2D_Color32(255, 255, 255, 255) : theme->control_icon,
                 game_bg);

    // 5c. Descargar la pista enfocada (SoundCloud): muestra el avance mientras baja
    if (search->show_download) {
        bool dl_pressed = touch_down && touch_x >= DL_BTN_X - 2 && touch_x <= DL_BTN_X + DL_BTN_W + 2 &&
                          touch_y >= 210 && touch_y <= 238;
        u32 dl_bg = dl_pressed ? theme->accent_active : theme->control_btn_bg;
        fill_round_rect(DL_BTN_X, 212, DL_BTN_W, 24, 7, 0.5f, dl_bg);
        if (search->download_active) {
            float k = search->download_percent / 100.0f;
            C2D_DrawRectSolid(DL_BTN_X + 4, 231, 0.55f, (DL_BTN_W - 8) * k, 2, theme->accent);
            char pct[8];
            snprintf(pct, sizeof(pct), "%d%%", search->download_percent);
            draw_text_centered_z(DL_BTN_X + DL_BTN_W / 2.0f, 215, 0.6f, 0.34f, theme->text_primary, pct);
        } else {
            icon_download(DL_BTN_X + DL_BTN_W / 2.0f, 223, 16, theme->control_icon);
        }
    }

    // 5d. Configuración
    draw_cfg_button(CFG_BTN_X, touch_down, touch_x, touch_y, theme);

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

    // 7. Volver al menú
    bool home_active = touch_down && touch_x >= HOME_X - 16 && touch_x <= HOME_X + 16 &&
                       touch_y >= HOME_Y - 14 && touch_y <= HOME_Y + 14;
    fill_circle(HOME_X, HOME_Y, 0.5f, 12, home_active ? theme->accent_active : theme->control_btn_bg);
    icon_home(HOME_X, HOME_Y + 1, 13, theme->control_icon);
}

// ---------------------------------------------------------------------------
// Playlist abierta (pantalla superior) y diálogo (pantalla inferior)
// ---------------------------------------------------------------------------

void gfx_render_playlist_top(const PlaylistTopViewModel* pl, const PlaybackViewModel* playback,
                             const MascotViewModel* mascot) {
    const UITheme* theme = theme_get_current();
    begin_top(theme, playback);

    // Carátula grande con borde y sombra
    const float cx = 18, cy = 34, size = 124;
    C2D_DrawRectSolid(cx + 4, cy + 4, 0.45f, size, size, C2D_Color32(0, 0, 0, 70));
    C2D_DrawRectSolid(cx - 2, cy - 2, 0.5f, size + 4, size + 4, theme->panel_border);
    if (s_pl_tex_loaded) {
        C2D_Image img = { &s_pl_tex, &s_cover_sub };
        C2D_DrawImageAt(img, cx, cy, 0.55f, NULL, size / 128.0f, size / 128.0f);
    } else {
        C2D_DrawRectSolid(cx, cy, 0.55f, size, size, theme->panel_bg);
        icon_playlist(cx + size / 2, cy + size / 2, 48, with_alpha(theme->accent, pl->loading ? 120 : 220));
    }

    // Datos de la playlist
    float tx = cx + size + 16, tw = SCREEN_TOP_W - tx - 14;
    draw_text(tx, 34, 0.38f, theme->accent, "PLAYLIST");
    draw_text_fit(tx, 50, 0.62f, theme->text_primary, pl->title, tw);
    draw_text_fit(tx, 74, 0.46f, theme->text_secondary, pl->creator, tw);
    if (pl->loading) {
        draw_text(tx, 98, 0.42f, theme->text_muted, "Cargando playlist...");
    } else {
        char info[64];
        uint32_t min = pl->total_ms / 60000;
        snprintf(info, sizeof(info), L("%d canciones - %lu min"), pl->track_count, (unsigned long)min);
        draw_text(tx, 98, 0.42f, theme->text_muted, info);
        if (pl->unavailable > 0) {
            snprintf(info, sizeof(info), L("%d no disponibles"), pl->unavailable);
            draw_text(tx, 114, 0.38f, theme->text_muted, info);
        }
    }
    if (pl->download_status[0]) {
        icon_download(tx + 6, 138, 12, theme->accent);
        draw_text_fit(tx + 16, 131, 0.40f, theme->accent, pl->download_status, tw - 16);
    }

    // Lo que suena, abajo
    float py = 172;
    C2D_DrawRectSolid(14, py, 0.5f, SCREEN_TOP_W - 28, 54, theme->panel_bg);
    C2D_DrawRectSolid(14, py, 0.5f, SCREEN_TOP_W - 28, 1, theme->panel_border);
    icon_note(30, py + 33, 16, playback->is_playing ? theme->accent : theme->text_muted);
    draw_text_fit(46, py + 8, 0.48f, theme->text_primary, playback->title, 250);
    draw_text_fit(46, py + 28, 0.40f, theme->text_secondary, playback->artist, 200);
    char time_str[32];
    uint32_t cur_s = playback->elapsed_ms / 1000, tot_s = playback->duration_ms / 1000;
    snprintf(time_str, sizeof(time_str), "%02lu:%02lu / %02lu:%02lu", (unsigned long)(cur_s / 60),
             (unsigned long)(cur_s % 60), (unsigned long)(tot_s / 60), (unsigned long)(tot_s % 60));
    draw_text(250, py + 30, 0.38f, theme->accent, time_str);
    if (playback->status_msg[0]) draw_text_fit(14, py - 16, 0.36f, C2D_Color32(250, 180, 60, 255), playback->status_msg, 360);
    draw_mascot(358, py + 46, 0.6f, mascot, theme);
}

void gfx_render_dialog_bottom(const DialogViewModel* d, bool touch_down, int touch_x, int touch_y) {
    const UITheme* theme = theme_get_current();
    C2D_TargetClear(s_gfx.bottomTarget, theme->bg_bottom);
    C2D_SceneBegin(s_gfx.bottomTarget);

    fill_round_rect(16, 40, SCREEN_BOT_W - 32, 168, 12, 0.5f, theme->panel_bg);
    C2D_DrawRectSolid(28, 72, 0.55f, SCREEN_BOT_W - 56, 1, theme->panel_border);
    if (d->account_icon) {
        // Silueta de persona: cabeza y hombros
        fill_circle(38, 54, 0.6f, 4.5f, theme->accent);
        fill_round_rect(31, 60, 14, 8, 4, 0.6f, theme->accent);
    } else {
        icon_download(38, 58, 16, theme->accent);
    }
    draw_text_fit(52, 50, 0.52f, theme->text_primary, d->title, SCREEN_BOT_W - 84);
    // Cuerpo en hasta dos líneas: se corta en el último espacio que quepa
    char line1[128], line2[128] = "";
    fit_text(d->body, 0.44f, SCREEN_BOT_W - 64, line1, sizeof(line1));
    size_t used = strlen(line1);
    if (used >= 3 && strcmp(line1 + used - 3, "...") == 0) {
        line1[used - 3] = '\0';
        char* sp = strrchr(line1, ' ');
        if (sp) *sp = '\0';
        const char* rest = L(d->body) + strlen(line1);
        while (*rest == ' ') rest++;
        fit_text(rest, 0.44f, SCREEN_BOT_W - 64, line2, sizeof(line2));
    }
    draw_text(32, 88, 0.44f, theme->text_secondary, line1);
    if (line2[0]) draw_text(32, 108, 0.44f, theme->text_secondary, line2);

    for (int k = 0; k < 2; k++) {
        float bx = k == 0 ? DIALOG_YES_X : DIALOG_NO_X;
        bool pressed = touch_down && touch_x >= bx && touch_x <= bx + DIALOG_BTN_W && touch_y >= DIALOG_BTN_Y &&
                       touch_y <= DIALOG_BTN_Y + DIALOG_BTN_H;
        u32 bg = pressed ? theme->accent_active : (k == 0 ? theme->accent : theme->control_btn_bg);
        fill_round_rect(bx, DIALOG_BTN_Y, DIALOG_BTN_W, DIALOG_BTN_H, 9, 0.5f, bg);
        char label[48];
        snprintf(label, sizeof(label), "%s %s", k == 0 ? "(A)" : "(B)", L(k == 0 ? d->yes : d->no));
        draw_text_centered(bx + DIALOG_BTN_W / 2.0f, DIALOG_BTN_Y + 9, 0.44f,
                           k == 0 ? C2D_Color32(255, 255, 255, 255) : theme->text_primary, label);
    }
}

// ---------------------------------------------------------------------------
// Cover flow del modo Local: álbumes en perspectiva con reflejo
// ---------------------------------------------------------------------------

#define CF_SLOTS   13      // texturas de carátula en GPU (álbumes visibles y vecinos)
#define CF_STRIPS  14      // franjas verticales para simular la perspectiva
#define CF_SIZE    100.0f  // lado de la carátula central
#define CF_CY      90.0f   // centro vertical de las carátulas

static struct {
    C3D_Tex tex;
    bool init;
    int album;      // índice en la biblioteca
    int vis;        // posición en el cover flow cuando se usó (para elegir cuál reemplazar)
    u32 gen;
    bool loaded;
} s_cf[CF_SLOTS];

static void coverflow_free(void) {
    for (int i = 0; i < CF_SLOTS; i++)
        if (s_cf[i].init) {
            C3D_TexDelete(&s_cf[i].tex);
            s_cf[i].init = false;
        }
}

void gfx_coverflow_reset(void) {
    for (int i = 0; i < CF_SLOTS; i++) s_cf[i].album = -1;
}

// Textura del álbum (asigna una ranura libre o la del álbum más lejano). NULL si aún
// no hay carátula: se dibuja un marcador mientras el hilo la prepara.
static C3D_Tex* cf_texture(int album, int vis, float center, int* uploads) {
    u32 gen = library_generation();
    int slot = -1;
    for (int i = 0; i < CF_SLOTS; i++)
        if (s_cf[i].init && s_cf[i].album == album && s_cf[i].gen == gen) { slot = i; break; }
    if (slot < 0) {
        float far_d = -1;
        for (int i = 0; i < CF_SLOTS; i++) {
            float d = (!s_cf[i].init || s_cf[i].album < 0 || s_cf[i].gen != gen) ? 1e9f : fabsf(s_cf[i].vis - center);
            if (d > far_d) { far_d = d; slot = i; }
        }
        if (!s_cf[slot].init) {
            if (!C3D_TexInit(&s_cf[slot].tex, LIB_COVER_SIZE, LIB_COVER_SIZE, GPU_RGB565)) return NULL;
            C3D_TexSetFilter(&s_cf[slot].tex, GPU_LINEAR, GPU_LINEAR);
            s_cf[slot].init = true;
        }
        s_cf[slot].album = album;
        s_cf[slot].gen = gen;
        s_cf[slot].loaded = false;
    }
    s_cf[slot].vis = vis;
    if (!s_cf[slot].loaded && *uploads < 3) {
        if (library_copy_cover(album, (u16*)s_cf[slot].tex.data)) {
            C3D_TexFlush(&s_cf[slot].tex);
            s_cf[slot].loaded = true;
            (*uploads)++;
        }
    }
    return s_cf[slot].loaded ? &s_cf[slot].tex : NULL;
}

// Carátula como trapecio: franjas verticales con altura que va de h_left a h_right.
// dark oscurece (0..1); alpha general 0..255. Con reflejo dibuja debajo la imagen
// invertida desvaneciéndose.
static void draw_cover_trapezoid(C3D_Tex* tex, float cx, float cy, float w, float h_left, float h_right, float z,
                                 float dark, u32 fallback) {
    float x_left = cx - w / 2;
    for (int k = 0; k < CF_STRIPS; k++) {
        float u0 = (float)k / CF_STRIPS, u1 = (float)(k + 1) / CF_STRIPS;
        float h = h_left + (h_right - h_left) * (u0 + u1) * 0.5f;
        float x = x_left + w * u0, sw = w / CF_STRIPS + 0.6f;
        float y = cy - h / 2;
        if (!tex) {
            C2D_DrawRectSolid(x, y, z, sw, h, lerp_color(fallback, C2D_Color32(0, 0, 0, 255), dark));
            C2D_DrawRectSolid(x, cy + h / 2 + 2, z, sw, h * 0.35f, with_alpha(fallback, (u8)(60 * (1 - dark))));
            continue;
        }
        Tex3DS_SubTexture sub = { .width = LIB_COVER_SIZE, .height = LIB_COVER_SIZE,
                                  .left = u0, .right = u1, .top = 1.0f, .bottom = 0.0f };
        C2D_Image img = { tex, &sub };
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, C2D_Color32(0, 0, 0, 255), dark);
        C2D_DrawParams p = { .pos = { x, y, sw, h }, .center = { 0, 0 }, .depth = z, .angle = 0 };
        C2D_DrawImage(img, &p, &tint);

        // Reflejo: la parte de abajo de la carátula, invertida y desvaneciéndose
        float rh = h * 0.42f;
        Tex3DS_SubTexture rsub = { .width = LIB_COVER_SIZE, .height = LIB_COVER_SIZE,
                                   .left = u0, .right = u1, .top = 0.0f, .bottom = 0.42f };
        C2D_Image rimg = { tex, &rsub };
        C2D_ImageTint rt;
        u8 a0 = (u8)(85 * (1 - dark * 0.6f));
        C2D_SetImageTint(&rt, C2D_TopLeft, C2D_Color32(0, 0, 0, a0), 0.35f + dark * 0.5f);
        C2D_SetImageTint(&rt, C2D_TopRight, C2D_Color32(0, 0, 0, a0), 0.35f + dark * 0.5f);
        C2D_SetImageTint(&rt, C2D_BotLeft, C2D_Color32(0, 0, 0, 0), 0.6f);
        C2D_SetImageTint(&rt, C2D_BotRight, C2D_Color32(0, 0, 0, 0), 0.6f);
        C2D_DrawParams rp = { .pos = { x, cy + h / 2 + 2, sw, rh }, .center = { 0, 0 }, .depth = z, .angle = 0 };
        C2D_DrawImage(rimg, &rp, &rt);
    }
}

void gfx_render_coverflow_top(const PlaybackViewModel* playback, const int* albums, int count, float pos,
                              int playing_album) {
    const UITheme* theme = theme_get_current();
    begin_top(theme, playback);

    if (count == 0 && library_album_count() > 0) {
        draw_text_centered(SCREEN_TOP_W / 2.0f, 100, 0.55f, theme->text_primary, "Ningún álbum coincide con el filtro");
    } else if (count == 0) {
        draw_text_centered(SCREEN_TOP_W / 2.0f, 92, 0.55f, theme->text_primary, "No hay música en la tarjeta SD");
        draw_text_centered(SCREEN_TOP_W / 2.0f, 118, 0.42f, theme->text_secondary,
                           "Copia tus MP3 con etiquetas a " LOCAL_MUSIC_HINT);
        draw_text_centered(SCREEN_TOP_W / 2.0f, 138, 0.42f, theme->text_secondary,
                           "o descárgalos desde SoundCloud");
    }

    int center = (int)floorf(pos + 0.5f);
    if (center >= 0 && center < count) library_set_focus(albums[center]);
    int uploads = 0;
    // De lo más lejano a lo más cercano para que el centro quede encima
    for (int ring = 5; ring >= 0; ring--) {
        for (int side = -1; side <= 1; side += 2) {
            if (ring == 0 && side > 0) continue;
            int i = center + ring * side;
            if (i < 0 || i >= count) continue;
            float d = i - pos, ad = fabsf(d);
            if (ad > 5.2f) continue;
            float t = ad < 1 ? ad : 1;
            float sgn = d < 0 ? -1.0f : 1.0f;
            float cx = SCREEN_TOP_W / 2.0f + (ad <= 1 ? d * 96 : sgn * (96 + (ad - 1) * 36));
            float w = CF_SIZE * (1 - 0.52f * t);
            float h_near = CF_SIZE * (1 - 0.03f * t), h_far = CF_SIZE * (1 - 0.30f * t);
            float hl = d > 0 ? h_near : h_far, hr = d > 0 ? h_far : h_near;
            if (ad < 0.01f) hl = hr = CF_SIZE;
            float dark = fminf(ad * 0.32f, 0.62f);
            float z = 0.5f - ad * 0.02f;
            C3D_Tex* tex = cf_texture(albums[i], i, pos, &uploads);
            draw_cover_trapezoid(tex, cx, CF_CY, w, hl, hr, z, dark, theme->panel_bg);

            // Álbum sin carátula: el nombre sobre la portada generada (solo el central)
            const LibAlbum* a = library_album(albums[i]);
            if (ad < 0.5f && a && !a->has_art && !a->is_soundcloud) {
                char fit[96];
                fit_text(a->name, 0.42f, CF_SIZE - 12, fit, sizeof(fit));
                draw_text_centered_z(cx, CF_CY + CF_SIZE / 2 - 18, 0.55f, 0.42f, C2D_Color32(255, 255, 255, 230), fit);
            }
        }
    }

    // Nombre y artista del álbum enfocado
    const LibAlbum* a = (center >= 0 && center < count) ? library_album(albums[center]) : NULL;
    if (a) {
        char fit[128];
        fit_text(a->name, 0.55f, 360, fit, sizeof(fit));
        draw_text_centered_z(SCREEN_TOP_W / 2.0f, 172, 0.7f, 0.55f, theme->text_primary, fit);
        char sub[128];
        snprintf(sub, sizeof(sub), "%s - %d %s", a->artist, a->count, L(a->count == 1 ? "canción" : "canciones"));
        fit_text(sub, 0.42f, 360, fit, sizeof(fit));
        draw_text_centered_z(SCREEN_TOP_W / 2.0f, 190, 0.7f, 0.42f, theme->text_secondary, fit);
        char idx[32];
        snprintf(idx, sizeof(idx), "%d / %d", center + 1, count);
        draw_text_z(12, 26, 0.7f, 0.38f, theme->text_muted, idx);
        if (center == playing_album) icon_note(SCREEN_TOP_W - 22, 38, 12, theme->accent);
    }

    // Barra "sonando ahora" (o el aviso temporal)
    float by = 212;
    fill_round_rect(10, by, SCREEN_TOP_W - 20, 24, 8, 0.7f, theme->panel_bg);
    if (playback->status_msg[0]) {
        char fit[96];
        fit_text(playback->status_msg, 0.40f, SCREEN_TOP_W - 40, fit, sizeof(fit));
        draw_text_z(20, by + 5, 0.75f, 0.40f, C2D_Color32(250, 180, 60, 255), fit);
    } else if (playing_album >= 0 || playback->is_playing) {
        if (playback->is_playing) icon_pause(24, by + 12, 10, theme->accent);
        else icon_play(24, by + 12, 10, theme->accent);
        char np[200], fit[160];
        if (playback->artist[0]) snprintf(np, sizeof(np), "%s - %s", playback->title, playback->artist);
        else snprintf(np, sizeof(np), "%s", playback->title);
        fit_text(np, 0.40f, 270, fit, sizeof(fit));
        draw_text_z(36, by + 5, 0.75f, 0.40f, theme->text_primary, fit);
        char tm[24];
        uint32_t cur = playback->elapsed_ms / 1000, tot = playback->duration_ms / 1000;
        snprintf(tm, sizeof(tm), "%lu:%02lu / %lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60),
                 (unsigned long)(tot / 60), (unsigned long)(tot % 60));
        draw_text_z(SCREEN_TOP_W - 86, by + 6, 0.75f, 0.36f, theme->text_secondary, tm);
        float ratio = playback->duration_ms ? fminf((float)playback->elapsed_ms / playback->duration_ms, 1.0f) : 0;
        C2D_DrawRectSolid(18, by + 21, 0.76f, (SCREEN_TOP_W - 36) * ratio, 2, theme->progress_fill);
    } else {
        draw_text_z(20, by + 5, 0.75f, 0.40f, theme->text_muted, "D-pad izq./der.: álbum - arriba/abajo: canción - A: reproducir");
    }
}

// ---------------------------------------------------------------------------
// Minijuego "Cubo vs. Ruido" en la pantalla superior
// ---------------------------------------------------------------------------

// Bloque de ruido: cuadrado girando con un núcleo oscuro (en Y2K, bloques rectos)
static void draw_hazard(const GameHazard* h, float ox, float oy, bool blocky, u32 color) {
    float x = h->x + ox, y = h->y + oy, r = h->r;
    if (blocky) {
        C2D_DrawRectSolid(x - r, y - r, 0.62f, 2 * r, 2 * r, color);
        C2D_DrawRectSolid(x - r * 0.45f, y - r * 0.45f, 0.63f, r * 0.9f, r * 0.9f, shade(color, 0.45f));
        return;
    }
    float c = cosf(h->rot), s = sinf(h->rot);
    for (int layer = 0; layer < 2; layer++) {
        float k = layer == 0 ? r * 1.2f : r * 0.55f;
        u32 col = layer == 0 ? color : shade(color, 0.45f);
        float ax = c * k, ay = s * k;   // medio lado rotado
        fill_quad(x - ax + ay, y - ay - ax, x + ax + ay, y + ay - ax, x + ax - ay, y + ay + ax,
                  x - ax - ay, y - ay + ax, 0.62f + layer * 0.01f, col);
    }
}

void gfx_render_game_top(const PlaybackViewModel* playback, const GameState* g, float energy) {
    const UITheme* theme = theme_get_current();
    begin_top(theme, playback);
    float t = anim_time();
    const u32 noise = C2D_Color32(239, 68, 68, 255);
    const u32 white = C2D_Color32(255, 255, 255, 255);

    // Sacudida tras un golpe
    float ox = 0, oy = 0;
    if (g->shake > 0) {
        float k = g->shake / 0.35f;
        ox = sinf(t * 95.0f) * 4.0f * k;
        oy = cosf(t * 83.0f) * 3.0f * k;
    }

    // Rejilla de puntos que late con los graves
    u32 grid = with_alpha(theme->text_muted, (u8)(35 + 90 * fminf(energy * 1.6f, 1.0f)));
    for (int gy = 36; gy < SCREEN_TOP_H; gy += 24)
        for (int gx = 14; gx < SCREEN_TOP_W; gx += 24)
            C2D_DrawRectSolid(gx + ox, gy + oy, 0.2f, 2, 2, grid);

    // Notas: flotan y parpadean antes de desaparecer
    for (int i = 0; i < GAME_MAX_NOTES; i++) {
        const GameNote* n = &g->notes[i];
        if (!n->alive) continue;
        if (n->age > 5.5f && fmodf(t, 0.2f) < 0.1f) continue;
        float bob = sinf(t * 4.0f + i) * 2.5f;
        fill_circle(n->x + ox, n->y + bob + oy, 0.55f, 12, with_alpha(theme->accent, 55));
        icon_note(n->x - 3 + ox, n->y + 4 + bob + oy, 14, theme->accent);
    }

    // Ruido
    for (int i = 0; i < GAME_MAX_HAZARDS; i++)
        if (g->hazards[i].alive) draw_hazard(&g->hazards[i], ox, oy, theme->is_segmented, noise);

    // Onda expansiva
    if (g->wave_t >= 0) {
        float k = g->wave_t / 0.45f;
        draw_ring(g->wave_x + ox, g->wave_y + oy, 130.0f * k, 3.0f, with_alpha(theme->accent, (u8)(230 * (1 - k))));
        draw_ring(g->wave_x + ox, g->wave_y + oy, 130.0f * k * 0.7f, 2.0f, with_alpha(theme->accent, (u8)(120 * (1 - k))));
    }

    // Estela del impulso
    if (g->dash_t > 0) {
        for (int i = 1; i <= 3; i++) {
            fill_ellipse(g->px - g->dir_x * i * 9 + ox, g->py - g->dir_y * i * 9 + oy, 11 - i * 2, 10 - i * 2, 0.66f,
                         with_alpha(theme->mascot_body, (u8)(150 - i * 40)));
        }
    }

    // El cubo (parpadea mientras es invulnerable)
    bool blink = g->invuln > 0 && fmodf(t, 0.16f) < 0.08f;
    if (!blink && !(g->phase == GAME_OVER)) {
        MascotViewModel m = { .state = MASCOT_AWAKE, .energy = energy, .state_since = 0 };
        draw_mascot(g->px + ox, g->py + 12 + oy, 0.5f, &m, theme);
    }

    // Partículas
    for (int i = 0; i < GAME_MAX_PARTICLES; i++) {
        const GameParticle* p = &g->parts[i];
        if (p->life <= 0) continue;
        u32 col = with_alpha(p->hazard ? noise : theme->mascot_body, (u8)(255 * fminf(p->life / 0.4f, 1.0f)));
        C2D_DrawRectSolid(p->x - 1.5f + ox, p->y - 1.5f + oy, 0.82f, 3, 3, col);
    }

    // HUD: puntos, vidas, récord y carga de la onda
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", g->score);
    draw_text_z(10, 26, 0.9f, 0.6f, theme->text_primary, buf);
    for (int i = 0; i < GAME_LIVES; i++) {
        u32 c = i < g->lives ? theme->mascot_body : with_alpha(theme->text_muted, 70);
        C2D_DrawRectSolid(12 + i * 13, 48, 0.9f, 9, 8, c);
        C2D_DrawRectSolid(12 + i * 13, 48, 0.91f, 9, 2, i < g->lives ? theme->mascot_light : c);
    }
    snprintf(buf, sizeof(buf), L("Récord %d"), g->best);
    draw_text_z(SCREEN_TOP_W - 92, 27, 0.9f, 0.42f, theme->text_secondary, buf);

    bool wave_ready = g->charge >= GAME_NOTES_FOR_WAVE;
    float pulse = wave_ready ? 0.5f + 0.5f * sinf(t * 8.0f) : 0.0f;
    fill_circle(SCREEN_TOP_W - 84, 50, 0.9f, 7, wave_ready ? lerp_color(theme->accent, white, pulse * 0.4f)
                                                            : theme->control_btn_bg);
    draw_text_centered_z(SCREEN_TOP_W - 84, 44, 0.92f, 0.38f, wave_ready ? white : theme->text_muted, "Y");
    for (int i = 0; i < GAME_NOTES_FOR_WAVE; i++) {
        u32 c = i < g->charge ? theme->accent : with_alpha(theme->text_muted, 70);
        C2D_DrawRectSolid(SCREEN_TOP_W - 72 + i * 15, 47, 0.9f, 11, 6, c);
    }

    // Lo que suena, discreto al pie
    if (playback->title[0]) {
        char np[200];
        if (playback->artist[0]) snprintf(np, sizeof(np), "%s - %s", playback->title, playback->artist);
        else snprintf(np, sizeof(np), "%s", playback->title);
        char fit[160];
        fit_text(np, 0.38f, 300, fit, sizeof(fit));
        draw_text_centered_z(SCREEN_TOP_W / 2.0f, 224, 0.9f, 0.38f, with_alpha(theme->text_muted, 200), fit);
    }

    // Avisos: listo / fin de la partida
    if (g->phase == GAME_READY) {
        fill_round_rect(110, 92, 180, 62, 10, 0.93f, with_alpha(theme->panel_bg, 235));
        draw_text_centered_z(200, 98, 0.95f, 0.7f, theme->text_primary, "¿Listo?");
        draw_text_centered_z(200, 128, 0.95f, 0.36f, theme->text_secondary, "Analógico mover - X impulso - Y onda");
    } else if (g->phase == GAME_OVER) {
        fill_round_rect(100, 74, 200, 100, 10, 0.93f, with_alpha(theme->panel_bg, 240));
        draw_text_centered_z(200, 80, 0.95f, 0.6f, theme->text_primary, "Fin de la partida");
        snprintf(buf, sizeof(buf), L("Puntos %d"), g->score);
        draw_text_centered_z(200, 108, 0.95f, 0.5f, theme->accent, buf);
        draw_text_centered_z(200, 130, 0.95f, 0.4f, g->new_best ? theme->accent : theme->text_secondary,
                             g->new_best ? "Nuevo récord" : "Sigue intentando");
        draw_text_centered_z(200, 152, 0.95f, 0.36f, theme->text_muted, "X para jugar otra vez");
    }
}

// ---------------------------------------------------------------------------
// Menú principal
// ---------------------------------------------------------------------------

static const char* mascot_caption(MascotState st) {
    switch (st) {
        case MASCOT_SLEEP:  return "Zzz... toca algo para despertarlo";
        case MASCOT_CLOUD:  return "Streaming desde SoundCloud";
        case MASCOT_LOCAL:  return "Música de tu tarjeta SD";
        case MASCOT_SEARCH: return "Buscando...";
        case MASCOT_LOADING: return "Cargando la canción...";
        case MASCOT_DANCE:  return "¡Qué temazo!";
        default:            return "Elige qué quieres escuchar";
    }
}

void gfx_render_menu_top(const PlaybackViewModel* playback, const MascotViewModel* mascot) {
    const UITheme* theme = theme_get_current();
    begin_top(theme, playback);

    // Cubo grande en el centro
    draw_mascot(200, 196, 2.2f, mascot, theme);

    draw_text_centered(SCREEN_TOP_W / 2.0f, 218, 0.45f, theme->text_secondary, mascot_caption(mascot->state));
}

void gfx_render_menu_bottom(const MenuViewModel* menu, bool touch_down, int touch_x, int touch_y) {
    const UITheme* theme = theme_get_current();
    C2D_TargetClear(s_gfx.bottomTarget, theme->bg_bottom);
    C2D_SceneBegin(s_gfx.bottomTarget);

    draw_text_centered(SCREEN_BOT_W / 2.0f, 9, 0.45f, theme->text_secondary, "¿Qué quieres escuchar?");

    static const char* const titles[MENU_ITEMS] = { "SoundCloud", "Local" };
    static const char* const subtitles[MENU_ITEMS] = { "Busca y escucha en streaming", "MP3 en sdmc:/music" };
    for (int i = 0; i < MENU_ITEMS; i++) {
        float y = i == 0 ? MENU_CARD_Y0 : MENU_CARD_Y1;
        bool focused = (i == menu->cursor);
        bool pressed = touch_down && touch_x >= MENU_CARD_X && touch_x <= MENU_CARD_X + MENU_CARD_W &&
                       touch_y >= y && touch_y <= y + MENU_CARD_H;

        if (focused) fill_round_rect(MENU_CARD_X - 2, y - 2, MENU_CARD_W + 4, MENU_CARD_H + 4, 10, 0.48f, theme->accent);
        fill_round_rect(MENU_CARD_X, y, MENU_CARD_W, MENU_CARD_H, 8, 0.5f, pressed ? theme->control_btn_bg : theme->panel_bg);

        float icx = MENU_CARD_X + 38, icy = y + MENU_CARD_H / 2.0f;
        if (i == 0) {
            fill_circle(icx, icy, 0.55f, 24, C2D_Color32(255, 119, 0, 255));
            icon_cloud(icx - 2, icy + 1, 30, C2D_Color32(255, 255, 255, 255));
        } else {
            fill_circle(icx, icy, 0.55f, 24, theme->accent);
            icon_sdcard(icx, icy, 28, C2D_Color32(255, 255, 255, 255), theme->accent);
        }
        draw_text(MENU_CARD_X + 74, y + 14, 0.62f, theme->text_primary, titles[i]);
        draw_text(MENU_CARD_X + 74, y + 40, 0.40f, theme->text_muted, subtitles[i]);
        icon_chevron(MENU_CARD_X + MENU_CARD_W - 18, icy, 14, true, focused ? theme->accent : theme->text_muted);
    }

    // Tema: mismo botón y lugar que en el reproductor
    bool theme_active = touch_down && touch_x >= THEME_BTN_X - 2 && touch_x <= THEME_BTN_X + BAR_BTN_W + 2 &&
                        touch_y >= 210 && touch_y <= 238;
    u32 theme_bg = theme_active ? theme->accent_active : theme->control_btn_bg;
    fill_round_rect(THEME_BTN_X, 212, BAR_BTN_W, 24, 7, 0.5f, theme_bg);
    icon_palette(THEME_BTN_X + BAR_BTN_W / 2.0f, 224, 20, theme_bg);

    draw_cfg_button(CFG_MENU_BTN_X, touch_down, touch_x, touch_y, theme);

    // Sonando ahora: toca para volver a esa lista
    bool np_active = touch_down && menu->has_track && touch_x >= NOWPLAY_X && touch_x <= SCREEN_BOT_W - 10 &&
                     touch_y >= 210 && touch_y <= 238;
    fill_round_rect(NOWPLAY_X, 212, SCREEN_BOT_W - 10 - NOWPLAY_X, 24, 12, 0.5f,
                    np_active ? theme->accent_active : theme->panel_bg);
    if (menu->has_track) {
        u32 ic = theme->accent;
        if (menu->is_playing) icon_pause(NOWPLAY_X + 15, 224, 10, ic);
        else icon_play(NOWPLAY_X + 15, 224, 10, ic);
        draw_text_fit(NOWPLAY_X + 28, 217, 0.40f, theme->text_primary, menu->now_playing,
                      SCREEN_BOT_W - 10 - NOWPLAY_X - 38);
    } else {
        draw_text(NOWPLAY_X + 14, 217, 0.40f, theme->text_muted, "Nada sonando");
    }
}

// ---------------------------------------------------------------------------
// Configuración
// ---------------------------------------------------------------------------

#define SET_ROW_X 14
#define SET_ROW_W (SCREEN_BOT_W - 28)
static const float s_set_row_y[SETTINGS_ROWS] = { 40, 92, 144, 194 };
static const float s_set_row_h[SETTINGS_ROWS] = { 46, 46, 44, 24 };
#define SET_DISCONNECT_W 96

int gfx_settings_row_at(int x, int y) {
    for (int i = 0; i < SETTINGS_ROWS; i++)
        if (x >= SET_ROW_X && x <= SET_ROW_X + SET_ROW_W && y >= s_set_row_y[i] && y <= s_set_row_y[i] + s_set_row_h[i])
            return i;
    return -1;
}

// Interruptor tipo píldora (encendido a la derecha)
static void draw_switch(float x, float y, bool on, const UITheme* theme) {
    fill_round_rect(x, y, 44, 22, 11, 0.55f, on ? theme->accent : theme->control_btn_bg);
    fill_circle(on ? x + 33 : x + 11, y + 11, 0.6f, 8.5f, C2D_Color32(255, 255, 255, 255));
}

void gfx_render_settings_bottom(const SettingsViewModel* st, bool touch_down, int touch_x, int touch_y) {
    const UITheme* theme = theme_get_current();
    C2D_TargetClear(s_gfx.bottomTarget, theme->bg_bottom);
    C2D_SceneBegin(s_gfx.bottomTarget);

    icon_gear(26, 22, 22, theme->accent, theme->bg_bottom);
    draw_text(44, 12, 0.62f, theme->text_primary, "Configuración");

    int pressed = touch_down ? gfx_settings_row_at(touch_x, touch_y) : -1;
    for (int i = 0; i < SETTINGS_ROWS; i++) {
        float y = s_set_row_y[i], h = s_set_row_h[i];
        bool focused = st->cursor == i;
        if (focused) fill_round_rect(SET_ROW_X - 2, y - 2, SET_ROW_W + 4, h + 4, 10, 0.48f, theme->accent);
        fill_round_rect(SET_ROW_X, y, SET_ROW_W, h, 8, 0.5f, pressed == i ? theme->control_btn_bg : theme->panel_bg);
    }

    // Idioma: dos opciones lado a lado
    draw_text(SET_ROW_X + 12, s_set_row_y[0] + 6, 0.45f, theme->text_secondary, "Idioma");
    const char* langs[2] = { "Español", "English" };
    for (int k = 0; k < 2; k++) {
        bool sel = (k == 1) == st->lang_en;
        float bx = SET_ROW_X + 12 + k * 140, by = s_set_row_y[0] + 22;
        fill_round_rect(bx, by, 128, 22, 8, 0.55f, sel ? theme->accent : theme->control_btn_bg);
        draw_text_centered_z(bx + 64, by + 4, 0.6f, 0.45f, sel ? C2D_Color32(255, 255, 255, 255) : theme->text_secondary,
                             langs[k]);
    }

    // LED RGB
    draw_text(SET_ROW_X + 12, s_set_row_y[1] + 5, 0.5f, theme->text_primary, "LED RGB al reproducir");
    draw_text_fit(SET_ROW_X + 12, s_set_row_y[1] + 25, 0.38f, theme->text_muted,
                  "El LED de notificación brilla en arcoíris", SET_ROW_W - 80);
    draw_switch(SET_ROW_X + SET_ROW_W - 56, s_set_row_y[1] + 12, st->led_rgb, theme);
    // muestra de colores del arcoíris junto al título
    static const u32 dots[6] = { 0xFF4040FF, 0xFF40C0FF, 0xFF40FF40, 0xFFFFFF40, 0xFFFF4040, 0xFFFF40FF };
    for (int k = 0; k < 6; k++)
        fill_circle(SET_ROW_X + 196 + k * 7, s_set_row_y[1] + 11, 0.6f, 2.6f, st->led_rgb ? dots[k] : theme->text_muted);

    // Cuenta de SoundCloud: nombre y Desconectar, o cómo conectarla
    float ay = s_set_row_y[SETTINGS_ROW_ACCOUNT];
    draw_text(SET_ROW_X + 12, ay + 4, 0.45f, theme->text_secondary, "Cuenta de SoundCloud");
    if (st->account[0]) {
        fill_circle(SET_ROW_X + 17, ay + 30, 0.6f, 4.0f, C2D_Color32(34, 197, 94, 255));
        draw_text_fit(SET_ROW_X + 26, ay + 22, 0.5f, theme->text_primary, st->account,
                      SET_ROW_W - SET_DISCONNECT_W - 40);
        float bx = SET_ROW_X + SET_ROW_W - SET_DISCONNECT_W - 8;
        fill_round_rect(bx, ay + 18, SET_DISCONNECT_W, 22, 8, 0.55f, theme->control_btn_bg);
        draw_text_centered_z(bx + SET_DISCONNECT_W / 2.0f, ay + 22, 0.6f, 0.42f, C2D_Color32(239, 68, 68, 255),
                             "Desconectar");
    } else if (st->account_expired) {
        draw_text_fit(SET_ROW_X + 12, ay + 24, 0.36f, C2D_Color32(255, 170, 60, 255),
                      "Venció: reconéctala desde Sound3DS Share", SET_ROW_W - 20);
    } else {
        draw_text_fit(SET_ROW_X + 12, ay + 24, 0.36f, theme->text_muted,
                      "Conéctala desde Sound3DS Share en el PC", SET_ROW_W - 20);
    }

    // Cerrar
    draw_text_centered_z(SCREEN_BOT_W / 2.0f, s_set_row_y[3] + 4, 0.6f, 0.48f, theme->text_primary, "Cerrar");

    draw_text_centered(SCREEN_BOT_W / 2.0f, 224, 0.34f, theme->text_muted,
                       "Toca una opción o usa la cruceta y A - B: volver");
}

void gfx_frame_end(void) {
    C3D_FrameEnd(0);
}
