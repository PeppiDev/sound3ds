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

// Lee un marco de texto pequeño del archivo y lo convierte a UTF-8
static void read_text_frame(FILE* f, size_t fsize, char* out, size_t out_sz) {
    uint8_t buf[512];
    size_t n = fsize < sizeof(buf) ? fsize : sizeof(buf);
    if (fread(buf, 1, n, f) == n) id3_text(buf, n, out, out_sz);
}

// Recorre la etiqueta ID3v2 (2.2, 2.3 o 2.4) leyendo solo las cabeceras de los marcos:
// textos que interesan y la posición de la carátula, sin cargar la imagen
static void read_tags(FILE* f, LocalTrack* t, char* title, size_t title_sz, char* artist, size_t artist_sz) {
    uint8_t hdr[10];
    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) return;
    int ver = hdr[3];
    if (ver < 2 || ver > 4) return;
    long end = 10 + (long)syncsafe(hdr + 6);
    long pos = 10;

    // Cabecera extendida: se salta
    if ((hdr[5] & 0x40) && ver >= 3) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4) return;
        size_t ext_size = ver == 4 ? syncsafe(ext) : ((size_t)ext[0] << 24 | (size_t)ext[1] << 16 | ext[2] << 8 | ext[3]) + 4;
        pos += (long)ext_size;
    }

    size_t id_len = (ver == 2) ? 3 : 4, hdr_len = (ver == 2) ? 6 : 10;
    while (pos + (long)hdr_len <= end) {
        uint8_t fr[10];
        fseek(f, pos, SEEK_SET);
        if (fread(fr, 1, hdr_len, f) != hdr_len || fr[0] == 0) break;
        size_t fsize = (ver == 2) ? ((size_t)fr[3] << 16 | fr[4] << 8 | fr[5])
                     : (ver == 4) ? syncsafe(fr + 4)
                     : ((size_t)fr[4] << 24 | (size_t)fr[5] << 16 | fr[6] << 8 | fr[7]);
        long data = pos + (long)hdr_len;
        if (fsize == 0 || data + (long)fsize > end) break;

#define IS(id4, id3) ((id_len == 4 && memcmp(fr, id4, 4) == 0) || (id_len == 3 && memcmp(fr, id3, 3) == 0))
        char num[16];
        if (IS("TIT2", "TT2")) read_text_frame(f, fsize, title, title_sz);
        else if (IS("TPE1", "TP1")) read_text_frame(f, fsize, artist, artist_sz);
        else if (IS("TALB", "TAL")) read_text_frame(f, fsize, t->album, sizeof(t->album));
        else if (IS("TPE2", "TP2")) read_text_frame(f, fsize, t->album_artist, sizeof(t->album_artist));
        else if (IS("TRCK", "TRK")) { num[0] = 0; read_text_frame(f, fsize, num, sizeof(num)); t->track_no = atoi(num); }
        else if (IS("TLEN", "TLE")) { num[0] = 0; read_text_frame(f, fsize, num, sizeof(num)); t->duration_ms = (uint32_t)strtoul(num, NULL, 10); }
        else if (IS("APIC", "PIC") && t->cover_len == 0) {
            // Encabezado de la imagen: codificación, tipo MIME (o formato de 3 letras en 2.2),
            // tipo de imagen y descripción; después vienen los bytes de la imagen
            uint8_t head[256];
            size_t hn = fsize < sizeof(head) ? fsize : sizeof(head);
            if (fread(head, 1, hn, f) == hn && hn > 4) {
                uint8_t enc = head[0];
                size_t i = 1;
                if (ver == 2) i += 3;
                else { while (i < hn && head[i]) i++; i++; }
                i++; // tipo de imagen
                if (enc == 1 || enc == 2) { while (i + 1 < hn && (head[i] || head[i + 1])) i += 2; i += 2; }
                else { while (i < hn && head[i]) i++; i++; }
                if (i < hn && i < fsize) {
                    t->cover_off = (uint32_t)(data + (long)i);
                    t->cover_len = (uint32_t)(fsize - i);
                }
            }
        }
#undef IS
        pos = data + (long)fsize;
    }
}

uint8_t* local_read_cover(const LocalTrack* t, size_t* out_len) {
    if (!t || t->cover_len == 0 || t->cover_len > 4 * 1024 * 1024) return NULL;
    FILE* f = fopen(t->path, "rb");
    if (!f) return NULL;
    uint8_t* buf = (uint8_t*)malloc(t->cover_len);
    bool ok = buf && fseek(f, (long)t->cover_off, SEEK_SET) == 0 && fread(buf, 1, t->cover_len, f) == t->cover_len;
    fclose(f);
    if (!ok) {
        free(buf);
        return NULL;
    }
    if (out_len) *out_len = t->cover_len;
    return buf;
}

