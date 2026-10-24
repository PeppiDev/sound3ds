#include "library.h"
#include "debuglog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define STBI_NO_STDIO
#include "stb_image.h"

#define COVER_DIR      "sdmc:/3ds/sound3ds/covers"
#define COVER_PIXELS   (LIB_COVER_SIZE * LIB_COVER_SIZE)
#define COVER_BYTES    (COVER_PIXELS * 2)
#define MAX_RESIDENT   72     // carátulas en memoria a la vez (~2,3 MB)
#define MAX_IMAGE_SIDE 3000   // imágenes más grandes se ignoran (memoria)

typedef enum { COVER_PENDING = 0, COVER_READY, COVER_FAILED } CoverState;

typedef struct {
    LibAlbum info;
    // Pista de la que se toma la carátula (copia: la biblioteca no depende del arreglo de pistas)
    LocalTrack art;
    volatile CoverState state;
    u16* pixels;            // COVER_PIXELS en mosaico, NULL si no está en memoria
} Album;

static Album s_albums[LIB_MAX_ALBUMS];
static int s_album_count = 0;
static int s_track_album[LOCAL_MAX_TRACKS];
static int s_track_count = 0;
static volatile int s_focus = 0;
static u32 s_generation = 0;

static LightLock s_lock;
static bool s_lock_ready = false;
static Thread s_thread = NULL;
static volatile bool s_stop = false;

// ---------------------------------------------------------------------------
// Píxeles
// ---------------------------------------------------------------------------

static const uint8_t s_morton[8][8] = {
    { 0,  1,  4,  5, 16, 17, 20, 21 },
    { 2,  3,  6,  7, 18, 19, 22, 23 },
    { 8,  9, 12, 13, 24, 25, 28, 29 },
    {10, 11, 14, 15, 26, 27, 30, 31 },
    {32, 33, 36, 37, 48, 49, 52, 53 },
    {34, 35, 38, 39, 50, 51, 54, 55 },
    {40, 41, 44, 45, 56, 57, 60, 61 },
    {42, 43, 46, 47, 58, 59, 62, 63 }
};

static inline int tiled_index(int x, int y) {
    return ((y >> 3) * (LIB_COVER_SIZE >> 3) + (x >> 3)) * 64 + s_morton[y & 7][x & 7];
}

static inline u16 rgb565(int r, int g, int b) {
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// RGB 128x128 lineal -> RGB565 en mosaico
static void tile_rgb(const uint8_t* rgb, u16* out) {
    for (int y = 0; y < LIB_COVER_SIZE; y++)
        for (int x = 0; x < LIB_COVER_SIZE; x++) {
            const uint8_t* p = rgb + (y * LIB_COVER_SIZE + x) * 3;
            out[tiled_index(x, y)] = rgb565(p[0], p[1], p[2]);
        }
}

// Reduce una imagen RGB a 128x128 promediando bloques (recorte cuadrado centrado)
static void downscale(const uint8_t* src, int w, int h, uint8_t* dst) {
    int side = w < h ? w : h;
    int ox = (w - side) / 2, oy = (h - side) / 2;
    for (int y = 0; y < LIB_COVER_SIZE; y++) {
        int y0 = oy + y * side / LIB_COVER_SIZE, y1 = oy + (y + 1) * side / LIB_COVER_SIZE;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < LIB_COVER_SIZE; x++) {
            int x0 = ox + x * side / LIB_COVER_SIZE, x1 = ox + (x + 1) * side / LIB_COVER_SIZE;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            // Como mucho 4x4 muestras por bloque: basta para que no se vea granulado
            int sy = (y1 - y0 + 3) / 4, sx = (x1 - x0 + 3) / 4;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx) {
                    const uint8_t* p = src + ((size_t)yy * w + xx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            uint8_t* d = dst + (y * LIB_COVER_SIZE + x) * 3;
            d[0] = (uint8_t)(r / n); d[1] = (uint8_t)(g / n); d[2] = (uint8_t)(b / n);
        }
    }
}

static void put_px(uint8_t* rgb, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= LIB_COVER_SIZE || y >= LIB_COVER_SIZE) return;
    uint8_t* p = rgb + (y * LIB_COVER_SIZE + x) * 3;
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
}

