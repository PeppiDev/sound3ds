#include "local.h"
#include "audio.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define LOCAL_THREAD_STACK_SZ (16 * 1024)
#define LOCAL_READ_CHUNK      (16 * 1024)
#define LOCAL_MAX_DEPTH       3

static const char* const s_music_dirs[] = {
    "sdmc:/music",
    "sdmc:/3ds/sound3ds/music",
};

static struct {
    Thread thread;
    volatile bool streaming;
    volatile bool stop;
    char path[256];
    float ratio;
    char error[64];
} s_local;

// ---------------------------------------------------------------------------
// Etiquetas ID3v2 (título y artista)
// ---------------------------------------------------------------------------

static size_t syncsafe(const uint8_t* p) {
    return ((size_t)(p[0] & 0x7F) << 21) | ((size_t)(p[1] & 0x7F) << 14) | ((size_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// Escribe un código Unicode como UTF-8; devuelve los bytes usados (0 si no cabe)
static size_t put_utf8(char* out, size_t room, uint32_t cp) {
    if (cp < 0x80 && room > 1) { out[0] = (char)cp; return 1; }
    if (cp < 0x800 && room > 2) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room > 3) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

// Texto de un marco ID3 (byte de codificación + contenido) a UTF-8
static void id3_text(const uint8_t* p, size_t len, char* out, size_t out_sz) {
    size_t o = 0;
    if (len == 0 || out_sz == 0) return;
    uint8_t enc = p[0];
    p++;
    len--;

    if (enc == 0 || enc == 3) {
        // ISO-8859-1 (cada byte es su código) o UTF-8 tal cual
        for (size_t i = 0; i < len && p[i]; i++) {
            if (enc == 3) {
                if (o + 1 >= out_sz) break;
                out[o++] = (char)p[i];
            } else {
                size_t n = put_utf8(out + o, out_sz - o, p[i]);
                if (!n) break;
                o += n;
            }
        }
    } else {
        // UTF-16 con BOM (1) o big endian (2); solo plano básico
        bool big = (enc == 2);
        size_t i = 0;
        if (enc == 1 && len >= 2) {
            big = (p[0] == 0xFE && p[1] == 0xFF);
            i = 2;
        }
        for (; i + 1 < len; i += 2) {
            uint32_t cp = big ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
            if (cp == 0) break;
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            size_t n = put_utf8(out + o, out_sz - o, cp);
            if (!n) break;
            o += n;
        }
    }
    out[o] = '\0';
}

// Lee título y artista de la etiqueta ID3v2 si existe
static void read_id3(FILE* f, char* title, size_t title_sz, char* artist, size_t artist_sz) {
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) return;
    int ver = hdr[3];
    size_t tag_size = syncsafe(hdr + 6);
    size_t to_read = tag_size < 8192 ? tag_size : 8192; // los marcos de texto suelen ir primero
    uint8_t* buf = (uint8_t*)malloc(to_read);
    if (!buf) return;
    to_read = fread(buf, 1, to_read, f);

    size_t pos = 0;
    size_t id_len = (ver == 2) ? 3 : 4, hdr_len = (ver == 2) ? 6 : 10;
    while (pos + hdr_len <= to_read && buf[pos]) {
        const uint8_t* fr = buf + pos;
        size_t fsize = (ver == 2) ? ((size_t)fr[3] << 16 | fr[4] << 8 | fr[5])
                     : (ver == 4) ? syncsafe(fr + 4)
                     : ((size_t)fr[4] << 24 | (size_t)fr[5] << 16 | fr[6] << 8 | fr[7]);
        if (fsize == 0 || pos + hdr_len + fsize > to_read) break;

        const uint8_t* data = fr + hdr_len;
        if ((id_len == 4 && memcmp(fr, "TIT2", 4) == 0) || (id_len == 3 && memcmp(fr, "TT2", 3) == 0))
            id3_text(data, fsize, title, title_sz);
        else if ((id_len == 4 && memcmp(fr, "TPE1", 4) == 0) || (id_len == 3 && memcmp(fr, "TP1", 3) == 0))
            id3_text(data, fsize, artist, artist_sz);

        pos += hdr_len + fsize;
    }
    free(buf);
}

// ---------------------------------------------------------------------------
// Escaneo de carpetas
// ---------------------------------------------------------------------------

static bool has_mp3_ext(const char* name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp3") == 0;
}

static void fill_track(LocalTrack* t, const char* full, const char* dir, const char* name) {
    snprintf(t->path, sizeof(t->path), "%s", full);

    // Por defecto: "Artista - Título.mp3", o el nombre y la carpeta
    char base[128];
    snprintf(base, sizeof(base), "%s", name);
    base[strlen(base) - 4] = '\0';
    const char* dash = strstr(base, " - ");
    if (dash) {
        snprintf(t->artist, sizeof(t->artist), "%.*s", (int)(dash - base), base);
        snprintf(t->title, sizeof(t->title), "%s", dash + 3);
    } else {
        snprintf(t->title, sizeof(t->title), "%s", base);
        const char* slash = strrchr(dir, '/');
        snprintf(t->artist, sizeof(t->artist), "%s", slash ? slash + 1 : dir);
    }

    // La etiqueta ID3 manda si trae datos
    FILE* f = fopen(t->path, "rb");
    if (f) {
        char title[128] = "", artist[64] = "";
        read_id3(f, title, sizeof(title), artist, sizeof(artist));
        fclose(f);
        if (title[0]) snprintf(t->title, sizeof(t->title), "%s", title);
        if (artist[0]) snprintf(t->artist, sizeof(t->artist), "%s", artist);
    }
}

static void scan_dir(const char* dir, int depth, LocalTrack* out, int max, int* count) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while (*count < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (strlen(full) >= sizeof(((LocalTrack*)0)->path)) continue; // ruta demasiado larga para guardarla

        bool is_dir = (e->d_type == DT_DIR);
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            if (depth < LOCAL_MAX_DEPTH) scan_dir(full, depth + 1, out, max, count);
        } else if (has_mp3_ext(e->d_name)) {
            fill_track(&out[(*count)++], full, dir, e->d_name);
        }
    }
    closedir(d);
}