// ---------------------------------------------------------------------------
// Escritura de etiquetas (descargas)
// ---------------------------------------------------------------------------

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// Marco de texto ID3v2.3 en UTF-16 con BOM (la versión 2.3 no admite UTF-8)
static size_t frame_text(uint8_t* out, size_t room, const char* id, const char* utf8) {
    if (!utf8 || !utf8[0]) return 0;
    uint8_t body[600];
    size_t n = 0;
    body[n++] = 1;      // UTF-16
    body[n++] = 0xFF;   // BOM little endian
    body[n++] = 0xFE;
    const uint8_t* p = (const uint8_t*)utf8;
    while (*p && n + 2 < sizeof(body) - 2) {
        uint32_t cp = *p++;
        if (cp >= 0xC0) {
            int extra = cp >= 0xF0 ? 3 : (cp >= 0xE0 ? 2 : 1);
            cp &= (0x3F >> extra);
            for (int k = 0; k < extra && (*p & 0xC0) == 0x80; k++) cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp > 0xFFFF) cp = '?';
        body[n++] = (uint8_t)cp;
        body[n++] = (uint8_t)(cp >> 8);
    }
    if (10 + n > room) return 0;
    memcpy(out, id, 4);
    put_be32(out + 4, (uint32_t)n);
    out[8] = out[9] = 0;
    memcpy(out + 10, body, n);
    return 10 + n;
}

bool local_write_id3(FILE* f, const LocalTagInfo* info) {
    size_t cap = 4096 + (info->jpeg ? info->jpeg_len + 32 : 0);
    uint8_t* tag = (uint8_t*)malloc(cap);
    if (!tag) return false;
    size_t n = 10;
    char num[24];
    n += frame_text(tag + n, cap - n, "TIT2", info->title);
    n += frame_text(tag + n, cap - n, "TPE1", info->artist);
    n += frame_text(tag + n, cap - n, "TALB", info->album);
    n += frame_text(tag + n, cap - n, "TPE2", info->album_artist);
    if (info->track_no > 0) {
        snprintf(num, sizeof(num), "%d", info->track_no);
        n += frame_text(tag + n, cap - n, "TRCK", num);
    }
    if (info->duration_ms > 0) {
        snprintf(num, sizeof(num), "%lu", (unsigned long)info->duration_ms);
        n += frame_text(tag + n, cap - n, "TLEN", num);
    }
    if (info->jpeg && info->jpeg_len > 0) {
        // APIC: codificación latin-1, "image/jpeg", portada (3), descripción vacía
        static const char mime[] = "image/jpeg";
        size_t body = 1 + sizeof(mime) + 1 + 1 + info->jpeg_len;
        memcpy(tag + n, "APIC", 4);
        put_be32(tag + n + 4, (uint32_t)body);
        tag[n + 8] = tag[n + 9] = 0;
        size_t o = n + 10;
        tag[o++] = 0;
        memcpy(tag + o, mime, sizeof(mime));
        o += sizeof(mime);
        tag[o++] = 3;
        tag[o++] = 0;
        memcpy(tag + o, info->jpeg, info->jpeg_len);
        n = o + info->jpeg_len;
    }
    // Cabecera: versión 2.3, tamaño en formato syncsafe
    size_t size = n - 10;
    memcpy(tag, "ID3", 3);
    tag[3] = 3;
    tag[4] = 0;
    tag[5] = 0;
    tag[6] = (uint8_t)((size >> 21) & 0x7F);
    tag[7] = (uint8_t)((size >> 14) & 0x7F);
    tag[8] = (uint8_t)((size >> 7) & 0x7F);
    tag[9] = (uint8_t)(size & 0x7F);
    bool ok = fwrite(tag, 1, n, f) == n;
    free(tag);
    return ok;
}

// ---------------------------------------------------------------------------
// Escaneo de carpetas
// ---------------------------------------------------------------------------

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_audio_file(const char* name) {
    return has_ext(name, ".mp3") || has_ext(name, ".aac");
}

static void fill_track(LocalTrack* t, const char* full, const char* dir, const char* name) {
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", full);
    t->is_aac = has_ext(name, ".aac");

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
        fseek(f, 0, SEEK_END);
        t->file_size = (uint32_t)ftell(f);
        read_tags(f, t, title, sizeof(title), artist, sizeof(artist));
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
        } else if (is_audio_file(e->d_name)) {
            fill_track(&out[(*count)++], full, dir, e->d_name);
        }
    }
    closedir(d);
}

// Orden de los álbumes: SoundCloud primero, luego alfabético, y las pistas sin álbum al final
static int album_rank(const LocalTrack* t) {
    if (!t->album[0]) return 2;
    return strcasecmp(t->album, LOCAL_SC_ALBUM) == 0 ? 0 : 1;
}

static int cmp_tracks(const void* a, const void* b) {
    const LocalTrack* ta = (const LocalTrack*)a;
    const LocalTrack* tb = (const LocalTrack*)b;
    int c = album_rank(ta) - album_rank(tb);
    if (c) return c;
    c = strcasecmp(ta->album, tb->album);
    if (c) return c;
    // Dentro del álbum: por número de pista (las que no tienen, al final) y título
    int na = ta->track_no > 0 ? ta->track_no : 100000, nb = tb->track_no > 0 ? tb->track_no : 100000;
    if (na != nb) return na < nb ? -1 : 1;
    c = strcasecmp(ta->title, tb->title);
    return c ? c : strcasecmp(ta->artist, tb->artist);
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
    if (has_ext(path, ".aac")) {
        // ADTS sin TLEN: se asume ~128 kbps (las descargas siempre traen TLEN)
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        long size = end - audio_start(f);
        fclose(f);
        return size > 0 ? (uint32_t)((uint64_t)size * 8 / 128) : 0;
    }
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