static bool in_circle(int x, int y, int cx, int cy, int r) {
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

// Carátula del álbum SoundCloud: degradado naranja con una nube blanca
static void make_soundcloud_cover(uint8_t* rgb) {
    for (int y = 0; y < LIB_COVER_SIZE; y++) {
        int g = 136 - y * 85 / LIB_COVER_SIZE;
        for (int x = 0; x < LIB_COVER_SIZE; x++) {
            bool cloud = in_circle(x, y, 46, 70, 17) || in_circle(x, y, 66, 58, 24) || in_circle(x, y, 88, 70, 17) ||
                         (x >= 46 && x <= 88 && y >= 64 && y <= 87);
            // Barras a la izquierda, como el logo
            bool bars = y >= 64 && y <= 87 && x >= 18 && x <= 39 && (x - 18) % 5 < 3 && y >= 87 - (x - 18) * 1;
            if (cloud || bars) put_px(rgb, x, y, 255, 255, 255);
            else put_px(rgb, x, y, 255, g, 0);
        }
    }
}

// Canciones sueltas del servidor propio: azul con el ícono de servidor (tres bandejas)
static void make_server_cover(uint8_t* rgb) {
    for (int y = 0; y < LIB_COVER_SIZE; y++) {
        int k = 100 - y * 40 / LIB_COVER_SIZE;
        for (int x = 0; x < LIB_COVER_SIZE; x++) {
            bool tray = x >= 30 && x <= 98 && ((y >= 30 && y <= 48) || (y >= 55 && y <= 73) || (y >= 80 && y <= 98));
            bool led = tray && x >= 38 && x <= 44 && ((y >= 36 && y <= 42) || (y >= 61 && y <= 67) || (y >= 86 && y <= 92));
            if (led) put_px(rgb, x, y, 52 * k / 100, 152 * k / 100, 219 * k / 100);
            else if (tray) put_px(rgb, x, y, 255, 255, 255);
            else put_px(rgb, x, y, 52 * k / 100, 152 * k / 100, 219 * k / 100);
        }
    }
}

// ¿Canción suelta descargada del servidor? Quedan en sdmc:/music/<nombre> con ese mismo
// nombre como álbum y como artista del álbum (así las etiqueta la descarga)
static bool is_server_single(const LocalTrack* t) {
    if (!t->album[0] || strcasecmp(t->album, t->album_artist) != 0 || strcasecmp(t->album, LOCAL_SC_ALBUM) == 0)
        return false;
    char dir[160];
    snprintf(dir, sizeof(dir), "sdmc:/music/%s/", t->album);
    size_t n = strlen(dir);
    return strncasecmp(t->path, dir, n) == 0 && !strchr(t->path + n, '/');
}

// Carátula genérica: degradado de un color propio del álbum con una corchea
static void make_generic_cover(uint8_t* rgb, const char* name) {
    u32 h = 2166136261u;
    for (const char* p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    int base[3] = { 60 + (int)(h & 0x7F), 60 + (int)((h >> 8) & 0x7F), 60 + (int)((h >> 16) & 0x7F) };
    for (int y = 0; y < LIB_COVER_SIZE; y++) {
        int k = 100 - y * 45 / LIB_COVER_SIZE;
        for (int x = 0; x < LIB_COVER_SIZE; x++) {
            bool head = ((x - 54) * (x - 54) * 100 / 196 + (y - 88) * (y - 88) * 100 / 100) <= 100;
            bool stem = x >= 62 && x <= 67 && y >= 36 && y <= 88;
            bool flag = y >= 36 && y <= 44 + (x - 67) / 3 && x >= 67 && x <= 86 && y >= 36 + (x - 67) / 2 - 2;
            if (head || stem || flag) put_px(rgb, x, y, 245, 245, 250);
            else put_px(rgb, x, y, base[0] * k / 100, base[1] * k / 100, base[2] * k / 100);
        }
    }
}

// ---------------------------------------------------------------------------
// Caché en la SD
// ---------------------------------------------------------------------------

static void cache_path(const LocalTrack* t, char* out, size_t out_sz) {
    u32 h = 2166136261u;
    for (const char* p = t->path; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h ^= t->file_size * 2654435761u;
    h ^= t->cover_len * 40503u;
    snprintf(out, out_sz, COVER_DIR "/%08lx.c565", (unsigned long)h);
}

static bool cache_load(const LocalTrack* t, u16* out) {
    char path[64];
    cache_path(t, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "C565", 4) == 0 &&
              fread(out, 1, COVER_BYTES, f) == COVER_BYTES;
    fclose(f);
    return ok;
}

static void cache_save(const LocalTrack* t, const u16* px) {
    char path[64];
    cache_path(t, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return;
    bool ok = fwrite("C565", 1, 4, f) == 4 && fwrite(px, 1, COVER_BYTES, f) == COVER_BYTES;
    fclose(f);
    if (!ok) remove(path);
}

// ---------------------------------------------------------------------------
// Hilo de carga
// ---------------------------------------------------------------------------

// Prepara la carátula del álbum en `out`. false si no se pudo (queda la genérica).
static bool load_cover(Album* a, u16* out, uint8_t* rgb) {
    if (a->info.is_soundcloud || a->info.is_server) {
        if (a->info.is_soundcloud) make_soundcloud_cover(rgb);
        else make_server_cover(rgb);
        tile_rgb(rgb, out);
        return true;
    }
    if (a->info.has_art) {
        if (cache_load(&a->art, out)) return true;
        size_t len = 0;
        uint8_t* img = local_read_cover(&a->art, &len);
        if (img) {
            int w = 0, h = 0, comp = 0;
            bool fits = stbi_info_from_memory(img, (int)len, &w, &h, &comp) && w <= MAX_IMAGE_SIDE && h <= MAX_IMAGE_SIDE;
            uint8_t* pix = fits ? stbi_load_from_memory(img, (int)len, &w, &h, &comp, 3) : NULL;
            free(img);
            if (pix) {
                downscale(pix, w, h, rgb);
                stbi_image_free(pix);
                tile_rgb(rgb, out);
                cache_save(&a->art, out);
                return true;
            }
        }
    }
    make_generic_cover(rgb, a->info.name);
    tile_rgb(rgb, out);
    return true;
}

static int resident_count(void) {
    int n = 0;
    for (int i = 0; i < s_album_count; i++) n += s_albums[i].pixels != NULL;
    return n;
}

// Libera la carátula más lejana al foco para no pasar de MAX_RESIDENT
static void evict_far(int focus) {
    int far = -1, far_d = -1;
    for (int i = 0; i < s_album_count; i++) {
        if (!s_albums[i].pixels) continue;
        int d = abs(i - focus);
        if (d > far_d) { far_d = d; far = i; }
    }
    if (far < 0 || far_d <= 6) return;
    LightLock_Lock(&s_lock);
    free(s_albums[far].pixels);
    s_albums[far].pixels = NULL;
    s_albums[far].state = COVER_PENDING;
    LightLock_Unlock(&s_lock);
}

static void cover_worker(void* arg) {
    uint8_t* rgb = (uint8_t*)malloc(COVER_PIXELS * 3);
    if (!rgb) return;
    while (!s_stop) {
        int focus = s_focus;
        // Pendiente más cercano al foco (alternando a ambos lados)
        int pick = -1;
        for (int d = 0; d < s_album_count && pick < 0; d++) {
            if (d > MAX_RESIDENT / 2 - 2) break;   // fuera de la ventana: no cargar todavía
            int c[2] = { focus + d, focus - d };
            for (int k = 0; k < 2; k++)
                if (c[k] >= 0 && c[k] < s_album_count && s_albums[c[k]].state == COVER_PENDING) { pick = c[k]; break; }
        }
        if (pick < 0) {
            svcSleepThread(60 * 1000 * 1000LL);
            continue;
        }
        if (resident_count() >= MAX_RESIDENT) evict_far(focus);

        u16* px = (u16*)malloc(COVER_BYTES);
        if (!px) { svcSleepThread(200 * 1000 * 1000LL); continue; }
        bool ok = load_cover(&s_albums[pick], px, rgb);
        LightLock_Lock(&s_lock);
        if (ok) {
            s_albums[pick].pixels = px;
            s_albums[pick].state = COVER_READY;
        } else {
            free(px);
            s_albums[pick].state = COVER_FAILED;
        }
        LightLock_Unlock(&s_lock);
    }
    free(rgb);
}

static void stop_thread(void) {
    s_stop = true;
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    s_stop = false;
}

static void free_covers(void) {
    for (int i = 0; i < s_album_count; i++) {
        free(s_albums[i].pixels);
        s_albums[i].pixels = NULL;
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void library_build(const LocalTrack* tracks, int count) {
    if (!s_lock_ready) {
        LightLock_Init(&s_lock);
        s_lock_ready = true;
    }
    stop_thread();
    free_covers();
    s_album_count = 0;
    s_track_count = count < LOCAL_MAX_TRACKS ? count : LOCAL_MAX_TRACKS;
    s_generation++;
    s_focus = 0;

    for (int i = 0; i < count && i < LOCAL_MAX_TRACKS; i++) {
        const LocalTrack* t = &tracks[i];
        Album* a = s_album_count > 0 ? &s_albums[s_album_count - 1] : NULL;
        bool same = a && strcasecmp(a->info.name, t->album[0] ? t->album : "Sin álbum") == 0 &&
                    a->info.is_unknown == (t->album[0] == '\0');
        if (!same) {
            a = &s_albums[s_album_count++];
            memset(a, 0, sizeof(*a));
            snprintf(a->info.name, sizeof(a->info.name), "%s", t->album[0] ? t->album : "Sin álbum");
            a->info.is_unknown = t->album[0] == '\0';
            a->info.is_soundcloud = strcasecmp(t->album, LOCAL_SC_ALBUM) == 0;
            a->info.is_server = is_server_single(t);
            a->info.first = i;
            snprintf(a->info.artist, sizeof(a->info.artist), "%s", t->album_artist[0] ? t->album_artist : t->artist);
        } else if (!t->album_artist[0] && strcasecmp(a->info.artist, t->artist) != 0 && !a->info.is_soundcloud) {
            snprintf(a->info.artist, sizeof(a->info.artist), "Varios artistas");
        }
        a->info.count++;
        if (!a->info.has_art && t->cover_len > 0) {
            a->info.has_art = true;
            a->art = *t;
        }
        s_track_album[i] = s_album_count - 1;
    }
    for (int i = 0; i < s_album_count; i++) {
        if (s_albums[i].info.is_soundcloud || s_albums[i].info.is_server) snprintf(s_albums[i].info.artist, sizeof(s_albums[i].info.artist), "Descargas");
        if (s_albums[i].info.is_unknown) snprintf(s_albums[i].info.artist, sizeof(s_albums[i].info.artist), "Varios artistas");
    }

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/sound3ds", 0777);
    mkdir(COVER_DIR, 0777);

    if (s_album_count > 0) {
        // Prioridad mínima: el audio y la red siempre van antes
        s_thread = threadCreate(cover_worker, NULL, 32 * 1024, 0x3F, -2, false);
    }
    dlog("biblioteca: %d pistas, %d álbumes", count, s_album_count);
}

void library_exit(void) {
    stop_thread();
    free_covers();
    s_album_count = 0;
    s_track_count = 0;
}

int library_album_count(void) {
    return s_album_count;
}

const LibAlbum* library_album(int i) {
    return (i >= 0 && i < s_album_count) ? &s_albums[i].info : NULL;
}

int library_album_of_track(int track_idx) {
    if (track_idx < 0 || track_idx >= s_track_count) return -1;
    return s_track_album[track_idx];
}

void library_set_focus(int album) {
    s_focus = album;
}

u32 library_generation(void) {
    return s_generation;
}

bool library_copy_cover(int album, u16* dst) {
    if (album < 0 || album >= s_album_count || !s_lock_ready) return false;
    bool ok = false;
    LightLock_Lock(&s_lock);
    if (s_albums[album].state == COVER_READY && s_albums[album].pixels) {
        memcpy(dst, s_albums[album].pixels, COVER_BYTES);
        ok = true;
    }
    LightLock_Unlock(&s_lock);
    return ok;
}