static int cmp_tracks(const void* a, const void* b) {
    const LocalTrack* ta = (const LocalTrack*)a;
    const LocalTrack* tb = (const LocalTrack*)b;
    int c = strcasecmp(ta->artist, tb->artist);
    return c ? c : strcasecmp(ta->title, tb->title);
}

int local_scan(LocalTrack* out, int max) {
    int count = 0;
    for (size_t i = 0; i < sizeof(s_music_dirs) / sizeof(s_music_dirs[0]); i++) {
        scan_dir(s_music_dirs[i], 0, out, max, &count);
    }
    qsort(out, (size_t)count, sizeof(LocalTrack), cmp_tracks);
    return count;
}

// ---------------------------------------------------------------------------
// Duración: cabecera de la primera trama MP3
// ---------------------------------------------------------------------------

static const int s_bitrates_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int s_bitrates_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const int s_rates[3] = {44100, 48000, 32000};

// Desplazamiento donde empieza el audio (tras la etiqueta ID3v2)
static long audio_start(FILE* f) {
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        return 10 + (long)syncsafe(hdr + 6) + ((hdr[5] & 0x10) ? 10 : 0);
    }
    return 0;
}

uint32_t local_estimate_duration_ms(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = audio_start(f);

    uint8_t buf[4096];
    fseek(f, start, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 4 < n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int version = (buf[i + 1] >> 3) & 3;   // 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5
        int layer = (buf[i + 1] >> 1) & 3;     // 1 = Layer III
        int br_idx = buf[i + 2] >> 4, sr_idx = (buf[i + 2] >> 2) & 3;
        if (version == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) continue;

        bool mpeg1 = (version == 3);
        int rate = s_rates[sr_idx] >> (mpeg1 ? 0 : (version == 2 ? 1 : 2));
        int kbps = mpeg1 ? s_bitrates_v1[br_idx] : s_bitrates_v2[br_idx];
        bool mono = ((buf[i + 3] >> 6) & 3) == 3;
        int samples_per_frame = mpeg1 ? 1152 : 576;

        // VBR: la cabecera Xing/Info dice cuántas tramas hay
        size_t xing = i + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
        if (xing + 12 <= n && (memcmp(buf + xing, "Xing", 4) == 0 || memcmp(buf + xing, "Info", 4) == 0) &&
            (buf[xing + 7] & 1)) {
            uint32_t frames = (uint32_t)buf[xing + 8] << 24 | buf[xing + 9] << 16 | buf[xing + 10] << 8 | buf[xing + 11];
            return (uint32_t)((uint64_t)frames * samples_per_frame * 1000 / rate);
        }
        // CBR (aproximado)
        return (uint32_t)((uint64_t)(size - start) * 8 / kbps);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Lectura en segundo plano
// ---------------------------------------------------------------------------

static void local_worker(void* arg) {
    FILE* f = fopen(s_local.path, "rb");
    if (!f) {
        snprintf(s_local.error, sizeof(s_local.error), "No se pudo abrir el archivo");
        s_local.streaming = false;
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = audio_start(f);
    long offset = start + (long)((float)(size - start) * s_local.ratio);
    fseek(f, offset, SEEK_SET);

    uint8_t* chunk = (uint8_t*)malloc(LOCAL_READ_CHUNK);
    while (chunk && !s_local.stop) {
        size_t n = fread(chunk, 1, LOCAL_READ_CHUNK, f);
        if (n == 0) break;
        size_t written = 0;
        while (!s_local.stop && written < n) {
            // Como en red: esperar mientras el ring buffer esté casi lleno
            if (audio_get_buffer_fill_percent() > 80) {
                svcSleepThread(15 * 1000 * 1000LL);
                continue;
            }
            written += audio_feed_mp3_data(chunk + written, n - written);
        }
    }
    free(chunk);
    fclose(f);
    s_local.streaming = false;
}

bool local_start_stream(const char* path, float ratio) {
    local_stop_stream();
    if (!path || !path[0]) return false;

    snprintf(s_local.path, sizeof(s_local.path), "%s", path);
    s_local.ratio = ratio < 0.0f ? 0.0f : (ratio > 0.99f ? 0.99f : ratio);
    s_local.error[0] = '\0';
    s_local.stop = false;
    s_local.streaming = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_local.thread = threadCreate(local_worker, NULL, LOCAL_THREAD_STACK_SZ, prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
    if (!s_local.thread) {
        s_local.streaming = false;
        snprintf(s_local.error, sizeof(s_local.error), "No se pudo crear el hilo de lectura");
        return false;
    }
    return true;
}

void local_stop_stream(void) {
    s_local.stop = true;
    if (s_local.thread) {
        threadJoin(s_local.thread, U64_MAX);
        threadFree(s_local.thread);
        s_local.thread = NULL;
    }
    s_local.streaming = false;
}

bool local_is_streaming(void) {
    return s_local.streaming;
}

const char* local_get_error(void) {
    return s_local.error;
}
