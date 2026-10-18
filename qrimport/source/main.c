// Sound3DS QR: escanea con la cámara un QR que muestra el PC y descarga las
// canciones a sdmc:/music, la carpeta que lee Sound3DS. Así no hace falta sacar
// la tarjeta SD.
//
// El QR puede traer:
//   - la URL de un archivo .mp3 / .aac  -> se descarga ese archivo
//   - la URL de una lista (texto, una URL por línea, como la que sirve
//     tools/share_music.py)             -> se descargan todas
//   - varias URLs separadas por saltos de línea

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "quirc/quirc.h"

#define CAM_W 400
#define CAM_H 240
#define MUSIC_DIR "sdmc:/music"
#define MAX_URLS 200
#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

typedef enum { ST_SCAN, ST_DONE } State;

static u16* s_cam_buf;
static u32 s_transfer_unit;
static bool s_cam_on = false;
// La cámara se detiene sola si llena su búfer mientras no hay una recepción pedida
// (por ejemplo, mientras se decodifica un QR). Este evento avisa para relanzarla.
static Handle s_cam_buf_err = 0;

// Consola de abajo con colores ANSI
#define C_RESET  "\x1b[0m"
#define C_TITLE  "\x1b[35;1m"
#define C_OK     "\x1b[32;1m"
#define C_WARN   "\x1b[33;1m"
#define C_ERR    "\x1b[31;1m"
#define C_DIM    "\x1b[37m"

static PrintConsole s_con;

// ---------------------------------------------------------------------------
// Cámara exterior (derecha) a 400x240 en RGB565
// ---------------------------------------------------------------------------

static bool cam_start(void) {
    if (s_cam_on) return true;
    if (R_FAILED(camInit())) return false;
    CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    CAMU_SetNoiseFilter(SELECT_OUT1, true);
    CAMU_SetAutoExposure(SELECT_OUT1, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetMaxBytes(&s_transfer_unit, CAM_W, CAM_H);
    CAMU_SetTransferBytes(PORT_CAM1, s_transfer_unit, CAM_W, CAM_H);
    CAMU_GetBufferErrorInterruptEvent(&s_cam_buf_err, PORT_CAM1);
    CAMU_Activate(SELECT_OUT1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
    s_cam_on = true;
    return true;
}

static void cam_stop(void) {
    if (!s_cam_on) return;
    CAMU_StopCapture(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    if (s_cam_buf_err) {
        svcCloseHandle(s_cam_buf_err);
        s_cam_buf_err = 0;
    }
    camExit();
    s_cam_on = false;
}

static void cam_restart(void) {
    CAMU_StopCapture(PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
}

static bool cam_frame(void) {
    // Si la captura se cortó por búfer lleno, volver a arrancarla antes de pedir un cuadro
    if (s_cam_buf_err && svcWaitSynchronization(s_cam_buf_err, 0) == 0) cam_restart();
    Handle ev = 0;
    if (R_FAILED(CAMU_SetReceiving(&ev, s_cam_buf, PORT_CAM1, CAM_W * CAM_H * 2, (s16)s_transfer_unit))) {
        cam_restart();
        return false;
    }
    bool ok = svcWaitSynchronization(ev, 300 * 1000 * 1000LL) == 0;
    svcCloseHandle(ev);
    if (!ok) cam_restart();   // sin cuadro en 300 ms: la captura quedó detenida
    return ok;
}

// Copia el cuadro a la pantalla de arriba (el framebuffer está girado) con un visor
static void draw_preview(bool found) {
    u16* fb = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < CAM_W; x++) {
        u16* col = fb + x * CAM_H;
        for (int y = 0; y < CAM_H; y++) col[CAM_H - 1 - y] = s_cam_buf[y * CAM_W + x];
    }
    // Esquinas del visor (moradas, verdes al reconocer un código)
    u16 c = found ? 0x07E0 : 0xA81F;
    const int x0 = 110, y0 = 30, x1 = 290, y1 = 210, len = 26, th = 3;
    for (int k = 0; k < len; k++)
        for (int t = 0; t < th; t++) {
            int pts[8][2] = {{x0 + k, y0 + t}, {x0 + t, y0 + k}, {x1 - k, y0 + t}, {x1 - t, y0 + k},
                             {x0 + k, y1 - t}, {x0 + t, y1 - k}, {x1 - k, y1 - t}, {x1 - t, y1 - k}};
            for (int p = 0; p < 8; p++) fb[pts[p][0] * CAM_H + (CAM_H - 1 - pts[p][1])] = c;
        }
}

static void clear_top(void) {
    u16* fb = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int i = 0; i < CAM_W * CAM_H; i++) fb[i] = 0x1083;
}

// ---------------------------------------------------------------------------
// Decodificación QR
// ---------------------------------------------------------------------------

static bool decode_qr(struct quirc* q, char* out, size_t out_sz) {
    int w, h;
    uint8_t* img = quirc_begin(q, &w, &h);
    for (int i = 0; i < CAM_W * CAM_H; i++) {
        u16 p = s_cam_buf[i];
        int r = (p >> 11) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
        img[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
    quirc_end(q);
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
            snprintf(out, out_sz, "%.*s", data.payload_len, (const char*)data.payload);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Descargas
// ---------------------------------------------------------------------------

typedef struct {
    char* data;
    size_t size, cap;
} Mem;

static size_t mem_write(void* p, size_t sz, size_t n, void* user) {
    Mem* m = (Mem*)user;
    size_t len = sz * n;
    if (m->size + len + 1 > 256 * 1024) return 0;   // una lista no debería pasar de esto
    if (m->size + len + 1 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 8192;
        while (nc < m->size + len + 1) nc *= 2;
        char* g = realloc(m->data, nc);
        if (!g) return 0;
        m->data = g;
        m->cap = nc;
    }
    memcpy(m->data + m->size, p, len);
    m->size += len;
    m->data[m->size] = 0;
    return len;
}

static void common_opts(CURL* c, const char* url) {
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Sound3DS-QR/1.0");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
}

static bool has_audio_ext(const char* s, size_t len) {
    return len > 4 && (strncasecmp(s + len - 4, ".mp3", 4) == 0 || strncasecmp(s + len - 4, ".aac", 4) == 0);
}

// Largo de la URL sin "?consulta" ni "#fragmento"
static size_t path_len(const char* url) {
    size_t n = strcspn(url, "?#");
    return n;
}

// Nombre de archivo desde la URL: último tramo de la ruta, %XX decodificado y sin
// caracteres que FAT no acepta
static void file_name_from_url(const char* url, char* out, size_t out_sz) {
    size_t n = path_len(url);
    const char* start = url;
    for (size_t i = 0; i < n; i++)
        if (url[i] == '/') start = url + i + 1;
    size_t o = 0;
    for (const char* p = start; p < url + n && o + 1 < out_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' && p + 2 < url + n) {
            char hex[3] = {p[1], p[2], 0};
            c = (unsigned char)strtol(hex, NULL, 16);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) c = '_';
        out[o++] = (char)c;
    }
    out[o] = 0;
    if (o == 0) snprintf(out, out_sz, "cancion.mp3");
}

static int s_last_pct = -1;

static int progress_cb(void* user, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)user; (void)ut; (void)un;
    hidScanInput();
    if (hidKeysDown() & KEY_B) return 1;   // (B) cancela
    int pct = total > 0 ? (int)(now * 100 / total) : -1;
    if (pct != s_last_pct) {
        s_last_pct = pct;
        int bars = pct < 0 ? 0 : pct * 30 / 100;
        printf("\r  [");
        for (int i = 0; i < 30; i++) putchar(i < bars ? '#' : '.');
        if (pct >= 0) printf("] %3d%%  ", pct);
        else printf("] %4lld KB", (long long)(now / 1024));
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
    return 0;
}

typedef enum { DL_OK, DL_EXISTS, DL_FAIL, DL_CANCEL } DlResult;

static DlResult download_file(const char* url) {
    char name[160], path[256], part[264];
    file_name_from_url(url, name, sizeof(name));
    snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);
    snprintf(part, sizeof(part), "%s.part", path);

    printf(C_RESET "%.38s\n", name);
    FILE* exists = fopen(path, "rb");
    if (exists) {
        fclose(exists);
        printf(C_WARN "  ya estaba en la SD\n" C_RESET);
        return DL_EXISTS;
    }
    FILE* f = fopen(part, "wb");
    if (!f) {
        printf(C_ERR "  no se pudo crear el archivo\n" C_RESET);
        return DL_FAIL;
    }
    CURL* c = curl_easy_init();
    common_opts(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    s_last_pct = -1;
    CURLcode res = curl_easy_perform(c);
    curl_easy_cleanup(c);
    bool closed = fclose(f) == 0;
    printf("\r%40s\r", "");

    if (res == CURLE_OK && closed && rename(part, path) == 0) {
        printf(C_OK "  listo\n" C_RESET);
        return DL_OK;
    }
    remove(part);
    if (res == CURLE_ABORTED_BY_CALLBACK) {
        printf(C_WARN "  cancelado\n" C_RESET);
        return DL_CANCEL;
    }
    printf(C_ERR "  error: %.30s\n" C_RESET, curl_easy_strerror(res));
    return DL_FAIL;
}

// Convierte una línea en URL absoluta (relativa a la URL de la lista si hace falta)
static void absolute_url(const char* base, const char* line, char* out, size_t out_sz) {
    if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", line);
        return;
    }
    const char* host_end = strstr(base, "://");
    host_end = host_end ? strchr(host_end + 3, '/') : NULL;
    if (line[0] == '/') {
        int n = host_end ? (int)(host_end - base) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s", n, base, line);
    } else {
        const char* slash = strrchr(base, '/');
        int n = slash && slash > strstr(base, "://") + 2 ? (int)(slash - base + 1) : (int)strlen(base);
        snprintf(out, out_sz, "%.*s%s%s", n, base, (slash ? "" : "/"), line);
    }
}

// Reúne las URLs de canciones a partir del contenido del QR
static int collect_urls(const char* qr, char urls[][512], int max) {
    int n = 0;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", qr);
    char* save = NULL;
    for (char* line = strtok_r(buf, "\r\n", &save); line && n < max; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ') line++;
        if (strncmp(line, "http", 4) != 0) continue;
        if (has_audio_ext(line, path_len(line))) {
            snprintf(urls[n++], 512, "%s", line);
            continue;
        }
        // No es una canción: se trata como una lista que hay que descargar
        printf(C_DIM "Leyendo la lista...\n" C_RESET);
        Mem m = {0};
        CURL* c = curl_easy_init();
        common_opts(c, line);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        CURLcode res = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (res != CURLE_OK || !m.data) {
            printf(C_ERR "No se pudo leer la lista:\n  %.34s\n" C_RESET, curl_easy_strerror(res));
            free(m.data);
            continue;
        }
        char* save2 = NULL;
        for (char* l = strtok_r(m.data, "\r\n", &save2); l && n < max; l = strtok_r(NULL, "\r\n", &save2)) {
            while (*l == ' ') l++;
            if (!*l || *l == '#') continue;
            char abs[512];
            absolute_url(line, l, abs, sizeof(abs));
            if (has_audio_ext(abs, path_len(abs))) snprintf(urls[n++], 512, "%s", abs);
        }
        free(m.data);
    }
    return n;
}

// ---------------------------------------------------------------------------

static void show_intro(void) {
    consoleClear();
    printf(C_TITLE "  Sound3DS QR\n" C_RESET);
    printf(C_DIM "  ------------------------------------\n" C_RESET);
    printf("  Apunta la camara al QR que muestra\n");
    printf("  el PC con tools/share_music.py.\n\n");
    printf("  Las canciones se guardan en\n");
    printf(C_OK "  " MUSIC_DIR "\n\n" C_RESET);
    printf("  Abre Sound3DS > Local para verlas.\n\n");
    printf(C_DIM "  START: salir\n" C_RESET);
}

int main(int argc, char** argv) {
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, &s_con);

    mkdir(MUSIC_DIR, 0777);

    u32* soc_buf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    bool net_ok = soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUFFERSIZE));
    curl_global_init(CURL_GLOBAL_ALL);

    s_cam_buf = (u16*)malloc(CAM_W * CAM_H * 2);
    struct quirc* q = quirc_new();
    bool ready = s_cam_buf && q && quirc_resize(q, CAM_W, CAM_H) >= 0 && cam_start();

    show_intro();
    if (!net_ok) printf(C_ERR "\n  Sin red: activa el Wi-Fi.\n" C_RESET);
    if (!ready) printf(C_ERR "\n  No se pudo abrir la camara.\n" C_RESET);

    State st = ST_SCAN;
    static char urls[MAX_URLS][512];
    char qr[1024];
    int frame = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) break;

        if (st == ST_SCAN && ready) {
            bool got = cam_frame();
            bool found = false;
            if (got && (++frame % 2) == 0) found = decode_qr(q, qr, sizeof(qr));
            if (got) draw_preview(found);
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();

            if (found) {
                cam_stop();
                clear_top();
                gfxFlushBuffers();
                gfxSwapBuffers();
                consoleClear();
                printf(C_TITLE "  QR leido\n" C_RESET C_DIM "  %.70s\n\n" C_RESET, qr);
                int n = collect_urls(qr, urls, MAX_URLS);
                int ok = 0, skipped = 0, failed = 0;
                if (n == 0) printf(C_ERR "No hay canciones (.mp3/.aac) en el QR.\n" C_RESET);
                for (int i = 0; i < n; i++) {
                    printf(C_DIM "[%d/%d] " C_RESET, i + 1, n);
                    DlResult r = download_file(urls[i]);
                    if (r == DL_OK) ok++;
                    else if (r == DL_EXISTS) skipped++;
                    else if (r == DL_CANCEL) break;
                    else failed++;
                }
                printf("\n" C_TITLE "  Resumen\n" C_RESET);
                printf(C_OK "  %d nuevas" C_RESET ", %d ya estaban, " C_ERR "%d errores\n" C_RESET, ok, skipped, failed);
                printf("\n  A: escanear otro QR   START: salir\n");
                st = ST_DONE;
            }
        } else {
            clear_top();   // con doble búfer hay que limpiar los dos: se hace en cada vuelta
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
            if (st == ST_DONE && (down & KEY_A)) {
                show_intro();
                ready = cam_start();
                st = ST_SCAN;
            }
        }
    }

    cam_stop();
    if (q) quirc_destroy(q);
    free(s_cam_buf);
    curl_global_cleanup();
    if (net_ok) socExit();
    free(soc_buf);
    gfxExit();
    return 0;
}
